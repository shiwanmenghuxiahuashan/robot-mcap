#include "robot_mcap/plugin_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace robot_mcap {
namespace detail {

// Instance-scoped service table. The registry is shared with live handles so a handle
// that outlives the runtime cannot dereference freed memory.
class ServiceRegistry {
public:
  struct Lookup {
    std::shared_ptr<void> object;
    std::string name;
    std::string provider;
    int version = 1;
  };

  struct Snapshot {
    std::string service;
    std::string name;
    std::string provider;
    int version = 1;
  };

  Status bind(const std::string& service, std::string name, int version,
              std::string provider, std::shared_ptr<void> object,
              std::uint64_t* token = nullptr) {
    if (service.empty() || name.empty() || version <= 0 || !object) {
      return {"service_bind", {}, "service id, name and version are required"};
    }
    std::lock_guard lock(mutex_);
    if (entries_.find(service) != entries_.end()) {
      return {"service_bind", {}, "duplicate provider for " + name};
    }
    Entry entry;
    entry.name = std::move(name);
    entry.provider = std::move(provider);
    entry.version = version;
    entry.object = std::move(object);
    entry.token = next_token_++;
    if (token != nullptr) {
      *token = entry.token;
    }
    entries_.emplace(service, std::move(entry));
    return {};
  }

  Status provide(const std::string& service, const std::string& name, int version,
                 const std::string& provider, std::shared_ptr<void> object,
                 std::uint64_t& token) {
    Status status = bind(service, name, version, provider, std::move(object), &token);
    return status;
  }

  std::optional<Lookup> find(const std::string& service) const {
    std::lock_guard lock(mutex_);
    const auto entry = entries_.find(service);
    if (entry == entries_.end()) {
      return std::nullopt;
    }
    return Lookup{entry->second.object, entry->second.name, entry->second.provider,
                  entry->second.version};
  }

  void revoke(const std::string& service, std::uint64_t token) {
    std::lock_guard lock(mutex_);
    const auto entry = entries_.find(service);
    if (entry != entries_.end() && entry->second.token == token) {
      entries_.erase(entry);
    }
  }

  // Removes every service provided by a plugin, even if its registration handle
  // leaked. Called before native modules unload so no module object remains reachable
  // from the registry.
  void revoke_provider(const std::string& provider) {
    std::lock_guard lock(mutex_);
    for (auto entry = entries_.begin(); entry != entries_.end();) {
      if (entry->second.provider == provider) {
        entry = entries_.erase(entry);
      } else {
        ++entry;
      }
    }
  }

  std::vector<Snapshot> snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<Snapshot> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
      result.push_back({entry.first, entry.second.name, entry.second.provider,
                        entry.second.version});
    }
    return result;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
  }

private:
  struct Entry {
    std::string name;
    std::string provider;
    int version = 0;
    std::shared_ptr<void> object;
    std::uint64_t token = 0;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::uint64_t next_token_ = 1;
};

}  // namespace detail

namespace {

Status error(const char* operation, std::string detail) {
  return {operation, {}, std::move(detail)};
}

bool valid_service_requirement(const ServiceRequirement& requirement) {
  return !requirement.service.empty() && !requirement.name.empty() &&
         requirement.min_version > 0;
}

bool valid_service_provision(const ServiceProvision& provision) {
  return !provision.service.empty() && !provision.name.empty() && provision.version > 0;
}

Status validate_descriptor(const PluginDescriptor& descriptor) {
  if (descriptor.id.empty()) {
    return error("plugin_descriptor", "plugin id is empty");
  }
  std::set<std::string> required;
  for (const auto& requirement : descriptor.requires) {
    if (!valid_service_requirement(requirement)) {
      return error("plugin_descriptor",
                   "plugin " + descriptor.id + " has an invalid service requirement");
    }
    if (!required.insert(requirement.service).second) {
      return error("plugin_descriptor",
                   "plugin " + descriptor.id + " declares a duplicate requirement " +
                       requirement.name);
    }
  }
  std::set<std::string> provided;
  for (const auto& provision : descriptor.provides) {
    if (!valid_service_provision(provision)) {
      return error("plugin_descriptor",
                   "plugin " + descriptor.id + " has an invalid service provision");
    }
    if (!provided.insert(provision.service).second) {
      return error("plugin_descriptor",
                   "plugin " + descriptor.id + " declares a duplicate provision " +
                       provision.name);
    }
  }
  return {};
}

}  // namespace

