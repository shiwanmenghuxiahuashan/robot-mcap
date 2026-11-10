#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace robot_mcap {

using ChannelId = std::uint16_t;

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
};

struct FileOptions {
  std::uint64_t chunk_size_bytes = 1024 * 1024;
};

}  // namespace robot_mcap
