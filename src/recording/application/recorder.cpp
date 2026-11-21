#include "recording/application/recorder_impl.hpp"

#include "infrastructure/mcap/version.hpp"
#include "recording/domain/message_rules.hpp"

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace robot_mcap {
namespace {

std::shared_future<StopResult> ready_future(StopResult result) {
  std::promise<StopResult> promise;
  auto future = promise.get_future().share();
  promise.set_value(std::move(result));
  return future;
}

}  // namespace
Status Recorder::Impl::start() {
  std::lock_guard lock(mutex);
  if (start_attempted) {
    return {"start", session_directory, "start has already been attempted"};
  }
  start_attempted = true;

  if (!detail::valid_session_id(options.session_id)) {
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
        options.queue_byte_limit, options.durability,
        std::string(detail::mcap_writer_version()));
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
  if (!detail::valid_channel_spec(spec)) {
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
  if (source_clock && !detail::valid_source_clock(*source_clock)) {
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

std::filesystem::path Recorder::parent_directory() const {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->session_directory.empty()) {
    return impl_->session_directory.parent_path();
  }
  std::error_code error;
  const auto absolute = std::filesystem::absolute(impl_->options.parent_directory, error);
  return error ? impl_->options.parent_directory : absolute;
}

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
