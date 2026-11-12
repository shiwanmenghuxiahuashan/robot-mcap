#include "ros2_adapter.hpp"

#include "robot_mcap/plugin_runtime.hpp"
#include "robot_mcap/recording_flow.hpp"

#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/generic_subscription.hpp>
#include <rclcpp/message_info.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/serialized_message.hpp>
#include <rclcpp/typesupport_helpers.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace robot_mcap::ros2 {
namespace {

std::uint64_t now_ns() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

class Ros2SourcePlugin;

class Ros2Precheck final : public IPrecheck {
public:
  explicit Ros2Precheck(Ros2SourcePlugin* plugin) : plugin_(plugin) {}
  PrecheckResult run(const StartContext&) override;

private:
  Ros2SourcePlugin* plugin_;
};

class Ros2SourcePlugin final : public robot_mcap::IPlugin {
public:
  Ros2SourcePlugin() = default;
  ~Ros2SourcePlugin() override {
    stopping_.store(true);
    if (executor_) {
      executor_->cancel();
    }
    if (discovery_timer_) {
      discovery_timer_->cancel();
    }
    if (executor_thread_.joinable()) {
      executor_thread_.join();
    }
  }

  PluginDescriptor describe() const override {
    PluginDescriptor descriptor;
    descriptor.id = "ros2_source";
    descriptor.requires = {
        require_service<IRos2SourceConfig>("IRos2SourceConfig"),
        require_service<IRos2Diagnostics>("IRos2Diagnostics"),
        require_service<IRos2SourceStats>("IRos2SourceStats"),
        require_service<robot_mcap::IMessageSink>("IMessageSink"),
        require_service<robot_mcap::IRecordingStatus>("IRecordingStatus"),
        require_service<robot_mcap::IPrecheckRegistry>("IPrecheckRegistry"),
    };
    return descriptor;
  }

  void apply(Context& context, Scope& scope) override {
    config_ = context.require<IRos2SourceConfig>();
    diagnostics_ = context.require<IRos2Diagnostics>();
    stats_ = context.require<IRos2SourceStats>();
    sink_ = context.require<robot_mcap::IMessageSink>();
    status_ = context.require<robot_mcap::IRecordingStatus>();
    auto precheck_registry = context.require<robot_mcap::IPrecheckRegistry>();

    for (const auto& option : config_->options().topics) {
      TopicState state;
      state.option = option;
      state.schema = load_message_schema(option.type);
      if (!state.schema.ok()) {
        diagnostics_->add({"ros2_schema_error", "error", option.topic, std::nullopt,
                           state.schema.error,
                           "reject_topic_and_continue"});
      }
      topics_.push_back(std::move(state));
    }

    static std::atomic<std::uint64_t> node_counter{0};
    std::string node_name = config_->options().node_name;
    if (node_name.empty()) {
      node_name = "robot_mcap_ros2_source";
    }
    node_name += "_" + std::to_string(node_counter.fetch_add(1) + 1);
    node_ = std::make_shared<rclcpp::Node>(node_name);

    scope.on_stop([this] { stop_executor(); });
    scope.on_wait([this] { join_executor(); });
    scope.on_cleanup([this] { cleanup(); });
    scope.own(precheck_registry->add("ros2_required_topics",
                                     std::make_shared<Ros2Precheck>(this)));
  }

  Status activate() override {
    if (!status_->recording()) {
      return {"ros2_source_activate", {}, "recording flow is not started"};
    }
    if (!node_) {
      return {"ros2_source_activate", {}, "ROS node was not created"};
    }
    executor_ = std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
    executor_->add_node(node_);
    discover();
    discovery_timer_ = node_->create_wall_timer(std::chrono::milliseconds(200),
                                                [this] { discover(); });
    executor_thread_ = std::thread([this] {
      try {
        executor_->spin();
      } catch (...) {
      }
    });
    return {};
  }

