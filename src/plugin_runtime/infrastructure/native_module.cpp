#include "plugin_runtime/infrastructure/native_module.hpp"

#include <string_view>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace robot_mcap::detail {
namespace {

Status load_error(const std::filesystem::path& path, const char* detail) {
  return {"load_plugin", path, detail};
}

}  // namespace

NativeModule::NativeModule(std::filesystem::path path) : path_(std::move(path)) {}

NativeModule::~NativeModule() {
  if (handle_ != nullptr) {
    unload();
  }
}

Status NativeModule::load() {
  if (handle_ != nullptr) {
    return load_error(path_, "module is already loaded");
  }
#ifdef _WIN32
  handle_ = LoadLibraryW(path_.c_str());
  if (handle_ == nullptr) {
    return load_error(path_, "LoadLibrary failed");
  }
  const auto entry = reinterpret_cast<RobotMcapPluginModuleV1Function>(
      GetProcAddress(static_cast<HMODULE>(handle_), kPluginModuleEntryPoint));
#else
  handle_ = dlopen(path_.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle_ == nullptr) {
    const char* detail = dlerror();
    return load_error(path_, detail ? detail : "dlopen failed");
  }
  const auto entry = reinterpret_cast<RobotMcapPluginModuleV1Function>(
      dlsym(handle_, kPluginModuleEntryPoint));
#endif
  if (entry == nullptr) {
    unload();
    return load_error(path_, "plugin module entry point is missing");
  }
  const RobotMcapPluginModuleV1* module = entry();
  if (module == nullptr || module->abi_version != kPluginModuleAbiVersion ||
      module->plugin_id == nullptr || module->plugin_id[0] == '\0' ||
      module->create == nullptr || module->destroy == nullptr) {
    unload();
    return load_error(path_, "plugin module ABI is invalid");
  }
  module_ = module;
  return {};
}

Status NativeModule::unload() noexcept {
  if (handle_ == nullptr) {
    return {};
  }
#ifdef _WIN32
  if (!FreeLibrary(static_cast<HMODULE>(handle_))) {
    return {"unload_plugin", path_, "FreeLibrary failed"};
  }
#else
  if (dlclose(handle_) != 0) {
    const char* detail = dlerror();
    return {"unload_plugin", path_, detail ? detail : "dlclose failed"};
  }
#endif
  handle_ = nullptr;
  return {};
}

std::string_view NativeModule::plugin_id() const noexcept {
  if (module_ == nullptr || module_->plugin_id == nullptr) {
    return {};
  }
  return module_->plugin_id;
}

}  // namespace robot_mcap::detail
