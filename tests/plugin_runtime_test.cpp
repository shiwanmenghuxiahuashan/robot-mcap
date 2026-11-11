#include "plugin_test_support.hpp"

#include "robot_mcap/plugin_runtime.hpp"
#include "robot_mcap/recording_flow.hpp"

#include <mcap/reader.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using robot_mcap::ChannelId;
using robot_mcap::ChannelSpec;
using robot_mcap::Context;
using robot_mcap::IPlugin;
using robot_mcap::IRecordingSink;
using robot_mcap::MessageView;
using robot_mcap::OwnedMessage;
using robot_mcap::PluginDescriptor;
using robot_mcap::PluginFactory;
using robot_mcap::PluginRuntime;
using robot_mcap::RecordingFlow;
using robot_mcap::RecordingFlowPlugin;
using robot_mcap::RecordingFlowResult;
using robot_mcap::RuntimeState;
using robot_mcap::RuntimeStopResult;
using robot_mcap::Scope;
using robot_mcap::ServiceProvision;
using robot_mcap::ServiceRequirement;
using robot_mcap::StartContext;
using robot_mcap::Status;
using robot_mcap::StopResult;
using robot_mcap::SubmitResult;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) +   \
                               ": check failed: " #condition);                            \
    }                                                                                     \
  } while (false)

void check_ok(const Status& status) {
  if (!status.ok()) {
    throw std::runtime_error(status.operation + " " + status.path.string() + ": " +
                             status.detail);
  }
}

#define CHECK_OK(expression) check_ok(expression)

class EventLog {
public:
  void add(std::string event) {
    std::lock_guard lock(mutex_);
    events_.push_back(std::move(event));
  }

  std::vector<std::string> events() const {
    std::lock_guard lock(mutex_);
    return events_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> events_;
};

void check_order(const EventLog& log,
                 std::initializer_list<std::string_view> expected) {
  const auto events = log.events();
  std::size_t position = 0;
  for (const auto needle : expected) {
    bool found = false;
    for (; position < events.size(); ++position) {
      if (events[position] == needle) {
        found = true;
        ++position;
        break;
      }
    }
    if (!found) {
      std::string seen;
      for (const auto& event : events) {
        seen += event + " ";
      }
      throw std::runtime_error("missing event order: " + std::string(needle) +
                               " in [" + seen + "]");
    }
  }
}

class ITestServiceA {
public:
  virtual ~ITestServiceA() = default;
};

class ITestServiceB {
public:
  virtual ~ITestServiceB() = default;
};

class TestServiceA final : public ITestServiceA {};
class TestServiceB final : public ITestServiceB {};

class IHostService {
public:
  virtual ~IHostService() = default;
  virtual int value() const = 0;
};

class HostService final : public IHostService {
public:
  explicit HostService(int value) : value_(value) {}
  int value() const override { return value_; }

private:
  int value_;
};

PluginDescriptor make_descriptor(std::string id) {
  PluginDescriptor descriptor;
  descriptor.id = std::move(id);
  return descriptor;
}

class TestPlugin final : public IPlugin {
public:
  using Apply = std::function<void(Context&, Scope&)>;

  TestPlugin(PluginDescriptor descriptor, Apply apply,
             std::function<Status()> activate)
      : descriptor_(std::move(descriptor)),
        apply_(std::move(apply)),
        activate_(std::move(activate)) {}

  PluginDescriptor describe() const override { return descriptor_; }
  void apply(Context& context, Scope& scope) override {
    if (apply_) {
      apply_(context, scope);
    }
  }
  Status activate() override { return activate_ ? activate_() : Status{}; }

private:
  PluginDescriptor descriptor_;
  Apply apply_;
  std::function<Status()> activate_;
};

void add_plugin(PluginRuntime& runtime, PluginDescriptor descriptor,
                TestPlugin::Apply apply = {}, std::function<Status()> activate = {}) {
  const std::string id = descriptor.id;
  check_ok(runtime.add_plugin_factory(
      id, [descriptor = std::move(descriptor), apply = std::move(apply),
           activate = std::move(activate)]() -> std::shared_ptr<IPlugin> {
        return std::make_shared<TestPlugin>(descriptor, apply, activate);
      }));
}

class FakeSink final : public IRecordingSink {
public:
  FakeSink(EventLog& log, std::filesystem::path output_directory)
      : log_(log), output_directory_(std::move(output_directory)) {}

  Status start() override {
    ++start_calls;
    log_.add("sink.start");
    if (fail_start) {
      return {"sink_start", {}, "injected start failure"};
    }
    return {};
  }

  Status register_channel(const ChannelSpec&, ChannelId& id) override {
    id = 1;
    return {};
  }

  SubmitResult submit_copy(ChannelId, MessageView) override { return {}; }
  SubmitResult submit_owned(ChannelId, OwnedMessage&&) override { return {}; }

  StopResult stop() override {
    ++stop_calls;
    log_.add("sink.stop");
    return stop_result;
  }

  std::filesystem::path output_directory() const override { return output_directory_; }

  int start_calls = 0;
  int stop_calls = 0;
  bool fail_start = false;
  StopResult stop_result;

private:
  EventLog& log_;
  std::filesystem::path output_directory_;
};

class Probe final : public robot_mcap_test::IProbe {
public:
  explicit Probe(EventLog& log) : log_(log) {}
  void record(const std::string& event) override { log_.add(event); }

private:
  EventLog& log_;
};

std::filesystem::path test_directory(const std::filesystem::path& root,
                                     std::string_view name) {
  const auto directory = root / std::string(name);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory);
  return directory;
}

