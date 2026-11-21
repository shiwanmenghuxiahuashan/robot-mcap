#include "plugin_runtime/domain/descriptor_rules.hpp"

#include <set>
#include <string>
#include <utility>

namespace robot_mcap::detail {
namespace {

Status error(const char *operation, std::string detail) {
  return {operation, {}, std::move(detail)};
}

bool valid_service_requirement(const ServiceRequirement &requirement) {
  return !requirement.service.empty() && !requirement.name.empty() &&
         requirement.min_version > 0;
}

bool valid_service_provision(const ServiceProvision &provision) {
  return !provision.service.empty() && !provision.name.empty() &&
         provision.version > 0;
}

} // namespace

Status validate_descriptor(const PluginDescriptor &descriptor) {
  if (descriptor.id.empty()) {
    return error("plugin_descriptor", "plugin id is empty");
  }
  std::set<std::string> required;
  for (const auto &requirement : descriptor.requires) {
    if (!valid_service_requirement(requirement)) {
      return error("plugin_descriptor",
                   "plugin " + descriptor.id +
                       " has an invalid service requirement");
    }
    if (!required.insert(requirement.service).second) {
      return error("plugin_descriptor",
                   "plugin " + descriptor.id +
                       " declares a duplicate requirement " + requirement.name);
    }
  }
  std::set<std::string> provided;
  for (const auto &provision : descriptor.provides) {
    if (!valid_service_provision(provision)) {
      return error("plugin_descriptor",
                   "plugin " + descriptor.id +
                       " has an invalid service provision");
    }
    if (!provided.insert(provision.service).second) {
      return error("plugin_descriptor", "plugin " + descriptor.id +
                                            " declares a duplicate provision " +
                                            provision.name);
    }
  }
  return {};
}

} // namespace robot_mcap::detail
