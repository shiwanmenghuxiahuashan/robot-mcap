#include "robot_mcap/plugin_runtime.hpp"

#include "plugin_runtime/domain/dependency_resolution.hpp"
#include "plugin_runtime/domain/descriptor_rules.hpp"
#include "plugin_runtime/infrastructure/native_module.hpp"
#include "plugin_runtime/infrastructure/service_registry.hpp"

#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace robot_mcap {
namespace {

Status error(const char* operation, std::string detail) {
  return {operation, {}, std::move(detail)};
}

}  // namespace

struct PluginRuntime::Impl {
  struct Record {
    std::string requested_id;
    std::shared_ptr<IPlugin> plugin;
    PluginDescriptor descriptor;
    std::unique_ptr<Scope> scope;
    std::unique_ptr<Context> context;
    bool applied = false;
  };

  std::shared_ptr<detail::ServiceRegistry> registry =
      std::make_shared<detail::ServiceRegistry>();
  std::unordered_map<std::string, PluginFactory> factories;
  // 原生模块句柄由运行时持有。最后一个由它创建的对象销毁后才卸载。
  std::vector<std::shared_ptr<detail::NativeModule>> modules;
  std::vector<std::unique_ptr<Record>> records;
  // 依赖（初始化）顺序中的记录。关闭和销毁都逆序遍历这个列表；
  // 选择顺序不能代替它。
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
      // 先放进 records 再做校验，确保失败时插件对象先于原生模块销毁。
      records.push_back(std::move(created));
      Record& record = *records.back();
      record.descriptor = record.plugin->describe();
      if (record.descriptor.id != id) {
        return fail_start(
            error("plugin_runtime",
                  "plugin " + id + " described itself as " + record.descriptor.id),
            {});
      }
      const Status valid = detail::validate_descriptor(record.descriptor);
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

  // 排序是描述符和宿主服务的纯函数。插件构造、apply 和 activate
  // 仍由运行时负责，解析器不执行任何插件代码。
  std::vector<PluginDescriptor> descriptors;
  descriptors.reserve(records.size());
  for (const auto& record : records) {
    descriptors.push_back(record->descriptor);
  }
  std::vector<detail::ServiceAvailability> host_services;
  for (const auto& snapshot : registry->snapshot()) {
    host_services.push_back({snapshot.service, snapshot.name, snapshot.provider,
                             snapshot.version});
  }
  std::vector<std::size_t> order;
  const Status resolved = detail::resolve_start_order(descriptors, host_services, order);
  if (!resolved.ok()) {
    return fail_start(resolved, {});
  }

  std::vector<Record*> initialized;
  initialized.reserve(records.size());
  for (const std::size_t index : order) {
    auto& record = records[index];
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
    // 提供者声明的服务必须真的注册成功，依赖者才能开始 apply。
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

  // 所有插件完成 apply 后才进入 activate，数据源只在此后开始生产。
  for (const std::size_t index : order) {
    auto& record = records[index];
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
  // 四个阶段都逆序执行：先停止生产，再等待在途工作，然后完成写入，
  // 最后清理资源和注册句柄。
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

  // 插件可能漏撤销自己提供的服务。在销毁插件对象和卸载原生模块之前
  // 统一清掉，防止注册表继续持有模块内的对象。
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
  // 按初始化顺序的逆序销毁插件。原生插件的删除器持有模块引用，
  // 因此模块会一直存活到最后一个对象销毁。
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
    // 部分初始化失败时的兜底。正常路径总会设置 initialized_order。
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

  // 加载、入口点查找和 ABI 校验由 NativeModule 负责。
  auto native = std::make_shared<detail::NativeModule>(module_path);
  const Status loaded = native->load();
  if (!loaded.ok()) {
    return loaded;
  }
  const std::string plugin_id(native->plugin_id());
  PluginFactory factory = [native]() -> std::shared_ptr<IPlugin> {
    IPlugin* plugin = native->module().create();
    if (plugin == nullptr) {
      throw std::runtime_error("native plugin create returned null");
    }
    // 删除器持有模块引用，保证插件对象销毁前模块仍然加载。
    return std::shared_ptr<IPlugin>(plugin, [native](IPlugin* value) {
      native->module().destroy(value);
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