struct RegistrationHandle::State {
  std::mutex mutex;
  std::function<void()> revoke;
  std::atomic<bool> active{true};
};

RegistrationHandle::RegistrationHandle(std::function<void()> revoke)
    : state_(std::make_shared<State>()) {
  state_->revoke = std::move(revoke);
}

RegistrationHandle::~RegistrationHandle() { reset(); }

RegistrationHandle::RegistrationHandle(RegistrationHandle&& other) noexcept
    : state_(std::move(other.state_)) {}

RegistrationHandle& RegistrationHandle::operator=(RegistrationHandle&& other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
  }
  return *this;
}

void RegistrationHandle::reset() noexcept {
  if (!state_) {
    return;
  }
  bool expected = true;
  if (!state_->active.compare_exchange_strong(expected, false)) {
    return;
  }
  std::function<void()> revoke;
  {
    std::lock_guard lock(state_->mutex);
    revoke = std::move(state_->revoke);
  }
  if (revoke) {
    try {
      revoke();
    } catch (...) {
      // Registration revocation is best effort and must not throw from a destructor.
    }
  }
}

RegistrationHandle::operator bool() const noexcept {
  return state_ && state_->active.load();
}

struct Scope::Impl {
  std::vector<std::function<void()>> stop_actions;
  std::vector<std::function<void()>> wait_actions;
  std::vector<std::function<void()>> finalize_actions;
  std::vector<std::function<void()>> cleanup_actions;
  std::vector<RegistrationHandle> handles;
  bool stop_ran = false;
  bool wait_ran = false;
  bool finalize_ran = false;
  bool released = false;
};

namespace {

std::vector<Status> run_actions(std::vector<std::function<void()>>& actions,
                                const char* operation, const std::string& plugin_id) {
  std::vector<Status> errors;
  for (auto action = actions.rbegin(); action != actions.rend(); ++action) {
    try {
      if (*action) {
        (*action)();
      }
    } catch (const std::exception& exception) {
      errors.push_back(
          {operation, {}, "plugin " + plugin_id + ": " + exception.what()});
    } catch (...) {
      errors.push_back({operation, {}, "plugin " + plugin_id + ": unknown error"});
    }
  }
  return errors;
}

std::vector<Status> scope_phase(std::vector<std::function<void()>>& actions, bool& ran,
                                const char* operation, const std::string& plugin_id) {
  if (ran) {
    return {};
  }
  ran = true;
  return run_actions(actions, operation, plugin_id);
}

}  // namespace

Scope::Scope() : impl_(std::make_unique<Impl>()) {}

Scope::~Scope() {
  if (impl_ && !impl_->released) {
    try {
      release({});
    } catch (...) {
    }
  }
}

void Scope::on_stop(std::function<void()> action) {
  impl_->stop_actions.push_back(std::move(action));
}

void Scope::on_wait(std::function<void()> action) {
  impl_->wait_actions.push_back(std::move(action));
}

void Scope::on_finalize(std::function<void()> action) {
  impl_->finalize_actions.push_back(std::move(action));
}

void Scope::own(RegistrationHandle handle) {
  impl_->handles.push_back(std::move(handle));
}

void Scope::on_cleanup(std::function<void()> action) {
  impl_->cleanup_actions.push_back(std::move(action));
}