void test_graph_and_shutdown_order(const std::filesystem::path& root) {
  (void)test_directory(root, "graph");
  EventLog log;
  PluginRuntime runtime;

  auto provider = make_descriptor("provider_a");
  provider.provides = {robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
  add_plugin(
      runtime, provider,
      [&log](Context& context, Scope& scope) {
        log.add("provider.apply");
        scope.on_stop([&log] { log.add("provider.stop"); });
        scope.on_wait([&log] { log.add("provider.wait"); });
        scope.on_finalize([&log] { log.add("provider.finalize"); });
        scope.on_cleanup([&log] { log.add("provider.cleanup"); });
        scope.own(context.provide<ITestServiceA>(std::make_shared<TestServiceA>()));
      },
      [&log] {
        log.add("provider.activate");
        return Status{};
      });

  auto consumer = make_descriptor("consumer_b");
  consumer.requires = {robot_mcap::require_service<ITestServiceA>("ITestServiceA")};
  add_plugin(
      runtime, consumer,
      [&log](Context& context, Scope& scope) {
        log.add("consumer.apply");
        const auto service = context.require<ITestServiceA>();
        CHECK(service != nullptr);
        scope.on_stop([&log] { log.add("consumer.stop"); });
        scope.on_wait([&log] { log.add("consumer.wait"); });
        scope.on_finalize([&log] { log.add("consumer.finalize"); });
        scope.on_cleanup([&log] { log.add("consumer.cleanup"); });
      },
      [&log] {
        log.add("consumer.activate");
        return Status{};
      });

  auto host_consumer = make_descriptor("host_consumer_c");
  host_consumer.requires = {robot_mcap::require_service<IHostService>("IHostService")};
  add_plugin(
      runtime, host_consumer,
      [&log](Context& context, Scope& scope) {
        const auto service = context.require<IHostService>();
        log.add("host.apply=" + std::to_string(service->value()));
        scope.on_stop([&log] { log.add("host.stop"); });
        scope.on_wait([&log] { log.add("host.wait"); });
        scope.on_finalize([&log] { log.add("host.finalize"); });
        scope.on_cleanup([&log] { log.add("host.cleanup"); });
      },
      [&log] {
        log.add("host.activate");
        return Status{};
      });

  CHECK_OK(runtime.bind_host_service<IHostService>(std::make_shared<HostService>(7),
                                                   "IHostService"));
  CHECK_OK(runtime.start({"consumer_b", "provider_a", "host_consumer_c"}));
  CHECK(runtime.state() == RuntimeState::Running);
  check_order(log, {"provider.apply", "consumer.apply", "host.apply=7",
                    "provider.activate", "consumer.activate", "host.activate"});

  const RuntimeStopResult stopped = runtime.stop();
  CHECK(stopped.ok());
  CHECK(runtime.state() == RuntimeState::Stopped);
  check_order(log, {"host.stop", "consumer.stop", "provider.stop", "host.wait",
                    "consumer.wait", "provider.wait", "host.finalize",
                    "consumer.finalize", "provider.finalize", "host.cleanup",
                    "consumer.cleanup", "provider.cleanup"});
  const RuntimeStopResult again = runtime.stop();
  CHECK(again.ok());
}

void test_missing_dependency(const std::filesystem::path& root) {
  (void)test_directory(root, "missing-dependency");
  EventLog log;
  PluginRuntime runtime;
  auto consumer = make_descriptor("consumer");
  consumer.requires = {robot_mcap::require_service<ITestServiceA>("ITestServiceA")};
  add_plugin(runtime, consumer, [&log](Context&, Scope&) { log.add("consumer.apply"); });

  const Status status = runtime.start({"consumer"});
  CHECK(!status.ok());
  CHECK(status.detail.find("missing dependency") != std::string::npos);
  CHECK(runtime.state() == RuntimeState::Stopped);
  CHECK(log.events().empty());
  const RuntimeStopResult stopped = runtime.stop();
  CHECK(!stopped.error.ok());
  CHECK(stopped.error.detail.find("missing dependency") != std::string::npos);
}

void test_duplicate_provider(const std::filesystem::path& root) {
  (void)test_directory(root, "duplicate-provider");
  PluginRuntime runtime;
  auto first = make_descriptor("first");
  first.provides = {robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
  auto second = make_descriptor("second");
  second.provides = {robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
  add_plugin(runtime, first);
  add_plugin(runtime, second);

  const Status status = runtime.start({"first", "second"});
  CHECK(!status.ok());
  CHECK(status.detail.find("duplicate provider") != std::string::npos);
}

void test_version_mismatch(const std::filesystem::path& root) {
  (void)test_directory(root, "version-mismatch");
  {
    PluginRuntime runtime;
    auto provider = make_descriptor("provider");
    provider.provides = {
        robot_mcap::provide_service<ITestServiceA>("ITestServiceA", 1)};
    auto consumer = make_descriptor("consumer");
    consumer.requires = {
        robot_mcap::require_service<ITestServiceA>("ITestServiceA", 2)};
    add_plugin(runtime, provider);
    add_plugin(runtime, consumer);
    const Status status = runtime.start({"provider", "consumer"});
    CHECK(!status.ok());
    CHECK(status.detail.find("version mismatch") != std::string::npos);
  }
  {
    PluginRuntime runtime;
    auto consumer = make_descriptor("consumer");
    consumer.requires = {
        robot_mcap::require_service<IHostService>("IHostService", 2)};
    add_plugin(runtime, consumer);
    CHECK_OK(runtime.bind_host_service<IHostService>(std::make_shared<HostService>(1),
                                                     "IHostService", 1));
    const Status status = runtime.start({"consumer"});
    CHECK(!status.ok());
    CHECK(status.detail.find("version mismatch") != std::string::npos);
  }
}

void test_cycle(const std::filesystem::path& root) {
  (void)test_directory(root, "cycle");
  PluginRuntime runtime;
  auto first = make_descriptor("cycle_a");
  first.provides = {robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
  first.requires = {robot_mcap::require_service<ITestServiceB>("ITestServiceB")};
  auto second = make_descriptor("cycle_b");
  second.provides = {robot_mcap::provide_service<ITestServiceB>("ITestServiceB")};
  second.requires = {robot_mcap::require_service<ITestServiceA>("ITestServiceA")};
  add_plugin(runtime, first);
  add_plugin(runtime, second);

  const Status status = runtime.start({"cycle_a", "cycle_b"});
  CHECK(!status.ok());
  CHECK(status.detail.find("dependency cycle") != std::string::npos);
}

void test_mid_apply_failure(const std::filesystem::path& root) {
  (void)test_directory(root, "mid-apply-failure");
  EventLog log;
  PluginRuntime runtime;

  auto stable = make_descriptor("stable");
  stable.provides = {robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
  add_plugin(
      runtime, stable,
      [&log](Context& context, Scope& scope) {
        log.add("stable.apply");
        scope.on_stop([&log] { log.add("stable.stop"); });
        scope.on_wait([&log] { log.add("stable.wait"); });
        scope.on_finalize([&log] { log.add("stable.finalize"); });
        scope.on_cleanup([&log] { log.add("stable.cleanup_ok"); });
        scope.on_cleanup([&log] {
          log.add("stable.cleanup_throw");
          throw std::runtime_error("cleanup boom");
        });
        scope.own(context.provide<ITestServiceA>(std::make_shared<TestServiceA>()));
      });

  auto failing = make_descriptor("failing");
  failing.requires = {robot_mcap::require_service<ITestServiceA>("ITestServiceA")};
  add_plugin(runtime, failing, [&log](Context& context, Scope& scope) {
    log.add("failing.apply");
    const auto service = context.require<ITestServiceA>();
    CHECK(service != nullptr);
    scope.on_stop([&log] { log.add("failing.stop"); });
    scope.on_wait([&log] { log.add("failing.wait"); });
    scope.on_finalize([&log] { log.add("failing.finalize"); });
    scope.on_cleanup([&log] { log.add("failing.cleanup"); });
    throw std::runtime_error("apply boom");
  });

  const Status status = runtime.start({"stable", "failing"});
  CHECK(!status.ok());
  CHECK(status.detail.find("plugin failing apply failed") != std::string::npos);
  CHECK(status.detail.find("apply boom") != std::string::npos);
  check_order(log, {"stable.apply", "failing.apply", "failing.stop", "stable.stop",
                    "failing.wait", "stable.wait", "failing.finalize",
                    "stable.finalize", "failing.cleanup", "stable.cleanup_throw",
                    "stable.cleanup_ok"});
  const RuntimeStopResult stopped = runtime.stop();
  CHECK(!stopped.error.ok());
  CHECK(!stopped.cleanup_errors.empty());
  bool found_cleanup_error = false;
  for (const auto& cleanup : stopped.cleanup_errors) {
    if (cleanup.operation == "plugin_cleanup" &&
        cleanup.detail.find("cleanup boom") != std::string::npos) {
      found_cleanup_error = true;
    }
  }
  CHECK(found_cleanup_error);
}

void test_activate_failure(const std::filesystem::path& root) {
  (void)test_directory(root, "activate-failure");
  EventLog log;
  PluginRuntime runtime;
  add_plugin(runtime, make_descriptor("first"),
             [&log](Context&, Scope& scope) {
               log.add("first.apply");
               scope.on_stop([&log] { log.add("first.stop"); });
               scope.on_finalize([&log] { log.add("first.finalize"); });
             });
  add_plugin(runtime, make_descriptor("second"),
             [&log](Context&, Scope& scope) {
               log.add("second.apply");
               scope.on_stop([&log] { log.add("second.stop"); });
               scope.on_finalize([&log] { log.add("second.finalize"); });
             },
             [] {
               return Status{"activate", {}, "activate boom"};
             });

  const Status status = runtime.start({"first", "second"});
  CHECK(!status.ok());
  CHECK(status.detail.find("plugin second activate failed") != std::string::npos);
  CHECK(status.detail.find("activate boom") != std::string::npos);
  check_order(log, {"first.apply", "second.apply", "second.stop", "first.stop",
                    "second.finalize", "first.finalize"});
}

class ThreadedPlugin final : public IPlugin {
public:
  explicit ThreadedPlugin(EventLog& log) : log_(log) {}

  PluginDescriptor describe() const override { return make_descriptor("threaded"); }

  void apply(Context&, Scope& scope) override {
    log_.add("threaded.apply");
    scope.on_stop([this] {
      log_.add("threaded.stop");
      stop_.store(true);
    });
    scope.on_wait([this] {
      log_.add("threaded.wait");
      if (thread_.joinable()) {
        thread_.join();
      }
      log_.add("threaded.joined");
    });
    scope.on_finalize([this] { log_.add("threaded.finalize"); });
    scope.on_cleanup([this] { log_.add("threaded.cleanup"); });
    thread_ = std::thread([this] {
      while (!stop_.load()) {
        callbacks_.fetch_add(1);
        {
          std::lock_guard lock(mutex_);
          first_callback_ = true;
        }
        condition_.notify_all();
        std::this_thread::yield();
      }
    });
  }

  Status activate() override {
    log_.add("threaded.activate");
    return {};
  }

  void wait_for_first_callback() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return first_callback_; });
  }

  int callbacks() const { return callbacks_.load(); }

private:
  EventLog& log_;
  std::atomic<bool> stop_{false};
  std::atomic<int> callbacks_{0};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool first_callback_ = false;
};

void test_threaded_cleanup_barrier(const std::filesystem::path& root) {
  (void)test_directory(root, "threaded-cleanup");
  EventLog log;
  PluginRuntime runtime;
  auto plugin = std::make_shared<ThreadedPlugin>(log);
  CHECK_OK(runtime.add_plugin_factory(
      "threaded", [plugin]() -> std::shared_ptr<IPlugin> { return plugin; }));

  CHECK_OK(runtime.start({"threaded"}));
  plugin->wait_for_first_callback();
  CHECK(plugin->callbacks() > 0);
  const RuntimeStopResult stopped = runtime.stop();
  CHECK(stopped.ok());
  const int callbacks_after_stop = plugin->callbacks();
  CHECK(callbacks_after_stop > 0);
  CHECK(plugin->callbacks() == callbacks_after_stop);
  check_order(log, {"threaded.apply", "threaded.activate", "threaded.stop",
                    "threaded.wait", "threaded.joined", "threaded.finalize",
                    "threaded.cleanup"});
}

void test_cross_instance_isolation(const std::filesystem::path& root) {
  (void)test_directory(root, "cross-instance");
  EventLog first_log;
  EventLog second_log;
  PluginRuntime first;
  PluginRuntime second;

  auto make_consumer = [](EventLog& log) {
    auto descriptor = make_descriptor("host_consumer");
    descriptor.requires = {
        robot_mcap::require_service<IHostService>("IHostService")};
    return std::make_pair(
        descriptor, [&log](Context& context, Scope&) {
          log.add("host=" + std::to_string(context.require<IHostService>()->value()));
        });
  };

  auto first_consumer = make_consumer(first_log);
  add_plugin(first, first_consumer.first, first_consumer.second);
  auto second_consumer = make_consumer(second_log);
  add_plugin(second, second_consumer.first, second_consumer.second);

  CHECK_OK(first.bind_host_service<IHostService>(std::make_shared<HostService>(1),
                                                 "IHostService"));
  CHECK_OK(second.bind_host_service<IHostService>(std::make_shared<HostService>(2),
                                                  "IHostService"));
  CHECK_OK(first.start({"host_consumer"}));
  CHECK_OK(second.start({"host_consumer"}));
  CHECK(first_log.events() == std::vector<std::string>{"host=1"});
  CHECK(second_log.events() == std::vector<std::string>{"host=2"});

  // A separate failing runtime must not disturb the running instance.
  PluginRuntime failing;
  auto missing = make_descriptor("missing");
  missing.requires = {robot_mcap::require_service<ITestServiceA>("ITestServiceA")};
  add_plugin(failing, missing);
  CHECK(!failing.start({"missing"}).ok());
  CHECK(second.state() == RuntimeState::Running);
  CHECK(second_log.events() == std::vector<std::string>{"host=2"});

  CHECK(first.stop().ok());
  CHECK(first.state() == RuntimeState::Stopped);
  CHECK(second.state() == RuntimeState::Running);
  CHECK(second_log.events() == std::vector<std::string>{"host=2"});
  CHECK(second.stop().ok());
}

void test_optional_dependency(const std::filesystem::path& root) {
  (void)test_directory(root, "optional-dependency");
  {
    EventLog log;
    PluginRuntime runtime;
    auto consumer = make_descriptor("optional");
    consumer.requires = {robot_mcap::require_service<ITestServiceA>(
        "ITestServiceA", 1, true)};
    add_plugin(runtime, consumer, [&log](Context& context, Scope&) {
      const auto service = context.find<ITestServiceA>();
      CHECK(service == nullptr);
      log.add("optional.missing");
    });
    CHECK_OK(runtime.start({"optional"}));
    CHECK(runtime.stop().ok());
    CHECK(log.events() == std::vector<std::string>{"optional.missing"});
  }
  {
    EventLog log;
    PluginRuntime runtime;
    auto provider = make_descriptor("provider");
    provider.provides = {robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
    add_plugin(runtime, provider, [](Context& context, Scope& scope) {
      scope.own(context.provide<ITestServiceA>(std::make_shared<TestServiceA>()));
    });
    auto consumer = make_descriptor("optional");
    consumer.requires = {robot_mcap::require_service<ITestServiceA>(
        "ITestServiceA", 1, true)};
    add_plugin(runtime, consumer, [&log](Context& context, Scope&) {
      const auto service = context.find<ITestServiceA>();
      CHECK(service != nullptr);
      log.add("optional.present");
    });
    CHECK_OK(runtime.start({"optional", "provider"}));
    CHECK(runtime.stop().ok());
    CHECK(log.events() == std::vector<std::string>{"optional.present"});
  }
  {
    // An optional provider below min_version is unavailable, not a start failure.
    EventLog log;
    PluginRuntime runtime;
    auto provider = make_descriptor("provider");
    provider.provides = {
        robot_mcap::provide_service<ITestServiceA>("ITestServiceA", 1)};
    add_plugin(runtime, provider, [](Context& context, Scope& scope) {
      scope.own(context.provide<ITestServiceA>(std::make_shared<TestServiceA>()));
    });
    auto consumer = make_descriptor("optional");
    consumer.requires = {robot_mcap::require_service<ITestServiceA>(
        "ITestServiceA", 2, true)};
    add_plugin(runtime, consumer, [&log](Context& context, Scope&) {
      const auto service = context.find<ITestServiceA>();
      CHECK(service == nullptr);
      log.add("optional.old_missing");
    });
    CHECK_OK(runtime.start({"optional", "provider"}));
    CHECK(runtime.stop().ok());
    CHECK(log.events() == std::vector<std::string>{"optional.old_missing"});
  }
}

void test_descriptor_and_context_errors(const std::filesystem::path& root) {
  (void)test_directory(root, "descriptor-errors");
  {
    PluginRuntime runtime;
    auto descriptor = make_descriptor("duplicate-provides");
    descriptor.provides = {
        robot_mcap::provide_service<ITestServiceA>("ITestServiceA"),
        robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
    add_plugin(runtime, descriptor);
    const Status status = runtime.start({"duplicate-provides"});
    CHECK(!status.ok());
    CHECK(status.detail.find("duplicate provision") != std::string::npos);
  }
  {
    EventLog log;
    PluginRuntime runtime;
    auto descriptor = make_descriptor("missing-provides");
    descriptor.provides = {
        robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
    add_plugin(runtime, descriptor, [&log](Context&, Scope&) {
      log.add("provider.apply");
    });
    auto consumer = make_descriptor("consumer");
    consumer.requires = {
        robot_mcap::require_service<ITestServiceA>("ITestServiceA")};
    add_plugin(runtime, consumer, [&log](Context&, Scope&) {
      log.add("consumer.apply");
    });
    const Status status = runtime.start({"missing-provides", "consumer"});
    CHECK(!status.ok());
    CHECK(status.detail.find("did not provide declared service") != std::string::npos);
    CHECK(log.events() == std::vector<std::string>{"provider.apply"});
  }
  {
    PluginRuntime runtime;
    add_plugin(runtime, make_descriptor("undeclared-require"), [](Context& context,
                                                                 Scope&) {
      (void)context.require<ITestServiceA>();
    });
    const Status status = runtime.start({"undeclared-require"});
    CHECK(!status.ok());
    CHECK(status.detail.find("not declared as a requirement") != std::string::npos);
  }
  {
    PluginRuntime runtime;
    add_plugin(runtime, make_descriptor("undeclared-provide"), [](Context& context,
                                                                 Scope&) {
      (void)context.provide<ITestServiceA>(std::make_shared<TestServiceA>());
    });
    const Status status = runtime.start({"undeclared-provide"});
    CHECK(!status.ok());
    CHECK(status.detail.find("not declared as a provision") != std::string::npos);
  }
}

void test_runtime_lifecycle_errors(const std::filesystem::path& root) {
  (void)test_directory(root, "runtime-lifecycle");
  {
    PluginRuntime runtime;
    const RuntimeStopResult stopped = runtime.stop();
    CHECK(!stopped.error.ok());
    CHECK(runtime.state() == RuntimeState::Created);
    add_plugin(runtime, make_descriptor("simple"));
    CHECK_OK(runtime.start({"simple"}));
    CHECK(runtime.state() == RuntimeState::Running);
    CHECK(runtime.stop().ok());
    CHECK(runtime.state() == RuntimeState::Stopped);
    CHECK(runtime.stop().ok());
    CHECK(!runtime.start({"simple"}).ok());
  }
  {
    PluginRuntime runtime;
    CHECK(!runtime.start({"missing"}).ok());
    CHECK(runtime.state() == RuntimeState::Stopped);
  }
  {
    PluginRuntime runtime;
    add_plugin(runtime, make_descriptor("duplicate"));
    const Status status = runtime.start({"duplicate", "duplicate"});
    CHECK(!status.ok());
    CHECK(status.detail.find("duplicate plugin selection") != std::string::npos);
    CHECK(runtime.state() == RuntimeState::Stopped);
    CHECK(!runtime.start({"duplicate"}).ok());
  }
  {
    PluginRuntime runtime;
    auto factory = []() -> std::shared_ptr<IPlugin> {
      return std::make_shared<TestPlugin>(make_descriptor("duplicate-factory"), nullptr,
                                          nullptr);
    };
    CHECK_OK(runtime.add_plugin_factory("duplicate-factory", factory));
    const Status status = runtime.add_plugin_factory("duplicate-factory", factory);
    CHECK(!status.ok());
    CHECK(status.detail.find("duplicate plugin factory") != std::string::npos);
  }
}

void test_flow_prechecks_and_postcheck(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "flow-prechecks");
  EventLog log;
  auto mismatched = std::make_shared<FakeSink>(log, test_directory(root, "flow-mismatch"));
  RecordingFlow mismatched_flow(StartContext{directory, 0});
  const Status attached_mismatch = mismatched_flow.attach_sink(mismatched);
  CHECK(!attached_mismatch.ok());
  CHECK(attached_mismatch.detail.find("does not match") != std::string::npos);

  auto sink = std::make_shared<FakeSink>(log, directory);
  RecordingFlow flow(StartContext{directory, 0});
  CHECK_OK(flow.attach_sink(sink));
  CHECK(!flow.recording());

  CHECK_OK(flow.start());
  CHECK(flow.recording());
  CHECK(sink->start_calls == 1);
  CHECK(flow.result().start == robot_mcap::FlowStartState::Started);
  CHECK(flow.result().prechecks.size() == 2);

  const RecordingFlowResult result = flow.stop();
  CHECK(result.start == robot_mcap::FlowStartState::Started);
  CHECK(result.stop.has_value());
  CHECK(result.stop->ok());
  CHECK(result.post_check == robot_mcap::PostCheckState::NotRequested);
  CHECK(!flow.recording());
  CHECK(sink->stop_calls == 1);
  const RecordingFlowResult repeated = flow.stop();
  CHECK(repeated.stop.has_value());
  CHECK(repeated.stop->ok());
  CHECK(sink->stop_calls == 1);
  check_order(log, {"sink.start", "sink.stop"});
}

void test_flow_precheck_failures(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "flow-precheck-failures");
  {
    const auto blocked = directory / "not-a-directory";
    {
      std::ofstream file(blocked);
      file << "x";
    }
    EventLog log;
    auto sink = std::make_shared<FakeSink>(log, blocked);
    RecordingFlow flow(StartContext{blocked, 0});
    CHECK_OK(flow.attach_sink(sink));
    const Status status = flow.start();
    CHECK(!status.ok());
    CHECK(status.operation == "precheck");
    CHECK(status.detail.find("output_writable") != std::string::npos);
    CHECK(sink->start_calls == 0);
    CHECK(!flow.recording());
    const RecordingFlowResult result = flow.result();
    CHECK(result.start == robot_mcap::FlowStartState::Failed);
    CHECK(!result.start_error.ok());
    CHECK(result.start_error.detail.find("output_writable") != std::string::npos);
    CHECK(!result.stop.has_value());
    const RecordingFlowResult stopped = flow.stop();
    CHECK(!stopped.stop.has_value());
    CHECK(sink->stop_calls == 0);
  }
  {
    EventLog log;
    auto sink = std::make_shared<FakeSink>(log, directory);
    RecordingFlow flow(
        StartContext{directory, std::numeric_limits<std::uint64_t>::max()});
    CHECK_OK(flow.attach_sink(sink));
    const Status status = flow.start();
    CHECK(!status.ok());
    CHECK(status.operation == "precheck");
    CHECK(status.detail.find("free_space") != std::string::npos);
    CHECK(sink->start_calls == 0);
    CHECK(flow.result().start == robot_mcap::FlowStartState::Failed);
    CHECK(!flow.result().stop.has_value());
    CHECK(sink->stop_calls == 0);
  }
}

void test_flow_sink_start_failure(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "flow-sink-start-failure");
  EventLog log;
  auto sink = std::make_shared<FakeSink>(log, directory);
  sink->fail_start = true;
  RecordingFlow flow(StartContext{directory, 0});
  CHECK_OK(flow.attach_sink(sink));
  const Status status = flow.start();
  CHECK(!status.ok());
  CHECK(status.operation == "sink_start");
  CHECK(flow.result().start == robot_mcap::FlowStartState::Failed);
  CHECK(!flow.result().start_error.ok());
  CHECK(!flow.result().stop.has_value());
  CHECK(sink->start_calls == 1);
  CHECK(sink->stop_calls == 0);
  const RecordingFlowResult stopped = flow.stop();
  CHECK(!stopped.stop.has_value());
  CHECK(sink->stop_calls == 0);
}

class FailingActivatePlugin final : public IPlugin {
public:
  PluginDescriptor describe() const override {
    PluginDescriptor descriptor;
    descriptor.id = "failing_source";
    descriptor.requires = {
        robot_mcap::require_service<robot_mcap::IMessageSink>("IMessageSink"),
        robot_mcap::require_service<robot_mcap::IRecordingStatus>("IRecordingStatus"),
    };
    return descriptor;
  }
  void apply(Context& context, Scope&) override {
    (void)context.require<robot_mcap::IMessageSink>();
    (void)context.require<robot_mcap::IRecordingStatus>();
  }
  Status activate() override {
    return {"activate", {}, "source activate boom"};
  }
};

void test_flow_activation_failure_after_start(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "flow-activation-failure");
  EventLog log;
  auto sink = std::make_shared<FakeSink>(log, directory);
  auto flow = std::make_shared<RecordingFlow>(StartContext{directory, 0});
  PluginRuntime runtime;
  CHECK_OK(runtime.bind_host_service<IRecordingSink>(sink, "IRecordingSink"));
  CHECK_OK(runtime.add_plugin_factory("recording_flow",
                                      robot_mcap::recording_flow_factory(flow)));
  CHECK_OK(runtime.add_plugin_factory(
      "failing_source", []() -> std::shared_ptr<IPlugin> {
        return std::make_shared<FailingActivatePlugin>();
      }));
  const Status status = runtime.start({"failing_source", "recording_flow"});
  CHECK(!status.ok());
  CHECK(status.detail.find("failing_source activate failed") != std::string::npos);
  CHECK(status.detail.find("source activate boom") != std::string::npos);
  CHECK(sink->start_calls == 1);
  // The flow started before the later plugin failed, so rollback stops the core and
  // leaves a cleanly finalized (empty) session plus the runtime start error.
  CHECK(sink->stop_calls == 1);
  CHECK(flow->result().start == robot_mcap::FlowStartState::Started);
  CHECK(flow->result().stop.has_value());
  CHECK(flow->result().stop->ok());
  CHECK(!runtime.stop().ok());
}

class FailingPrecheck final : public robot_mcap::IPrecheck {
public:
  explicit FailingPrecheck(EventLog& log) : log_(log) {}
  robot_mcap::PrecheckResult run(const StartContext&) override {
    log_.add("extra.run");
    return {"", false, "blocked by test"};
  }

private:
  EventLog& log_;
};

void test_flow_plugin_and_precheck_registration(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "flow-plugin");
  {
    EventLog log;
    auto flow = std::make_shared<RecordingFlow>(StartContext{directory, 0});
    auto sink = std::make_shared<FakeSink>(log, directory);
    PluginRuntime runtime;
    CHECK_OK(runtime.bind_host_service<IRecordingSink>(sink, "IRecordingSink"));
    CHECK_OK(runtime.add_plugin_factory("recording_flow",
                                        robot_mcap::recording_flow_factory(flow)));
    auto extra = make_descriptor("extra_precheck");
    extra.requires = {robot_mcap::require_service<robot_mcap::IPrecheckRegistry>(
        "IPrecheckRegistry")};
    add_plugin(runtime, extra, [&log](Context& context, Scope& scope) {
      auto registry = context.require<robot_mcap::IPrecheckRegistry>();
      scope.own(registry->add("extra", std::make_shared<FailingPrecheck>(log)));
      log.add("extra.apply");
    });

    const Status status = runtime.start({"recording_flow", "extra_precheck"});
    CHECK(!status.ok());
    CHECK(status.detail.find("precheck") != std::string::npos);
    CHECK(status.detail.find("blocked by test") != std::string::npos);
    CHECK(sink->start_calls == 0);
    CHECK(sink->stop_calls == 0);
    CHECK(flow->result().start == robot_mcap::FlowStartState::Failed);
    CHECK(!flow->result().stop.has_value());
    check_order(log, {"extra.apply", "extra.run"});
    CHECK(!runtime.stop().ok());
  }
  {
    EventLog log;
    auto flow = std::make_shared<RecordingFlow>(StartContext{directory, 0});
    auto sink = std::make_shared<FakeSink>(log, directory);
    PluginRuntime runtime;
    CHECK_OK(runtime.bind_host_service<IRecordingSink>(sink, "IRecordingSink"));
    CHECK_OK(runtime.add_plugin_factory("recording_flow",
                                        robot_mcap::recording_flow_factory(flow)));
    CHECK_OK(runtime.start({"recording_flow"}));
    CHECK(flow->recording());
    CHECK(sink->start_calls == 1);
    const RuntimeStopResult stopped = runtime.stop();
    CHECK(stopped.ok());
    CHECK(sink->stop_calls == 1);
    CHECK(!flow->recording());
    CHECK(flow->result().stop.has_value());
    CHECK(flow->result().stop->ok());
    CHECK(flow->result().post_check == robot_mcap::PostCheckState::NotRequested);
    check_order(log, {"sink.start", "sink.stop"});
  }
}

