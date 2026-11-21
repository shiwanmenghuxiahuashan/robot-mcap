#pragma once

#include "robot_mcap/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace robot_mcap::detail {

// Plain persistence shapes for one session. The recorder fills them at stop
// time, the renderer turns them into text, and an out-of-process reader can
// rebuild the same shape from disk without linking the recorder or plugins.
//
// Keep these types free of recorder, plugin and ROS 2 types so the v1 file
// contract stays explainable from this header alone.

struct Diagnostic {
  std::string code;
  std::string severity = "error";
  std::optional<std::string> topic;
  std::optional<std::uint64_t> log_time_ns;
  std::string detail;
  std::string action;
};

struct FileFacts {
  std::filesystem::path path;
  bool finalized = false;
  bool present = true;
  std::optional<std::uint64_t> bytes;
  std::optional<std::uint64_t> message_count;
  std::optional<std::uint64_t> first_log_time_ns;
  std::optional<std::uint64_t> last_log_time_ns;
};

struct TimingFacts {
  bool present = false;
  std::filesystem::path path;
  std::uint64_t row_count = 0;
  std::optional<std::string> sha256;
  bool sha256_ok = true;
  bool finalized = false;
};

struct Statistics {
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t writer_processed = 0;
  std::optional<std::uint64_t> finalized_messages;
};

struct ManifestInput {
  std::string session_id;
  bool session_finalized = false;
  FileFacts data;
  TimingFacts timing;
  std::vector<std::pair<ChannelId, ChannelSpec>> channels;
  Statistics statistics;
  DurabilityMode durability_mode = DurabilityMode::Close;
  std::vector<Diagnostic> diagnostics;
};

struct PublishResult {
  bool manifest_published = false;
  Status error;
};

// The v1 layout fixes this name; both the publisher and the renderer use the
// same constant instead of repeating the string.
inline constexpr const char* kConfigFileName = "recording-config.json";

}  // namespace robot_mcap::detail
