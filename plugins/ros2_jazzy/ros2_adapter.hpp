#pragma once

#include "robot_mcap/plugin_runtime.hpp"
#include "robot_mcap/recording_flow.hpp"
#include "robot_mcap/types.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rclcpp/qos.hpp>

namespace robot_mcap::ros2 {

struct Ros2Qos {
  int depth = 10;
  bool reliable = true;
  bool transient_local = false;

  rclcpp::QoS to_rclcpp() const;
};

struct Ros2TopicOption {
  std::string topic;
  std::string type;  // pkg/msg/Type
  bool required = false;
  bool allow_late = false;
  Ros2Qos qos;
};

struct Ros2SourceOptions {
  std::vector<Ros2TopicOption> topics;
  std::string node_name = "robot_mcap_ros2_source";
};

struct Ros2Diagnostic {
  std::string code;
  std::string severity = "warning";
  std::optional<std::string> topic;
  std::optional<std::uint64_t> log_time_ns;
  std::string detail;
  std::string action;
};

class IRos2SourceConfig {
public:
  virtual ~IRos2SourceConfig() = default;
  virtual const Ros2SourceOptions& options() const = 0;
};

class IRos2Diagnostics {
public:
  virtual ~IRos2Diagnostics() = default;
  virtual void add(Ros2Diagnostic diagnostic) = 0;
  virtual std::vector<Ros2Diagnostic> snapshot() const = 0;
};

class Ros2DiagnosticsLog final : public IRos2Diagnostics {
public:
  void add(Ros2Diagnostic diagnostic) override;
  std::vector<Ros2Diagnostic> snapshot() const override;

private:
  mutable std::mutex mutex_;
  std::vector<Ros2Diagnostic> diagnostics_;
};

// Host-bound received-message counters used by sources and the replay test.
class IRos2SourceStats {
public:
  virtual ~IRos2SourceStats() = default;
  virtual void received(const std::string& topic) = 0;
  virtual std::uint64_t count(const std::string& topic) const = 0;
};

class Ros2SourceStats final : public IRos2SourceStats {
public:
  void received(const std::string& topic) override;
  std::uint64_t count(const std::string& topic) const override;

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t> counts_;
};

struct SchemaText {
  std::string type;
  std::string encoding;  // ros2msg or ros2idl
  std::string data;
  std::string error;

  bool ok() const noexcept { return error.empty() && !data.empty(); }
};

// Reads installed .msg definitions and recursively expands message dependencies.
SchemaText load_message_schema(const std::string& type);

struct Ros2MetadataOptions {
  // Test hook: fail before writing metadata while still publishing the failed manifest.
  bool fail_for_test = false;
};

// Adapter-only finalization: reads the finalized MCAP, writes metadata.yaml with the
// Jazzy rosbag2 metadata API, then atomically rewrites manifest.json's ros2_replay and
// diagnostics. The core does not link ROS.
class Ros2MetadataWriter {
public:
  explicit Ros2MetadataWriter(Ros2MetadataOptions options = {});

  Status write(const std::filesystem::path& session_directory,
               const Ros2SourceOptions& source_options,
               const IRos2Diagnostics& diagnostics) const;

private:
  Ros2MetadataOptions options_;
};

}  // namespace robot_mcap::ros2
