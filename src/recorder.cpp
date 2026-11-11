#include "robot_mcap/recorder.hpp"

#include "session_artifacts.hpp"
#include "text.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace robot_mcap {
namespace {

bool ascii_alphanumeric(char value) {
  return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z');
}

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
      !detail::valid_utf8(spec.topic) || !detail::valid_utf8(spec.message_encoding)) {
    return false;
  }
  if (spec.schema) {
    if (spec.schema->name.empty() || spec.schema->name.size() > max_field_size ||
        spec.schema->encoding.empty() || spec.schema->encoding.size() > max_field_size ||
        spec.schema->data.size() > max_field_size ||
        !detail::valid_utf8(spec.schema->name) ||
        !detail::valid_utf8(spec.schema->encoding)) {
      return false;
    }
  }
  return true;
}

bool valid_source_clock(const SourceClock& clock) {
  return !clock.clock_id.empty() && !clock.meaning.empty() && !clock.epoch.empty() &&
         detail::valid_utf8(clock.clock_id) && detail::valid_utf8(clock.meaning) &&
         detail::valid_utf8(clock.epoch) &&
         (clock.unit == "ns" || clock.unit == "us" || clock.unit == "ms" ||
          clock.unit == "tick");
}

std::shared_future<StopResult> ready_future(StopResult result) {
  std::promise<StopResult> promise;
  auto future = promise.get_future().share();
  promise.set_value(std::move(result));
  return future;
}

}  // namespace

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

Status Recorder::Impl::start() {
  std::lock_guard lock(mutex);
  if (start_attempted) {
    return {"start", session_directory, "start has already been attempted"};
  }
  start_attempted = true;

  if (!valid_session_id(options.session_id)) {
    start_error = {"start", {}, "session_id must be 1-64 ASCII letters, digits, '_' or '-' and start with a letter or digit"};
    return start_error;
  }
  if (options.parent_directory.empty()) {
    start_error = {"start", {}, "parent directory is required"};
    return start_error;
  }
  if (options.parent_directory.native().find(std::filesystem::path::value_type{}) !=
      std::filesystem::path::string_type::npos) {
    start_error = {"start", options.parent_directory, "parent directory contains an embedded NUL"};
    return start_error;
  }
  if (options.queue_byte_limit == 0) {
    start_error = {"start", {}, "queue byte limit must be positive"};
    return start_error;
  }
  if (options.file_options.chunk_size_bytes == 0) {
    start_error = {"start", {}, "chunk size must be positive"};
    return start_error;
  }

  try {
    file_system = options.file_system ? options.file_system : native_file_system();
    session_directory =
        std::filesystem::absolute(options.parent_directory) / options.session_id;
    artifacts = std::make_shared<detail::SessionArtifacts>(
        file_system, session_directory, options.session_id, options.file_options,
        options.queue_byte_limit, options.durability);
    artifacts->create();

    FileOptions file_options = options.file_options;
    file_options.file_system = file_system;
    writer = std::make_unique<McapFile>();
    const auto opened = writer->open(artifacts->data_partial_path(), file_options);
    if (!opened.ok()) {
      throw std::runtime_error(opened.operation + ": " + opened.detail);
    }

    promise = std::promise<StopResult>();
    future = promise.get_future().share();
    state = State::Running;
    started = true;
    thread = std::thread([this] { run(); });
    return {};
  } catch (const std::exception& error) {
    start_error = {"start", session_directory, error.what()};
    state = State::Stopped;
    started = false;
    writer.reset();
    if (artifacts) {
      artifacts->cleanup_after_failed_start();
    }
    artifacts.reset();
    return start_error;
  }
}