std::vector<Status> Scope::run_stop(const std::string& plugin_id) {
  return scope_phase(impl_->stop_actions, impl_->stop_ran, "plugin_stop", plugin_id);
}

std::vector<Status> Scope::run_wait(const std::string& plugin_id) {
  return scope_phase(impl_->wait_actions, impl_->wait_ran, "plugin_wait", plugin_id);
}

std::vector<Status> Scope::run_finalize(const std::string& plugin_id) {
  return scope_phase(impl_->finalize_actions, impl_->finalize_ran, "plugin_finalize",
                     plugin_id);
}

std::vector<Status> Scope::release(const std::string& plugin_id) {
  if (impl_->released) {
    return {};
  }
  impl_->released = true;
  std::vector<Status> errors =
      run_actions(impl_->cleanup_actions, "plugin_cleanup", plugin_id);
  for (auto handle = impl_->handles.rbegin(); handle != impl_->handles.rend(); ++handle) {
    handle->reset();
  }
  impl_->handles.clear();
  return errors;
}

std::shared_ptr<void> Context::require_erased(const std::string& service) {
  const ServiceRequirement* requirement = nullptr;
  for (const auto& candidate : descriptor_->requires) {
    if (candidate.service == service) {
      requirement = &candidate;
      break;
    }
  }
  if (requirement == nullptr) {
    throw std::runtime_error("service is not declared as a requirement: " + service);
  }
  const auto lookup = registry_->find(service);
  if (!lookup) {
    throw std::runtime_error("missing dependency: " + requirement->name);
  }
  if (lookup->version < requirement->min_version) {
    throw std::runtime_error(
        "service version mismatch: " + requirement->name + " requires >= " +
        std::to_string(requirement->min_version) + " but provider offers " +
        std::to_string(lookup->version));
  }
  return lookup->object;
}

std::shared_ptr<void> Context::find_erased(const std::string& service) {
  const ServiceRequirement* requirement = nullptr;
  for (const auto& candidate : descriptor_->requires) {
    if (candidate.service == service) {
      requirement = &candidate;
      break;
    }
  }
  if (requirement == nullptr) {
    throw std::runtime_error("service is not declared as a requirement: " + service);
  }
  const auto lookup = registry_->find(service);
  if (!lookup) {
    return nullptr;
  }
  if (lookup->version < requirement->min_version) {
    if (requirement->optional) {
      return nullptr;
    }
    throw std::runtime_error(
        "service version mismatch: " + requirement->name + " requires >= " +
        std::to_string(requirement->min_version) + " but provider offers " +
        std::to_string(lookup->version));
  }
  return lookup->object;
}

RegistrationHandle Context::provide_erased(const std::string& service,
                                           std::shared_ptr<void> object) {
  const ServiceProvision* provision = nullptr;
  for (const auto& candidate : descriptor_->provides) {
    if (candidate.service == service) {
      provision = &candidate;
      break;
    }
  }
  if (provision == nullptr) {
    throw std::runtime_error("service is not declared as a provision: " + service);
  }
  if (!object) {
    throw std::runtime_error("service object is null: " + provision->name);
  }
  std::uint64_t token = 0;
  const Status status =
      registry_->provide(service, provision->name, provision->version, descriptor_->id,
                         std::move(object), token);
  if (!status.ok()) {
    throw std::runtime_error(status.detail);
  }
  auto registry = registry_;
  return RegistrationHandle(
      [registry, service, token] { registry->revoke(service, token); });
}

struct PluginRuntime::Impl {
  struct Record {
    std::string requested_id;
    std::shared_ptr<IPlugin> plugin;
    PluginDescriptor descriptor;
    std::unique_ptr<Scope> scope;
    std::unique_ptr<Context> context;
    bool applied = false;
  };

  struct NativeModule {
    void* handle = nullptr;
    const RobotMcapPluginModuleV1* module = nullptr;
    std::filesystem::path path;

    ~NativeModule() {
      if (handle != nullptr) {
        unload();
      }
    }

