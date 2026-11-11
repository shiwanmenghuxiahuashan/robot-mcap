#pragma once

#include "robot_mcap/io.hpp"
#include "robot_mcap/recorder.hpp"
#include "robot_mcap/types.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace robot_mcap::detail {

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

class SessionArtifacts {
public:
  SessionArtifacts(std::shared_ptr<FileSystem> file_system,
                   std::filesystem::path session_directory, std::string session_id,
                   FileOptions file_options, std::uint64_t queue_byte_limit,
                   DurabilityMode durability_mode);

  const std::filesystem::path& directory() const noexcept { return directory_; }
  const std::filesystem::path& data_partial_path() const noexcept { return data_partial_; }
  const std::filesystem::path& data_final_path() const noexcept { return data_final_; }
  const std::filesystem::path& timing_partial_path() const noexcept { return timing_partial_; }
  const std::filesystem::path& timing_final_path() const noexcept { return timing_final_; }
  const std::filesystem::path& config_path() const noexcept { return config_; }

  // Creates the session directory exclusively and writes the config snapshot.
  void create();

  // Best-effort removal of artifacts after start() failed.
  void cleanup_after_failed_start() noexcept;

  // Timing sidecar. append_timing_row throws on I/O failure.
  void append_timing_row(std::uint64_t ordinal, ChannelId channel, bool publish_time_fallback,
                         const std::optional<SourceClock>& source_clock);
  Status close_timing() noexcept;
  TimingFacts timing_facts(bool data_finalized) const;
  Status finalize_timing(bool data_finalized);

  Status finalize_data();

  // Writes the manifest through a temporary file, then syncs and renames it.
  PublishResult publish_manifest(const ManifestInput& input) noexcept;

private:
  std::string config_json() const;
  std::string manifest_json(const ManifestInput& input) const;
  std::string relative_name(const std::filesystem::path& path) const;
  Status sync_file_checked(const std::filesystem::path& path) noexcept;
  Status sync_directory_checked(const std::filesystem::path& path) noexcept;

  std::shared_ptr<FileSystem> file_system_;
  std::filesystem::path directory_;
  std::filesystem::path parent_directory_;
  std::filesystem::path data_partial_;
  std::filesystem::path data_final_;
  std::filesystem::path timing_partial_;
  std::filesystem::path timing_final_;
  std::filesystem::path config_;
  std::filesystem::path manifest_;
  std::filesystem::path manifest_temp_;
  std::string session_id_;
  FileOptions file_options_;
  std::uint64_t queue_byte_limit_;
  DurabilityMode durability_mode_;

  std::unique_ptr<OutputSink> timing_sink_;
  bool timing_created_ = false;
  bool timing_renamed_ = false;
  std::uint64_t timing_rows_ = 0;
  bool created_directory_ = false;
  bool data_renamed_ = false;
  Status initial_sync_error_;
};

}  // namespace robot_mcap::detail