// Lock order: writer_mutex before mutex. The writer thread and finalize take only
// writer_mutex for McapFile calls and only mutex for state; no path acquires them in
// the opposite order.
Status Recorder::Impl::register_channel(const ChannelSpec& spec, ChannelId& id) {
  id = 0;
  std::lock_guard writer_lock(writer_mutex);
  std::lock_guard state_lock(mutex);
  if (state != State::Running) {
    return {"register_channel", session_directory, "recorder is not accepting channels"};
  }
  if (!valid_channel_spec(spec)) {
    return {"register_channel", session_directory, "invalid topic, encoding or schema"};
  }
  const Status status = writer->register_channel(spec, id);
  if (!status.ok()) {
    id = 0;
    if (!writer->is_open()) {
      writer_failed = true;
      failed = true;
      stop_requested = true;
      state = State::Stopping;
      if (primary_error.ok()) {
        primary_error = status;
      }
      detail::Diagnostic diagnostic;
      diagnostic.code = "writer_io_error";
      diagnostic.topic = spec.topic;
      diagnostic.detail = status.detail;
      diagnostic.action = "retain_partial_and_reject_new_messages";
      diagnostics.push_back(std::move(diagnostic));
      condition.notify_all();
    } else {
      // A description conflict or another non-fatal registration error only rejects
      // this channel; the session remains usable.
      detail::Diagnostic diagnostic;
      diagnostic.code = "channel_description_conflict";
      diagnostic.severity = "error";
      diagnostic.topic = spec.topic;
      diagnostic.detail = status.detail;
      diagnostic.action = "reject_channel_registration";
      diagnostics.push_back(std::move(diagnostic));
    }
    return status;
  }
  channels.emplace(id, spec);
  return {};
}

SubmitResult Recorder::Impl::check_submission(
    ChannelId channel, std::uint64_t size, bool payload_pointer_ok,
    const std::optional<std::uint64_t>& publish_time_ns,
    const std::optional<SourceClock>& source_clock, PublishTimePolicy& policy) {
  if (state != State::Running) {
    if (!started) {
      return {SubmitError::Stopped, {"submit", session_directory, "recorder is not started"}};
    }
    if (!counters_frozen) {
      ++statistics.rejected;
    }
    if (writer_failed) {
      return {SubmitError::WriterFailed,
              {"submit", session_directory, "writer failed; recorder rejects new messages"}};
    }
    return {SubmitError::Stopped, {"submit", session_directory, "recorder is stopped"}};
  }
  const auto existing = channels.find(channel);
  if (existing == channels.end()) {
    return {SubmitError::InvalidArgument,
            {"submit", session_directory, "channel is not registered"}};
  }
  if (!payload_pointer_ok) {
    return {SubmitError::InvalidArgument,
            {"submit", session_directory, "payload pointer is required for nonempty messages"}};
  }
  policy = existing->second.publish_time_policy;
  if ((policy == PublishTimePolicy::SourceRequired && !publish_time_ns) ||
      (policy == PublishTimePolicy::LogTimeOnly && publish_time_ns)) {
    return {SubmitError::InvalidArgument,
            {"submit", session_directory, "publish time does not match the channel policy"}};
  }
  if (source_clock && !valid_source_clock(*source_clock)) {
    return {SubmitError::InvalidArgument,
            {"submit", session_directory, "invalid source clock description"}};
  }
  if (size > options.queue_byte_limit || queue_bytes > options.queue_byte_limit - size) {
    ++statistics.rejected;
    return {SubmitError::QueueFull, {"submit", session_directory, "queue byte limit exceeded"}};
  }
  return {};
}

SubmitResult Recorder::Impl::submit_copy(ChannelId channel, const MessageView& message) {
  std::lock_guard lock(mutex);
  PublishTimePolicy policy = PublishTimePolicy::LogTimeOnly;
  const bool payload_pointer_ok = message.data != nullptr || message.size == 0;
  auto result = check_submission(channel, message.size, payload_pointer_ok,
                                 message.publish_time_ns, message.source_clock, policy);
  if (!result.ok()) {
    return result;
  }

  Item item;
  item.channel = channel;
  item.log_time_ns = message.log_time_ns;
  item.publish_time_ns = message.publish_time_ns;
  item.sequence = message.sequence;
  item.source_clock = message.source_clock;
  item.policy = policy;
  if (message.size != 0) {
    item.data.assign(message.data, message.data + message.size);
  }
  // Enqueue before accounting so a throwing allocation cannot inflate the published
  // accepted count or shrink the remaining queue budget.
  const auto payload_bytes = static_cast<std::uint64_t>(item.data.size());
  queue.push_back(std::move(item));
  queue_bytes += payload_bytes;
  ++statistics.accepted;
  condition.notify_one();
  return {};
}

