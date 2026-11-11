#pragma once

#include "robot_mcap/types.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

namespace robot_mcap {

class Context;
class PluginRuntime;
class Scope;

namespace detail {
class ServiceRegistry;
}

// Service identifiers are derived from typeid(T).name(). Native modules must be built
// with the same toolchain, standard library and headers as the host (plan Q16).
template <typename T>
std::string service_id() {
  return typeid(T).name();
}

struct ServiceRequirement {
  std::string service;  // service_id<T>()
  std::string name;     // human-readable name for diagnostics
  int min_version = 1;
  bool optional = false;
};

struct ServiceProvision {
  std::string service;
  std::string name;
  int version = 1;
};

template <typename T>
ServiceRequirement require_service(std::string name, int min_version = 1,
                                   bool optional = false) {
  return {service_id<T>(), std::move(name), min_version, optional};
}

template <typename T>
ServiceProvision provide_service(std::string name, int version = 1) {
  return {service_id<T>(), std::move(name), version};
}

struct PluginDescriptor {
  std::string id;
  std::vector<ServiceRequirement> requires;
  std::vector<ServiceProvision> provides;
};

// RAII registration. reset() unregisters at most once; destruction unregisters if the
// handle is still active. Moving a handle transfers ownership.
class RegistrationHandle {
public:
  RegistrationHandle() noexcept = default;
  explicit RegistrationHandle(std::function<void()> revoke);
  ~RegistrationHandle();

  RegistrationHandle(RegistrationHandle&& other) noexcept;
  RegistrationHandle& operator=(RegistrationHandle&& other) noexcept;
  RegistrationHandle(const RegistrationHandle&) = delete;
  RegistrationHandle& operator=(const RegistrationHandle&) = delete;

  void reset() noexcept;
  explicit operator bool() const noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

// Per-plugin resource scope. apply() registers stop, wait, finalize and release
// actions here. Shutdown runs four runtime-wide phases in reverse initialization
// order: all stop actions (stop producers), all wait actions (join in-flight work),
// all finalize actions (finish the writer), then all cleanup actions and registration
// handles. Callbacks run without runtime locks; exceptions are collected and the
// remaining actions still run. Actions must be safe if the plugin is rolled back
// before activate() (apply-created threads belong in stop/wait, not finalize).
class Scope {
public:
  Scope();
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  void on_stop(std::function<void()> action);
  void on_wait(std::function<void()> action);
  void on_finalize(std::function<void()> action);
  // Takes ownership of a registration; it is revoked during the release phase.
  void own(RegistrationHandle handle);
  void on_cleanup(std::function<void()> action);