    Status unload() noexcept {
      if (handle == nullptr) {
        return {};
      }
#ifdef _WIN32
      if (!FreeLibrary(static_cast<HMODULE>(handle))) {
        return {"unload_plugin", path, "FreeLibrary failed"};
      }
#else
      if (dlclose(handle) != 0) {
        const char* detail = dlerror();
        return {"unload_plugin", path, detail ? detail : "dlclose failed"};
      }
#endif
      handle = nullptr;
      return {};
    }
  };

  std::shared_ptr<detail::ServiceRegistry> registry =
      std::make_shared<detail::ServiceRegistry>();
  std::unordered_map<std::string, PluginFactory> factories;
  std::vector<std::shared_ptr<NativeModule>> modules;
  std::vector<std::unique_ptr<Record>> records;
  // Records in dependency (initialization) order. Shutdown and destruction walk this
  // list in reverse; record selection order is not a substitute.
  std::vector<Record*> initialized_order;
  RuntimeState state = RuntimeState::Created;
  Status start_error;
  RuntimeStopResult stop_result;
  bool stop_called = false;

  Status start(const std::vector<std::string>& plugin_ids);
  RuntimeStopResult stop();
  Status fail_start(const Status& failure, std::vector<Record*> initialized);
  RuntimeStopResult shutdown(const std::vector<Record*>& initialized);
  void destroy_all();
};

namespace {

void append_cleanup(RuntimeStopResult& result, std::vector<Status> errors) {
  for (auto& status : errors) {
    if (status.ok()) {
      continue;
    }
    if (result.error.ok()) {
      result.error = status;
    }
    result.cleanup_errors.push_back(std::move(status));
  }
}

Status native_load_error(const std::filesystem::path& path, const char* detail) {
  return {"load_plugin", path, detail};
}

}  // namespace

