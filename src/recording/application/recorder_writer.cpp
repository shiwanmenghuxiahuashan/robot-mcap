#include "recording/application/recorder_impl.hpp"

#include "recording/domain/message_rules.hpp"

#include <exception>
#include <string>
#include <utility>

namespace robot_mcap {

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
  const bool needs_timing_row = detail::requires_timing_row(
      item.policy, item.publish_time_ns.has_value(), item.source_clock.has_value());
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


}  // namespace robot_mcap