SubmitResult Recorder::Impl::submit_owned(ChannelId channel, OwnedMessage&& message) {
  std::lock_guard lock(mutex);
  PublishTimePolicy policy = PublishTimePolicy::LogTimeOnly;
  auto result = check_submission(channel, message.data.size(), true, message.publish_time_ns,
                                 message.source_clock, policy);
  if (!result.ok()) {
    // Ownership transfers only on success; a rejected buffer stays with the caller.
    return result;
  }

  // Reserve the queue slot before moving the payload so a rejected or throwing enqueue
  // leaves the caller's buffer intact.
  queue.emplace_back();
  Item& item = queue.back();
  item.channel = channel;
  item.log_time_ns = message.log_time_ns;
  item.publish_time_ns = message.publish_time_ns;
  item.sequence = message.sequence;
  item.source_clock = std::move(message.source_clock);
  item.policy = policy;
  item.data = std::move(message.data);
  queue_bytes += item.data.size();
  ++statistics.accepted;
  condition.notify_one();
  return {};
}

void Recorder::Impl::record_failure(bool writer_io, const Status& status, const Item* item,
                                    const char* action) {
  std::lock_guard lock(mutex);
  if (writer_io) {
    writer_failed = true;
  } else {
    timing_failed = true;
  }
  failed = true;
  stop_requested = true;
  if (state == State::Running) {
    state = State::Stopping;
  }
  if (primary_error.ok()) {
    primary_error = status;
  }
  detail::Diagnostic diagnostic;
  diagnostic.code = writer_io ? "writer_io_error" : "timing_io_error";
  diagnostic.severity = "error";
  if (item) {
    diagnostic.log_time_ns = item->log_time_ns;
    const auto channel = channels.find(item->channel);
    if (channel != channels.end()) {
      diagnostic.topic = channel->second.topic;
    }
  }
  diagnostic.detail = status.detail;
  diagnostic.action = action;
  diagnostics.push_back(std::move(diagnostic));
  condition.notify_all();
}

void Recorder::Impl::record_session_error(const Status& status, const char* code,
                                          const char* action) {
  std::lock_guard lock(mutex);
  failed = true;
  stop_requested = true;
  if (state == State::Running) {
    state = State::Stopping;
  }
  if (primary_error.ok()) {
    primary_error = status;
  }
  detail::Diagnostic diagnostic;
  diagnostic.code = code;
  diagnostic.severity = "error";
  diagnostic.detail = status.detail;
  diagnostic.action = action;
  diagnostics.push_back(std::move(diagnostic));
  condition.notify_all();
}

void Recorder::Impl::record_publish_failure(const Status& status, const char* code,
                                            const char* action) {
  record_session_error(status, code, action);
}

void Recorder::Impl::process(Item item) {
  Status status;
  {
    std::lock_guard writer_lock(writer_mutex);
    // writer_failed is published under the state mutex; the writer handle is the
    // authoritative check on this thread.
    if (!writer->is_open()) {
      return;
    }
    MessageView view;
    view.log_time_ns = item.log_time_ns;
    view.publish_time_ns = item.publish_time_ns;
    view.data = item.data.data();
    view.size = item.data.size();
    view.sequence = item.sequence;
    view.source_clock = item.source_clock;
    status = writer->write(item.channel, view);
  }
  if (!status.ok()) {
    record_failure(true, status, &item);
    return;
  }

  std::uint64_t ordinal = 0;
  {
    std::lock_guard lock(mutex);
    ordinal = statistics.writer_processed;
    ++statistics.writer_processed;
    if (!first_log_time_ns || item.log_time_ns < *first_log_time_ns) {
      first_log_time_ns = item.log_time_ns;
    }
    if (!last_log_time_ns || item.log_time_ns > *last_log_time_ns) {
      last_log_time_ns = item.log_time_ns;
    }
  }

  const bool publish_time_fallback = !item.publish_time_ns;
  const bool needs_timing_row =
      item.source_clock.has_value() ||
      (item.policy == PublishTimePolicy::PerMessage && publish_time_fallback);
  if (needs_timing_row) {
    try {
      artifacts->append_timing_row(ordinal, item.channel, publish_time_fallback,
                                   item.source_clock);
    } catch (const std::exception& error) {
      record_failure(false,
                     {"append_timing", artifacts->timing_partial_path(), error.what()}, &item);
    } catch (...) {
      record_failure(false,
                     {"append_timing", artifacts->timing_partial_path(),
                      "unknown timing write error"},
                     &item);
    }
  }
}

