#pragma once

#include "robot_mcap/plugin_runtime.hpp"

namespace robot_mcap::detail {

// 校验插件描述符：ID 非空，依赖和提供的字段合法且不重复。
// 这是纯规则，不接触服务表或插件对象，因此可以独立于运行时检查。
Status validate_descriptor(const PluginDescriptor& descriptor);

}  // namespace robot_mcap::detail
