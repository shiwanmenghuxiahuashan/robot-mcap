#include "robot_mcap/recording_flow.hpp"

#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace robot_mcap {
namespace {

class PrecheckRegistry final : public IPrecheckRegistry,
                               public std::enable_shared_from_this<PrecheckRegistry> {
public:
  RegistrationHandle add(std::string name,
                         std::shared_ptr<IPrecheck> precheck) override {
    if (name.empty() || !precheck) {
      throw std::invalid_argument("precheck name and object are required");
    }
    std::lock_guard lock(mutex_);
    if (closed_) {
      throw std::runtime_error("precheck registry is closed");
    }
    for (const auto& entry : entries_) {
      if (entry.name == name) {
        throw std::runtime_error("duplicate precheck name: " + name);
      }
    }
    const auto id = next_id_++;
    entries_.push_back({id, std::move(name), std::move(precheck)});
    auto self = shared_from_this();
    return RegistrationHandle([self, id] { self->remove(id); });
  }

  void clear() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    entries_.clear();
  }

  std::vector<PrecheckResult> run_all(const StartContext& context) override {
    std::vector<Entry> entries;
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      entries = entries_;
    }
    std::vector<PrecheckResult> results;
    results.reserve(entries.size());
    for (auto& entry : entries) {
      try {
        PrecheckResult result = entry.precheck->run(context);
        result.name = entry.name;
        results.push_back(std::move(result));
      } catch (const std::exception& exception) {
        results.push_back({entry.name, false, exception.what()});
      } catch (...) {
        results.push_back({entry.name, false, "unknown precheck error"});
      }
    }
    return results;
  }

private:
  struct Entry {
    std::uint64_t id = 0;
    std::string name;
    std::shared_ptr<IPrecheck> precheck;
  };

  void remove(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
      if (entry->id == id) {
        entries_.erase(entry);
        return;
      }
    }
  }

  std::mutex mutex_;
  std::vector<Entry> entries_;
  std::uint64_t next_id_ = 1;
  bool closed_ = false;
};

class OutputWritablePrecheck final : public IPrecheck {
public:
  PrecheckResult run(const StartContext& context) override {
    if (context.output_directory.empty()) {
      return {"", false, "output directory is empty"};
    }
    std::error_code error;
    std::filesystem::create_directories(context.output_directory, error);
    if (error) {
      return {"", false, "cannot create output directory: " + error.message()};
    }
    static std::atomic<std::uint64_t> probe_counter{0};
    const auto probe = context.output_directory /
                       (".robot_mcap_write_probe_" +
                        std::to_string(probe_counter.fetch_add(1) + 1));
    std::ofstream file(probe, std::ios::binary | std::ios::trunc);
    if (!file) {
      return {"", false, "output directory is not writable"};
    }
    file.put('x');
    file.flush();
    const bool wrote = static_cast<bool>(file);
    file.close();
    std::filesystem::remove(probe, error);
    if (!wrote) {
      return {"", false, "writing the output probe failed"};
    }
    return {"", true, {}};
  }
};

class FreeSpacePrecheck final : public IPrecheck {
public:
  PrecheckResult run(const StartContext& context) override {
    if (context.required_free_bytes == 0) {
      return {"", true, {}};
    }
    std::error_code error;
    const auto space = std::filesystem::space(context.output_directory, error);
    if (error) {
      return {"", false, "cannot determine free space: " + error.message()};
    }
    if (space.available < context.required_free_bytes) {
      return {"", false,
              "insufficient free space: need " +
                  std::to_string(context.required_free_bytes) + " bytes, have " +
                  std::to_string(space.available)};
    }
    return {"", true, {}};
  }
};

}  // namespace

struct RecordingFlow::Impl {
  StartContext context;
  std::shared_ptr<IRecordingSink> sink;
  std::shared_ptr<PrecheckRegistry> prechecks;
  std::vector<RegistrationHandle> base_precheck_handles;
  mutable std::mutex mutex;
  FlowStartState start_state = FlowStartState::NotStarted;
  bool starting = false;
  bool stopped = false;
  std::atomic<bool> recording{false};
  RecordingFlowResult result;
};

