#include "robot_mcap/recording_flow.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace robot_mcap {

// Composition adapter. This is the only translation unit where the recording
// context and the plugin runtime meet: recording_flow.cpp stays free of plugin
// SDK types, and plugin_runtime.cpp stays free of recording types.

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
