#include "plugin_test_support.hpp"

#include "robot_mcap/plugin_runtime.hpp"

#include <memory>

namespace {

class ModuleService final : public robot_mcap_test::IModuleService {
public:
  explicit ModuleService(std::shared_ptr<robot_mcap_test::IProbe> probe)
      : probe_(std::move(probe)) {}

  void ping() const override { probe_->record("module_service.ping"); }

  ~ModuleService() override { probe_->record("module_service.destroy"); }

private:
  std::shared_ptr<robot_mcap_test::IProbe> probe_;
};

class NativeTestPlugin final : public robot_mcap::IPlugin {
public:
  robot_mcap::PluginDescriptor describe() const override {
    robot_mcap::PluginDescriptor descriptor;
    descriptor.id = "native.test";
    descriptor.requires = {
        robot_mcap::require_service<robot_mcap_test::IProbe>("IProbe")};
    descriptor.provides = {robot_mcap::provide_service<robot_mcap_test::IModuleService>(
        "IModuleService")};
    return descriptor;
  }

  void apply(robot_mcap::Context& context, robot_mcap::Scope& scope) override {
    probe_ = context.require<robot_mcap_test::IProbe>();
    probe_->record("native.apply");
    scope.on_stop([this] { probe_->record("native.stop"); });
    scope.on_wait([this] { probe_->record("native.wait"); });
    scope.on_finalize([this] { probe_->record("native.finalize"); });
    scope.on_cleanup([this] { probe_->record("native.cleanup"); });
    scope.own(context.provide<robot_mcap_test::IModuleService>(
        std::make_shared<ModuleService>(probe_)));
  }

  robot_mcap::Status activate() override {
    probe_->record("native.activate");
    return {};
  }

  ~NativeTestPlugin() override {
    if (probe_) {
      probe_->record("native.destroy");
    }
  }

private:
  std::shared_ptr<robot_mcap_test::IProbe> probe_;
};

}  // namespace

extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const robot_mcap::RobotMcapPluginModuleV1* robot_mcap_plugin_module_v1() {
  static const robot_mcap::RobotMcapPluginModuleV1 module{
      robot_mcap::kPluginModuleAbiVersion,
      "native.test",
      []() -> robot_mcap::IPlugin* { return new NativeTestPlugin(); },
      [](robot_mcap::IPlugin* plugin) { delete plugin; },
  };
  return &module;
}
}
