#pragma once

#include "robot_mcap/plugin_runtime.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace robot_mcap::detail {

// 插件初始化之前就已经可用的服务，通常由宿主绑定。
struct ServiceAvailability {
  std::string service;
  std::string name;
  std::string provider;
  int version = 0;
};

// 纯依赖解析：校验插件提供者和依赖，然后返回描述符下标组成的初始化顺序。
// 它不构造插件，也不执行插件代码，因此排序规则可以单独检查。
// 返回错误时 order 保持为空。
Status resolve_start_order(const std::vector<PluginDescriptor>& descriptors,
                           const std::vector<ServiceAvailability>& host_services,
                           std::vector<std::size_t>& order);

}  // namespace robot_mcap::detail
