#pragma once

#include "robot_mcap/recorder.hpp"
#include "session_format/infrastructure/session_artifacts.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace robot_mcap {

struct Recorder::Impl {
  enum class State { Created, Running, Stopping, Stopped };

  struct Item {
    ChannelId channel = 0;
    std::uint64_t log_time_ns = 0;
    std::optional<std::uint64_t> publish_time_ns;
    std::optional<SourceClock> source_clock;
    std::uint32_t sequence = 0;
    PublishTimePolicy policy = PublishTimePolicy::LogTimeOnly;
    std::vector<std::byte> data;
  };

  explicit Impl(RecorderOptions recorder_options) : options(std::move(recorder_options)) {}
  ~Impl() {
    if (thread.joinable()) {
      try {
        stop_async();
      } catch (...) {
      }
      try {
        thread.join();
      } catch (...) {
      }
    }
  }

  Status start();
  Status register_channel(const ChannelSpec& spec, ChannelId& id);
  SubmitResult submit_copy(ChannelId channel, const MessageView& message);
  SubmitResult submit_owned(ChannelId channel, OwnedMessage&& message);
  std::shared_future<StopResult> stop_async();
  StopResult stop();

  void run() noexcept;
  void process(Item item);
  StopResult finalize() noexcept;
  SubmitResult check_submission(ChannelId channel, std::uint64_t size, bool payload_pointer_ok,
                                const std::optional<std::uint64_t>& publish_time_ns,
                                const std::optional<SourceClock>& source_clock,
                                PublishTimePolicy& policy);
  void record_failure(bool writer_io, const Status& status, const Item* item,
                      const char* action = "retain_partial_and_reject_new_messages");
  void record_session_error(const Status& status, const char* code,
                            const char* action);
  void record_publish_failure(const Status& status, const char* code,
                              const char* action = "retain_artifact_for_diagnosis");

  RecorderOptions options;
  std::shared_ptr<FileSystem> file_system;
  std::shared_ptr<detail::SessionArtifacts> artifacts;
  std::filesystem::path session_directory;

  std::mutex mutex;
  std::condition_variable condition;
  State state = State::Created;
  bool started = false;
  bool start_attempted = false;
  bool stop_requested = false;
  bool failed = false;
  bool writer_failed = false;
  bool timing_failed = false;
  bool counters_frozen = false;
  std::deque<Item> queue;
  std::uint64_t queue_bytes = 0;
  std::map<ChannelId, ChannelSpec> channels;
  std::vector<detail::Diagnostic> diagnostics;
  detail::Statistics statistics;
  std::optional<std::uint64_t> first_log_time_ns;
  std::optional<std::uint64_t> last_log_time_ns;
  Status start_error;
  Status primary_error;

  std::mutex writer_mutex;
  std::unique_ptr<McapFile> writer;
  std::thread thread;
  std::promise<StopResult> promise;
  std::shared_future<StopResult> future;
};

}  // namespace robot_mcap
