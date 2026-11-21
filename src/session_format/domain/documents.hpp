#pragma once

#include "robot_mcap/types.hpp"
#include "session_format/domain/manifest.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace robot_mcap::detail {

// Values captured once when the session directory is created. The upstream
// writer version is supplied by storage/mcap as data, so this module never
// includes the MCAP headers.
struct ConfigSnapshot {
  std::string session_id;
  std::uint64_t chunk_size_bytes = 0;
  std::uint64_t queue_byte_limit = 0;
  DurabilityMode durability_mode = DurabilityMode::Close;
  std::string writer_version;
};

// Pure renderers: text in, text out, no I/O and no session state. The caller
// owns temporary files, syncing and the publication rename order. Keeping them
// separate lets the v1 format be checked byte-for-byte against fixtures.
std::string render_config_json(const ConfigSnapshot& snapshot);
std::string render_manifest_json(const ManifestInput& input);
std::string render_timing_row(std::uint64_t ordinal, ChannelId channel,
                              bool publish_time_fallback,
                              const std::optional<SourceClock>& source_clock);

}  // namespace robot_mcap::detail