Status PluginRuntime::Impl::start(const std::vector<std::string>& plugin_ids) {
  if (state != RuntimeState::Created) {
    return error("plugin_runtime", "runtime has already been started");
  }

  std::set<std::string> selected;
  for (const auto& id : plugin_ids) {
    if (!selected.insert(id).second) {
      return fail_start(error("plugin_runtime", "duplicate plugin selection: " + id),
                        {});
    }
  }

  try {
    records.reserve(plugin_ids.size());
    for (const auto& id : plugin_ids) {
      const auto factory = factories.find(id);
      if (factory == factories.end()) {
        return fail_start(error("plugin_runtime", "plugin factory not found: " + id), {});
      }
      auto created = std::make_unique<Record>();
      created->requested_id = id;
      created->plugin = factory->second();
      if (!created->plugin) {
        records.push_back(std::move(created));
        return fail_start(
            error("plugin_runtime", "plugin factory returned null: " + id), {});
      }
      // Keep the record in the runtime before validation so a failure destroys the
      // plugin object before its native module is unloaded.
      records.push_back(std::move(created));
      Record& record = *records.back();
      record.descriptor = record.plugin->describe();
      if (record.descriptor.id != id) {
        return fail_start(
            error("plugin_runtime",
                  "plugin " + id + " described itself as " + record.descriptor.id),
            {});
      }
      const Status valid = validate_descriptor(record.descriptor);
      if (!valid.ok()) {
        return fail_start(valid, {});
      }
    }
  } catch (const std::exception& exception) {
    return fail_start(
        error("plugin_runtime", std::string("plugin creation failed: ") + exception.what()),
        {});
  } catch (...) {
    return fail_start(error("plugin_runtime", "plugin creation failed"), {});
  }

  struct Provider {
    std::string name;
    std::string provider;
    int version = 0;
    int plugin_index = -1;
  };
  std::unordered_map<std::string, Provider> providers;
  for (const auto& snapshot : registry->snapshot()) {
    providers.emplace(snapshot.service,
                      Provider{snapshot.name, snapshot.provider, snapshot.version, -1});
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& descriptor = records[index]->descriptor;
    for (const auto& provision : descriptor.provides) {
      const auto inserted = providers.emplace(
          provision.service,
          Provider{provision.name, descriptor.id, provision.version,
                   static_cast<int>(index)});
      if (!inserted.second) {
        return fail_start(
            error("plugin_runtime",
                  "duplicate provider for " + provision.name + ": " + descriptor.id +
                      " conflicts with " + inserted.first->second.provider),
            {});
      }
    }
  }

  const auto count = records.size();
  std::vector<std::vector<int>> dependents(count);
  std::vector<int> indegree(count, 0);
  for (std::size_t index = 0; index < count; ++index) {
    const auto& descriptor = records[index]->descriptor;
    for (const auto& requirement : descriptor.requires) {
      const auto provider = providers.find(requirement.service);
      if (provider == providers.end()) {
        if (requirement.optional) {
          continue;
        }
        return fail_start(
            error("plugin_runtime",
                  "plugin " + descriptor.id + " missing dependency: " +
                      requirement.name),
            {});
      }
      if (provider->second.version < requirement.min_version) {
        if (requirement.optional) {
          // An optional capability with an incompatible provider is unavailable;
          // find<T>() returns null and no ordering edge is added.
          continue;
        }
        return fail_start(
            error("plugin_runtime",
                  "service version mismatch: " + requirement.name + " requires >= " +
                      std::to_string(requirement.min_version) + " but " +
                      provider->second.provider + " provides " +
                      std::to_string(provider->second.version)),
            {});
      }
      if (provider->second.plugin_index >= 0) {
        dependents[static_cast<std::size_t>(provider->second.plugin_index)]
            .push_back(static_cast<int>(index));
        ++indegree[index];
      }
    }
  }

  std::vector<int> order;
  order.reserve(count);
  std::vector<bool> done(count, false);
  for (std::size_t step = 0; step < count; ++step) {
    int pick = -1;
    for (std::size_t index = 0; index < count; ++index) {
      if (!done[index] && indegree[index] == 0) {
        pick = static_cast<int>(index);
        break;
      }
    }
    if (pick < 0) {
      std::string cycle;
      for (std::size_t index = 0; index < count; ++index) {
        if (!done[index]) {
          if (!cycle.empty()) {
            cycle += ", ";
          }
          cycle += records[index]->descriptor.id;
        }
      }
      return fail_start(error("plugin_runtime", "dependency cycle among plugins: " + cycle),
                        {});
    }
    done[static_cast<std::size_t>(pick)] = true;
    order.push_back(pick);
    for (const int dependent : dependents[static_cast<std::size_t>(pick)]) {
      --indegree[static_cast<std::size_t>(dependent)];
    }
  }

  std::vector<Record*> initialized;
  initialized.reserve(count);
  for (const int index : order) {
    auto& record = records[static_cast<std::size_t>(index)];
    record->scope = std::make_unique<Scope>();
    record->context = std::unique_ptr<Context>(new Context(registry, record->descriptor));
    try {
      record->plugin->apply(*record->context, *record->scope);
    } catch (const std::exception& exception) {
      initialized.push_back(record.get());
      return fail_start(
          error("plugin_runtime",
                "plugin " + record->descriptor.id + " apply failed: " + exception.what()),
          initialized);
    } catch (...) {
      initialized.push_back(record.get());
      return fail_start(
          error("plugin_runtime",
                "plugin " + record->descriptor.id + " apply failed"),
          initialized);
    }
    for (const auto& provision : record->descriptor.provides) {
      const auto lookup = registry->find(provision.service);
      if (!lookup || lookup->provider != record->descriptor.id ||
          lookup->version != provision.version) {
        initialized.push_back(record.get());
        return fail_start(
            error("plugin_runtime",
                  "plugin " + record->descriptor.id +
                      " did not provide declared service: " + provision.name),
            initialized);
      }
    }
    record->applied = true;
    initialized.push_back(record.get());
  }
  initialized_order = initialized;

  for (const int index : order) {
    auto& record = records[static_cast<std::size_t>(index)];
    try {
      const Status activated = record->plugin->activate();
      if (!activated.ok()) {
        return fail_start(
            error("plugin_runtime",
                  "plugin " + record->descriptor.id + " activate failed: " +
                      activated.operation + ": " + activated.detail),
            initialized);
      }
    } catch (const std::exception& exception) {
      return fail_start(
          error("plugin_runtime",
                "plugin " + record->descriptor.id +
                    " activate failed: " + exception.what()),
          initialized);
    } catch (...) {
      return fail_start(
          error("plugin_runtime",
                "plugin " + record->descriptor.id + " activate failed"),
          initialized);
    }
  }

  state = RuntimeState::Running;
  return {};
}