class SourcePlugin final : public IPlugin {
public:
  explicit SourcePlugin(EventLog& log) : log_(log) {}

  PluginDescriptor describe() const override {
    PluginDescriptor descriptor;
    descriptor.id = "source";
    descriptor.requires = {
        robot_mcap::require_service<robot_mcap::IMessageSink>("IMessageSink"),
        robot_mcap::require_service<robot_mcap::IRecordingStatus>("IRecordingStatus"),
    };
    return descriptor;
  }

  void apply(Context& context, Scope& scope) override {
    sink_ = context.require<robot_mcap::IMessageSink>();
    status_ = context.require<robot_mcap::IRecordingStatus>();
    log_.add("source.apply");
    scope.on_finalize([this] {
      OwnedMessage last;
      last.log_time_ns = 2;
      last.sequence = 2;
      last.data = {std::byte{0x02}};
      const SubmitResult submitted = sink_->submit_owned(channel_, std::move(last));
      if (!submitted.ok()) {
        throw std::runtime_error("final source submit failed");
      }
      log_.add("source.finalize");
    });
  }

  Status activate() override {
    if (!status_->recording()) {
      return {"source_activate", {}, "recording flow is not started"};
    }
    const ChannelSpec spec{"/flow/source", "raw", std::nullopt,
                           robot_mcap::PublishTimePolicy::LogTimeOnly};
    const Status registered = sink_->register_channel(spec, channel_);
    if (!registered.ok()) {
      return registered;
    }
    OwnedMessage first;
    first.log_time_ns = 1;
    first.sequence = 1;
    first.data = {std::byte{0x01}};
    const SubmitResult submitted = sink_->submit_owned(channel_, std::move(first));
    if (!submitted.ok()) {
      return submitted.status;
    }
    log_.add("source.activate");
    return {};
  }

private:
  EventLog& log_;
  std::shared_ptr<robot_mcap::IMessageSink> sink_;
  std::shared_ptr<robot_mcap::IRecordingStatus> status_;
  ChannelId channel_ = 0;
};

