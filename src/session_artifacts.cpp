#include "session_artifacts.hpp"

#include <mcap/types.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace robot_mcap::detail {
namespace {

std::string json_string(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (std::size_t index = 0; index < value.size();) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte < 0x80) {
      switch (byte) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\b':
          out += "\\b";
          break;
        case '\f':
          out += "\\f";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (byte < 0x20) {
            out += "\\u00";
            out.push_back(kHex[(byte >> 4) & 0x0fU]);
            out.push_back(kHex[byte & 0x0fU]);
          } else {
            out.push_back(static_cast<char>(byte));
          }
          break;
      }
      ++index;
      continue;
    }

    std::size_t length = 0;
    if (byte >= 0xc2U && byte <= 0xdfU) {
      length = 2;
    } else if (byte >= 0xe0U && byte <= 0xefU) {
      length = 3;
    } else if (byte >= 0xf0U && byte <= 0xf4U) {
      length = 4;
    }
    bool valid = length != 0 && index + length <= value.size();
    if (valid) {
      const auto second = static_cast<unsigned char>(value[index + 1]);
      if ((byte == 0xe0U && second < 0xa0U) || (byte == 0xedU && second >= 0xa0U) ||
          (byte == 0xf0U && second < 0x90U) || (byte == 0xf4U && second >= 0x90U)) {
        valid = false;
      }
      for (std::size_t offset = 1; valid && offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(value[index + offset]);
        if (continuation < 0x80U || continuation > 0xbfU) {
          valid = false;
        }
      }
    }
    if (valid) {
      out.append(value.data() + index, length);
      index += length;
    } else {
      out += "\xef\xbf\xbd";  // U+FFFD replacement character.
      ++index;
    }
  }
  out.push_back('"');
  return out;
}

std::string optional_count(const std::optional<std::uint64_t>& value) {
  return value ? std::to_string(*value) : "null";
}

std::string optional_time(const std::optional<std::uint64_t>& value) {
  return value ? json_string(std::to_string(*value)) : "null";
}

std::string policy_string(PublishTimePolicy policy) {
  switch (policy) {
    case PublishTimePolicy::SourceRequired:
      return "source_required";
    case PublishTimePolicy::LogTimeOnly:
      return "log_time_only";
    case PublishTimePolicy::PerMessage:
      return "per_message";
  }
  return "log_time_only";
}

std::string duration_mode_string(DurabilityMode mode) {
  return mode == DurabilityMode::Sync ? "sync" : "close";
}

}  // namespace

SessionArtifacts::SessionArtifacts(std::shared_ptr<FileSystem> file_system,
                                   std::filesystem::path session_directory,
                                   std::string session_id, FileOptions file_options,
                                   std::uint64_t queue_byte_limit,
                                   DurabilityMode durability_mode)
    : file_system_(std::move(file_system)),
      directory_(std::move(session_directory)),
      parent_directory_(directory_.parent_path()),
      data_partial_(directory_ / "data_0000.mcap.partial"),
      data_final_(directory_ / "data_0000.mcap"),
      timing_partial_(directory_ / "data_0000.timing.ndjson.partial"),
      timing_final_(directory_ / "data_0000.timing.ndjson"),
      config_(directory_ / "recording-config.json"),
      manifest_(directory_ / "manifest.json"),
      manifest_temp_(directory_ / "manifest.json.tmp"),
      session_id_(std::move(session_id)),
      file_options_(std::move(file_options)),
      queue_byte_limit_(queue_byte_limit),
      durability_mode_(durability_mode) {}

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

  const auto config_temp = directory_ / "recording-config.json.tmp";
  write_file(*file_system_, config_temp, config_json());
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
  file_system_->remove(directory_ / "recording-config.json.tmp");
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
  std::string line = "{\"message_ordinal\":" + std::to_string(ordinal) +
                     ",\"channel_id\":" + std::to_string(channel) +
                     ",\"publish_time_fallback\":" +
                     (publish_time_fallback ? "true" : "false") + ",\"source_clock\":";
  if (source_clock) {
    line += "{\"clock_id\":" + json_string(source_clock->clock_id) +
            ",\"value\":" + json_string(std::to_string(source_clock->value)) +
            ",\"unit\":" + json_string(source_clock->unit) +
            ",\"meaning\":" + json_string(source_clock->meaning) +
            ",\"epoch\":" + json_string(source_clock->epoch) + "}";
  } else {
    line += "null";
  }
  line += "}\n";
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
    write_file(*file_system_, manifest_temp_, manifest_json(input));
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

std::string SessionArtifacts::relative_name(const std::filesystem::path& path) const {
  return path.filename().generic_string();
}

std::string SessionArtifacts::config_json() const {
  std::string out;
  out += "{\n";
  out += "  \"schema_version\": 1,\n";
  out += "  \"session_id\": " + json_string(session_id_) + ",\n";
  out += "  \"writer\": {\n";
  out += "    \"version\": " + json_string(mcap::LibraryVersion) + ",\n";
  out += "    \"profile\": \"\",\n";
  out += "    \"chunk_size_bytes\": " + std::to_string(file_options_.chunk_size_bytes) + ",\n";
  out += "    \"compression\": \"none\",\n";
  out += "    \"chunk_crc\": true,\n";
  out += "    \"chunking\": true,\n";
  out += "    \"message_index\": true,\n";
  out += "    \"chunk_index\": true,\n";
  out += "    \"summary\": true,\n";
  out += "    \"rotate\": false\n";
  out += "  },\n";
  out += "  \"durability_mode\": " + json_string(duration_mode_string(durability_mode_)) + ",\n";
  out += "  \"queue_byte_limit\": " + std::to_string(queue_byte_limit_) + ",\n";
  out += "  \"adapter\": null\n";
  out += "}\n";
  return out;
}

