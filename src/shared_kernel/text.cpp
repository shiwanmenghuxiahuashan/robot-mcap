#include "shared_kernel/text.hpp"

#include <cstddef>

namespace robot_mcap::detail {

bool valid_utf8(std::string_view value) {
  for (std::size_t position = 0; position < value.size();) {
    const auto lead = static_cast<unsigned char>(value[position++]);
    if (lead < 0x80) {
      continue;
    }
    if (lead < 0xc2 || lead > 0xf4) {
      return false;
    }
    std::size_t continuation = 1;
    if (lead >= 0xf0) {
      continuation = 3;
    } else if (lead >= 0xe0) {
      continuation = 2;
    }
    if (continuation > value.size() - position) {
      return false;
    }
    // Exclude overlong encodings, surrogate code points, and values above U+10FFFF.
    const auto second = static_cast<unsigned char>(value[position]);
    if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
        (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90)) {
      return false;
    }
    for (std::size_t index = 0; index < continuation; ++index) {
      const auto byte = static_cast<unsigned char>(value[position++]);
      if (byte < 0x80 || byte > 0xbf) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace robot_mcap::detail
