#pragma once

#include "robot_mcap/io.hpp"
#include "robot_mcap/types.hpp"
#include "session_format/domain/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace robot_mcap::detail {

// Owns one session directory and the publication order of its files. Text
// rendering lives in documents.cpp; this class only creates, syncs and renames
// artifacts, so the format can be tested without a recorder and the recorder
// can be tested without inspecting JSON.
class SessionArtifacts {
public:
  SessionArtifacts(std::shared_ptr<FileSystem> file_system,
                   std::filesystem::path session_directory, std::string session_id,
                   FileOptions file_options, std::uint64_t queue_byte_limit,
                   DurabilityMode durability_mode, std::string writer_version);

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
  std::string writer_version_;

  std::unique_ptr<OutputSink> timing_sink_;
  bool timing_created_ = false;
  bool timing_renamed_ = false;
  std::uint64_t timing_rows_ = 0;
  bool created_directory_ = false;
  bool data_renamed_ = false;
  Status initial_sync_error_;
};

}  // namespace robot_mcap::detail
