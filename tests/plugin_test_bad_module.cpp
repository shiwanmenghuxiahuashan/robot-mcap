#include "robot_mcap/plugin_runtime.hpp"

// Deliberately reports an unsupported ABI version so the runtime rejects it.
extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const robot_mcap::RobotMcapPluginModuleV1* robot_mcap_plugin_module_v1() {
  static const robot_mcap::RobotMcapPluginModuleV1 module{
      999,
      "bad.abi",
      []() -> robot_mcap::IPlugin* { return nullptr; },
      [](robot_mcap::IPlugin*) {},
  };
  return &module;
}
}
