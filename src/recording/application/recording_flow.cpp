#include "robot_mcap/recording_flow.hpp"

#include "recording/application/prechecks.hpp"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace robot_mcap {

struct RecordingFlow::Impl {
  StartContext context;
  std::shared_ptr<IRecordingSink> sink;
  std::shared_ptr<detail::PrecheckRegistry> prechecks;
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
  impl_->prechecks = std::make_shared<detail::PrecheckRegistry>();
  impl_->base_precheck_handles = detail::add_base_prechecks(*impl_->prechecks);
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
  std::shared_ptr<detail::PrecheckRegistry> prechecks;
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

}  // namespace robot_mcap
