#pragma once

#include <string>

namespace robot_mcap_test {

// Host-provided probe used by the native test module.
class IProbe {
public:
  virtual ~IProbe() = default;
  virtual void record(const std::string& event) = 0;
};

// Service provided by the native test module and consumed by a host-built plugin.
class IModuleService {
public:
  virtual ~IModuleService() = default;
  virtual void ping() const = 0;
};

}  // namespace robot_mcap_test
