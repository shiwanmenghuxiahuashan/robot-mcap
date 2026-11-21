#pragma once

#include <string_view>

namespace robot_mcap::detail {

// The pinned upstream MCAP library version recorded in the configuration
// snapshot. Storage owns the value; session_format only formats it, so the
// format code does not need to include the MCAP headers.
std::string_view mcap_writer_version();

}  // namespace robot_mcap::detail
