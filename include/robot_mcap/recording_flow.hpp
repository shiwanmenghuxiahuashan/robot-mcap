#pragma once

#include "robot_mcap/plugin_runtime.hpp"
#include "robot_mcap/recorder.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace robot_mcap {

// Inputs available to prechecks before the core session starts.
struct StartContext {
  std::filesystem::path output_directory;
  std::uint64_t required_free_bytes = 0;
};

struct PrecheckResult {
  std::string name;
  bool ok = true;
  std::string detail;
};

class IPrecheck {
public:
  virtual ~IPrecheck() = default;
  // Runs after every plugin's apply() (so source discovery/subscription setup done
  // there is visible) and before any plugin activate() (production starts).
  virtual PrecheckResult run(const StartContext& context) = 0;
};

// Instance-scoped precheck collection. Plugins (for example a ROS adapter) add checks
// during apply(); the recording flow runs them during activate and closes the
// registry, so adds from activate or later fail instead of being silently skipped.
class IPrecheckRegistry {
public:
  virtual ~IPrecheckRegistry() = default;
  virtual RegistrationHandle add(std::string name, std::shared_ptr<IPrecheck> precheck) = 0;
  virtual std::vector<PrecheckResult> run_all(const StartContext& context) = 0;
};

// Message-only capability given to data sources. Sources can register channels and
// submit messages but cannot start or stop the core.
class IMessageSink {
public:
  virtual ~IMessageSink() = default;

  virtual Status register_channel(const ChannelSpec& spec, ChannelId& id) = 0;
  virtual SubmitResult submit_copy(ChannelId channel, MessageView message) = 0;
  virtual SubmitResult submit_owned(ChannelId channel, OwnedMessage&& message) = 0;
};

// Control capability reserved for the recording flow. The host creates one Recorder
// and binds a CoreRecordingSink; the flow plugin requires this interface and provides
// IMessageSink to sources after starting the core.
class IRecordingSink : public IMessageSink {
public:
  virtual Status start() = 0;
  virtual StopResult stop() = 0;
  // The parent directory the core will record under. RecordingFlow::attach_sink
  // checks that prechecks target the same directory.
  virtual std::filesystem::path output_directory() const = 0;
};

class CoreRecordingSink final : public IRecordingSink {
public:
  explicit CoreRecordingSink(std::shared_ptr<Recorder> recorder);

  Status start() override;
  Status register_channel(const ChannelSpec& spec, ChannelId& id) override;
  SubmitResult submit_copy(ChannelId channel, MessageView message) override;
  SubmitResult submit_owned(ChannelId channel, OwnedMessage&& message) override;
  StopResult stop() override;
  std::filesystem::path output_directory() const override;

private:
  std::shared_ptr<Recorder> recorder_;
};

enum class PostCheckState { NotRequested };

enum class FlowStartState { NotStarted, Started, Failed };

struct RecordingFlowResult {
  FlowStartState start = FlowStartState::NotStarted;
  // Set when start() failed, independently of the core's StopResult.
  Status start_error;
  // All prechecks that ran, including failures.
  std::vector<PrecheckResult> prechecks;
  // Present only when the core was started and stop() ran.
  std::optional<StopResult> stop;
  // Task 3 does not request a post-check; later tasks add pipeline states.
  PostCheckState post_check = PostCheckState::NotRequested;
};

class IRecordingStatus {
public:
  virtual ~IRecordingStatus() = default;
  virtual bool recording() const = 0;
  virtual RecordingFlowResult result() const = 0;
};

// Host-owned flow orchestration: prechecks, core start/stop, and post-check state.
// Source plugins require the IRecordingStatus service so they initialize after the
// flow and activate only after the core has started.
class RecordingFlow final : public IRecordingStatus {
public:
  explicit RecordingFlow(StartContext context);
  ~RecordingFlow() override;
  RecordingFlow(const RecordingFlow&) = delete;
  RecordingFlow& operator=(const RecordingFlow&) = delete;

  // Exactly one sink, before start. The sink's output_directory() must match
  // StartContext.output_directory (the Recorder's parent directory).
  Status attach_sink(std::shared_ptr<IRecordingSink> sink);
  std::shared_ptr<IPrecheckRegistry> precheck_registry() const;

  // Runs all prechecks, then starts the core. On failure the flow stays failed and
  // the core is not started.
  Status start();
  // In a PluginRuntime stop must go through PluginRuntime::stop(); the flow plugin
  // calls this from its finalize phase after sources have stopped and waited. The
  // host may call it directly only for core-only use without a runtime.
  RecordingFlowResult stop();

  bool recording() const override;
  RecordingFlowResult result() const override;
  bool started() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Built-in plugin that connects a host-owned RecordingFlow to the runtime. It requires
// IRecordingSink, provides IPrecheckRegistry and IRecordingStatus, starts the flow in
// activate(), and stops it during the finalize phase.
class RecordingFlowPlugin final : public IPlugin {
public:
  explicit RecordingFlowPlugin(std::shared_ptr<RecordingFlow> flow);

  PluginDescriptor describe() const override;
  void apply(Context& context, Scope& scope) override;
  Status activate() override;

private:
  std::shared_ptr<RecordingFlow> flow_;
};

PluginFactory recording_flow_factory(std::shared_ptr<RecordingFlow> flow);

}  // namespace robot_mcap
