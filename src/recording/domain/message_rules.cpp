#include "recording/domain/message_rules.hpp"

#include "shared_kernel/text.hpp"

#include <limits>

namespace robot_mcap::detail {
namespace {

bool ascii_alphanumeric(char value) {
  return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z');
}

}  // namespace

bool valid_session_id(std::string_view value) {
  if (value.empty() || value.size() > 64 || !ascii_alphanumeric(value.front())) {
    return false;
  }
  for (const char character : value) {
    if (!ascii_alphanumeric(character) && character != '_' && character != '-') {
      return false;
    }
  }
  return true;
}

bool valid_channel_spec(const ChannelSpec& spec) {
  constexpr auto max_field_size = std::numeric_limits<std::uint32_t>::max();
  if (spec.topic.empty() || spec.topic.size() > max_field_size ||
      spec.message_encoding.empty() || spec.message_encoding.size() > max_field_size ||
      !valid_utf8(spec.topic) || !valid_utf8(spec.message_encoding)) {
    return false;
  }
  if (spec.schema) {
    if (spec.schema->name.empty() || spec.schema->name.size() > max_field_size ||
        spec.schema->encoding.empty() || spec.schema->encoding.size() > max_field_size ||
        spec.schema->data.size() > max_field_size ||
        !valid_utf8(spec.schema->name) || !valid_utf8(spec.schema->encoding)) {
      return false;
    }
  }
  return true;
}

bool valid_source_clock(const SourceClock& clock) {
  return !clock.clock_id.empty() && !clock.meaning.empty() && !clock.epoch.empty() &&
         valid_utf8(clock.clock_id) && valid_utf8(clock.meaning) &&
         valid_utf8(clock.epoch) &&
         (clock.unit == "ns" || clock.unit == "us" || clock.unit == "ms" ||
          clock.unit == "tick");
}

bool requires_timing_row(PublishTimePolicy policy, bool has_publish_time,
                         bool has_source_clock) {
  return has_source_clock ||
         (policy == PublishTimePolicy::PerMessage && !has_publish_time);
}

}  // namespace robot_mcap::detail
