#include "session_format/infrastructure/session_artifacts.hpp"

#include "session_format/domain/documents.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace robot_mcap::detail {

SessionArtifacts::SessionArtifacts(std::shared_ptr<FileSystem> file_system,
                                   std::filesystem::path session_directory,
                                   std::string session_id, FileOptions file_options,
                                   std::uint64_t queue_byte_limit,
                                   DurabilityMode durability_mode,
                                   std::string writer_version)
    : file_system_(std::move(file_system)),
      directory_(std::move(session_directory)),
      parent_directory_(directory_.parent_path()),
      data_partial_(directory_ / "data_0000.mcap.partial"),
      data_final_(directory_ / "data_0000.mcap"),
      timing_partial_(directory_ / "data_0000.timing.ndjson.partial"),
      timing_final_(directory_ / "data_0000.timing.ndjson"),
      config_(directory_ / kConfigFileName),
      manifest_(directory_ / "manifest.json"),
      manifest_temp_(directory_ / "manifest.json.tmp"),
      session_id_(std::move(session_id)),
      file_options_(std::move(file_options)),
      queue_byte_limit_(queue_byte_limit),
      durability_mode_(durability_mode),
      writer_version_(std::move(writer_version)) {}

void SessionArtifacts::create() {
  file_system_->create_directories(parent_directory_);
  if (!file_system_->create_directory_exclusive(directory_)) {
    throw std::runtime_error("session directory already exists: " +
                             directory_.generic_string());
  }
  created_directory_ = true;

  if (durability_mode_ == DurabilityMode::Sync) {
    // The contract asks for the parent to be synced when a directory is newly created.
    try {
      file_system_->sync_directory(parent_directory_);
    } catch (const std::exception& error) {
      initial_sync_error_ = {"sync_directory", parent_directory_, error.what()};
    } catch (...) {
      initial_sync_error_ = {"sync_directory", parent_directory_,
                             "unknown parent sync error"};
    }
  }

  const auto config_temp = directory_ / (std::string(kConfigFileName) + ".tmp");
  ConfigSnapshot snapshot;
  snapshot.session_id = session_id_;
  snapshot.chunk_size_bytes = file_options_.chunk_size_bytes;
  snapshot.queue_byte_limit = queue_byte_limit_;
  snapshot.durability_mode = durability_mode_;
  snapshot.writer_version = writer_version_;
  write_file(*file_system_, config_temp, render_config_json(snapshot));
  try {
    file_system_->rename(config_temp, config_);
  } catch (...) {
    file_system_->remove(config_temp);
    throw;
  }
}

void SessionArtifacts::cleanup_after_failed_start() noexcept {
  if (!created_directory_) {
    return;
  }
  file_system_->remove(manifest_temp_);
  file_system_->remove(config_);
  file_system_->remove(directory_ / (std::string(kConfigFileName) + ".tmp"));
  file_system_->remove(data_partial_);
  file_system_->remove(timing_partial_);
  file_system_->remove(directory_);
}

void SessionArtifacts::append_timing_row(std::uint64_t ordinal, ChannelId channel,
                                         bool publish_time_fallback,
                                         const std::optional<SourceClock>& source_clock) {
  if (!timing_sink_) {
    timing_sink_ = file_system_->create_exclusive(timing_partial_);
    timing_created_ = true;
  }
  // The ordinal is assigned by the writer thread, so the row order matches the
  // physical MCAP message order rather than the caller's submission order.
  const std::string line =
      render_timing_row(ordinal, channel, publish_time_fallback, source_clock);
  timing_sink_->write(reinterpret_cast<const std::byte*>(line.data()), line.size());
  ++timing_rows_;
}

Status SessionArtifacts::close_timing() noexcept {
  if (!timing_sink_) {
    return {};
  }
  try {
    timing_sink_->close();
    timing_sink_.reset();
    return {};
  } catch (const std::exception& error) {
    timing_sink_.reset();
    return {"close_timing", timing_partial_, error.what()};
  } catch (...) {
    timing_sink_.reset();
    return {"close_timing", timing_partial_, "unknown timing close error"};
  }
}

