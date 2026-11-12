#include "ros2_adapter.hpp"

#include "robot_mcap/plugin_runtime.hpp"
#include "robot_mcap/recorder.hpp"
#include "robot_mcap/recording_flow.hpp"

#include <mcap/reader.hpp>

#include <nlohmann/json.hpp>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/int64.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using robot_mcap::ros2::IRos2Diagnostics;
using robot_mcap::ros2::IRos2SourceConfig;
using robot_mcap::ros2::IRos2SourceStats;
using robot_mcap::ros2::Ros2DiagnosticsLog;
using robot_mcap::ros2::Ros2MetadataWriter;
using robot_mcap::ros2::Ros2SourceOptions;
using robot_mcap::ros2::Ros2SourceStats;
using robot_mcap::ros2::Ros2TopicOption;
using robot_mcap::ros2::Ros2Qos;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) +   \
                               ": check failed: " #condition);                            \
    }                                                                                     \
  } while (false)

void check_ok(const robot_mcap::Status& status) {
  if (!status.ok()) {
    throw std::runtime_error(status.operation + " " + status.path.string() + ": " +
                             status.detail);
  }
}

#define CHECK_OK(expression) check_ok(expression)

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open " + path.string());
  }
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

void reset_directory(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
  std::filesystem::create_directories(path);
}

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(20ms);
  }
  return true;
}

std::pair<int, std::string> run_capture(const std::string& command) {
  std::string output;
  std::FILE* pipe = popen((command + " 2>&1").c_str(), "r");
  if (pipe == nullptr) {
    return {-1, "popen failed"};
  }
  char buffer[4096];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  const int status = pclose(pipe);
  return {status, output};
}

class ConfigHolder final : public IRos2SourceConfig {
public:
  explicit ConfigHolder(Ros2SourceOptions options) : options_(std::move(options)) {}
  const Ros2SourceOptions& options() const override { return options_; }

private:
  Ros2SourceOptions options_;
};

class Fixture {
public:
  Fixture(const std::filesystem::path& parent, std::string session_id,
          Ros2SourceOptions options, const std::filesystem::path& module_path)
      : parent_(parent),
        session_id_(std::move(session_id)),
        options_(std::move(options)),
        module_path_(module_path),
        config_(std::make_shared<ConfigHolder>(options_)),
        diagnostics_(std::make_shared<Ros2DiagnosticsLog>()),
        stats_(std::make_shared<Ros2SourceStats>()) {
    robot_mcap::RecorderOptions recorder_options;
    recorder_options.parent_directory = parent_;
    recorder_options.session_id = session_id_;
    recorder_ = std::make_shared<robot_mcap::Recorder>(recorder_options);
    sink_ = std::make_shared<robot_mcap::CoreRecordingSink>(recorder_);
    flow_ = std::make_shared<robot_mcap::RecordingFlow>(
        robot_mcap::StartContext{parent_, 0});
    runtime_ = std::make_unique<robot_mcap::PluginRuntime>();
    check_ok(runtime_->bind_host_service<robot_mcap::IRecordingSink>(sink_,
                                                                    "IRecordingSink"));
    check_ok(runtime_->bind_host_service<IRos2SourceConfig>(config_,
                                                            "IRos2SourceConfig"));
    check_ok(runtime_->bind_host_service<IRos2Diagnostics>(diagnostics_,
                                                           "IRos2Diagnostics"));
    check_ok(runtime_->bind_host_service<IRos2SourceStats>(stats_,
                                                           "IRos2SourceStats"));
    check_ok(runtime_->add_plugin_factory(
        "recording_flow", robot_mcap::recording_flow_factory(flow_)));
    check_ok(runtime_->load_plugin_module(module_path_));
  }

  robot_mcap::Status start() {
    return runtime_->start({"ros2_source", "recording_flow"});
  }

  robot_mcap::RuntimeStopResult stop() { return runtime_->stop(); }

  const Ros2SourceOptions& options() const { return options_; }
  const std::shared_ptr<Ros2DiagnosticsLog>& diagnostics() const { return diagnostics_; }
  const std::shared_ptr<Ros2SourceStats>& stats() const { return stats_; }
  const std::shared_ptr<robot_mcap::RecordingFlow>& flow() const { return flow_; }
  std::filesystem::path session_directory() const { return parent_ / session_id_; }

private:
  std::filesystem::path parent_;
  std::string session_id_;
  Ros2SourceOptions options_;
  std::filesystem::path module_path_;
  std::shared_ptr<robot_mcap::Recorder> recorder_;
  std::shared_ptr<robot_mcap::CoreRecordingSink> sink_;
  std::shared_ptr<robot_mcap::RecordingFlow> flow_;
  std::shared_ptr<ConfigHolder> config_;
  std::shared_ptr<Ros2DiagnosticsLog> diagnostics_;
  std::shared_ptr<Ros2SourceStats> stats_;
  std::unique_ptr<robot_mcap::PluginRuntime> runtime_;
};