void Recorder::Impl::run() noexcept {
  try {
    for (;;) {
      Item item;
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] {
          return !queue.empty() || stop_requested || writer_failed || timing_failed;
        });
        if (failed || writer_failed || timing_failed) {
          break;
        }
        if (!queue.empty()) {
          item = std::move(queue.front());
          queue.pop_front();
          queue_bytes -= item.data.size();
        } else if (stop_requested) {
          break;
        }
      }
      process(std::move(item));
      {
        std::lock_guard lock(mutex);
        if (failed || writer_failed || timing_failed) {
          break;
        }
      }
    }
  } catch (const std::exception& error) {
    record_session_error({"recorder_thread", session_directory, error.what()},
                         "recorder_thread_error", "retain_partial_and_reject_new_messages");
  } catch (...) {
    record_session_error(
        {"recorder_thread", session_directory, "unknown recorder thread error"},
        "recorder_thread_error", "retain_partial_and_reject_new_messages");
  }
  finalize();
}

StopResult Recorder::Impl::finalize() noexcept {
  StopResult result;
  try {
    bool timing_was_failed = false;
    {
      // Freeze submission counters before publishing; later submissions are outside the
      // published session statistics.
      std::lock_guard lock(mutex);
      counters_frozen = true;
      timing_was_failed = timing_failed;
    }
    const std::filesystem::path data_path_partial = artifacts->data_partial_path();
    const std::filesystem::path data_path_final = artifacts->data_final_path();

    bool data_finalized = false;
    {
      std::lock_guard writer_lock(writer_mutex);
      // is_open() is the authoritative writer state under writer_mutex; it is also false
      // after a write or registration failure that abandoned the writer.
      if (writer && writer->is_open()) {
        const Status finished = writer->finish();
        if (finished.ok()) {
          data_finalized = true;
        } else {
          record_failure(true, finished, nullptr);
        }
      }
    }

    if (data_finalized) {
      const Status renamed = artifacts->finalize_data();
      if (!renamed.ok()) {
        record_publish_failure(renamed, "publish_io_error");
        data_finalized = false;
      }
    }

    bool timing_complete = !timing_was_failed;
    const Status timing_closed = artifacts->close_timing();
    if (!timing_closed.ok()) {
      record_failure(false, timing_closed, nullptr);
      timing_complete = false;
    }
    if (timing_complete) {
      const Status timing_renamed = artifacts->finalize_timing(data_finalized);
      if (!timing_renamed.ok()) {
        record_publish_failure(timing_renamed, "publish_io_error");
      }
    }

    detail::TimingFacts timing = artifacts->timing_facts(data_finalized);
    if (!timing.sha256_ok) {
      // Keep the sidecar referenced with an unknown digest instead of silently
      // dropping existing evidence.
      record_failure(false,
                     {"hash_timing", timing.path,
                      "could not hash the completed timing sidecar"},
                     nullptr, "report_unknown_digest");
    }

    const auto data_path = data_finalized ? data_path_final : data_path_partial;
    std::uint64_t data_bytes = 0;
    bool data_file_present = true;
    try {
      data_bytes = file_system->file_size(data_path);
    } catch (...) {
      data_file_present = false;
    }

    detail::ManifestInput input;
    input.session_id = options.session_id;
    input.data.path = data_path;
    input.data.finalized = data_finalized;
    input.data.present = data_file_present;
    input.data.bytes = data_file_present
                           ? std::optional<std::uint64_t>(data_bytes)
                           : std::nullopt;
    input.timing = timing;
    input.durability_mode = options.durability;
    {
      std::lock_guard lock(mutex);
      input.session_finalized = data_finalized && !failed;
      input.data.message_count = data_finalized
                                     ? std::optional<std::uint64_t>(statistics.writer_processed)
                                     : std::nullopt;
      input.data.first_log_time_ns =
          data_finalized ? first_log_time_ns : std::nullopt;
      input.data.last_log_time_ns =
          data_finalized ? last_log_time_ns : std::nullopt;
      input.statistics = statistics;
      input.statistics.finalized_messages =
          data_finalized ? std::optional<std::uint64_t>(statistics.writer_processed)
                         : std::nullopt;
      input.channels.assign(channels.begin(), channels.end());
      input.diagnostics = diagnostics;
    }

    const detail::PublishResult published = artifacts->publish_manifest(input);

    result.file_state =
        data_finalized ? FileState::Finalized
                       : (data_file_present ? FileState::Partial : FileState::Unknown);
    if (options.durability == DurabilityMode::Sync) {
      result.durability_state =
          published.error.ok() ? DurabilityState::Confirmed : DurabilityState::Failed;
    } else {
      result.durability_state = DurabilityState::NotRequested;
    }

    {
      std::lock_guard lock(mutex);
      result.error = primary_error;
      if (result.error.ok()) {
        result.error = published.error;
      }
      if (writer_failed) {
        result.reason = StopReason::WriterFailure;
      } else if (timing_failed) {
        result.reason = StopReason::TimingFailure;
      } else if (failed) {
        result.reason = StopReason::PublishFailure;
      } else if (!published.error.ok() &&
                 (published.error.operation == "write_manifest" ||
                  published.error.operation == "rename_manifest")) {
        result.reason = StopReason::PublishFailure;
      } else {
        result.reason = StopReason::Requested;
      }
      state = State::Stopped;
      queue.clear();
      queue_bytes = 0;
    }
  } catch (const std::exception& error) {
    result.file_state = FileState::Partial;
    result.durability_state = options.durability == DurabilityMode::Sync
                                  ? DurabilityState::Failed
                                  : DurabilityState::NotRequested;
    result.reason = StopReason::PublishFailure;
    result.error = {"finalize", session_directory, error.what()};
    std::lock_guard lock(mutex);
    state = State::Stopped;
  } catch (...) {
    result.file_state = FileState::Partial;
    result.durability_state = options.durability == DurabilityMode::Sync
                                  ? DurabilityState::Failed
                                  : DurabilityState::NotRequested;
    result.reason = StopReason::PublishFailure;
    result.error = {"finalize", session_directory, "unknown finalize error"};
    std::lock_guard lock(mutex);
    state = State::Stopped;
  }

  promise.set_value(result);
  return result;
}

