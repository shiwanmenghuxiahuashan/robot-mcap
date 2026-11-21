#pragma once

#include "robot_mcap/types.hpp"

#include <string_view>

namespace robot_mcap::detail {

// Pure rules shared by channel registration and message submission. Only values
// crossing the public API boundary are checked here; the recorder keeps every
// stateful check (lifecycle, queue budget, counters) on its own side, so these
// predicates stay testable without constructing a recorder.

bool valid_session_id(std::string_view value);
bool valid_channel_spec(const ChannelSpec& spec);
bool valid_source_clock(const SourceClock& clock);

// A timing sidecar row is written when the message carries a raw source clock,
// or when its channel allows mixed publish times and this message fell back to
// the log time. The physical MCAP message ordinal is assigned by the writer.
bool requires_timing_row(PublishTimePolicy policy, bool has_publish_time,
                         bool has_source_clock);

}  // namespace robot_mcap::detail