std::size_t count_messages(const std::filesystem::path& path) {
  mcap::McapReader reader;
  if (!reader.open(path.string()).ok()) {
    return 0;
  }
  std::size_t count = 0;
  auto view = reader.readMessages();
  for (auto iterator = view.begin(); iterator != view.end(); ++iterator) {
    ++count;
  }
  if (!view.status().ok()) {
    return 0;
  }
  reader.close();
  return count;
}

void test_schema_expansion() {
  const auto schema = robot_mcap::ros2::load_message_schema("geometry_msgs/msg/Twist");
  CHECK(schema.ok());
  CHECK(schema.encoding == "ros2msg");
  CHECK(schema.data.find("Vector3") != std::string::npos);
  CHECK(schema.data.find("float64 x") != std::string::npos);
  const auto missing =
      robot_mcap::ros2::load_message_schema("no_such_package/msg/NoSuchType");
  CHECK(!missing.ok());
}

void test_missing_required_topic(const std::filesystem::path& root,
                                 const std::filesystem::path& module_path) {
  const auto directory = root / "missing";
  reset_directory(directory);
  Ros2SourceOptions options;
  Ros2TopicOption topic;
  topic.topic = "/test/missing";
  topic.type = "std_msgs/msg/Int32";
  topic.required = true;
  options.topics.push_back(topic);
  Fixture fixture(directory, "missing-session", options, module_path);
  const auto status = fixture.start();
  CHECK(!status.ok());
  CHECK(status.detail.find("required topic is missing") != std::string::npos);
  CHECK(fixture.flow()->result().start == robot_mcap::FlowStartState::Failed);
  CHECK(!fixture.flow()->result().stop.has_value());
  CHECK(!std::filesystem::exists(fixture.session_directory()));
  CHECK(!fixture.stop().ok());
}

void test_metadata_failure(const std::filesystem::path& root,
                           const std::filesystem::path& module_path) {
  const auto directory = root / "metadata-failure";
  reset_directory(directory);
  Ros2SourceOptions options;
  options.topics.push_back(
      {"/test/int", "std_msgs/msg/Int32", true, false, Ros2Qos{}});

  auto publisher_node = rclcpp::Node::make_shared("metadata_failure_publisher");
  auto publisher = publisher_node->create_publisher<std_msgs::msg::Int32>(
      "/test/int", rclcpp::QoS(10).reliable());

  Fixture fixture(directory, "metadata-failure-session", options, module_path);
  CHECK_OK(fixture.start());
  CHECK(wait_for([&] { return publisher->get_subscription_count() > 0; }, 5000ms));
  for (int value = 1; value <= 2; ++value) {
    std_msgs::msg::Int32 message;
    message.data = value;
    publisher->publish(message);
  }
  CHECK(wait_for([&] { return fixture.stats()->count("/test/int") == 2; }, 5000ms));
  const auto stopped = fixture.stop();
  CHECK(stopped.ok());

  Ros2MetadataWriter writer({true});
  const auto failed = writer.write(fixture.session_directory(), options,
                                   *fixture.diagnostics());
  CHECK(!failed.ok());
  CHECK(failed.detail.find("injected") != std::string::npos);

  const auto manifest = nlohmann::json::parse(
      read_file(fixture.session_directory() / "manifest.json"));
  CHECK(manifest["ros2_replay"]["state"] == "failed");
  CHECK(manifest["ros2_replay"]["metadata_path"].is_null());
  bool found_metadata_error = false;
  for (const auto& diagnostic : manifest["diagnostics"]) {
    if (diagnostic["code"] == "ros2_metadata_error") {
      found_metadata_error = true;
    }
  }
  CHECK(found_metadata_error);
  CHECK(!std::filesystem::exists(fixture.session_directory() / "metadata.yaml"));
  CHECK(count_messages(fixture.session_directory() / "data_0000.mcap") == 2);
}

