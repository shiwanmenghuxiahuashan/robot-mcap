#pragma once

#include <string_view>

namespace robot_mcap::detail {

// Validates UTF-8 without overlong encodings, surrogates, or values above U+10FFFF.
bool valid_utf8(std::string_view value);

}  // namespace robot_mcap::detail