std::string SessionArtifacts::manifest_json(const ManifestInput& input) const {
  std::string out;
  out += "{\n";
  out += "  \"schema_version\": 1,\n";
  out += "  \"session_id\": " + json_string(input.session_id) + ",\n";
  out += "  \"state\": " + json_string(input.session_finalized ? "finalized" : "failed") + ",\n";
  out += "  \"files\": [\n";
  const std::string file_state =
      input.data.finalized ? "finalized" : (input.data.present ? "partial" : "unknown");
  out += "    {\n";
  out += "      \"path\": " + json_string(relative_name(input.data.path)) + ",\n";
  out += "      \"state\": " + json_string(file_state) + ",\n";
  out += "      \"bytes\": " + optional_count(input.data.bytes) + ",\n";
  out += "      \"message_count\": " + optional_count(input.data.message_count) + ",\n";
  out += "      \"first_log_time_ns\": " + optional_time(input.data.first_log_time_ns) + ",\n";
  out += "      \"last_log_time_ns\": " + optional_time(input.data.last_log_time_ns) + ",\n";
  if (input.timing.present) {
    const std::string timing_state =
        !input.timing.sha256 ? "unknown"
                             : (input.timing.finalized ? "finalized" : "partial");
    out += "      \"timing\": {\n";
    out += "        \"path\": " + json_string(relative_name(input.timing.path)) + ",\n";
    out += "        \"row_count\": " + std::to_string(input.timing.row_count) + ",\n";
    out += "        \"sha256\": " +
           (input.timing.sha256 ? json_string(*input.timing.sha256) : std::string("null")) +
           ",\n";
    out += "        \"state\": " + json_string(timing_state) + "\n";
    out += "      }\n";
  } else {
    out += "      \"timing\": null\n";
  }
  out += "    }\n";
  out += "  ],\n";

  out += "  \"channels\": [";
  if (input.channels.empty()) {
    out += "],\n";
  } else {
    out += "\n";
    for (std::size_t index = 0; index < input.channels.size(); ++index) {
      const auto& channel = input.channels[index];
      out += "    {\n";
      out += "      \"file_path\": " + json_string(relative_name(input.data.path)) + ",\n";
      out += "      \"channel_id\": " + std::to_string(channel.first) + ",\n";
      out += "      \"topic\": " + json_string(channel.second.topic) + ",\n";
      out += "      \"message_encoding\": " + json_string(channel.second.message_encoding) + ",\n";
      if (channel.second.schema) {
        out += "      \"schema\": {\n";
        out += "        \"name\": " + json_string(channel.second.schema->name) + ",\n";
        out += "        \"encoding\": " + json_string(channel.second.schema->encoding) + ",\n";
        out += "        \"sha256\": " + json_string(sha256_hex(channel.second.schema->data)) +
               "\n";
        out += "      },\n";
      } else {
        out += "      \"schema\": null,\n";
      }
      out += "      \"publish_time_policy\": " +
             json_string(policy_string(channel.second.publish_time_policy)) + "\n";
      out += (index + 1 == input.channels.size()) ? "    }\n" : "    },\n";
    }
    out += "  ],\n";
  }

  out += "  \"statistics\": {\n";
  out += "    \"accepted\": " + std::to_string(input.statistics.accepted) + ",\n";
  out += "    \"rejected\": " + std::to_string(input.statistics.rejected) + ",\n";
  out += "    \"writer_processed\": " + std::to_string(input.statistics.writer_processed) + ",\n";
  out += "    \"finalized_messages\": " + optional_count(input.statistics.finalized_messages) +
         "\n";
  out += "  },\n";
  out += "  \"durability\": {\n";
  out += "    \"requested_mode\": " + json_string(duration_mode_string(input.durability_mode)) +
         ",\n";
  out += "    \"confirmation\": " +
         json_string(input.durability_mode == DurabilityMode::Sync ? "see_stop_result"
                                                                   : "not_requested") +
         "\n";
  out += "  },\n";
  out += "  \"ros2_replay\": {\n";
  out += "    \"state\": \"not_requested\",\n";
  out += "    \"metadata_path\": null\n";
  out += "  },\n";
  out += "  \"config_path\": " + json_string(relative_name(config_)) + ",\n";

  out += "  \"diagnostics\": [";
  if (input.diagnostics.empty()) {
    out += "]\n";
  } else {
    out += "\n";
    for (std::size_t index = 0; index < input.diagnostics.size(); ++index) {
      const auto& diagnostic = input.diagnostics[index];
      out += "    {\n";
      out += "      \"code\": " + json_string(diagnostic.code) + ",\n";
      out += "      \"severity\": " + json_string(diagnostic.severity) + ",\n";
      out += "      \"topic\": " +
             (diagnostic.topic ? json_string(*diagnostic.topic) : std::string("null")) + ",\n";
      out += "      \"log_time_ns\": " + optional_time(diagnostic.log_time_ns) + ",\n";
      out += "      \"detail\": " + json_string(diagnostic.detail) + ",\n";
      out += "      \"action\": " + json_string(diagnostic.action) + "\n";
      out += "    }";
      out += (index + 1 == input.diagnostics.size()) ? "\n" : ",\n";
    }
    out += "  ]\n";
  }
  out += "}\n";
  return out;
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