void test_record_and_replay(const std::filesystem::path& root,
                            const std::filesystem::path& module_path) {
  const auto directory = root / "record-replay";
  reset_directory(directory);
  Ros2SourceOptions options;
  options.topics = {
      {"/test/int", "std_msgs/msg/Int32", true, false, Ros2Qos{}},
      {"/test/late", "std_msgs/msg/Int32", true, true, Ros2Qos{}},
      {"/test/conflict", "std_msgs/msg/Int32", false, true, Ros2Qos{}},
      {"/test/best_effort", "std_msgs/msg/Int32", true, false, Ros2Qos{5, false, false}},
  };

  auto publisher_node = rclcpp::Node::make_shared("record_replay_publisher");
  auto int_publisher = publisher_node->create_publisher<std_msgs::msg::Int32>(
      "/test/int", rclcpp::QoS(10).reliable());
  auto best_effort_publisher = publisher_node->create_publisher<std_msgs::msg::Int32>(
      "/test/best_effort", rclcpp::QoS(5).best_effort());
  auto unselected_publisher = publisher_node->create_publisher<std_msgs::msg::Int32>(
      "/test/unselected", rclcpp::QoS(10).reliable());

  Fixture fixture(directory, "record-replay-session", options, module_path);
  CHECK_OK(fixture.start());
  CHECK(wait_for([&] { return int_publisher->get_subscription_count() > 0; }, 5000ms));
  CHECK(wait_for(
      [&] { return best_effort_publisher->get_subscription_count() > 0; }, 5000ms));

  auto late_publisher = publisher_node->create_publisher<std_msgs::msg::Int32>(
      "/test/late", rclcpp::QoS(10).reliable());
  CHECK(wait_for([&] { return late_publisher->get_subscription_count() > 0; }, 5000ms));

  auto conflict_publisher = publisher_node->create_publisher<std_msgs::msg::Int64>(
      "/test/conflict", rclcpp::QoS(10).reliable());
  CHECK(wait_for(
      [&] {
        for (const auto& diagnostic : fixture.diagnostics()->snapshot()) {
          if (diagnostic.code == "ros2_topic_type_conflict" &&
              diagnostic.topic.value_or("") == "/test/conflict") {
            return true;
          }
        }
        return false;
      },
      5000ms));
  CHECK(conflict_publisher->get_subscription_count() == 0);

  for (int value = 1; value <= 5; ++value) {
    std_msgs::msg::Int32 message;
    message.data = value;
    int_publisher->publish(message);
    std::this_thread::sleep_for(10ms);
  }
  for (int value = 11; value <= 13; ++value) {
    std_msgs::msg::Int32 message;
    message.data = value;
    late_publisher->publish(message);
    std::this_thread::sleep_for(10ms);
  }
  for (int value = 21; value <= 22; ++value) {
    std_msgs::msg::Int32 message;
    message.data = value;
    best_effort_publisher->publish(message);
    std::this_thread::sleep_for(10ms);
  }
  {
    std_msgs::msg::Int32 message;
    message.data = 99;
    unselected_publisher->publish(message);
  }
  CHECK(wait_for(
      [&] {
        return fixture.stats()->count("/test/int") == 5 &&
               fixture.stats()->count("/test/late") == 3 &&
               fixture.stats()->count("/test/best_effort") == 2;
      },
      5000ms));
  CHECK(fixture.stats()->count("/test/unselected") == 0);

  const auto stopped = fixture.stop();
  CHECK(stopped.ok());
  CHECK(fixture.flow()->result().stop.has_value());
  CHECK(fixture.flow()->result().stop->ok());
  CHECK(fixture.flow()->result().stop->file_state ==
        robot_mcap::FileState::Finalized);
  CHECK(count_messages(fixture.session_directory() / "data_0000.mcap") == 10);

  Ros2MetadataWriter writer;
  CHECK_OK(writer.write(fixture.session_directory(), options,
                        *fixture.diagnostics()));
  const auto metadata_path = fixture.session_directory() / "metadata.yaml";
  CHECK(std::filesystem::exists(metadata_path));
  const auto metadata = read_file(metadata_path);
  CHECK(metadata.find("storage_identifier: mcap") != std::string::npos);
  CHECK(metadata.find("message_count: 10") != std::string::npos);
  CHECK(metadata.find("/test/int") != std::string::npos);
  CHECK(metadata.find("/test/late") != std::string::npos);
  CHECK(metadata.find("/test/best_effort") != std::string::npos);
  CHECK(metadata.find("std_msgs/msg/Int32") != std::string::npos);
  CHECK(metadata.find("reliability: best_effort") != std::string::npos);
  CHECK(metadata.find("/test/unselected") == std::string::npos);

  const auto manifest = nlohmann::json::parse(
      read_file(fixture.session_directory() / "manifest.json"));
  CHECK(manifest["state"] == "finalized");
  CHECK(manifest["ros2_replay"]["state"] == "ready");
  CHECK(manifest["ros2_replay"]["metadata_path"] == "metadata.yaml");
  CHECK(manifest["channels"].size() == 3);
  bool unselected_channel = false;
  for (const auto& channel : manifest["channels"]) {
    if (channel["topic"] == "/test/unselected") {
      unselected_channel = true;
    }
  }
  CHECK(!unselected_channel);
  bool late_allowed = false;
  bool late_seen = false;
  bool conflict_seen = false;
  bool queue_full = false;
  for (const auto& diagnostic : manifest["diagnostics"]) {
    const auto code = diagnostic["code"].get<std::string>();
    late_allowed = late_allowed || code == "ros2_topic_late_allowed";
    late_seen = late_seen || code == "ros2_topic_late";
    conflict_seen = conflict_seen || code == "ros2_topic_type_conflict";
    queue_full = queue_full || code == "ros2_queue_full";
  }
  CHECK(late_allowed);
  CHECK(late_seen);
  CHECK(conflict_seen);
  CHECK(!queue_full);

  const auto info = run_capture("ros2 bag info \"" +
                                fixture.session_directory().string() + "\"");
  CHECK(info.first == 0);
  CHECK(info.second.find("Messages:") != std::string::npos);
  CHECK(info.second.find("Count: 5") != std::string::npos);
  CHECK(info.second.find("Count: 3") != std::string::npos);
  CHECK(info.second.find("Count: 2") != std::string::npos);
  CHECK(info.second.find("/test/int") != std::string::npos);
  CHECK(info.second.find("/test/late") != std::string::npos);
  CHECK(info.second.find("/test/best_effort") != std::string::npos);
  CHECK(info.second.find("std_msgs/msg/Int32") != std::string::npos);
  CHECK(info.second.find("/test/unselected") == std::string::npos);

  std::mutex values_mutex;
  std::vector<std::int32_t> int_values;
  std::vector<std::int32_t> late_values;
  std::vector<std::int32_t> best_effort_values;
  auto subscriber_node = rclcpp::Node::make_shared("record_replay_subscriber");
  auto int_subscription =
      subscriber_node->create_subscription<std_msgs::msg::Int32>(
          "/test/int", rclcpp::QoS(10).reliable(),
          [&](const std_msgs::msg::Int32::SharedPtr message) {
            std::lock_guard lock(values_mutex);
            int_values.push_back(message->data);
          });
  auto late_subscription =
      subscriber_node->create_subscription<std_msgs::msg::Int32>(
          "/test/late", rclcpp::QoS(10).reliable(),
          [&](const std_msgs::msg::Int32::SharedPtr message) {
            std::lock_guard lock(values_mutex);
            late_values.push_back(message->data);
          });
  auto best_effort_subscription =
      subscriber_node->create_subscription<std_msgs::msg::Int32>(
          "/test/best_effort", rclcpp::QoS(5).best_effort(),
          [&](const std_msgs::msg::Int32::SharedPtr message) {
            std::lock_guard lock(values_mutex);
            best_effort_values.push_back(message->data);
          });
  auto executor = std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
  executor->add_node(subscriber_node);
  std::thread spin_thread([&] {
    try {
      executor->spin();
    } catch (...) {
    }
  });

  const auto play = run_capture("ros2 bag play --delay 1.0 \"" +
                                fixture.session_directory().string() + "\"");
  CHECK(play.first == 0);
  const bool received = wait_for(
      [&] {
        std::lock_guard lock(values_mutex);
        return int_values.size() == 5 && late_values.size() == 3 &&
               best_effort_values.size() == 2;
      },
      5000ms);
  executor->cancel();
  spin_thread.join();
  CHECK(received);
  {
    std::lock_guard lock(values_mutex);
    std::sort(int_values.begin(), int_values.end());
    std::sort(late_values.begin(), late_values.end());
    std::sort(best_effort_values.begin(), best_effort_values.end());
    CHECK(int_values == std::vector<std::int32_t>({1, 2, 3, 4, 5}));
    CHECK(late_values == std::vector<std::int32_t>({11, 12, 13}));
    CHECK(best_effort_values == std::vector<std::int32_t>({21, 22}));
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "expected plugin module path and test output directory\n";
      return 1;
    }
    rclcpp::init(0, nullptr);
    const std::filesystem::path module_path = argv[1];
    const auto root = std::filesystem::path(argv[2]);
    reset_directory(root);

    test_schema_expansion();
    test_missing_required_topic(root, module_path);
    test_metadata_failure(root, module_path);
    test_record_and_replay(root, module_path);

    rclcpp::shutdown();
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    if (rclcpp::ok()) {
      rclcpp::shutdown();
    }
    return 1;
  }
  return 0;
}