void test_source_before_flow_and_finalize_submit(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "source-flow");
  EventLog log;
  robot_mcap::RecorderOptions options;
  options.parent_directory = directory;
  options.session_id = "source-flow";
  auto recorder = std::make_shared<robot_mcap::Recorder>(options);
  auto sink = std::make_shared<robot_mcap::CoreRecordingSink>(recorder);
  auto flow = std::make_shared<RecordingFlow>(StartContext{directory, 0});

  PluginRuntime runtime;
  CHECK_OK(runtime.bind_host_service<IRecordingSink>(sink, "IRecordingSink"));
  CHECK_OK(runtime.add_plugin_factory("recording_flow",
                                      robot_mcap::recording_flow_factory(flow)));
  CHECK_OK(runtime.add_plugin_factory(
      "source", [&log]() -> std::shared_ptr<IPlugin> {
        return std::make_shared<SourcePlugin>(log);
      }));

  // The source is selected first, but depends on services provided by the flow, so
  // the flow must apply and activate first.
  CHECK_OK(runtime.start({"source", "recording_flow"}));
  CHECK(flow->recording());
  CHECK(flow->result().start == robot_mcap::FlowStartState::Started);
  check_order(log, {"source.apply", "source.activate"});

  const RuntimeStopResult stopped = runtime.stop();
  CHECK(stopped.ok());
  check_order(log, {"source.activate", "source.finalize"});
  CHECK(flow->result().stop.has_value());
  CHECK(flow->result().stop->ok());
  CHECK(flow->result().stop->file_state == robot_mcap::FileState::Finalized);

  mcap::McapReader reader;
  CHECK(reader.open((directory / "source-flow" / "data_0000.mcap").string()).ok());
  std::size_t messages = 0;
  auto view = reader.readMessages();
  for (auto iterator = view.begin(); iterator != view.end(); ++iterator) {
    CHECK(iterator->channel->topic == "/flow/source");
    ++messages;
  }
  CHECK(view.status().ok());
  CHECK(messages == 2);
  reader.close();
}

