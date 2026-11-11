#pragma once

#include "robot_mcap/types.hpp"

#include <memory>

namespace robot_mcap {

// Single-use writer. All calls must be serialized by the caller; the recorder uses a
// mutex so channel registration can run on a producer thread while the writer thread
// writes messages. Call finish() to close a complete MCAP. Session-level publication
// and timing evidence are the caller's responsibility.
class McapFile {
public:
  McapFile();
  ~McapFile();

  McapFile(const McapFile&) = delete;
  McapFile& operator=(const McapFile&) = delete;

  Status open(const std::filesystem::path& path, const FileOptions& options = {});
  Status register_channel(const ChannelSpec& spec, ChannelId& id);
  Status write(ChannelId channel, MessageView message);
  Status finish();

  // False after a fatal I/O failure or after finish().
  bool is_open() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace robot_mcap