RecordingFlow::RecordingFlow(StartContext context) : impl_(std::make_unique<Impl>()) {
  impl_->context = std::move(context);
  impl_->prechecks = std::make_shared<PrecheckRegistry>();
  impl_->base_precheck_handles.push_back(impl_->prechecks->add(
      "output_writable", std::make_shared<OutputWritablePrecheck>()));
  impl_->base_precheck_handles.push_back(
      impl_->prechecks->add("free_space", std::make_shared<FreeSpacePrecheck>()));
}

RecordingFlow::~RecordingFlow() = default;

Status RecordingFlow::attach_sink(std::shared_ptr<IRecordingSink> sink) {
  if (!sink) {
    return {"flow_attach", {}, "recording sink is required"};
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->sink) {
    return {"flow_attach", {}, "recording sink is already attached"};
  }
  if (impl_->start_state != FlowStartState::NotStarted || impl_->starting ||
      impl_->stopped) {
    return {"flow_attach", {}, "flow has already started or stopped"};
  }
  try {
    const auto sink_directory =
        std::filesystem::absolute(sink->output_directory()).lexically_normal();
    const auto flow_directory =
        std::filesystem::absolute(impl_->context.output_directory).lexically_normal();
    if (sink_directory != flow_directory) {
      return {"flow_attach", {},
              "precheck output directory does not match the recording sink"};
    }
  } catch (const std::exception& exception) {
    return {"flow_attach", {},
            std::string("cannot resolve output directories: ") + exception.what()};
  }
  impl_->sink = std::move(sink);
  return {};
}

std::shared_ptr<IPrecheckRegistry> RecordingFlow::precheck_registry() const {
  return impl_->prechecks;
}

Status RecordingFlow::start() {
  std::shared_ptr<IRecordingSink> sink;
  std::shared_ptr<PrecheckRegistry> prechecks;
  StartContext context;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->start_state != FlowStartState::NotStarted || impl_->starting ||
        impl_->stopped) {
      return {"flow_start", {}, "flow has already started or stopped"};
    }
    if (!impl_->sink) {
      impl_->start_state = FlowStartState::Failed;
      impl_->result.start = FlowStartState::Failed;
      impl_->result.start_error = {"flow_start", {}, "recording sink is not attached"};
      return impl_->result.start_error;
    }
    impl_->starting = true;
    sink = impl_->sink;
    prechecks = impl_->prechecks;
    context = impl_->context;
  }

  // Prechecks and sink.start() run without the flow mutex: prechecks are plugin
  // extension points and may call IRecordingStatus::result()/started().
  std::vector<PrecheckResult> results;
  Status error;
  try {
    results = prechecks->run_all(context);
  } catch (const std::exception& exception) {
    error = {"precheck", {}, exception.what()};
  } catch (...) {
    error = {"precheck", {}, "unknown precheck error"};
  }
  if (error.ok()) {
    for (const auto& result : results) {
      if (!result.ok) {
        error = {"precheck", {}, result.name + ": " + result.detail};
        break;
      }
    }
  }
  if (error.ok()) {
    try {
      error = sink->start();
    } catch (const std::exception& exception) {
      error = {"flow_start", {}, exception.what()};
    } catch (...) {
      error = {"flow_start", {}, "unknown sink start error"};
    }
  }

  bool stopped_during_start = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->starting = false;
    impl_->result.prechecks = std::move(results);
    if (!error.ok()) {
      impl_->start_state = FlowStartState::Failed;
      impl_->result.start = FlowStartState::Failed;
      impl_->result.start_error = error;
    } else if (impl_->stopped) {
      stopped_during_start = true;
      impl_->start_state = FlowStartState::Failed;
      impl_->result.start = FlowStartState::Failed;
      impl_->result.start_error = {"flow_start", {},
                                   "flow was stopped while starting"};
      error = impl_->result.start_error;
    } else {
      impl_->start_state = FlowStartState::Started;
      impl_->result.start = FlowStartState::Started;
      impl_->recording.store(true);
    }
  }
  if (stopped_during_start) {
    StopResult stopped_core;
    try {
      stopped_core = sink->stop();
    } catch (const std::exception& exception) {
      stopped_core.error = {"flow_stop", {}, exception.what()};
    } catch (...) {
      stopped_core.error = {"flow_stop", {}, "unknown stop error"};
    }
    std::lock_guard lock(impl_->mutex);
    impl_->result.stop = stopped_core;
  }
  return error;
}