class ReentrantPrecheck final : public robot_mcap::IPrecheck {
public:
  explicit ReentrantPrecheck(std::weak_ptr<robot_mcap::IRecordingStatus> status)
      : status_(std::move(status)) {}

  robot_mcap::PrecheckResult run(const StartContext&) override {
    if (auto status = status_.lock()) {
      (void)status->result();
      (void)status->recording();
    }
    return {"", true, {}};
  }

private:
  std::weak_ptr<robot_mcap::IRecordingStatus> status_;
};

void test_precheck_reentrancy(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "precheck-reentrant");
  EventLog log;
  auto sink = std::make_shared<FakeSink>(log, directory);
  auto flow = std::make_shared<RecordingFlow>(StartContext{directory, 0});
  CHECK_OK(flow->attach_sink(sink));
  auto handle = flow->precheck_registry()->add(
      "reentrant", std::make_shared<ReentrantPrecheck>(flow));
  CHECK_OK(flow->start());
  CHECK(sink->start_calls == 1);
  CHECK(flow->result().start == robot_mcap::FlowStartState::Started);
  const RecordingFlowResult result = flow->stop();
  CHECK(result.stop.has_value());
  CHECK(result.stop->ok());
}

class ThrowingPrecheck final : public robot_mcap::IPrecheck {
public:
  robot_mcap::PrecheckResult run(const StartContext&) override {
    throw std::runtime_error("precheck boom");
  }
};

