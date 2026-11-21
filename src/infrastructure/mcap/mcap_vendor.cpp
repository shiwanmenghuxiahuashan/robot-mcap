#define MCAP_IMPLEMENTATION
#include <mcap/reader.hpp>
#include <mcap/writer.hpp>

#include "infrastructure/mcap/version.hpp"

namespace robot_mcap::detail {

std::string_view mcap_writer_version() { return mcap::LibraryVersion; }

}  // namespace robot_mcap::detail
