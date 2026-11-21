#include "plugin_runtime/domain/dependency_resolution.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace robot_mcap::detail {
namespace {

Status error(std::string detail) {
  return {"plugin_runtime", {}, std::move(detail)};
}

struct Provider {
  std::string name;
  std::string provider;
  int version = 0;
  int plugin_index = -1;  // -1 表示宿主服务。
};

}  // namespace

Status resolve_start_order(const std::vector<PluginDescriptor>& descriptors,
                           const std::vector<ServiceAvailability>& host_services,
                           std::vector<std::size_t>& order) {
  order.clear();

  // 每个服务只允许一个提供者。宿主服务也放在同一张表里，插件不能覆盖宿主，
  // 缺少宿主服务时也走统一的缺失依赖报错。
  std::unordered_map<std::string, Provider> providers;
  for (const auto& service : host_services) {
    providers.emplace(service.service,
                      Provider{service.name, service.provider, service.version, -1});
  }
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const auto& descriptor = descriptors[index];
    for (const auto& provision : descriptor.provides) {
      const auto inserted = providers.emplace(
          provision.service,
          Provider{provision.name, descriptor.id, provision.version,
                   static_cast<int>(index)});
      if (!inserted.second) {
        return error("duplicate provider for " + provision.name + ": " + descriptor.id +
                     " conflicts with " + inserted.first->second.provider);
      }
    }
  }

  // 建立依赖边。可选依赖缺失或版本过旧时不加边，插件通过 find() 观察到
  // 该能力不可用。
  const auto count = descriptors.size();
  std::vector<std::vector<int>> dependents(count);
  std::vector<int> indegree(count, 0);
  for (std::size_t index = 0; index < count; ++index) {
    const auto& descriptor = descriptors[index];
    for (const auto& requirement : descriptor.requires) {
      const auto provider = providers.find(requirement.service);
      if (provider == providers.end()) {
        if (requirement.optional) {
          continue;
        }
        return error("plugin " + descriptor.id + " missing dependency: " +
                     requirement.name);
      }
      if (provider->second.version < requirement.min_version) {
        if (requirement.optional) {
          continue;
        }
        return error("service version mismatch: " + requirement.name + " requires >= " +
                     std::to_string(requirement.min_version) + " but " +
                     provider->second.provider + " provides " +
                     std::to_string(provider->second.version));
      }
      if (provider->second.plugin_index >= 0) {
        dependents[static_cast<std::size_t>(provider->second.plugin_index)]
            .push_back(static_cast<int>(index));
        ++indegree[index];
      }
    }
  }

  // 稳定的拓扑排序。多个插件同时就绪时选择下标最小的那个，
  // 因此调用方的选择顺序是平局时的决定因素。
  std::vector<bool> done(count, false);
  order.reserve(count);
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
          cycle += descriptors[index].id;
        }
      }
      return error("dependency cycle among plugins: " + cycle);
    }
    done[static_cast<std::size_t>(pick)] = true;
    order.push_back(static_cast<std::size_t>(pick));
    for (const int dependent : dependents[static_cast<std::size_t>(pick)]) {
      --indegree[static_cast<std::size_t>(dependent)];
    }
  }
  return {};
}

}  // namespace robot_mcap::detail