  // Called by PluginRuntime. Each phase runs once, in reverse registration order.
  // Errors are returned and the remaining actions still run.
  std::vector<Status> run_stop(const std::string& plugin_id);
  std::vector<Status> run_wait(const std::string& plugin_id);
  std::vector<Status> run_finalize(const std::string& plugin_id);
  std::vector<Status> release(const std::string& plugin_id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Assembly-time view of the instance service registry. A plugin may only require
// services declared in its descriptor and may only provide declared services.
// Service identity is the string returned by service_id<T>() (the implementation's
// typeid name), so native modules must use matching headers/toolchain. Provide the
// interface type explicitly: context.provide<IBase>(ptr), not provide(derived_ptr).
// Do not retain a Context after apply() returns.
class Context {
public:
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  template <typename T>
  std::shared_ptr<T> require() {
    return std::static_pointer_cast<T>(require_erased(service_id<T>()));
  }

  template <typename T>
  std::shared_ptr<T> find() {
    return std::static_pointer_cast<T>(find_erased(service_id<T>()));
  }

  // The returned handle must be owned by the plugin's Scope: scope.own(handle).
  // Dropping it unregisters the service immediately and fails provision validation;
  // keeping it in long-lived global state outlives the runtime's rollback contract.
  template <typename T>
  RegistrationHandle provide(std::shared_ptr<T> service) {
    return provide_erased(service_id<T>(),
                          std::static_pointer_cast<void>(std::move(service)));
  }

  const PluginDescriptor& descriptor() const noexcept { return *descriptor_; }

private:
  friend class PluginRuntime;
  Context(std::shared_ptr<detail::ServiceRegistry> registry,
          const PluginDescriptor& descriptor)
      : registry_(std::move(registry)), descriptor_(&descriptor) {}

  std::shared_ptr<void> require_erased(const std::string& service);
  std::shared_ptr<void> find_erased(const std::string& service);
  RegistrationHandle provide_erased(const std::string& service,
                                    std::shared_ptr<void> object);

  std::shared_ptr<detail::ServiceRegistry> registry_;
  const PluginDescriptor* descriptor_;
};

class IPlugin {
public:
  virtual ~IPlugin() = default;

  virtual PluginDescriptor describe() const = 0;
  // Register services and resources. All resources created here must be owned by the
  // Scope or a service registration handle so a later failure can roll them back.
  // describe() and apply() report failure by throwing; the runtime catches per plugin
  // and rolls back. Plugins must not retain the Context after apply().
  virtual void apply(Context& context, Scope& scope) = 0;
  // Startup has two phases: every selected plugin applies in dependency order (with
  // each provider's declared provisions verified before any dependent applies), then
  // every plugin activates in the same order. A plugin's activate runs only after all
  // required providers activated successfully. Sources register channels and start
  // producing here; the recording flow starts the core here. On failure the runtime
  // rolls back and never presents a successful result.
  virtual Status activate() { return {}; }
};

// shared_ptr supports native plugins whose objects must be destroyed by code inside
// the loaded module (the deleter keeps the module alive).
using PluginFactory = std::function<std::shared_ptr<IPlugin>()>;

enum class RuntimeState { Created, Running, Stopped };

struct RuntimeStopResult {
  // The first failure observed during shutdown.
  Status error;
  std::vector<Status> cleanup_errors;

  bool ok() const noexcept { return error.ok() && cleanup_errors.empty(); }
};

// Stable C entry point for native plugin modules. The module must be built with the
// same toolchain and headers as the host. The runtime keeps the module loaded until
// every object created by it has been destroyed.
struct RobotMcapPluginModuleV1 {
  std::uint32_t abi_version;
  const char* plugin_id;
  IPlugin* (*create)();
  void (*destroy)(IPlugin*);
};
using RobotMcapPluginModuleV1Function = const RobotMcapPluginModuleV1* (*)();
inline constexpr std::uint32_t kPluginModuleAbiVersion = 1;
inline constexpr const char* kPluginModuleEntryPoint = "robot_mcap_plugin_module_v1";

// One runtime per recording instance. Not thread-safe; the host serializes start(),
// stop() and factory/service registration. Plugins may own their own threads.
// Host services and factories must be registered before start(). Resolution rejects
// missing mandatory dependencies, duplicate providers, version mismatches and cycles
// before any apply. The runtime is one-shot: a failed or completed start is terminal,
// start() after stop/failure returns an error, and stop() before start returns an
// error without consuming the runtime. Native modules must be built with the same
// toolchain, standard library and headers; the module stays loaded until all objects
// created by it have been destroyed.
class PluginRuntime {
public:
  PluginRuntime();
  ~PluginRuntime();
  PluginRuntime(const PluginRuntime&) = delete;
  PluginRuntime& operator=(const PluginRuntime&) = delete;

  template <typename T>
  Status bind_host_service(std::shared_ptr<T> service, std::string name, int version = 1) {
    return bind_host_service_erased(service_id<T>(), std::move(name), version,
                                    std::static_pointer_cast<void>(std::move(service)));
  }

  Status add_plugin_factory(std::string plugin_id, PluginFactory factory);
  Status load_plugin_module(const std::filesystem::path& module_path);

  // Resolves the plugin list, validates host and plugin services, initializes in
  // dependency order, then activates. On failure the runtime rolls back and enters
  // Stopped; stop() then returns the cached failure.
  Status start(const std::vector<std::string>& plugin_ids);
  RuntimeStopResult stop();

  RuntimeState state() const noexcept;

private:
  Status bind_host_service_erased(std::string service, std::string name, int version,
                                  std::shared_ptr<void> object);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace robot_mcap