RecordingFlowResult RecordingFlow::stop() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->stopped) {
    return impl_->result;
  }
  impl_->stopped = true;
  impl_->recording.store(false);
  if (impl_->start_state == FlowStartState::Started) {
    try {
      impl_->result.stop = impl_->sink->stop();
    } catch (const std::exception& exception) {
      StopResult failed;
      failed.error = {"flow_stop", {}, exception.what()};
      impl_->result.stop = failed;
    } catch (...) {
      StopResult failed;
      failed.error = {"flow_stop", {}, "unknown stop error"};
      impl_->result.stop = failed;
    }
  }
  impl_->result.post_check = PostCheckState::NotRequested;
  // Release the sink and plugin-provided prechecks before the runtime unloads native
  // modules; the host may own the flow longer than the runtime.
  impl_->sink.reset();
  impl_->prechecks->clear();
  return impl_->result;
}

bool RecordingFlow::recording() const { return impl_->recording.load(); }

RecordingFlowResult RecordingFlow::result() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->result;
}

bool RecordingFlow::started() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->start_state == FlowStartState::Started;
}

CoreRecordingSink::CoreRecordingSink(std::shared_ptr<Recorder> recorder)
    : recorder_(std::move(recorder)) {
  if (!recorder_) {
    throw std::invalid_argument("recorder is required");
  }
}

Status CoreRecordingSink::start() { return recorder_->start(); }

Status CoreRecordingSink::register_channel(const ChannelSpec& spec, ChannelId& id) {
  return recorder_->register_channel(spec, id);
}

SubmitResult CoreRecordingSink::submit_copy(ChannelId channel, MessageView message) {
  return recorder_->submit_copy(channel, message);
}

SubmitResult CoreRecordingSink::submit_owned(ChannelId channel, OwnedMessage&& message) {
  return recorder_->submit_owned(channel, std::move(message));
}

StopResult CoreRecordingSink::stop() { return recorder_->stop(); }

std::filesystem::path CoreRecordingSink::output_directory() const {
  return recorder_->parent_directory();
}

RecordingFlowPlugin::RecordingFlowPlugin(std::shared_ptr<RecordingFlow> flow)
    : flow_(std::move(flow)) {}

PluginDescriptor RecordingFlowPlugin::describe() const {
  PluginDescriptor descriptor;
  descriptor.id = "recording_flow";
  descriptor.requires = {require_service<IRecordingSink>("IRecordingSink")};
  descriptor.provides = {
      provide_service<IMessageSink>("IMessageSink"),
      provide_service<IPrecheckRegistry>("IPrecheckRegistry"),
      provide_service<IRecordingStatus>("IRecordingStatus"),
  };
  return descriptor;
}

void RecordingFlowPlugin::apply(Context& context, Scope& scope) {
  if (!flow_) {
    throw std::runtime_error("recording flow is null");
  }
  auto sink = context.require<IRecordingSink>();
  const Status attached = flow_->attach_sink(sink);
  if (!attached.ok()) {
    throw std::runtime_error(attached.detail);
  }
  scope.own(context.provide<IMessageSink>(std::static_pointer_cast<IMessageSink>(sink)));
  scope.own(context.provide<IPrecheckRegistry>(flow_->precheck_registry()));
  scope.own(context.provide<IRecordingStatus>(flow_));
  auto flow = flow_;
  scope.on_finalize([flow]() {
    const RecordingFlowResult result = flow->stop();
    if (result.stop && !result.stop->ok()) {
      throw std::runtime_error("recording stop failed: " +
                               result.stop->error.operation + ": " +
                               result.stop->error.detail);
    }
  });
}

Status RecordingFlowPlugin::activate() { return flow_->start(); }

PluginFactory recording_flow_factory(std::shared_ptr<RecordingFlow> flow) {
  return [flow = std::move(flow)]() -> std::shared_ptr<IPlugin> {
    return std::make_shared<RecordingFlowPlugin>(flow);
  };
}

}  // namespace robot_mcap