class RecordingPrecheck final : public robot_mcap::IPrecheck {
public:
  explicit RecordingPrecheck(EventLog& log) : log_(log) {}
  robot_mcap::PrecheckResult run(const StartContext&) override {
    log_.add("precheck.ok");
    return {"", true, {}};
  }

private:
  EventLog& log_;
};

void test_precheck_registry_edges(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "precheck-registry");
  EventLog log;
  auto sink = std::make_shared<FakeSink>(log, directory);
  RecordingFlow flow(StartContext{directory, 0});
  CHECK_OK(flow.attach_sink(sink));
  auto registry = flow.precheck_registry();

  bool duplicate_name = false;
  try {
    (void)registry->add("output_writable", std::make_shared<RecordingPrecheck>(log));
  } catch (const std::exception&) {
    duplicate_name = true;
  }
  CHECK(duplicate_name);

  auto throwing = registry->add("throwing", std::make_shared<ThrowingPrecheck>());
  auto okay = registry->add("ok", std::make_shared<RecordingPrecheck>(log));
  const Status status = flow.start();
  CHECK(!status.ok());
  CHECK(status.operation == "precheck");
  CHECK(status.detail.find("throwing") != std::string::npos);
  CHECK(status.detail.find("precheck boom") != std::string::npos);
  CHECK(sink->start_calls == 0);

  const RecordingFlowResult result = flow.result();
  CHECK(result.start == robot_mcap::FlowStartState::Failed);
  CHECK(!result.stop.has_value());
  CHECK(result.prechecks.size() >= 4);
  bool saw_throwing = false;
  bool saw_ok = false;
  for (const auto& precheck : result.prechecks) {
    if (precheck.name == "throwing" && !precheck.ok) {
      saw_throwing = true;
    }
    if (precheck.name == "ok" && precheck.ok) {
      saw_ok = true;
    }
  }
  CHECK(saw_throwing);
  CHECK(saw_ok);
  bool ok_ran = false;
  for (const auto& event : log.events()) {
    if (event == "precheck.ok") {
      ok_ran = true;
    }
  }
  CHECK(ok_ran);

  bool closed = false;
  try {
    (void)registry->add("late", std::make_shared<RecordingPrecheck>(log));
  } catch (const std::exception&) {
    closed = true;
  }
  CHECK(closed);
}

class ThrowingDescribePlugin final : public IPlugin {
public:
  PluginDescriptor describe() const override {
    throw std::runtime_error("describe boom");
  }
  void apply(Context&, Scope&) override {}
};

void test_host_and_creation_errors(const std::filesystem::path& root) {
  (void)test_directory(root, "host-creation-errors");
  {
    PluginRuntime runtime;
    auto service = std::make_shared<HostService>(1);
    CHECK_OK(runtime.bind_host_service<IHostService>(service, "IHostService"));
    const Status duplicate =
        runtime.bind_host_service<IHostService>(service, "IHostService");
    CHECK(!duplicate.ok());
    CHECK(duplicate.detail.find("duplicate provider") != std::string::npos);
    const Status null_service =
        runtime.bind_host_service<IHostService>(std::shared_ptr<IHostService>(), "null");
    CHECK(!null_service.ok());
    const Status empty_name = runtime.bind_host_service<IHostService>(
        std::make_shared<HostService>(1), "");
    CHECK(!empty_name.ok());
    const Status bad_version = runtime.bind_host_service<IHostService>(
        std::make_shared<HostService>(1), "bad", 0);
    CHECK(!bad_version.ok());
  }
  {
    PluginRuntime runtime;
    add_plugin(runtime, make_descriptor("simple"));
    CHECK_OK(runtime.start({"simple"}));
    const Status late = runtime.bind_host_service<IHostService>(
        std::make_shared<HostService>(1), "IHostService");
    CHECK(!late.ok());
    CHECK(late.detail.find("before start") != std::string::npos);
    CHECK(runtime.stop().ok());
  }
  {
    PluginRuntime runtime;
    CHECK_OK(runtime.add_plugin_factory(
        "null", []() -> std::shared_ptr<IPlugin> { return nullptr; }));
    const Status status = runtime.start({"null"});
    CHECK(!status.ok());
    CHECK(status.detail.find("returned null") != std::string::npos);
  }
  {
    PluginRuntime runtime;
    CHECK_OK(runtime.add_plugin_factory(
        "throw_factory", []() -> std::shared_ptr<IPlugin> {
          throw std::runtime_error("factory boom");
        }));
    const Status status = runtime.start({"throw_factory"});
    CHECK(!status.ok());
    CHECK(status.detail.find("factory boom") != std::string::npos);
  }
  {
    PluginRuntime runtime;
    CHECK_OK(runtime.add_plugin_factory(
        "throw_describe", []() -> std::shared_ptr<IPlugin> {
          return std::make_shared<ThrowingDescribePlugin>();
        }));
    const Status status = runtime.start({"throw_describe"});
    CHECK(!status.ok());
    CHECK(status.detail.find("describe boom") != std::string::npos);
  }
}