std::shared_future<StopResult> Recorder::Impl::stop_async() {
  std::lock_guard lock(mutex);
  if (!started) {
    StopResult result;
    if (start_error.ok()) {
      result.reason = StopReason::NotStarted;
      result.error = {"stop", session_directory, "recorder has not been started"};
    } else {
      result.reason = StopReason::StartFailure;
      result.error = start_error;
    }
    return ready_future(std::move(result));
  }
  if (state == State::Running) {
    state = State::Stopping;
    stop_requested = true;
    condition.notify_all();
  }
  return future;
}

StopResult Recorder::Impl::stop() { return stop_async().get(); }

Recorder::Recorder(RecorderOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

Recorder::~Recorder() = default;

Status Recorder::start() { return impl_->start(); }

Status Recorder::register_channel(const ChannelSpec& spec, ChannelId& id) {
  return impl_->register_channel(spec, id);
}

SubmitResult Recorder::submit_copy(ChannelId channel, MessageView message) {
  return impl_->submit_copy(channel, message);
}

SubmitResult Recorder::submit_owned(ChannelId channel, OwnedMessage&& message) {
  return impl_->submit_owned(channel, std::move(message));
}

std::shared_future<StopResult> Recorder::stop_async() { return impl_->stop_async(); }

StopResult Recorder::stop() { return impl_->stop(); }

}  // namespace robot_mcap
