#pragma once

#include "robot_mcap/plugin_runtime.hpp"

#include <filesystem>
#include <string_view>

namespace robot_mcap::detail {

// 拥有一个原生插件模块句柄。load() 负责加载、查找入口点并校验 ABI；
// unload() 只在模块创建的所有对象都销毁后调用。
class NativeModule {
public:
  explicit NativeModule(std::filesystem::path path);
  ~NativeModule();

  NativeModule(const NativeModule&) = delete;
  NativeModule& operator=(const NativeModule&) = delete;

  Status load();
  Status unload() noexcept;

  const RobotMcapPluginModuleV1& module() const noexcept { return *module_; }
  std::string_view plugin_id() const noexcept;
  const std::filesystem::path& path() const noexcept { return path_; }

private:
  void* handle_ = nullptr;
  const RobotMcapPluginModuleV1* module_ = nullptr;
  std::filesystem::path path_;
};

}  // namespace robot_mcap::detail