  PrecheckResult precheck() {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(2);
    for (;;) {
      const auto graph = node_ ? node_->get_topic_names_and_types()
                               : std::map<std::string, std::vector<std::string>>{};
      bool all_required_present = true;
      for (auto& state : topics_) {
        if (!state.option.required) {
          continue;
        }
        const auto entry = graph.find(state.option.topic);
        if (entry == graph.end() ||
            std::find(entry->second.begin(), entry->second.end(), state.option.type) ==
                entry->second.end()) {
          if (state.option.allow_late) {
            continue;
          }
          all_required_present = false;
          break;
        }
      }
      if (all_required_present || clock::now() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const auto graph = node_ ? node_->get_topic_names_and_types()
                             : std::map<std::string, std::vector<std::string>>{};
    for (auto& state : topics_) {
      const std::string name = state.option.topic;
      if (!state.schema.ok()) {
        if (state.option.required) {
          return {"", false, "message definition unavailable for " + name + ": " +
                                 state.schema.error};
        }
        continue;
      }
      try {
        (void)rclcpp::get_typesupport_library(state.option.type,
                                              "rosidl_typesupport_cpp");
      } catch (const std::exception& exception) {
        if (state.option.required) {
          return {"", false, "type support unavailable for " + name + " (" +
                                 state.option.type + "): " + exception.what()};
        }
        diagnostics_->add({"ros2_type_support_missing", "warning", name, now_ns(),
                           exception.what(), "skip_topic"});
        continue;
      }

      const auto entry = graph.find(name);
      if (entry == graph.end()) {
        if (state.option.required && state.option.allow_late) {
          diagnostics_->add({"ros2_topic_late_allowed", "warning", name, now_ns(),
                             "required topic is not present at start; configured to allow late appearance",
                             "wait_for_topic"});
          continue;
        }
        if (state.option.required) {
          return {"", false, "required topic is missing: " + name};
        }
        continue;
      }
      const bool expected =
          std::find(entry->second.begin(), entry->second.end(), state.option.type) !=
          entry->second.end();
      if (!expected || entry->second.size() > 1) {
        const std::string detail =
            "topic type conflict for " + name + ": expected " + state.option.type;
        if (state.option.required) {
          return {"", false, detail};
        }
        diagnostics_->add({"ros2_topic_type_conflict", "warning", name, now_ns(), detail,
                           "skip_topic"});
        continue;
      }
      state.present_at_start = true;
    }
    return {"", true, {}};
  }

private:
  struct TopicState {
    Ros2TopicOption option;
    SchemaText schema;
    bool present_at_start = false;
    bool subscribed = false;
    bool late_reported = false;
    bool conflict_reported = false;
    bool queue_full_reported = false;
    robot_mcap::ChannelId channel = 0;
    rclcpp::GenericSubscription::SharedPtr subscription;
  };

  void discover() {
    if (stopping_.load() || !node_) {
      return;
    }
    const auto graph = node_->get_topic_names_and_types();
    for (std::size_t index = 0; index < topics_.size(); ++index) {
      auto& state = topics_[index];
      const auto entry = graph.find(state.option.topic);
      if (entry == graph.end()) {
        continue;
      }
      const auto& types = entry->second;
      const bool expected =
          std::find(types.begin(), types.end(), state.option.type) != types.end();
      if (!expected || types.size() > 1) {
        if (!state.conflict_reported) {
          diagnostics_->add(
              {"ros2_topic_type_conflict", "warning", state.option.topic, now_ns(),
               "discovered types do not uniquely match " + state.option.type,
               "skip_topic"});
          state.conflict_reported = true;
        }
        continue;
      }
      if (state.subscribed) {
        continue;
      }
      if (!create_subscription(index)) {
        continue;
      }
      if (!state.present_at_start && !state.late_reported) {
        diagnostics_->add({"ros2_topic_late", "warning", state.option.topic, now_ns(),
                           "topic appeared after recording started", "subscribe_late"});
        state.late_reported = true;
      }
    }
  }

  bool create_subscription(std::size_t index) {
    auto& state = topics_[index];
    if (!state.schema.ok()) {
      diagnostics_->add({"ros2_schema_error", "error", state.option.topic, now_ns(),
                         state.schema.error, "skip_topic"});
      return false;
    }
    const robot_mcap::ChannelSpec spec{
        state.option.topic, "cdr",
        robot_mcap::SchemaSpec{state.schema.type, state.schema.encoding,
                               state.schema.data},
        robot_mcap::PublishTimePolicy::PerMessage};
    const Status registered = sink_->register_channel(spec, state.channel);
    if (!registered.ok()) {
      diagnostics_->add({"ros2_channel_registration_failed", "error",
                         state.option.topic, now_ns(),
                         registered.operation + ": " + registered.detail,
                         "skip_topic"});
      return false;
    }
    try {
      state.subscription = node_->create_generic_subscription(
          state.option.topic, state.option.type, state.option.qos.to_rclcpp(),
          [this, index](std::shared_ptr<rclcpp::SerializedMessage> message,
                        const rclcpp::MessageInfo& info) {
            on_message(index, message, info);
          });
    } catch (const std::exception& exception) {
      diagnostics_->add({"ros2_subscription_failed", "error", state.option.topic,
                         now_ns(), exception.what(), "skip_topic"});
      return false;
    }
    state.subscribed = true;
    return true;
  }

  void on_message(std::size_t index,
                  const std::shared_ptr<rclcpp::SerializedMessage>& message,
                  const rclcpp::MessageInfo& info) {
    if (!sink_ || index >= topics_.size()) {
      return;
    }
    auto& state = topics_[index];
    const auto& rmw_info = info.get_rmw_message_info();
    const std::uint64_t log_time =
        rmw_info.received_timestamp > 0
            ? static_cast<std::uint64_t>(rmw_info.received_timestamp)
            : now_ns();
    std::optional<std::uint64_t> publish_time;
    if (rmw_info.source_timestamp > 0) {
      publish_time = static_cast<std::uint64_t>(rmw_info.source_timestamp);
    }
    const auto& serialized = message->get_rcl_serialized_message();
    const robot_mcap::MessageView view{
        log_time,
        publish_time,
        reinterpret_cast<const std::byte*>(serialized.buffer),
        serialized.buffer_length,
        0,
        std::nullopt};
    const auto result = sink_->submit_copy(state.channel, view);
    if (result.ok()) {
      stats_->received(state.option.topic);
    } else if (result.error == robot_mcap::SubmitError::QueueFull &&
               !state.queue_full_reported) {
      diagnostics_->add({"ros2_queue_full", "warning", state.option.topic, log_time,
                         "recording queue rejected a message", "drop_message"});
      state.queue_full_reported = true;
    }
  }

  void stop_executor() {
    stopping_.store(true);
    if (discovery_timer_) {
      discovery_timer_->cancel();
    }
    if (executor_) {
      executor_->cancel();
    }
  }

  void join_executor() {
    if (executor_thread_.joinable()) {
      executor_thread_.join();
    }
  }

  void cleanup() {
    stop_executor();
    join_executor();
    for (auto& state : topics_) {
      state.subscription.reset();
    }
    discovery_timer_.reset();
    executor_.reset();
    node_.reset();
  }

  std::shared_ptr<IRos2SourceConfig> config_;
  std::shared_ptr<IRos2Diagnostics> diagnostics_;
  std::shared_ptr<IRos2SourceStats> stats_;
  std::shared_ptr<robot_mcap::IMessageSink> sink_;
  std::shared_ptr<robot_mcap::IRecordingStatus> status_;
  rclcpp::Node::SharedPtr node_;
  rclcpp::executors::SingleThreadedExecutor::SharedPtr executor_;
  rclcpp::TimerBase::SharedPtr discovery_timer_;
  std::vector<TopicState> topics_;
  std::thread executor_thread_;
  std::atomic<bool> stopping_{false};
};

PrecheckResult Ros2Precheck::run(const StartContext&) { return plugin_->precheck(); }

}  // namespace

std::unique_ptr<robot_mcap::IPlugin> make_ros2_source_plugin() {
  return std::make_unique<Ros2SourcePlugin>();
}

}  // namespace robot_mcap::ros2

extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const robot_mcap::RobotMcapPluginModuleV1* robot_mcap_plugin_module_v1() {
  static const robot_mcap::RobotMcapPluginModuleV1 module{
      robot_mcap::kPluginModuleAbiVersion,
      "ros2_source",
      []() -> robot_mcap::IPlugin* {
        return robot_mcap::ros2::make_ros2_source_plugin().release();
      },
      [](robot_mcap::IPlugin* plugin) { delete plugin; },
  };
  return &module;
}
}