class RollbackThreadPlugin final : public IPlugin {
public:
  explicit RollbackThreadPlugin(EventLog& log) : log_(log) {}

  PluginDescriptor describe() const override {
    PluginDescriptor descriptor;
    descriptor.id = "rollback_thread";
    descriptor.requires = {robot_mcap::require_service<ITestServiceA>("ITestServiceA")};
    descriptor.provides = {robot_mcap::provide_service<ITestServiceB>("ITestServiceB")};
    return descriptor;
  }

  void apply(Context& context, Scope& scope) override {
    CHECK(context.require<ITestServiceA>() != nullptr);
    scope.own(context.provide<ITestServiceB>(std::make_shared<TestServiceB>()));
    log_.add("rollback.apply");
    scope.on_stop([this] {
      log_.add("rollback.stop");
      stop_.store(true);
    });
    scope.on_wait([this] {
      log_.add("rollback.wait");
      if (thread_.joinable()) {
        thread_.join();
      }
      log_.add("rollback.joined");
    });
    scope.on_finalize([this] { log_.add("rollback.finalize"); });
    scope.on_cleanup([this] { log_.add("rollback.cleanup"); });
    thread_ = std::thread([this] {
      while (!stop_.load()) {
        callbacks_.fetch_add(1);
        std::this_thread::yield();
      }
    });
    throw std::runtime_error("rollback boom");
  }

private:
  EventLog& log_;
  std::atomic<bool> stop_{false};
  std::atomic<int> callbacks_{0};
  std::thread thread_;
};

