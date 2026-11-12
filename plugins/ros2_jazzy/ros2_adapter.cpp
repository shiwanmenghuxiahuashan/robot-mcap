#include "ros2_adapter.hpp"

#include <utility>

namespace robot_mcap::ros2 {

rclcpp::QoS Ros2Qos::to_rclcpp() const {
  rclcpp::QoS qos(depth);
  if (reliable) {
    qos.reliable();
  } else {
    qos.best_effort();
  }
  if (transient_local) {
    qos.transient_local();
  }
  return qos;
}

void Ros2DiagnosticsLog::add(Ros2Diagnostic diagnostic) {
  std::lock_guard lock(mutex_);
  diagnostics_.push_back(std::move(diagnostic));
}

std::vector<Ros2Diagnostic> Ros2DiagnosticsLog::snapshot() const {
  std::lock_guard lock(mutex_);
  return diagnostics_;
}

void Ros2SourceStats::received(const std::string& topic) {
  std::lock_guard lock(mutex_);
  ++counts_[topic];
}

std::uint64_t Ros2SourceStats::count(const std::string& topic) const {
  std::lock_guard lock(mutex_);
  const auto entry = counts_.find(topic);
  return entry == counts_.end() ? 0 : entry->second;
}

}  // namespace robot_mcap::ros2
