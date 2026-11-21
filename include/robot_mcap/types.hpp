#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace robot_mcap {

class FileSystem;

using ChannelId = std::uint16_t;

// How the session files are made durable before the manifest is published.
// Defined here, not in recorder.hpp, so session_format can describe the
// recording result without including the recorder API.
enum class DurabilityMode { Close, Sync };

struct Status {
  std::string operation;
  std::filesystem::path path;
  std::string detail;

  bool ok() const noexcept { return operation.empty(); }
};

struct SchemaSpec {
  std::string name;
  std::string encoding;
  std::string data;
};

enum class PublishTimePolicy { SourceRequired, LogTimeOnly, PerMessage };

// A raw device clock value supplied outside the message payload.
// The unit is limited to values the v1 contract can represent losslessly.
// v1 stores the value verbatim in the timing sidecar and never derives
// MessageView::publish_time_ns from it; callers supply publish_time_ns separately
// when the source time is representable as Unix-epoch nanoseconds.
struct SourceClock {
  std::string clock_id;
  std::uint64_t value = 0;
  std::string unit;
  std::string meaning;
  std::string epoch;
};

struct ChannelSpec {
  std::string topic;
  std::string message_encoding;
  std::optional<SchemaSpec> schema;
  PublishTimePolicy publish_time_policy = PublishTimePolicy::LogTimeOnly;
};

struct MessageView {
  std::uint64_t log_time_ns;
  std::optional<std::uint64_t> publish_time_ns;
  const std::byte* data;
  std::size_t size;
  std::uint32_t sequence = 0;
  std::optional<SourceClock> source_clock;
};

struct FileOptions {
  std::uint64_t chunk_size_bytes = 1024 * 1024;
  // Optional I/O seam. A null value uses the native file system.
  std::shared_ptr<FileSystem> file_system;
};

}  // namespace robot_mcap