Status PluginRuntime::Impl::fail_start(const Status& failure,
                                       std::vector<Record*> initialized) {
  initialized_order = initialized;
  RuntimeStopResult cleanup = shutdown(initialized);
  start_error = failure;
  stop_result.error = failure;
  stop_result.cleanup_errors = std::move(cleanup.cleanup_errors);
  stop_called = true;
  state = RuntimeState::Stopped;
  return failure;
}

RuntimeStopResult PluginRuntime::Impl::shutdown(const std::vector<Record*>& initialized) {
  RuntimeStopResult result;
  for (auto record = initialized.rbegin(); record != initialized.rend(); ++record) {
    append_cleanup(result, (*record)->scope->run_stop((*record)->descriptor.id));
  }
  for (auto record = initialized.rbegin(); record != initialized.rend(); ++record) {
    append_cleanup(result, (*record)->scope->run_wait((*record)->descriptor.id));
  }
  for (auto record = initialized.rbegin(); record != initialized.rend(); ++record) {
    append_cleanup(result, (*record)->scope->run_finalize((*record)->descriptor.id));
  }
  for (auto record = initialized.rbegin(); record != initialized.rend(); ++record) {
    append_cleanup(result, (*record)->scope->release((*record)->descriptor.id));
  }

  // Drop any plugin-provided service a plugin failed to unregister, before the plugin
  // objects are destroyed and any native module is unloaded.
  for (const auto& record : records) {
    registry->revoke_provider(record->descriptor.id);
  }

  destroy_all();

  for (const auto& module : modules) {
    const Status unloaded = module->unload();
    if (!unloaded.ok()) {
      append_cleanup(result, {unloaded});
    }
  }
  modules.clear();
  registry->clear();
  return result;
}

void PluginRuntime::Impl::destroy_all() {
  // Destroy plugins in reverse initialization order; the deleter for a native plugin
  // keeps its module alive until this point.
  std::set<Record*> ordered(initialized_order.begin(), initialized_order.end());
  for (auto record = initialized_order.rbegin(); record != initialized_order.rend();
       ++record) {
    (*record)->plugin.reset();
  }
  for (auto record = records.rbegin(); record != records.rend(); ++record) {
    if (ordered.find(record->get()) == ordered.end()) {
      (*record)->plugin.reset();
    }
  }
  records.clear();
  initialized_order.clear();
}

RuntimeStopResult PluginRuntime::Impl::stop() {
  if (state == RuntimeState::Created) {
    RuntimeStopResult result;
    result.error = error("plugin_runtime", "runtime has not been started");
    return result;
  }
  if (stop_called) {
    return stop_result;
  }
  std::vector<Record*> initialized = initialized_order;
  if (initialized.empty() && !records.empty()) {
    // Defensive fallback for a partially initialized runtime; normal paths always set
    // initialized_order.
    initialized.reserve(records.size());
    for (const auto& record : records) {
      if (record->scope) {
        initialized.push_back(record.get());
      }
    }
  }
  stop_result = shutdown(initialized);
  stop_called = true;
  state = RuntimeState::Stopped;
  return stop_result;
}

