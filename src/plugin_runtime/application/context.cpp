#include "robot_mcap/plugin_runtime.hpp"

#include "plugin_runtime/infrastructure/service_registry.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace robot_mcap {

std::shared_ptr<void> Context::require_erased(const std::string& service) {
  // 只允许使用描述符中声明的依赖，防止插件绕过装配检查。
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
    // 可选依赖版本不满足时视为不可用；强制依赖仍然报错。
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
  // 只允许提供描述符中声明的服务。返回的句柄必须交给插件 Scope 持有。
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
  // 句柄与注册表共享所有权，撤销时按 token 精确删除本次注册。
  auto registry = registry_;
  return RegistrationHandle(
      [registry, service, token] { registry->revoke(service, token); });
}

}  // namespace robot_mcap
