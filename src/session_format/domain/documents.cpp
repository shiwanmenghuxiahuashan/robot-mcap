#include "session_format/domain/documents.hpp"

#include "shared_kernel/sha256.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace robot_mcap::detail {
namespace {

// Serializes one UTF-8 string as a JSON string. Invalid UTF-8 is replaced with
// U+FFFD instead of being emitted raw, so every manifest stays valid JSON even
// when a source supplies broken text.
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

std::string file_name(const std::filesystem::path& path) {
  return path.filename().generic_string();
}

}  // namespace

std::string render_config_json(const ConfigSnapshot& snapshot) {
  std::string out;
  out += "{\n";
  out += "  \"schema_version\": 1,\n";
  out += "  \"session_id\": " + json_string(snapshot.session_id) + ",\n";
  out += "  \"writer\": {\n";
  out += "    \"version\": " + json_string(snapshot.writer_version) + ",\n";
  out += "    \"profile\": \"\",\n";
  out += "    \"chunk_size_bytes\": " + std::to_string(snapshot.chunk_size_bytes) + ",\n";
  out += "    \"compression\": \"none\",\n";
  out += "    \"chunk_crc\": true,\n";
  out += "    \"chunking\": true,\n";
  out += "    \"message_index\": true,\n";
  out += "    \"chunk_index\": true,\n";
  out += "    \"summary\": true,\n";
  out += "    \"rotate\": false\n";
  out += "  },\n";
  out += "  \"durability_mode\": " +
         json_string(duration_mode_string(snapshot.durability_mode)) + ",\n";
  out += "  \"queue_byte_limit\": " + std::to_string(snapshot.queue_byte_limit) + ",\n";
  out += "  \"adapter\": null\n";
  out += "}\n";
  return out;
}

std::string render_manifest_json(const ManifestInput& input) {
  std::string out;
  out += "{\n";
  out += "  \"schema_version\": 1,\n";
  out += "  \"session_id\": " + json_string(input.session_id) + ",\n";
  out += "  \"state\": " + json_string(input.session_finalized ? "finalized" : "failed") + ",\n";
  out += "  \"files\": [\n";
  const std::string file_state =
      input.data.finalized ? "finalized" : (input.data.present ? "partial" : "unknown");
  out += "    {\n";
  out += "      \"path\": " + json_string(file_name(input.data.path)) + ",\n";
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
    out += "        \"path\": " + json_string(file_name(input.timing.path)) + ",\n";
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
      out += "      \"file_path\": " + json_string(file_name(input.data.path)) + ",\n";
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
  out += "  \"config_path\": " + json_string(kConfigFileName) + ",\n";

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

std::string render_timing_row(std::uint64_t ordinal, ChannelId channel,
                              bool publish_time_fallback,
                              const std::optional<SourceClock>& source_clock) {
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
  return line;
}

}  // namespace robot_mcap::detail