void test_rollback_thread_and_dependent(const std::filesystem::path& root) {
  (void)test_directory(root, "rollback-thread");
  EventLog log;
  PluginRuntime runtime;

  auto stable = make_descriptor("stable");
  stable.provides = {robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
  add_plugin(runtime, stable, [&log](Context& context, Scope& scope) {
    log.add("stable.apply");
    scope.on_stop([&log] { log.add("stable.stop"); });
    scope.on_wait([&log] { log.add("stable.wait"); });
    scope.on_finalize([&log] { log.add("stable.finalize"); });
    scope.on_cleanup([&log] { log.add("stable.cleanup"); });
    scope.own(context.provide<ITestServiceA>(std::make_shared<TestServiceA>()));
  });
  CHECK_OK(runtime.add_plugin_factory(
      "rollback_thread", [&log]() -> std::shared_ptr<IPlugin> {
        return std::make_shared<RollbackThreadPlugin>(log);
      }));

  auto dependent = make_descriptor("dependent");
  dependent.requires = {robot_mcap::require_service<ITestServiceB>("ITestServiceB")};
  add_plugin(runtime, dependent, [&log](Context&, Scope&) { log.add("dependent.apply"); });

  const Status status = runtime.start({"stable", "rollback_thread", "dependent"});
  CHECK(!status.ok());
  CHECK(status.detail.find("rollback_thread") != std::string::npos);
  CHECK(status.detail.find("rollback boom") != std::string::npos);
  CHECK(log.events().size() > 0);
  bool dependent_applied = false;
  for (const auto& event : log.events()) {
    if (event == "dependent.apply") {
      dependent_applied = true;
    }
  }
  CHECK(!dependent_applied);
  check_order(log, {"stable.apply", "rollback.apply", "rollback.stop", "stable.stop",
                    "rollback.wait", "rollback.joined", "stable.wait",
                    "rollback.finalize", "stable.finalize", "rollback.cleanup",
                    "stable.cleanup"});
}

void test_throwing_stop_and_handle_release(const std::filesystem::path& root) {
  (void)test_directory(root, "throwing-stop");
  EventLog log;
  PluginRuntime runtime;
  auto descriptor = make_descriptor("throwing");
  descriptor.provides = {
      robot_mcap::provide_service<ITestServiceA>("ITestServiceA")};
  add_plugin(runtime, descriptor, [&log](Context& context, Scope& scope) {
    auto handle = std::make_shared<robot_mcap::RegistrationHandle>(
        context.provide<ITestServiceA>(std::make_shared<TestServiceA>()));
    scope.on_cleanup([handle, &log] {
      handle->reset();
      log.add("handle.reset");
    });
    scope.on_stop([&log] {
      log.add("throwing.stop");
      throw std::runtime_error("stop boom");
    });
    scope.on_wait([&log] { log.add("throwing.wait"); });
    scope.on_finalize([&log] { log.add("throwing.finalize"); });
    scope.on_cleanup([&log] { log.add("throwing.cleanup"); });
  });

  CHECK_OK(runtime.start({"throwing"}));
  const RuntimeStopResult result = runtime.stop();
  CHECK(!result.error.ok());
  CHECK(!result.cleanup_errors.empty());
  bool stop_error = false;
  for (const auto& error : result.cleanup_errors) {
    if (error.operation == "plugin_stop" &&
        error.detail.find("stop boom") != std::string::npos) {
      stop_error = true;
    }
  }
  CHECK(stop_error);
  check_order(log, {"throwing.stop", "throwing.wait", "throwing.finalize",
                    "throwing.cleanup", "handle.reset"});
}

void test_core_recording_without_plugins(const std::filesystem::path& root) {
  const auto directory = test_directory(root, "core-flow");
  robot_mcap::RecorderOptions options;
  options.parent_directory = directory;
  options.session_id = "flow-core";
  auto recorder = std::make_shared<robot_mcap::Recorder>(options);
  auto sink = std::make_shared<robot_mcap::CoreRecordingSink>(recorder);
  RecordingFlow flow(StartContext{directory, 0});
  CHECK_OK(flow.attach_sink(sink));
  CHECK_OK(flow.start());

  ChannelId channel = 0;
  CHECK_OK(sink->register_channel(
      {"/flow/raw", "raw", std::nullopt, robot_mcap::PublishTimePolicy::LogTimeOnly},
      channel));
  OwnedMessage message;
  message.log_time_ns = 42;
  message.sequence = 1;
  message.data = {std::byte{0x01}, std::byte{0x02}};
  CHECK(sink->submit_owned(channel, std::move(message)).ok());

  const RecordingFlowResult result = flow.stop();
  CHECK(result.stop.has_value());
  CHECK(result.stop->ok());
  CHECK(result.stop->file_state == robot_mcap::FileState::Finalized);
  CHECK(result.post_check == robot_mcap::PostCheckState::NotRequested);

  mcap::McapReader reader;
  const auto data_file = directory / "flow-core" / "data_0000.mcap";
  CHECK(reader.open(data_file.string()).ok());
  std::size_t messages = 0;
  auto view = reader.readMessages();
  for (auto iterator = view.begin(); iterator != view.end(); ++iterator) {
    CHECK(iterator->channel->topic == "/flow/raw");
    ++messages;
  }
  CHECK(view.status().ok());
  CHECK(messages == 1);
  reader.close();
}

void test_native_module(const std::filesystem::path& root,
                        const std::filesystem::path& module_path) {
  if (module_path.empty()) {
    return;
  }
  const auto directory = test_directory(root, "native-module");
  EventLog log;
  auto probe = std::make_shared<Probe>(log);
  PluginRuntime runtime;
  CHECK_OK(runtime.bind_host_service<robot_mcap_test::IProbe>(probe, "IProbe"));

  const auto bogus = directory / "not-a-module.txt";
  {
    std::ofstream file(bogus);
    file << "not a module";
  }
  const Status bogus_status = runtime.load_plugin_module(bogus);
  CHECK(!bogus_status.ok());
  CHECK(runtime.state() == RuntimeState::Created);

  CHECK_OK(runtime.load_plugin_module(module_path));
  CHECK_OK(runtime.start({"native.test"}));
  CHECK(runtime.state() == RuntimeState::Running);
  const RuntimeStopResult stopped = runtime.stop();
  CHECK(stopped.ok());
  check_order(log, {"native.apply", "native.activate", "native.stop", "native.wait",
                    "native.finalize", "native.cleanup", "module_service.destroy",
                    "native.destroy"});
}

class ModuleConsumerPlugin final : public IPlugin {
public:
  explicit ModuleConsumerPlugin(EventLog& log) : log_(log) {}

  PluginDescriptor describe() const override {
    PluginDescriptor descriptor;
    descriptor.id = "module_consumer";
    descriptor.requires = {robot_mcap::require_service<robot_mcap_test::IModuleService>(
        "IModuleService")};
    return descriptor;
  }

  void apply(Context& context, Scope& scope) override {
    service_ = context.require<robot_mcap_test::IModuleService>();
    log_.add("consumer.apply");
    scope.on_cleanup([this] { log_.add("consumer.cleanup"); });
  }

  ~ModuleConsumerPlugin() override {
    log_.add("consumer.destroy");
    service_.reset();
  }

private:
  EventLog& log_;
  std::shared_ptr<robot_mcap_test::IModuleService> service_;
};

void test_native_module_service_lifetime(const std::filesystem::path& root,
                                         const std::filesystem::path& module_path) {
  if (module_path.empty()) {
    return;
  }
  (void)test_directory(root, "native-service");
  EventLog log;
  auto probe = std::make_shared<Probe>(log);
  PluginRuntime runtime;
  CHECK_OK(runtime.bind_host_service<robot_mcap_test::IProbe>(probe, "IProbe"));
  CHECK_OK(runtime.load_plugin_module(module_path));
  CHECK_OK(runtime.add_plugin_factory(
      "module_consumer", [&log]() -> std::shared_ptr<IPlugin> {
        return std::make_shared<ModuleConsumerPlugin>(log);
      }));
  CHECK_OK(runtime.start({"native.test", "module_consumer"}));
  const RuntimeStopResult stopped = runtime.stop();
  CHECK(stopped.ok());
  check_order(log, {"native.apply", "consumer.apply", "native.activate", "native.stop",
                    "native.wait", "native.finalize", "consumer.cleanup",
                    "native.cleanup", "consumer.destroy", "module_service.destroy",
                    "native.destroy"});
}

void test_native_bad_module(const std::filesystem::path& root,
                            const std::filesystem::path& bad_module_path,
                            const std::filesystem::path& good_module_path) {
  if (bad_module_path.empty()) {
    return;
  }
  (void)test_directory(root, "native-bad-module");
  EventLog log;
  auto probe = std::make_shared<Probe>(log);
  PluginRuntime runtime;
  CHECK_OK(runtime.bind_host_service<robot_mcap_test::IProbe>(probe, "IProbe"));
  const Status rejected = runtime.load_plugin_module(bad_module_path);
  CHECK(!rejected.ok());
  CHECK(rejected.detail.find("ABI is invalid") != std::string::npos);
  CHECK(runtime.state() == RuntimeState::Created);

  // A rejected module must not poison a later good load.
  CHECK_OK(runtime.load_plugin_module(good_module_path));
  CHECK_OK(runtime.start({"native.test"}));
  CHECK(runtime.stop().ok());
}

void test_native_module_isolation(const std::filesystem::path& root,
                                  const std::filesystem::path& module_path) {
  if (module_path.empty()) {
    return;
  }
  (void)test_directory(root, "native-isolation");
  EventLog first_log;
  EventLog second_log;
  auto first_probe = std::make_shared<Probe>(first_log);
  auto second_probe = std::make_shared<Probe>(second_log);
  PluginRuntime first;
  PluginRuntime second;
  CHECK_OK(first.bind_host_service<robot_mcap_test::IProbe>(first_probe, "IProbe"));
  CHECK_OK(second.bind_host_service<robot_mcap_test::IProbe>(second_probe, "IProbe"));
  CHECK_OK(first.load_plugin_module(module_path));
  CHECK_OK(second.load_plugin_module(module_path));
  CHECK_OK(first.start({"native.test"}));
  CHECK_OK(second.start({"native.test"}));
  CHECK(first.state() == RuntimeState::Running);
  CHECK(second.state() == RuntimeState::Running);

  CHECK(first.stop().ok());
  CHECK(first.state() == RuntimeState::Stopped);
  CHECK(second.state() == RuntimeState::Running);
  CHECK(second.stop().ok());
  check_order(first_log, {"native.apply", "native.activate", "native.stop", "native.wait",
                          "native.finalize", "native.cleanup",
                          "module_service.destroy", "native.destroy"});
  check_order(second_log, {"native.apply", "native.activate", "native.stop",
                           "native.wait", "native.finalize", "native.cleanup",
                           "module_service.destroy", "native.destroy"});
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || argc > 4) {
      std::cerr << "expected a test output directory and optional plugin modules\n";
      return 1;
    }
    const std::filesystem::path root = argv[1];
    const std::filesystem::path module_path =
        argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path();
    const std::filesystem::path bad_module_path =
        argc >= 4 ? std::filesystem::path(argv[3]) : std::filesystem::path();
    const auto test_root = root / "plugin_runtime";
    std::error_code error;
    std::filesystem::remove_all(test_root, error);
    std::filesystem::create_directories(test_root);

    test_graph_and_shutdown_order(test_root);
    test_missing_dependency(test_root);
    test_duplicate_provider(test_root);
    test_version_mismatch(test_root);
    test_cycle(test_root);
    test_mid_apply_failure(test_root);
    test_activate_failure(test_root);
    test_threaded_cleanup_barrier(test_root);
    test_cross_instance_isolation(test_root);
    test_optional_dependency(test_root);
    test_descriptor_and_context_errors(test_root);
    test_runtime_lifecycle_errors(test_root);
    test_flow_prechecks_and_postcheck(test_root);
    test_flow_precheck_failures(test_root);
    test_flow_plugin_and_precheck_registration(test_root);
    test_flow_sink_start_failure(test_root);
    test_flow_activation_failure_after_start(test_root);
    test_precheck_registry_edges(test_root);
    test_precheck_reentrancy(test_root);
    test_source_before_flow_and_finalize_submit(test_root);
    test_host_and_creation_errors(test_root);
    test_rollback_thread_and_dependent(test_root);
    test_throwing_stop_and_handle_release(test_root);
    test_core_recording_without_plugins(test_root);
    test_native_module(test_root, module_path);
    test_native_bad_module(test_root, bad_module_path, module_path);
    test_native_module_service_lifetime(test_root, module_path);
    test_native_module_isolation(test_root, module_path);
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
