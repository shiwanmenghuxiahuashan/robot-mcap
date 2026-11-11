#pragma once

#include "robot_mcap/io.hpp"
#include "robot_mcap/mcap_file.hpp"
#include "robot_mcap/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace robot_mcap {

// A message whose payload buffer is handed to the recorder.
struct OwnedMessage {
  std::uint64_t log_time_ns = 0;
  std::optional<std::uint64_t> publish_time_ns;
  std::uint32_t sequence = 0;
  std::optional<SourceClock> source_clock;
  std::vector<std::byte> data;
};

enum class DurabilityMode { Close, Sync };

struct RecorderOptions {
  // The session directory is created as parent_directory / session_id.
  std::filesystem::path parent_directory;
  std::string session_id;
  // Maximum payload bytes waiting in the queue. A message already being written is no
  // longer counted, so resident memory is bounded by this limit plus one message.
  // Submissions that would exceed it fail immediately with SubmitError::QueueFull.
  std::uint64_t queue_byte_limit = 64ULL * 1024 * 1024;
  FileOptions file_options;
  DurabilityMode durability = DurabilityMode::Close;
  // I/O seam for every session artifact, including the MCAP data file. This overrides
  // file_options.file_system. A null value uses the native file system.
  std::shared_ptr<FileSystem> file_system;
};

// QueueFull, Stopped and WriterFailed count toward the manifest's rejected total.
// InvalidArgument is a caller error and is not counted as a rejected source message.
// QueueFull also covers a single message larger than the fixed session budget; such a
// message can never be accepted by this recorder.
enum class SubmitError {
  None,
  QueueFull,
  Stopped,
  WriterFailed,
  InvalidArgument,
};

struct SubmitResult {
  SubmitError error = SubmitError::None;
  Status status;

  bool ok() const noexcept { return error == SubmitError::None; }
};

enum class FileState { Unknown, Partial, Finalized };
enum class DurabilityState { NotRequested, Confirmed, Failed };
enum class StopReason {
  Requested,
  WriterFailure,
  TimingFailure,
  PublishFailure,
  StartFailure,
  NotStarted,
};

struct StopResult {
  FileState file_state = FileState::Unknown;
  DurabilityState durability_state = DurabilityState::NotRequested;
  StopReason reason = StopReason::Requested;
  // The first failure that prevented a clean stop, if any. The published manifest may
  // contain additional diagnostics.
  Status error;

  bool ok() const noexcept { return error.ok(); }
};

// One recorder owns one session, one queue and one writer thread.
// start(), register_channel(), submit_*() and stop_*() may be called from different threads.
class Recorder {
public:
  explicit Recorder(RecorderOptions options);
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  Status start();
  Status register_channel(const ChannelSpec& spec, ChannelId& id);
  SubmitResult submit_copy(ChannelId channel, MessageView message);
  // Ownership transfers only when the result is accepted. On rejection the caller's
  // buffer is untouched and may be retried.
  SubmitResult submit_owned(ChannelId channel, OwnedMessage&& message);

  // stop_async() synchronously stops accepting new messages and returns the shared
  // result of draining, finishing and publishing. stop() waits for the same result.
  // A writer failure finalizes autonomously; this future then resolves without a stop
  // call. stop()/stop_async() before start return a ready error result and do not
  // consume the recorder. Destruction performs a stop and joins the writer thread, so
  // it may block for the duration of the longest write, payload copy, or sync.
  std::shared_future<StopResult> stop_async();
  StopResult stop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace robot_mcap