PluginRuntime::PluginRuntime() : impl_(std::make_unique<Impl>()) {}

PluginRuntime::~PluginRuntime() {
  if (impl_ && impl_->state == RuntimeState::Running) {
    try {
      impl_->stop();
    } catch (...) {
    }
  }
}

Status PluginRuntime::bind_host_service_erased(std::string service, std::string name,
                                               int version,
                                               std::shared_ptr<void> object) {
  if (impl_->state != RuntimeState::Created) {
    return error("plugin_runtime", "host services must be bound before start()");
  }
  return impl_->registry->bind(service, std::move(name), version, "host",
                               std::move(object));
}

Status PluginRuntime::add_plugin_factory(std::string plugin_id, PluginFactory factory) {
  if (impl_->state != RuntimeState::Created) {
    return error("plugin_runtime", "plugin factories must be added before start()");
  }
  if (plugin_id.empty() || !factory) {
    return error("plugin_runtime", "plugin id and factory are required");
  }
  const auto inserted = impl_->factories.emplace(plugin_id, std::move(factory));
  if (!inserted.second) {
    return error("plugin_runtime", "duplicate plugin factory: " + plugin_id);
  }
  return {};
}

Status PluginRuntime::load_plugin_module(const std::filesystem::path& module_path) {
  if (impl_->state != RuntimeState::Created) {
    return error("plugin_runtime", "plugin modules must be loaded before start()");
  }
  if (module_path.empty()) {
    return error("plugin_runtime", "plugin module path is empty");
  }

  auto native = std::make_shared<Impl::NativeModule>();
  native->path = module_path;
#ifdef _WIN32
  native->handle = LoadLibraryW(module_path.c_str());
  if (native->handle == nullptr) {
    return native_load_error(module_path, "LoadLibrary failed");
  }
  const auto entry = reinterpret_cast<RobotMcapPluginModuleV1Function>(
      GetProcAddress(static_cast<HMODULE>(native->handle), kPluginModuleEntryPoint));
#else
  native->handle = dlopen(module_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (native->handle == nullptr) {
    const char* detail = dlerror();
    return native_load_error(module_path, detail ? detail : "dlopen failed");
  }
  const auto entry = reinterpret_cast<RobotMcapPluginModuleV1Function>(
      dlsym(native->handle, kPluginModuleEntryPoint));
#endif
  if (entry == nullptr) {
    native->unload();
    return native_load_error(module_path, "plugin module entry point is missing");
  }
  const RobotMcapPluginModuleV1* module = entry();
  if (module == nullptr || module->abi_version != kPluginModuleAbiVersion ||
      module->plugin_id == nullptr || module->plugin_id[0] == '\0' ||
      module->create == nullptr || module->destroy == nullptr) {
    native->unload();
    return native_load_error(module_path, "plugin module ABI is invalid");
  }
  native->module = module;
  const std::string plugin_id = module->plugin_id;
  PluginFactory factory = [native]() -> std::shared_ptr<IPlugin> {
    IPlugin* plugin = native->module->create();
    if (plugin == nullptr) {
      throw std::runtime_error("native plugin create returned null");
    }
    return std::shared_ptr<IPlugin>(plugin, [native](IPlugin* value) {
      native->module->destroy(value);
    });
  };
  const Status added = add_plugin_factory(plugin_id, std::move(factory));
  if (!added.ok()) {
    native->unload();
    return added;
  }
  impl_->modules.push_back(std::move(native));
  return {};
}

Status PluginRuntime::start(const std::vector<std::string>& plugin_ids) {
  return impl_->start(plugin_ids);
}

RuntimeStopResult PluginRuntime::stop() { return impl_->stop(); }

RuntimeState PluginRuntime::state() const noexcept { return impl_->state; }

}  // namespace robot_mcap