TimingFacts SessionArtifacts::timing_facts(bool data_finalized) const {
  TimingFacts facts;
  if (!timing_created_ || timing_rows_ == 0) {
    return facts;
  }
  facts.present = true;
  facts.path = timing_renamed_ ? timing_final_ : timing_partial_;
  facts.row_count = timing_rows_;
  facts.finalized = timing_renamed_ && data_finalized;
  try {
    facts.sha256 = file_system_->file_sha256(facts.path);
  } catch (...) {
    facts.sha256_ok = false;
  }
  return facts;
}

Status SessionArtifacts::finalize_timing(bool data_finalized) {
  if (!timing_created_) {
    return {};
  }
  if (timing_rows_ == 0) {
    file_system_->remove(timing_partial_);
    timing_created_ = false;
    return {};
  }
  if (!data_finalized) {
    return {};
  }
  try {
    file_system_->rename(timing_partial_, timing_final_);
    timing_renamed_ = true;
    return {};
  } catch (const std::exception& error) {
    return {"rename", timing_partial_, error.what()};
  } catch (...) {
    return {"rename", timing_partial_, "unknown timing rename error"};
  }
}

Status SessionArtifacts::finalize_data() {
  try {
    file_system_->rename(data_partial_, data_final_);
    data_renamed_ = true;
    return {};
  } catch (const std::exception& error) {
    return {"rename", data_partial_, error.what()};
  } catch (...) {
    return {"rename", data_partial_, "unknown data rename error"};
  }
}

PublishResult SessionArtifacts::publish_manifest(const ManifestInput& input) noexcept {
  PublishResult result;
  try {
    write_file(*file_system_, manifest_temp_, render_manifest_json(input));
  } catch (const std::exception& error) {
    result.error = {"write_manifest", manifest_temp_, error.what()};
    return result;
  } catch (...) {
    result.error = {"write_manifest", manifest_temp_, "unknown manifest write error"};
    return result;
  }

  // A failure to sync the parent at session creation still fails the requested
  // durability, but it does not prevent publishing the manifest.
  result.error = initial_sync_error_;

  const auto first_error = [&result](Status status) {
    if (result.error.ok() && !status.ok()) {
      result.error = std::move(status);
    }
  };

  if (input.durability_mode == DurabilityMode::Sync) {
    first_error(sync_file_checked(input.data.path));
    if (input.timing.present) {
      first_error(sync_file_checked(input.timing.path));
    }
    first_error(sync_file_checked(config_));
    first_error(sync_file_checked(manifest_temp_));
  }

  try {
    file_system_->rename(manifest_temp_, manifest_);
    result.manifest_published = true;
  } catch (const std::exception& error) {
    // Publication failure is more important than an earlier durability error.
    result.error = {"rename_manifest", manifest_temp_, error.what()};
    return result;
  } catch (...) {
    result.error = {"rename_manifest", manifest_temp_, "unknown manifest rename error"};
    return result;
  }

  if (input.durability_mode == DurabilityMode::Sync) {
    first_error(sync_directory_checked(directory_));
    first_error(sync_directory_checked(parent_directory_));
  }
  return result;
}

Status SessionArtifacts::sync_file_checked(const std::filesystem::path& path) noexcept {
  try {
    file_system_->sync_file(path);
    return {};
  } catch (const std::exception& error) {
    return {"sync_file", path, error.what()};
  } catch (...) {
    return {"sync_file", path, "unknown sync error"};
  }
}

Status SessionArtifacts::sync_directory_checked(const std::filesystem::path& path) noexcept {
  try {
    file_system_->sync_directory(path);
    return {};
  } catch (const std::exception& error) {
    return {"sync_directory", path, error.what()};
  } catch (...) {
    return {"sync_directory", path, "unknown sync error"};
  }
}

}  // namespace robot_mcap::detail
