#include "robot_mcap/io.hpp"
#include "robot_mcap/recorder.hpp"

#include <mcap/reader.hpp>
#include <mcap/types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using robot_mcap::ChannelId;
using robot_mcap::ChannelSpec;
using robot_mcap::DurabilityMode;
using robot_mcap::DurabilityState;
using robot_mcap::FileState;
using robot_mcap::MessageView;
using robot_mcap::OwnedMessage;
using robot_mcap::PublishTimePolicy;
using robot_mcap::Recorder;
using robot_mcap::RecorderOptions;
using robot_mcap::SchemaSpec;
using robot_mcap::SourceClock;
using robot_mcap::Status;
using robot_mcap::StopReason;
using robot_mcap::SubmitError;
using robot_mcap::SubmitResult;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) +   \
                               ": check failed: " #condition);                            \
    }                                                                                     \
  } while (false)

void check_ok(const Status& status) {
  if (!status.ok()) {
    throw std::runtime_error(status.operation + " " + status.path.string() + ": " +
                             status.detail);
  }
}

#define CHECK_OK(expression) check_ok(expression)

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> result(text.size());
  if (!text.empty()) {
    std::memcpy(result.data(), text.data(), text.size());
  }
  return result;
}

std::vector<std::byte> sequence_payload(std::uint32_t sequence, std::size_t size = 8) {
  std::vector<std::byte> result(std::max<std::size_t>(size, 4), std::byte{0xA5});
  for (int index = 0; index < 4; ++index) {
    result[index] = static_cast<std::byte>((sequence >> (index * 8)) & 0xFFU);
  }
  return result;
}

std::uint32_t payload_sequence(const std::vector<std::byte>& data) {
  std::uint32_t sequence = 0;
  for (int index = 0; index < 4 && static_cast<std::size_t>(index) < data.size(); ++index) {
    sequence |= static_cast<std::uint32_t>(data[index]) << (index * 8);
  }
  return sequence;
}

OwnedMessage owned_message(std::vector<std::byte> data, std::uint64_t log_time = 1,
                           std::uint32_t sequence = 0) {
  OwnedMessage message;
  message.log_time_ns = log_time;
  message.sequence = sequence;
  message.data = std::move(data);
  return message;
}

RecorderOptions base_options(const std::filesystem::path& parent, std::string session_id) {
  RecorderOptions options;
  options.parent_directory = parent;
  options.session_id = std::move(session_id);
  options.queue_byte_limit = 1ULL << 20;
  return options;
}

ChannelSpec raw_channel(std::string topic) {
  return {std::move(topic), "raw", std::nullopt, PublishTimePolicy::LogTimeOnly};
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open " + path.string());
  }
  std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (input.bad()) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return data;
}

void reset_directory(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
  std::filesystem::create_directories(path);
}

// Minimal JSON parser used to inspect manifests and timing rows.
struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  std::string text;
  std::vector<Json> items;
  std::map<std::string, Json> fields;

  bool is_null() const { return type == Type::Null; }

  const Json& at(std::string_view key) const {
    const auto found = fields.find(std::string(key));
    if (found == fields.end()) {
      throw std::runtime_error("missing JSON key: " + std::string(key));
    }
    return found->second;
  }

  const Json* find(std::string_view key) const {
    const auto found = fields.find(std::string(key));
    return found == fields.end() ? nullptr : &found->second;
  }

  const Json& operator[](std::size_t index) const {
    if (type != Type::Array || index >= items.size()) {
      throw std::runtime_error("invalid JSON array access");
    }
    return items[index];
  }

  std::size_t size() const {
    if (type == Type::Array) {
      return items.size();
    }
    if (type == Type::Object) {
      return fields.size();
    }
    return 0;
  }

  std::uint64_t as_u64() const {
    if (type != Type::Number) {
      throw std::runtime_error("JSON value is not a number");
    }
    return std::stoull(text);
  }

  bool as_bool() const {
    if (type != Type::Bool) {
      throw std::runtime_error("JSON value is not a bool");
    }
    return boolean;
  }

  const std::string& as_string() const {
    if (type != Type::String) {
      throw std::runtime_error("JSON value is not a string");
    }
    return text;
  }
};

class JsonParser {
public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  Json parse() {
    skip_whitespace();
    Json value = parse_value();
    skip_whitespace();
    if (position_ != input_.size()) {
      fail("trailing data");
    }
    return value;
  }

private:
  [[noreturn]] void fail(const char* message) const {
    throw std::runtime_error(std::string("JSON error: ") + message + " at " +
                             std::to_string(position_));
  }

  void skip_whitespace() {
    while (position_ < input_.size()) {
      const auto character = static_cast<unsigned char>(input_[position_]);
      if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
        break;
      }
      ++position_;
    }
  }

  char take() {
    if (position_ >= input_.size()) {
      fail("unexpected end of input");
    }
    return input_[position_++];
  }

  void expect(char expected) {
    const char actual = take();
    if (actual != expected) {
      fail("unexpected character");
    }
  }

  Json parse_value() {
    skip_whitespace();
    if (position_ >= input_.size()) {
      fail("unexpected end of input");
    }
    switch (input_[position_]) {
      case '{':
        return parse_object();
      case '[':
        return parse_array();
      case '"': {
        Json value;
        value.type = Json::Type::String;
        value.text = parse_string();
        return value;
      }
      case 't':
        parse_literal("true");
        return make_bool(true);
      case 'f':
        parse_literal("false");
        return make_bool(false);
      case 'n':
        parse_literal("null");
        return Json{};
      default:
        return parse_number();
    }
  }

  static Json make_bool(bool value) {
    Json result;
    result.type = Json::Type::Bool;
    result.boolean = value;
    return result;
  }

  void parse_literal(std::string_view literal) {
    if (input_.substr(position_, literal.size()) != literal) {
      fail("invalid literal");
    }
    position_ += literal.size();
  }

  Json parse_object() {
    expect('{');
    Json result;
    result.type = Json::Type::Object;
    skip_whitespace();
    if (position_ < input_.size() && input_[position_] == '}') {
      ++position_;
      return result;
    }
    for (;;) {
      skip_whitespace();
      std::string key = parse_string();
      skip_whitespace();
      expect(':');
      Json value = parse_value();
      if (!result.fields.emplace(key, std::move(value)).second) {
        fail("duplicate object key");
      }
      skip_whitespace();
      const char separator = take();
      if (separator == '}') {
        return result;
      }
      if (separator != ',') {
        fail("expected ',' or '}'");
      }
    }
  }

  Json parse_array() {
    expect('[');
    Json result;
    result.type = Json::Type::Array;
    skip_whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return result;
    }
    for (;;) {
      result.items.push_back(parse_value());
      skip_whitespace();
      const char separator = take();
      if (separator == ']') {
        return result;
      }
      if (separator != ',') {
        fail("expected ',' or ']'");
      }
    }
  }

  std::string parse_string() {
    expect('"');
    std::string result;
    for (;;) {
      const char character = take();
      if (character == '"') {
        return result;
      }
      if (static_cast<unsigned char>(character) < 0x20) {
        fail("control character in string");
      }
      if (character != '\\') {
        result.push_back(character);
        continue;
      }
      const char escape = take();
      switch (escape) {
        case '"':
          result.push_back('"');
          break;
        case '\\':
          result.push_back('\\');
          break;
        case '/':
          result.push_back('/');
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'u': {
          std::uint32_t code = parse_hex4();
          if (code >= 0xD800U && code <= 0xDBFFU) {
            if (take() != '\\' || take() != 'u') {
              fail("missing low surrogate");
            }
            const std::uint32_t low = parse_hex4();
            if (low < 0xDC00U || low > 0xDFFFU) {
              fail("invalid low surrogate");
            }
            code = 0x10000U + ((code - 0xD800U) << 10U) + (low - 0xDC00U);
          } else if (code >= 0xDC00U && code <= 0xDFFFU) {
            fail("unexpected low surrogate");
          }
          append_utf8(result, code);
          break;
        }
        default:
          fail("unknown escape");
      }
    }
  }

  std::uint32_t parse_hex4() {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      const char digit = take();
      value <<= 4U;
      if (digit >= '0' && digit <= '9') {
        value |= static_cast<std::uint32_t>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        value |= static_cast<std::uint32_t>(digit - 'a' + 10);
      } else if (digit >= 'A' && digit <= 'F') {
        value |= static_cast<std::uint32_t>(digit - 'A' + 10);
      } else {
        fail("invalid hex digit");
      }
    }
    return value;
  }

  static void append_utf8(std::string& output, std::uint32_t code) {
    if (code <= 0x7FU) {
      output.push_back(static_cast<char>(code));
    } else if (code <= 0x7FFU) {
      output.push_back(static_cast<char>(0xC0U | (code >> 6U)));
      output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else if (code <= 0xFFFFU) {
      output.push_back(static_cast<char>(0xE0U | (code >> 12U)));
      output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
      output.push_back(static_cast<char>(0xF0U | (code >> 18U)));
      output.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
  }

  Json parse_number() {
    const std::size_t start = position_;
    if (position_ < input_.size() && input_[position_] == '-') {
      ++position_;
    }
    while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
      ++position_;
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ == start) {
      fail("invalid value");
    }
    Json result;
    result.type = Json::Type::Number;
    result.text = std::string(input_.substr(start, position_ - start));
    return result;
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

Json parse_json(std::string_view text) { return JsonParser(text).parse(); }

struct RecordedMessage {
  std::string topic;
  std::string encoding;
  std::string schema_name;
  std::uint64_t log_time = 0;
  std::uint64_t publish_time = 0;
  std::uint32_t sequence = 0;
  std::vector<std::byte> data;
};

std::vector<RecordedMessage> read_messages(const std::filesystem::path& path) {
  mcap::McapReader reader;
  const auto opened = reader.open(path.string());
  if (!opened.ok()) {
    throw std::runtime_error("reader open failed: " + opened.message);
  }
  std::vector<RecordedMessage> messages;
  auto view = reader.readMessages();
  for (auto iterator = view.begin(); iterator != view.end(); ++iterator) {
    RecordedMessage message;
    message.topic = iterator->channel->topic;
    message.encoding = iterator->channel->messageEncoding;
    if (iterator->schema) {
      message.schema_name = iterator->schema->name;
    }
    message.log_time = iterator->message.logTime;
    message.publish_time = iterator->message.publishTime;
    message.sequence = iterator->message.sequence;
    if (iterator->message.dataSize > 0) {
      message.data.assign(iterator->message.data,
                          iterator->message.data + iterator->message.dataSize);
    }
    messages.push_back(std::move(message));
  }
  if (!view.status().ok()) {
    throw std::runtime_error("reader iteration failed: " + view.status().message);
  }
  reader.close();
  return messages;
}

class Barrier {
public:
  explicit Barrier(std::size_t count) : count_(count) {}

  void arrive_and_wait() {
    std::unique_lock lock(mutex_);
    const auto generation = generation_;
    ++waiting_;
    if (waiting_ == count_) {
      waiting_ = 0;
      ++generation_;
      condition_.notify_all();
    } else {
      // Untimed waits keep ThreadSanitizer clean on this glibc; CTest's TIMEOUT is
      // the hang detector for the test process.
      condition_.wait(lock, [&] { return generation != generation_; });
    }
  }

private:
  std::size_t count_;
  std::size_t waiting_ = 0;
  std::uint64_t generation_ = 0;
  std::mutex mutex_;
  std::condition_variable condition_;
};

// Blocks the writer thread inside a sink write so queue and stop boundaries can be
// observed deterministically.
class Gate {
public:
  void enable() {
    std::lock_guard lock(mutex_);
    blocking_ = true;
  }

  void release() {
    std::lock_guard lock(mutex_);
    blocking_ = false;
    condition_.notify_all();
  }

  void block() {
    std::unique_lock lock(mutex_);
    if (!blocking_) {
      return;
    }
    blocked_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return !blocking_; });
    blocked_ = false;
  }

  void wait_until_blocked() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&] { return blocked_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool blocking_ = false;
  bool blocked_ = false;
};

class GateRelease {
public:
  explicit GateRelease(Gate& gate) : gate_(gate) {}
  ~GateRelease() { gate_.release(); }
  GateRelease(const GateRelease&) = delete;
  GateRelease& operator=(const GateRelease&) = delete;

private:
  Gate& gate_;
};

// Parks a finalization stage until the test releases it.
class BlockingPoint {
public:
  void enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
  }

  void wait_entered() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&] { return entered_; });
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

class BlockingRelease {
public:
  explicit BlockingRelease(BlockingPoint& point) : point_(point) {}
  ~BlockingRelease() { point_.release(); }
  BlockingRelease(const BlockingRelease&) = delete;
  BlockingRelease& operator=(const BlockingRelease&) = delete;

private:
  BlockingPoint& point_;
};

struct SinkControl {
  std::atomic<bool> fail_write{false};
  std::atomic<bool> fail_flush{false};
  std::atomic<bool> fail_close{false};
  // One-based write call number to fail; zero disables.
  std::atomic<int> fail_on_write_call{0};
  std::atomic<int> write_calls{0};
  std::shared_ptr<Gate> gate;
};

class ControlledSink final : public robot_mcap::OutputSink {
public:
  ControlledSink(std::unique_ptr<robot_mcap::OutputSink> inner,
                 std::shared_ptr<SinkControl> control)
      : inner_(std::move(inner)), control_(std::move(control)) {}
  ~ControlledSink() override { abandon(); }

  void write(const std::byte* data, std::size_t size) override {
    if (control_->gate) {
      control_->gate->block();
    }
    const int call = control_->write_calls.fetch_add(1) + 1;
    const int fail_on = control_->fail_on_write_call.load();
    if (control_->fail_write.load() || (fail_on > 0 && call == fail_on)) {
      throw std::runtime_error("injected write failure");
    }
    inner_->write(data, size);
  }

  void flush() override {
    if (control_->fail_flush.load()) {
      throw std::runtime_error("injected flush failure");
    }
    inner_->flush();
  }

  void close() override {
    if (control_->fail_close.load()) {
      throw std::runtime_error("injected close failure");
    }
    inner_->close();
    closed_ = true;
  }

  void abandon() noexcept override {
    if (inner_) {
      inner_->abandon();
    }
  }

  std::uint64_t size() const override { return inner_->size(); }

private:
  std::unique_ptr<robot_mcap::OutputSink> inner_;
  std::shared_ptr<SinkControl> control_;
  bool closed_ = false;
};

class TestFileSystem final : public robot_mcap::FileSystem {
public:
  std::shared_ptr<robot_mcap::FileSystem> native = robot_mcap::native_file_system();
  std::shared_ptr<SinkControl> sink_control = std::make_shared<SinkControl>();
  std::shared_ptr<SinkControl> timing_control;
  std::function<void(const std::filesystem::path&)> on_create;
  std::function<void(const std::filesystem::path&)> on_sync_file;
  std::function<void(const std::filesystem::path&)> on_sync_directory;
  std::function<void(const std::filesystem::path&)> on_rename;
  std::function<void(const std::filesystem::path&)> on_sha256;

  std::unique_ptr<robot_mcap::OutputSink> create_exclusive(
      const std::filesystem::path& path) override {
    if (on_create) {
      on_create(path);
    }
    auto sink = native->create_exclusive(path);
    if (path.filename() == "data_0000.mcap.partial") {
      return std::make_unique<ControlledSink>(std::move(sink), sink_control);
    }
    if (timing_control && path.filename() == "data_0000.timing.ndjson.partial") {
      return std::make_unique<ControlledSink>(std::move(sink), timing_control);
    }
    return sink;
  }

  void sync_file(const std::filesystem::path& path) override {
    record("sync_file:" + path.filename().string());
    if (on_sync_file) {
      on_sync_file(path);
    }
    native->sync_file(path);
  }

  void sync_directory(const std::filesystem::path& path) override {
    record("sync_directory:" + path.filename().string());
    if (on_sync_directory) {
      on_sync_directory(path);
    }
    native->sync_directory(path);
  }

  void rename(const std::filesystem::path& from, const std::filesystem::path& to) override {
    if (on_rename) {
      on_rename(from);
    }
    record("rename:" + from.filename().string() + "->" + to.filename().string());
    native->rename(from, to);
  }

  void remove(const std::filesystem::path& path) noexcept override { native->remove(path); }

  std::uint64_t file_size(const std::filesystem::path& path) override {
    return native->file_size(path);
  }

  std::string file_sha256(const std::filesystem::path& path) override {
    if (on_sha256) {
      on_sha256(path);
    }
    return native->file_sha256(path);
  }

  void create_directories(const std::filesystem::path& path) override {
    native->create_directories(path);
  }

  bool create_directory_exclusive(const std::filesystem::path& path) override {
    return native->create_directory_exclusive(path);
  }

  std::vector<std::string> calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }

private:
  void record(std::string call) {
    std::lock_guard lock(mutex_);
    calls_.push_back(std::move(call));
  }

  mutable std::mutex mutex_;
  std::vector<std::string> calls_;
};

std::size_t call_index(const std::vector<std::string>& calls, std::string_view needle) {
  for (std::size_t index = 0; index < calls.size(); ++index) {
    if (calls[index].find(needle) != std::string::npos) {
      return index;
    }
  }
  return calls.size();
}

std::filesystem::path make_session(const std::filesystem::path& parent,
                                   std::string_view session_id) {
  // The recorder resolves its session directory against the process working directory.
  return std::filesystem::absolute(parent) / std::string(session_id);
}

void test_finalized_session(const std::filesystem::path& root) {
  const auto parent = root / "finalized";
  reset_directory(parent);
  Recorder recorder(base_options(parent, "finalized-session"));
  CHECK_OK(recorder.start());

  const std::string schema_text =
      R"({"type":"object","properties":{"celsius":{"type":"number"}}})";
  const ChannelSpec temperature{"/sensor/temperature", "json",
                                SchemaSpec{"Temperature", "jsonschema", schema_text},
                                PublishTimePolicy::SourceRequired};
  ChannelId temperature_id = 0;
  CHECK_OK(recorder.register_channel(temperature, temperature_id));
  CHECK(temperature_id == 1);
  ChannelId duplicate_id = 0;
  CHECK_OK(recorder.register_channel(temperature, duplicate_id));
  CHECK(duplicate_id == temperature_id);
  ChannelId changed_id = 7;
  auto changed = temperature;
  changed.message_encoding = "raw";
  CHECK(!recorder.register_channel(changed, changed_id).ok());
  CHECK(changed_id == 0);
  ChannelId invalid_id = 7;
  CHECK(!recorder.register_channel({"\x80", "raw", std::nullopt, PublishTimePolicy::LogTimeOnly},
                                   invalid_id)
             .ok());
  CHECK(invalid_id == 0);

  ChannelId raw_id = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/sensor/raw"), raw_id));
  CHECK(raw_id == 2);

  // Invalid submissions are reported but are not counted as rejected source messages.
  CHECK(recorder.submit_copy(99, MessageView{1, std::nullopt, nullptr, 0, 0, std::nullopt}).error ==
        SubmitError::InvalidArgument);
  CHECK(recorder.submit_copy(temperature_id,
                             MessageView{1, std::nullopt, nullptr, 0, 0, std::nullopt})
            .error == SubmitError::InvalidArgument);
  CHECK(recorder.submit_copy(raw_id, MessageView{1, 2, nullptr, 0, 0, std::nullopt}).error ==
        SubmitError::InvalidArgument);
  CHECK(recorder.submit_copy(raw_id, MessageView{1, std::nullopt, nullptr, 1, 0, std::nullopt})
            .error == SubmitError::InvalidArgument);

  auto json_payload = bytes_of(R"({"celsius":23.5})");
  const std::uint64_t log0 = 1794326400000000000ULL;
  const std::uint64_t publish0 = log0 - 1;
  CHECK(recorder
            .submit_copy(temperature_id,
                         MessageView{log0, publish0, json_payload.data(), json_payload.size(), 42,
                                     std::nullopt})
            .ok());
  std::fill(json_payload.begin(), json_payload.end(), std::byte{0xAA});

  const std::uint64_t log1 = log0 + 1000000000ULL;
  std::vector<std::byte> raw_payload{std::byte{0x00}, std::byte{0xFF}, std::byte{0x7F}};
  CHECK(recorder
            .submit_copy(raw_id,
                         MessageView{log1, std::nullopt, raw_payload.data(), raw_payload.size(), 0,
                                     std::nullopt})
            .ok());
  std::fill(raw_payload.begin(), raw_payload.end(), std::byte{0x11});

  OwnedMessage owned;
  owned.log_time_ns = log1 + 1;
  owned.sequence = 7;
  owned.data = bytes_of("owned-payload");
  CHECK(recorder.submit_owned(raw_id, std::move(owned)).ok());
  CHECK(owned.data.empty());

  auto first_future = recorder.stop_async();
  auto second_future = recorder.stop_async();
  const auto result = recorder.stop();
  CHECK(result.file_state == FileState::Finalized);
  CHECK(result.durability_state == DurabilityState::NotRequested);
  CHECK(result.reason == StopReason::Requested);
  CHECK(result.error.ok());
  CHECK(first_future.get().ok());
  CHECK(second_future.get().ok());

  const auto session = make_session(parent, "finalized-session");
  CHECK(std::filesystem::exists(session / "data_0000.mcap"));
  CHECK(!std::filesystem::exists(session / "data_0000.mcap.partial"));
  CHECK(!std::filesystem::exists(session / "data_0000.timing.ndjson"));
  CHECK(!std::filesystem::exists(session / "manifest.json.tmp"));

  const auto messages = read_messages(session / "data_0000.mcap");
  CHECK(messages.size() == 3);
  CHECK(messages[0].topic == "/sensor/temperature");
  CHECK(messages[0].encoding == "json");
  CHECK(messages[0].schema_name == "Temperature");
  CHECK(messages[0].log_time == log0);
  CHECK(messages[0].publish_time == publish0);
  CHECK(messages[0].sequence == 42);
  CHECK(messages[0].data == bytes_of(R"({"celsius":23.5})"));
  CHECK(messages[1].topic == "/sensor/raw");
  CHECK(messages[1].schema_name.empty());
  CHECK(messages[1].log_time == log1);
  CHECK(messages[1].publish_time == log1);
  CHECK(messages[1].data == std::vector<std::byte>({std::byte{0x00}, std::byte{0xFF},
                                                     std::byte{0x7F}}));
  CHECK(messages[2].log_time == log1 + 1);
  CHECK(messages[2].publish_time == log1 + 1);
  CHECK(messages[2].sequence == 7);
  CHECK(messages[2].data == bytes_of("owned-payload"));

  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("schema_version").as_u64() == 1);
  CHECK(manifest.at("session_id").as_string() == "finalized-session");
  CHECK(manifest.at("state").as_string() == "finalized");
  CHECK(manifest.at("files").size() == 1);
  const auto no_separators = [](const std::string& value) {
    return value.find('/') == std::string::npos &&
           value.find('\\') == std::string::npos;
  };
  const auto& file = manifest.at("files")[0];
  CHECK(file.at("path").as_string() == "data_0000.mcap");
  CHECK(no_separators(file.at("path").as_string()));
  CHECK(file.at("state").as_string() == "finalized");
  CHECK(file.at("bytes").as_u64() == std::filesystem::file_size(session / "data_0000.mcap"));
  CHECK(file.at("message_count").as_u64() == 3);
  CHECK(file.at("first_log_time_ns").as_string() == std::to_string(log0));
  CHECK(file.at("last_log_time_ns").as_string() == std::to_string(log1 + 1));
  CHECK(file.at("timing").is_null());
  CHECK(manifest.at("channels").size() == 2);
  const auto& channel0 = manifest.at("channels")[0];
  CHECK(channel0.at("file_path").as_string() == "data_0000.mcap");
  CHECK(no_separators(channel0.at("file_path").as_string()));
  CHECK(channel0.at("channel_id").as_u64() == 1);
  CHECK(channel0.at("topic").as_string() == "/sensor/temperature");
  CHECK(channel0.at("message_encoding").as_string() == "json");
  CHECK(channel0.at("publish_time_policy").as_string() == "source_required");
  CHECK(channel0.at("schema").at("name").as_string() == "Temperature");
  CHECK(channel0.at("schema").at("encoding").as_string() == "jsonschema");
  CHECK(channel0.at("schema").at("sha256").as_string() ==
        robot_mcap::sha256_hex(schema_text));
  const auto& channel1 = manifest.at("channels")[1];
  CHECK(channel1.at("channel_id").as_u64() == 2);
  CHECK(channel1.at("topic").as_string() == "/sensor/raw");
  CHECK(channel1.at("schema").is_null());
  CHECK(channel1.at("publish_time_policy").as_string() == "log_time_only");
  const auto& statistics = manifest.at("statistics");
  CHECK(statistics.at("accepted").as_u64() == 3);
  CHECK(statistics.at("rejected").as_u64() == 0);
  CHECK(statistics.at("writer_processed").as_u64() == 3);
  CHECK(statistics.at("finalized_messages").as_u64() == 3);
  CHECK(manifest.at("durability").at("requested_mode").as_string() == "close");
  CHECK(manifest.at("durability").at("confirmation").as_string() == "not_requested");
  CHECK(manifest.at("ros2_replay").at("state").as_string() == "not_requested");
  CHECK(manifest.at("ros2_replay").at("metadata_path").is_null());
  CHECK(manifest.at("config_path").as_string() == "recording-config.json");
  CHECK(no_separators(manifest.at("config_path").as_string()));
  CHECK(manifest.at("diagnostics").size() == 1);
  const auto& diagnostic = manifest.at("diagnostics")[0];
  CHECK(diagnostic.at("code").as_string() == "channel_description_conflict");
  CHECK(diagnostic.at("severity").as_string() == "error");
  CHECK(diagnostic.at("topic").as_string() == "/sensor/temperature");
  CHECK(diagnostic.at("log_time_ns").is_null());
  CHECK(diagnostic.at("action").as_string() == "reject_channel_registration");
  CHECK(!diagnostic.at("detail").as_string().empty());

  const auto config = parse_json(read_file(session / "recording-config.json"));
  CHECK(config.at("schema_version").as_u64() == 1);
  CHECK(config.at("session_id").as_string() == "finalized-session");
  CHECK(config.at("writer").at("version").as_string() == mcap::LibraryVersion);
  CHECK(config.at("writer").at("profile").as_string().empty());
  CHECK(config.at("writer").at("chunk_size_bytes").as_u64() == 1048576);
  CHECK(config.at("writer").at("compression").as_string() == "none");
  CHECK(config.at("writer").at("chunk_crc").as_bool());
  CHECK(config.at("durability_mode").as_string() == "close");
  CHECK(config.at("adapter").is_null());
  CHECK(!std::filesystem::exists(session / "recording-config.json.tmp"));
}

void test_empty_session(const std::filesystem::path& root) {
  const auto parent = root / "empty";
  reset_directory(parent);
  Recorder recorder(base_options(parent, "empty-session"));
  CHECK_OK(recorder.start());
  const auto result = recorder.stop();
  CHECK(result.ok());
  CHECK(result.file_state == FileState::Finalized);

  const auto session = make_session(parent, "empty-session");
  const auto messages = read_messages(session / "data_0000.mcap");
  CHECK(messages.empty());
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("state").as_string() == "finalized");
  CHECK(manifest.at("channels").size() == 0);
  CHECK(manifest.at("files")[0].at("message_count").as_u64() == 0);
  CHECK(manifest.at("files")[0].at("first_log_time_ns").is_null());
  CHECK(manifest.at("files")[0].at("last_log_time_ns").is_null());
  CHECK(manifest.at("files")[0].at("timing").is_null());
  CHECK(manifest.at("statistics").at("accepted").as_u64() == 0);
  CHECK(manifest.at("statistics").at("writer_processed").as_u64() == 0);
  CHECK(manifest.at("statistics").at("finalized_messages").as_u64() == 0);
}

void test_session_validation(const std::filesystem::path& root) {
  const auto parent = root / "validation";
  reset_directory(parent);

  for (const auto* invalid :
       {"", "-leading", "_leading", "contains.dot", "contains space", "contains/slash",
        "contains\\backslash", "01234567890123456789012345678901234567890123456789012345678901234"}) {
    Recorder recorder(base_options(parent, invalid));
    CHECK(!recorder.start().ok());
  }
  {
    Recorder recorder(base_options(parent, std::string("\xE6\xB8\xA9", 3)));
    CHECK(!recorder.start().ok());
  }

  {
    const std::string valid_long(64, 'a');
    Recorder recorder(base_options(parent, valid_long));
    CHECK_OK(recorder.start());
    CHECK(recorder.stop().ok());
  }
  {
    Recorder recorder(base_options(parent, "a_b-c1"));
    CHECK_OK(recorder.start());
    CHECK(recorder.stop().ok());
  }

  const auto existing = parent / "existing-session";
  std::filesystem::create_directories(existing);
  const auto sentinel = existing / "keep.txt";
  {
    std::ofstream output(sentinel);
    output << "keep";
  }
  {
    Recorder recorder(base_options(parent, "existing-session"));
    CHECK(!recorder.start().ok());
  }
  CHECK(std::filesystem::exists(sentinel));
  CHECK(read_file(sentinel) == "keep");

  {
    Recorder recorder(base_options(parent, "double-start"));
    CHECK_OK(recorder.start());
    CHECK(!recorder.start().ok());
    CHECK(recorder.stop().ok());
  }

  {
    Recorder recorder(base_options(parent, "stop-before-start"));
    const auto stopped = recorder.stop();
    CHECK(stopped.reason == StopReason::NotStarted);
    CHECK(stopped.file_state == FileState::Unknown);
    CHECK(stopped.durability_state == DurabilityState::NotRequested);
    CHECK(!stopped.error.ok());
    CHECK_OK(recorder.start());
    CHECK(recorder.stop().ok());
  }

  {
    Recorder recorder(base_options(parent, "before-start"));
    ChannelId id = 0;
    CHECK(!recorder.register_channel(raw_channel("/before"), id).ok());
    OwnedMessage message = owned_message(sequence_payload(1));
    const auto original = message.data;
    CHECK(recorder.submit_owned(1, std::move(message)).error == SubmitError::Stopped);
    CHECK(message.data == original);
    CHECK_OK(recorder.start());
    CHECK_OK(recorder.register_channel(raw_channel("/before"), id));
    CHECK(recorder.submit_owned(id, owned_message(sequence_payload(1))).ok());
    CHECK(recorder.stop().ok());
    CHECK(!recorder.register_channel(raw_channel("/after"), id).ok());
    CHECK(recorder.submit_owned(id, owned_message(sequence_payload(2))).error ==
          SubmitError::Stopped);
    // Lifecycle state takes precedence over argument validation.
    CHECK(recorder.submit_copy(id, MessageView{0, std::nullopt, nullptr, 1, 0, std::nullopt})
              .error == SubmitError::Stopped);
    const auto manifest = parse_json(read_file(parent / "before-start" / "manifest.json"));
    CHECK(manifest.at("statistics").at("rejected").as_u64() == 0);
  }
}

void test_queue_full(const std::filesystem::path& root) {
  const auto parent = root / "queue";
  reset_directory(parent);
  auto file_system = std::make_shared<TestFileSystem>();
  auto gate = std::make_shared<Gate>();
  file_system->sink_control->gate = gate;

  RecorderOptions options = base_options(parent, "queue-session");
  options.file_system = file_system;
  options.queue_byte_limit = 8;
  options.file_options.chunk_size_bytes = 1;

  Recorder recorder(options);
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/queue"), channel));

  gate->enable();
  GateRelease release(*gate);
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1))).ok());
  gate->wait_until_blocked();

  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(2))).ok());
  OwnedMessage rejected = owned_message(sequence_payload(3));
  const auto rejected_payload = rejected.data;
  CHECK(recorder.submit_owned(channel, std::move(rejected)).error == SubmitError::QueueFull);
  CHECK(rejected.data == rejected_payload);
  CHECK(recorder
            .submit_owned(channel, owned_message(sequence_payload(4, 9)))
            .error == SubmitError::QueueFull);

  // Stop while the writer is still gated and message 2 is queued; releasing the gate
  // must drain message 2 before finalization.
  auto future = recorder.stop_async();
  gate->release();
  const auto result = future.get();
  CHECK(result.ok());
  const auto session = make_session(parent, "queue-session");
  const auto messages = read_messages(session / "data_0000.mcap");
  CHECK(messages.size() == 2);
  CHECK(payload_sequence(messages[0].data) == 1);
  CHECK(payload_sequence(messages[1].data) == 2);
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("statistics").at("accepted").as_u64() == 2);
  CHECK(manifest.at("statistics").at("rejected").as_u64() == 2);
  CHECK(manifest.at("statistics").at("writer_processed").as_u64() == 2);
  CHECK(manifest.at("statistics").at("finalized_messages").as_u64() == 2);
}

void test_stop_race(const std::filesystem::path& root) {
  constexpr int kProducers = 4;
  constexpr int kMessages = 150;
  constexpr int kIterations = 8;
  const auto parent = root / "stop-race";
  reset_directory(parent);

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const auto session_id = "race-" + std::to_string(iteration);
    Recorder recorder(base_options(parent, session_id));
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/race"), channel));

    auto barrier = std::make_shared<Barrier>(kProducers + 1);
    // The stopper waits until every producer has at least one accepted message, so
    // the drain-at-stop path is exercised instead of only the already-stopped path.
    auto first_accepted = std::make_shared<Barrier>(kProducers + 1);
    std::vector<std::vector<bool>> succeeded(
        kProducers, std::vector<bool>(kMessages, false));
    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
      producers.emplace_back([&, producer] {
        barrier->arrive_and_wait();
        bool signaled = false;
        for (int index = 0; index < kMessages; ++index) {
          const auto sequence = static_cast<std::uint32_t>(
              producer * kMessages + index + 1);
          auto payload = sequence_payload(sequence);
          SubmitResult result;
          if ((producer + index) % 2 == 0) {
            result = recorder.submit_copy(
                channel, MessageView{sequence, std::nullopt, payload.data(), payload.size(),
                                     sequence, std::nullopt});
          } else {
            result = recorder.submit_owned(
                channel, owned_message(std::move(payload), sequence, sequence));
          }
          succeeded[producer][index] = result.ok();
          if (!signaled && result.ok()) {
            signaled = true;
            first_accepted->arrive_and_wait();
          }
        }
        if (!signaled) {
          first_accepted->arrive_and_wait();
        }
      });
    }
    std::thread stopper([&] {
      barrier->arrive_and_wait();
      first_accepted->arrive_and_wait();
      recorder.stop();
    });
    for (auto& producer : producers) {
      producer.join();
    }
    stopper.join();

    std::size_t successes = 0;
    for (const auto& producer : succeeded) {
      successes += static_cast<std::size_t>(
          std::count(producer.begin(), producer.end(), true));
    }
    const std::size_t total = static_cast<std::size_t>(kProducers) * kMessages;
    CHECK(successes >= static_cast<std::size_t>(kProducers));

    const auto session = make_session(parent, session_id);
    const auto messages = read_messages(session / "data_0000.mcap");
    std::map<std::uint32_t, int> counts;
    for (const auto& message : messages) {
      ++counts[payload_sequence(message.data)];
    }
    CHECK(messages.size() == successes);
    for (int producer = 0; producer < kProducers; ++producer) {
      for (int index = 0; index < kMessages; ++index) {
        const auto sequence =
            static_cast<std::uint32_t>(producer * kMessages + index + 1);
        if (succeeded[producer][index]) {
          CHECK(counts[sequence] == 1);
        } else {
          CHECK(counts.count(sequence) == 0);
        }
      }
    }

    const auto manifest = parse_json(read_file(session / "manifest.json"));
    const auto& statistics = manifest.at("statistics");
    CHECK(statistics.at("accepted").as_u64() == successes);
    CHECK(statistics.at("writer_processed").as_u64() == successes);
    CHECK(statistics.at("finalized_messages").as_u64() == successes);
    CHECK(statistics.at("rejected").as_u64() <= total - successes);
  }
}

void test_write_failure(const std::filesystem::path& root) {
  const auto parent = root / "write-failure";
  reset_directory(parent);
  auto file_system = std::make_shared<TestFileSystem>();
  auto gate = std::make_shared<Gate>();
  file_system->sink_control->gate = gate;
  RecorderOptions options = base_options(parent, "write-failure-session");
  options.file_system = file_system;
  options.file_options.chunk_size_bytes = 1;

  Recorder recorder(options);
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/failure"), channel));

  // Park the writer in the first message's chunk write and queue all three messages
  // before arming the failure. All submissions are therefore accepted, then the
  // in-flight write fails and the remaining queued messages are dropped.
  gate->enable();
  GateRelease release(*gate);
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 1, 1)).ok());
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(2), 2, 2)).ok());
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(3), 3, 3)).ok());
  gate->wait_until_blocked();
  file_system->sink_control->fail_write.store(true);
  gate->release();

  const auto result = recorder.stop_async().get();
  CHECK(result.file_state == FileState::Partial);
  CHECK(result.durability_state == DurabilityState::NotRequested);
  CHECK(result.reason == StopReason::WriterFailure);
  CHECK(!result.error.ok());
  CHECK(result.error.operation == "write");

  const auto session = make_session(parent, "write-failure-session");
  CHECK(std::filesystem::exists(session / "data_0000.mcap.partial"));
  CHECK(!std::filesystem::exists(session / "data_0000.mcap"));
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("state").as_string() == "failed");
  const auto& file = manifest.at("files")[0];
  CHECK(file.at("path").as_string() == "data_0000.mcap.partial");
  CHECK(file.at("state").as_string() == "partial");
  CHECK(file.at("bytes").as_u64() ==
        std::filesystem::file_size(session / "data_0000.mcap.partial"));
  CHECK(file.at("message_count").is_null());
  CHECK(file.at("first_log_time_ns").is_null());
  CHECK(file.at("last_log_time_ns").is_null());
  CHECK(file.at("timing").is_null());
  const auto& statistics = manifest.at("statistics");
  CHECK(statistics.at("accepted").as_u64() == 3);
  CHECK(statistics.at("rejected").as_u64() == 0);
  CHECK(statistics.at("writer_processed").as_u64() == 0);
  CHECK(statistics.at("finalized_messages").is_null());
  CHECK(manifest.at("diagnostics").size() >= 1);
  bool found_writer_error = false;
  for (std::size_t index = 0; index < manifest.at("diagnostics").size(); ++index) {
    if (manifest.at("diagnostics")[index].at("code").as_string() == "writer_io_error") {
      found_writer_error = true;
      CHECK(manifest.at("diagnostics")[index].at("log_time_ns").as_string() == "1");
      CHECK(manifest.at("diagnostics")[index].at("topic").as_string() == "/failure");
      CHECK(manifest.at("diagnostics")[index].at("action").as_string() ==
            "retain_partial_and_reject_new_messages");
    }
  }
  CHECK(found_writer_error);
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(4))).error ==
        SubmitError::WriterFailed);
}

void test_failure_while_stopping(const std::filesystem::path& root) {
  const auto parent = root / "failure-while-stopping";
  reset_directory(parent);
  auto file_system = std::make_shared<TestFileSystem>();
  auto gate = std::make_shared<Gate>();
  file_system->sink_control->gate = gate;
  RecorderOptions options = base_options(parent, "failure-while-stopping-session");
  options.file_system = file_system;
  options.file_options.chunk_size_bytes = 1;

  Recorder recorder(options);
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/failure"), channel));

  gate->enable();
  GateRelease release(*gate);
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 1, 1)).ok());
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(2), 2, 2)).ok());
  gate->wait_until_blocked();

  auto future = recorder.stop_async();
  file_system->sink_control->fail_write.store(true);
  gate->release();
  const auto result = future.get();
  CHECK(result.file_state == FileState::Partial);
  CHECK(result.reason == StopReason::WriterFailure);
  CHECK(!result.error.ok());

  const auto session = make_session(parent, "failure-while-stopping-session");
  CHECK(std::filesystem::exists(session / "manifest.json"));
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("state").as_string() == "failed");
  const auto& statistics = manifest.at("statistics");
  CHECK(statistics.at("accepted").as_u64() == 2);
  CHECK(statistics.at("writer_processed").as_u64() == 0);
  CHECK(statistics.at("finalized_messages").is_null());
  const auto calls = file_system->calls();
  CHECK(std::count(calls.begin(), calls.end(),
                   "rename:manifest.json.tmp->manifest.json") == 1);
}

void test_finish_failure(const std::filesystem::path& root) {
  const auto parent = root / "finish-failure";
  reset_directory(parent);
  auto file_system = std::make_shared<TestFileSystem>();
  RecorderOptions options = base_options(parent, "finish-failure-session");
  options.file_system = file_system;
  options.file_options.chunk_size_bytes = 1;

  Recorder recorder(options);
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/failure"), channel));
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 1, 1)).ok());
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(2), 2, 2)).ok());
  file_system->sink_control->fail_close.store(true);

  const auto result = recorder.stop();
  CHECK(result.file_state == FileState::Partial);
  CHECK(result.reason == StopReason::WriterFailure);
  CHECK(!result.error.ok());
  CHECK(result.error.operation == "finish");

  const auto session = make_session(parent, "finish-failure-session");
  CHECK(std::filesystem::exists(session / "data_0000.mcap.partial"));
  CHECK(!std::filesystem::exists(session / "data_0000.mcap"));
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("state").as_string() == "failed");
  CHECK(manifest.at("files")[0].at("state").as_string() == "partial");
  CHECK(manifest.at("files")[0].at("message_count").is_null());
  CHECK(manifest.at("statistics").at("writer_processed").as_u64() == 2);
  CHECK(manifest.at("statistics").at("finalized_messages").is_null());
}

void test_sync_modes(const std::filesystem::path& root) {
  const auto parent = root / "sync";
  reset_directory(parent);
  {
    RecorderOptions options = base_options(parent, "sync-success");
    options.durability = DurabilityMode::Sync;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/sync"), channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 1, 1)).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.durability_state == DurabilityState::Confirmed);
    CHECK(result.reason == StopReason::Requested);
    CHECK(result.error.ok());
    const auto session = make_session(parent, "sync-success");
    const auto manifest = parse_json(read_file(session / "manifest.json"));
    CHECK(manifest.at("durability").at("requested_mode").as_string() == "sync");
    CHECK(manifest.at("durability").at("confirmation").as_string() == "see_stop_result");
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);
  }

  {
    auto file_system = std::make_shared<TestFileSystem>();
    file_system->on_sync_file = [](const std::filesystem::path& path) {
      if (path.filename() == "data_0000.mcap") {
        throw std::runtime_error("injected sync failure");
      }
    };
    RecorderOptions options = base_options(parent, "sync-file-failure");
    options.durability = DurabilityMode::Sync;
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/sync"), channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 1, 1)).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.durability_state == DurabilityState::Failed);
    CHECK(result.reason == StopReason::Requested);
    CHECK(!result.error.ok());
    CHECK(result.error.operation == "sync_file");
    const auto session = make_session(parent, "sync-file-failure");
    const auto manifest = parse_json(read_file(session / "manifest.json"));
    CHECK(manifest.at("state").as_string() == "finalized");
    CHECK(manifest.at("files")[0].at("state").as_string() == "finalized");
    CHECK(manifest.at("durability").at("confirmation").as_string() == "see_stop_result");
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);
  }

  {
    auto file_system = std::make_shared<TestFileSystem>();
    const auto session = make_session(parent, "sync-directory-failure");
    auto native = robot_mcap::native_file_system();
    file_system->on_sync_directory = [session, native](const std::filesystem::path& path) {
      if (path == session) {
        throw std::runtime_error("injected directory sync failure");
      }
      native->sync_directory(path);
    };
    RecorderOptions options = base_options(parent, "sync-directory-failure");
    options.durability = DurabilityMode::Sync;
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/sync"), channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 1, 1)).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.durability_state == DurabilityState::Failed);
    CHECK(result.reason == StopReason::Requested);
    CHECK(!result.error.ok());
    CHECK(result.error.operation == "sync_directory");
    CHECK(std::filesystem::exists(session / "manifest.json"));
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);

    const auto calls = file_system->calls();
    const auto data_sync = call_index(calls, "sync_file:data_0000.mcap");
    const auto manifest_sync = call_index(calls, "sync_file:manifest.json.tmp");
    const auto manifest_rename = call_index(calls, "rename:manifest.json.tmp->manifest.json");
    const auto directory_sync = call_index(calls, "sync_directory:" + session.filename().string());
    CHECK(data_sync < manifest_sync);
    CHECK(manifest_sync < manifest_rename);
    CHECK(manifest_rename < directory_sync);
  }
}

std::vector<std::string> timing_lines(const std::filesystem::path& path) {
  const auto text = read_file(path);
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    if (end == std::string::npos) {
      throw std::runtime_error("timing sidecar line is not LF terminated");
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

void test_timing_sidecar(const std::filesystem::path& root) {
  const auto parent = root / "timing";
  reset_directory(parent);
  RecorderOptions options = base_options(parent, "timing-session");
  Recorder recorder(options);
  CHECK_OK(recorder.start());

  ChannelId mixed = 0;
  CHECK_OK(recorder.register_channel(
      {"/timing/mixed", "raw", std::nullopt, PublishTimePolicy::PerMessage}, mixed));
  ChannelId log_only = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/timing/log"), log_only));
  ChannelId source_required = 0;
  CHECK_OK(recorder.register_channel(
      {"/timing/source", "raw", std::nullopt, PublishTimePolicy::SourceRequired},
      source_required));

  const std::uint64_t log0 = 1000;
  const std::uint64_t publish0 = 900;
  SourceClock imu_clock{"imu-01", 4107, "tick", "sample", "device_boot"};
  CHECK(recorder
            .submit_copy(mixed,
                         MessageView{log0, publish0, nullptr, 0, 0, imu_clock})
            .ok());
  CHECK(recorder
            .submit_copy(mixed, MessageView{log0 + 1, std::nullopt, nullptr, 0, 0, std::nullopt})
            .ok());
  // A genuine publish time equal to log_time is not a fallback on a per_message channel.
  CHECK(recorder
            .submit_copy(mixed, MessageView{log0 + 2, log0 + 2, nullptr, 0, 0, std::nullopt})
            .ok());
  SourceClock log_clock{"clock-\"2\"\\\n\t\x01", 99, "ns", "sample", "unix"};
  CHECK(recorder
            .submit_copy(log_only,
                         MessageView{log0 + 3, std::nullopt, nullptr, 0, 0, log_clock})
            .ok());
  SourceClock source_clock{"clock-3", 18446744073709551615ULL, "ms", "sample", "device_boot"};
  CHECK(recorder
            .submit_copy(source_required,
                         MessageView{log0 + 4, publish0 + 4, nullptr, 0, 0, source_clock})
            .ok());

  SourceClock invalid_clock = source_clock;
  invalid_clock.unit = "seconds";
  CHECK(recorder
            .submit_copy(source_required,
                         MessageView{log0 + 5, publish0 + 5, nullptr, 0, 0, invalid_clock})
            .error == SubmitError::InvalidArgument);
  CHECK(recorder
            .submit_copy(source_required,
                         MessageView{log0 + 5, std::nullopt, nullptr, 0, 0, std::nullopt})
            .error == SubmitError::InvalidArgument);
  CHECK(recorder
            .submit_copy(log_only, MessageView{log0 + 5, publish0 + 5, nullptr, 0, 0,
                                               std::nullopt})
            .error == SubmitError::InvalidArgument);
  SourceClock invalid_utf8 = source_clock;
  invalid_utf8.clock_id = "\x80";
  CHECK(recorder
            .submit_copy(source_required,
                         MessageView{log0 + 5, publish0 + 5, nullptr, 0, 0, invalid_utf8})
            .error == SubmitError::InvalidArgument);

  const auto result = recorder.stop();
  CHECK(result.ok());
  const auto session = make_session(parent, "timing-session");
  const auto lines = timing_lines(session / "data_0000.timing.ndjson");
  CHECK(lines.size() == 4);

  const auto row0 = parse_json(lines[0]);
  CHECK(row0.at("message_ordinal").as_u64() == 0);
  CHECK(row0.at("channel_id").as_u64() == 1);
  CHECK(!row0.at("publish_time_fallback").as_bool());
  CHECK(row0.at("source_clock").at("clock_id").as_string() == "imu-01");
  CHECK(row0.at("source_clock").at("value").as_string() == "4107");
  CHECK(row0.at("source_clock").at("unit").as_string() == "tick");
  CHECK(row0.at("source_clock").at("meaning").as_string() == "sample");
  CHECK(row0.at("source_clock").at("epoch").as_string() == "device_boot");

  const auto row1 = parse_json(lines[1]);
  CHECK(row1.at("message_ordinal").as_u64() == 1);
  CHECK(row1.at("channel_id").as_u64() == 1);
  CHECK(row1.at("publish_time_fallback").as_bool());
  CHECK(row1.at("source_clock").is_null());

  // The third mixed-channel message needed no row but still consumed ordinal 2.
  const auto row2 = parse_json(lines[2]);
  CHECK(row2.at("message_ordinal").as_u64() == 3);
  CHECK(row2.at("channel_id").as_u64() == 2);
  CHECK(row2.at("publish_time_fallback").as_bool());
  CHECK(row2.at("source_clock").at("clock_id").as_string() == "clock-\"2\"\\\n\t\x01");
  CHECK(row2.at("source_clock").at("value").as_string() == "99");
  CHECK(row2.at("source_clock").at("unit").as_string() == "ns");

  const auto row3 = parse_json(lines[3]);
  CHECK(row3.at("message_ordinal").as_u64() == 4);
  CHECK(row3.at("channel_id").as_u64() == 3);
  CHECK(!row3.at("publish_time_fallback").as_bool());
  CHECK(row3.at("source_clock").at("clock_id").as_string() == "clock-3");
  CHECK(row3.at("source_clock").at("value").as_string() == "18446744073709551615");
  CHECK(row3.at("source_clock").at("unit").as_string() == "ms");

  const auto manifest = parse_json(read_file(session / "manifest.json"));
  const auto& timing = manifest.at("files")[0].at("timing");
  CHECK(timing.at("path").as_string() == "data_0000.timing.ndjson");
  CHECK(timing.at("row_count").as_u64() == 4);
  CHECK(timing.at("sha256").as_string() ==
        robot_mcap::sha256_hex(read_file(session / "data_0000.timing.ndjson")));
  CHECK(timing.at("state").as_string() == "finalized");
  CHECK(manifest.at("statistics").at("accepted").as_u64() == 5);
  CHECK(manifest.at("statistics").at("rejected").as_u64() == 0);
  CHECK(!std::filesystem::exists(session / "data_0000.timing.ndjson.partial"));

  const auto messages = read_messages(session / "data_0000.mcap");
  CHECK(messages.size() == 5);
  CHECK(messages[0].topic == "/timing/mixed");
  CHECK(messages[1].topic == "/timing/mixed");
  CHECK(messages[2].topic == "/timing/mixed");
  CHECK(messages[3].topic == "/timing/log");
  CHECK(messages[4].topic == "/timing/source");
  CHECK(messages[1].publish_time == log0 + 1);
  CHECK(messages[1].log_time == log0 + 1);
  CHECK(messages[0].publish_time == publish0);
  CHECK(messages[2].publish_time == messages[2].log_time);
  CHECK(messages[3].publish_time == log0 + 3);

  // No sidecar is created when no per-message evidence is required.
  const auto none_parent = root / "timing-none";
  reset_directory(none_parent);
  Recorder plain(base_options(none_parent, "timing-none-session"));
  CHECK_OK(plain.start());
  ChannelId plain_channel = 0;
  CHECK_OK(plain.register_channel(raw_channel("/plain"), plain_channel));
  CHECK(plain.submit_owned(plain_channel, owned_message(sequence_payload(1))).ok());
  CHECK(plain.stop().ok());
  const auto plain_session = make_session(none_parent, "timing-none-session");
  CHECK(!std::filesystem::exists(plain_session / "data_0000.timing.ndjson"));
  CHECK(!std::filesystem::exists(plain_session / "data_0000.timing.ndjson.partial"));
  const auto plain_manifest = parse_json(read_file(plain_session / "manifest.json"));
  CHECK(plain_manifest.at("files")[0].at("timing").is_null());
}

void test_instances_are_isolated(const std::filesystem::path& root) {
  const auto parent = root / "instances";
  reset_directory(parent);
  auto failing_file_system = std::make_shared<TestFileSystem>();
  auto failing_gate = std::make_shared<Gate>();
  failing_file_system->sink_control->gate = failing_gate;

  RecorderOptions failing_options = base_options(parent, "instance-failing");
  failing_options.file_system = failing_file_system;
  failing_options.file_options.chunk_size_bytes = 1;
  RecorderOptions healthy_options = base_options(parent, "instance-healthy");
  healthy_options.file_options.chunk_size_bytes = 1;

  Recorder failing(failing_options);
  Recorder healthy(healthy_options);
  CHECK_OK(failing.start());
  CHECK_OK(healthy.start());
  ChannelId failing_channel = 0;
  ChannelId healthy_channel = 0;
  CHECK_OK(failing.register_channel(raw_channel("/failing"), failing_channel));
  CHECK_OK(healthy.register_channel(raw_channel("/healthy"), healthy_channel));

  // Queue both failing-instance messages before the first write fails. The healthy
  // instance is independent and must be unaffected.
  failing_gate->enable();
  GateRelease release(*failing_gate);
  CHECK(failing.submit_owned(failing_channel, owned_message(sequence_payload(1), 1, 1)).ok());
  failing_gate->wait_until_blocked();
  CHECK(failing.submit_owned(failing_channel, owned_message(sequence_payload(3), 3, 3)).ok());
  CHECK(healthy.submit_owned(healthy_channel, owned_message(sequence_payload(2), 2, 2)).ok());
  CHECK(healthy.submit_owned(healthy_channel, owned_message(sequence_payload(4), 4, 4)).ok());
  failing_file_system->sink_control->fail_write.store(true);
  failing_gate->release();

  const auto failing_result = failing.stop();
  const auto healthy_result = healthy.stop();
  CHECK(failing_result.file_state == FileState::Partial);
  CHECK(healthy_result.file_state == FileState::Finalized);
  CHECK(healthy_result.error.ok());

  const auto healthy_session = make_session(parent, "instance-healthy");
  const auto healthy_messages = read_messages(healthy_session / "data_0000.mcap");
  CHECK(healthy_messages.size() == 2);
  CHECK(payload_sequence(healthy_messages[0].data) == 2);
  CHECK(payload_sequence(healthy_messages[1].data) == 4);
  const auto healthy_manifest = parse_json(read_file(healthy_session / "manifest.json"));
  CHECK(healthy_manifest.at("state").as_string() == "finalized");
  CHECK(healthy_manifest.at("statistics").at("accepted").as_u64() == 2);
  CHECK(healthy_manifest.at("statistics").at("finalized_messages").as_u64() == 2);

  const auto failing_session = make_session(parent, "instance-failing");
  const auto failing_manifest = parse_json(read_file(failing_session / "manifest.json"));
  CHECK(failing_manifest.at("state").as_string() == "failed");
  CHECK(failing_manifest.at("statistics").at("finalized_messages").is_null());
}

void test_destructor_finalizes(const std::filesystem::path& root) {
  const auto parent = root / "destructor";
  reset_directory(parent);
  {
    Recorder recorder(base_options(parent, "destructor-session"));
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/destructor"), channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1))).ok());
  }
  const auto session = make_session(parent, "destructor-session");
  CHECK(std::filesystem::exists(session / "manifest.json"));
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("state").as_string() == "finalized");
  CHECK(read_messages(session / "data_0000.mcap").size() == 1);
}

void test_out_of_order_and_edge_payloads(const std::filesystem::path& root) {
  const auto parent = root / "edge-payloads";
  reset_directory(parent);
  Recorder recorder(base_options(parent, "edge-session"));
  CHECK_OK(recorder.start());

  ChannelId raw = 0;
  ChannelId source = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/edge/raw"), raw));
  CHECK_OK(recorder.register_channel(
      {"/edge/source", "raw", std::nullopt, PublishTimePolicy::SourceRequired}, source));

  const auto letter = bytes_of("a");
  CHECK(recorder
            .submit_copy(raw, MessageView{100, std::nullopt, letter.data(), letter.size(), 0,
                                          std::nullopt})
            .ok());
  CHECK(recorder.submit_copy(raw, MessageView{0, std::nullopt, nullptr, 0, 0, std::nullopt})
            .ok());
  const std::vector<std::byte> binary{std::byte{0x00}, std::byte{0x01}, std::byte{0x7F},
                                      std::byte{0x80}, std::byte{0xFF}};
  CHECK(recorder
            .submit_copy(raw, MessageView{300, std::nullopt, binary.data(), binary.size(), 0,
                                          std::nullopt})
            .ok());
  CHECK(recorder
            .submit_copy(raw, MessageView{200, std::nullopt, letter.data(), letter.size(), 0,
                                          std::nullopt})
            .ok());
  const auto published = bytes_of("b");
  CHECK(recorder
            .submit_copy(source, MessageView{400, 400, published.data(), published.size(), 9,
                                             std::nullopt})
            .ok());

  const auto result = recorder.stop();
  CHECK(result.ok());
  const auto session = make_session(parent, "edge-session");
  const auto messages = read_messages(session / "data_0000.mcap");
  CHECK(messages.size() == 5);
  CHECK(messages[0].log_time == 100 && messages[0].publish_time == 100);
  CHECK(messages[1].log_time == 0 && messages[1].publish_time == 0);
  CHECK(messages[1].data.empty());
  CHECK(messages[2].log_time == 300);
  CHECK(messages[2].data == binary);
  CHECK(messages[3].log_time == 200);
  CHECK(messages[4].log_time == 400 && messages[4].publish_time == 400);
  CHECK(messages[4].sequence == 9);
  CHECK(messages[4].topic == "/edge/source");

  const auto manifest = parse_json(read_file(session / "manifest.json"));
  const auto& file = manifest.at("files")[0];
  CHECK(file.at("message_count").as_u64() == 5);
  CHECK(file.at("first_log_time_ns").as_string() == "0");
  CHECK(file.at("last_log_time_ns").as_string() == "400");
  CHECK(file.at("timing").is_null());
  CHECK(manifest.at("statistics").at("finalized_messages").as_u64() == 5);
}

void test_unused_channel(const std::filesystem::path& root) {
  const auto parent = root / "unused-channel";
  reset_directory(parent);
  Recorder recorder(base_options(parent, "unused-channel-session"));
  CHECK_OK(recorder.start());
  ChannelId used = 0;
  ChannelId unused = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/used"), used));
  CHECK_OK(recorder.register_channel(raw_channel("/unused"), unused));
  CHECK(recorder.submit_owned(used, owned_message(sequence_payload(1))).ok());
  CHECK(recorder.stop().ok());

  const auto session = make_session(parent, "unused-channel-session");
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("channels").size() == 2);
  CHECK(manifest.at("channels")[0].at("topic").as_string() == "/used");
  CHECK(manifest.at("channels")[1].at("topic").as_string() == "/unused");
  CHECK(read_messages(session / "data_0000.mcap").size() == 1);
}

void test_concurrent_registration(const std::filesystem::path& root) {
  const auto parent = root / "concurrent-registration";
  reset_directory(parent);
  Recorder recorder(base_options(parent, "concurrent-registration-session"));
  CHECK_OK(recorder.start());

  const ChannelSpec spec = raw_channel("/concurrent");
  auto barrier = std::make_shared<Barrier>(2);
  std::array<Status, 2> statuses;
  std::array<ChannelId, 2> ids{};
  std::vector<std::thread> threads;
  for (int index = 0; index < 2; ++index) {
    threads.emplace_back([&, index] {
      barrier->arrive_and_wait();
      ChannelId id = 0;
      statuses[index] = recorder.register_channel(spec, id);
      ids[index] = id;
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(statuses[0].ok() && statuses[1].ok());
  CHECK(ids[0] == ids[1]);
  CHECK(ids[0] != 0);
  CHECK(recorder.submit_owned(ids[0], owned_message(sequence_payload(1))).ok());
  CHECK(recorder.stop().ok());

  const auto session = make_session(parent, "concurrent-registration-session");
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("channels").size() == 1);
  CHECK(read_messages(session / "data_0000.mcap").size() == 1);
}

void test_concurrent_same_session(const std::filesystem::path& root) {
  const auto parent = root / "same-session";
  reset_directory(parent);
  std::array<std::unique_ptr<Recorder>, 2> recorders;
  std::array<Status, 2> starts;
  auto barrier = std::make_shared<Barrier>(2);
  std::vector<std::thread> threads;
  for (int index = 0; index < 2; ++index) {
    threads.emplace_back([&, index] {
      recorders[index] =
          std::make_unique<Recorder>(base_options(parent, "same-session"));
      barrier->arrive_and_wait();
      starts[index] = recorders[index]->start();
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const int successes =
      (starts[0].ok() ? 1 : 0) + (starts[1].ok() ? 1 : 0);
  CHECK(successes == 1);
  for (int index = 0; index < 2; ++index) {
    if (starts[index].ok()) {
      CHECK(recorders[index]->stop().ok());
    }
  }
  recorders[0].reset();
  recorders[1].reset();
  CHECK(std::filesystem::exists(parent / "same-session" / "manifest.json"));
}

void test_stop_while_parked(const std::filesystem::path& root) {
  const auto parent = root / "stop-parked";
  reset_directory(parent);
  auto file_system = std::make_shared<TestFileSystem>();
  auto gate = std::make_shared<Gate>();
  file_system->sink_control->gate = gate;
  RecorderOptions options = base_options(parent, "stop-parked-session");
  options.file_system = file_system;
  options.file_options.chunk_size_bytes = 1;

  Recorder recorder(options);
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/parked"), channel));

  gate->enable();
  GateRelease release(*gate);
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1))).ok());
  gate->wait_until_blocked();

  auto future = recorder.stop_async();
  CHECK(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(2))).error ==
        SubmitError::Stopped);

  gate->release();
  const auto result = future.get();
  CHECK(result.ok());
  const auto session = make_session(parent, "stop-parked-session");
  CHECK(read_messages(session / "data_0000.mcap").size() == 1);
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("statistics").at("accepted").as_u64() == 1);
  CHECK(manifest.at("statistics").at("rejected").as_u64() == 1);
  CHECK(manifest.at("statistics").at("finalized_messages").as_u64() == 1);
}

void test_start_failure_cleanup(const std::filesystem::path& root) {
  {
    const auto parent = root / "start-failure" / "config";
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    file_system->on_create = [](const std::filesystem::path& path) {
      if (path.filename() == "recording-config.json.tmp") {
        throw std::runtime_error("injected config failure");
      }
    };
    RecorderOptions options = base_options(parent, "config-failure");
    options.file_system = file_system;
    Recorder recorder(options);
    CHECK(!recorder.start().ok());
    CHECK(!std::filesystem::exists(parent / "config-failure"));
    const auto stopped = recorder.stop();
    CHECK(stopped.reason == StopReason::StartFailure);
    CHECK(stopped.file_state == FileState::Unknown);
    CHECK(stopped.durability_state == DurabilityState::NotRequested);
    CHECK(stopped.error.operation == "start");
  }
  {
    const auto parent = root / "start-failure" / "data";
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    file_system->on_create = [](const std::filesystem::path& path) {
      if (path.filename() == "data_0000.mcap.partial") {
        throw std::runtime_error("injected data open failure");
      }
    };
    RecorderOptions options = base_options(parent, "data-failure");
    options.file_system = file_system;
    Recorder recorder(options);
    CHECK(!recorder.start().ok());
    CHECK(!std::filesystem::exists(parent / "data-failure"));
    CHECK(recorder.submit_owned(1, owned_message(sequence_payload(1))).error ==
          SubmitError::Stopped);
  }
}

void test_manifest_publication_failure(const std::filesystem::path& root) {
  {
    const auto parent = root / "manifest-failure" / "write";
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    file_system->on_create = [](const std::filesystem::path& path) {
      if (path.filename() == "manifest.json.tmp") {
        throw std::runtime_error("injected manifest write failure");
      }
    };
    RecorderOptions options = base_options(parent, "manifest-write-failure");
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/manifest"), channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1))).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.reason == StopReason::PublishFailure);
    CHECK(result.error.operation == "write_manifest");
    const auto session = make_session(parent, "manifest-write-failure");
    CHECK(!std::filesystem::exists(session / "manifest.json"));
    CHECK(!std::filesystem::exists(session / "manifest.json.tmp"));
    CHECK(std::filesystem::exists(session / "data_0000.mcap"));
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);
  }
  {
    const auto parent = root / "manifest-failure" / "rename";
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    file_system->on_rename = [](const std::filesystem::path& from) {
      if (from.filename() == "manifest.json.tmp") {
        throw std::runtime_error("injected manifest rename failure");
      }
    };
    RecorderOptions options = base_options(parent, "manifest-rename-failure");
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/manifest"), channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1))).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.reason == StopReason::PublishFailure);
    CHECK(result.error.operation == "rename_manifest");
    const auto session = make_session(parent, "manifest-rename-failure");
    CHECK(!std::filesystem::exists(session / "manifest.json"));
    CHECK(std::filesystem::exists(session / "manifest.json.tmp"));
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);
  }
}

void test_timing_failure(const std::filesystem::path& root) {
  const auto parent = root / "timing-failure";
  reset_directory(parent);
  auto file_system = std::make_shared<TestFileSystem>();
  file_system->timing_control = std::make_shared<SinkControl>();
  RecorderOptions options = base_options(parent, "timing-failure-session");
  options.file_system = file_system;
  options.file_options.chunk_size_bytes = 1;

  Recorder recorder(options);
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(
      {"/timing/failure", "raw", std::nullopt, PublishTimePolicy::PerMessage}, channel));
  file_system->timing_control->fail_on_write_call.store(2);
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 10, 1)).ok());
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(2), 20, 2)).ok());

  const auto result = recorder.stop();
  CHECK(result.file_state == FileState::Finalized);
  CHECK(result.reason == StopReason::TimingFailure);
  CHECK(!result.error.ok());
  CHECK(result.error.operation == "append_timing");

  const auto session = make_session(parent, "timing-failure-session");
  CHECK(std::filesystem::exists(session / "data_0000.mcap"));
  CHECK(std::filesystem::exists(session / "data_0000.timing.ndjson.partial"));
  CHECK(!std::filesystem::exists(session / "data_0000.timing.ndjson"));
  const auto messages = read_messages(session / "data_0000.mcap");
  CHECK(messages.size() == 2);

  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("state").as_string() == "failed");
  const auto& file = manifest.at("files")[0];
  CHECK(file.at("path").as_string() == "data_0000.mcap");
  CHECK(file.at("state").as_string() == "finalized");
  CHECK(file.at("message_count").as_u64() == 2);
  CHECK(file.at("first_log_time_ns").as_string() == "10");
  CHECK(file.at("last_log_time_ns").as_string() == "20");
  CHECK(manifest.at("statistics").at("finalized_messages").as_u64() == 2);
  const auto& timing = file.at("timing");
  CHECK(timing.at("path").as_string() == "data_0000.timing.ndjson.partial");
  CHECK(timing.at("row_count").as_u64() == 1);
  CHECK(timing.at("state").as_string() == "partial");
  CHECK(timing.at("sha256").as_string() ==
        robot_mcap::sha256_hex(read_file(session / "data_0000.timing.ndjson.partial")));
  const auto partial_lines =
      timing_lines(session / "data_0000.timing.ndjson.partial");
  CHECK(partial_lines.size() == 1);
  const auto partial_row = parse_json(partial_lines[0]);
  CHECK(partial_row.at("message_ordinal").as_u64() == 0);
  CHECK(partial_row.at("channel_id").as_u64() == 1);
  CHECK(partial_row.at("publish_time_fallback").as_bool());
  CHECK(partial_row.at("source_clock").is_null());
  CHECK(timing.at("row_count").as_u64() == partial_lines.size());

  bool found_timing_error = false;
  for (std::size_t index = 0; index < manifest.at("diagnostics").size(); ++index) {
    const auto& diagnostic = manifest.at("diagnostics")[index];
    if (diagnostic.at("code").as_string() == "timing_io_error") {
      found_timing_error = true;
      CHECK(diagnostic.at("log_time_ns").as_string() == "20");
      CHECK(diagnostic.at("topic").as_string() == "/timing/failure");
      CHECK(diagnostic.at("action").as_string() ==
            "retain_partial_and_reject_new_messages");
    }
  }
  CHECK(found_timing_error);
}

void test_timing_close_and_rename_failure(const std::filesystem::path& root) {
  {
    const auto parent = root / "timing-close-failure";
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    file_system->timing_control = std::make_shared<SinkControl>();
    file_system->timing_control->fail_close.store(true);
    RecorderOptions options = base_options(parent, "timing-close-failure-session");
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(
        {"/timing/close", "raw", std::nullopt, PublishTimePolicy::PerMessage}, channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 10, 1)).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.reason == StopReason::TimingFailure);
    CHECK(result.error.operation == "close_timing");
    const auto session = make_session(parent, "timing-close-failure-session");
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);
    const auto manifest = parse_json(read_file(session / "manifest.json"));
    CHECK(manifest.at("state").as_string() == "failed");
    const auto& timing = manifest.at("files")[0].at("timing");
    CHECK(timing.at("path").as_string() == "data_0000.timing.ndjson.partial");
    CHECK(timing.at("row_count").as_u64() == 1);
    CHECK(timing.at("state").as_string() == "partial");
    CHECK(!timing.at("sha256").is_null());
    const auto partial_lines =
        timing_lines(session / "data_0000.timing.ndjson.partial");
    CHECK(partial_lines.size() == 1);
    const auto partial_row = parse_json(partial_lines[0]);
    CHECK(partial_row.at("message_ordinal").as_u64() == 0);
    CHECK(partial_row.at("channel_id").as_u64() == 1);
    CHECK(partial_row.at("publish_time_fallback").as_bool());
    CHECK(partial_row.at("source_clock").is_null());
    CHECK(timing.at("row_count").as_u64() == partial_lines.size());
  }

  {
    const auto parent = root / "timing-rename-failure";
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    file_system->on_rename = [](const std::filesystem::path& from) {
      if (from.filename() == "data_0000.timing.ndjson.partial") {
        throw std::runtime_error("injected timing rename failure");
      }
    };
    RecorderOptions options = base_options(parent, "timing-rename-failure-session");
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(
        {"/timing/rename", "raw", std::nullopt, PublishTimePolicy::PerMessage}, channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 10, 1)).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.reason == StopReason::PublishFailure);
    CHECK(result.error.operation == "rename");
    const auto session = make_session(parent, "timing-rename-failure-session");
    CHECK(std::filesystem::exists(session / "data_0000.mcap"));
    CHECK(std::filesystem::exists(session / "data_0000.timing.ndjson.partial"));
    CHECK(!std::filesystem::exists(session / "data_0000.timing.ndjson"));
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);
    const auto manifest = parse_json(read_file(session / "manifest.json"));
    CHECK(manifest.at("state").as_string() == "failed");
    const auto& timing = manifest.at("files")[0].at("timing");
    CHECK(timing.at("path").as_string() == "data_0000.timing.ndjson.partial");
    CHECK(timing.at("row_count").as_u64() == 1);
    CHECK(timing.at("state").as_string() == "partial");
    CHECK(!timing.at("sha256").is_null());
    const auto partial_lines =
        timing_lines(session / "data_0000.timing.ndjson.partial");
    CHECK(partial_lines.size() == 1);
    const auto partial_row = parse_json(partial_lines[0]);
    CHECK(partial_row.at("message_ordinal").as_u64() == 0);
    CHECK(partial_row.at("channel_id").as_u64() == 1);
    CHECK(partial_row.at("publish_time_fallback").as_bool());
    CHECK(partial_row.at("source_clock").is_null());
    CHECK(timing.at("row_count").as_u64() == partial_lines.size());
  }
}

void test_timing_hash_failure(const std::filesystem::path& root) {
  const auto parent = root / "timing-hash-failure";
  reset_directory(parent);
  auto file_system = std::make_shared<TestFileSystem>();
  file_system->on_sha256 = [](const std::filesystem::path& path) {
    if (path.filename() == "data_0000.timing.ndjson") {
      throw std::runtime_error("injected hash failure");
    }
  };
  RecorderOptions options = base_options(parent, "timing-hash-failure-session");
  options.file_system = file_system;
  Recorder recorder(options);
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(
      {"/timing/hash", "raw", std::nullopt, PublishTimePolicy::PerMessage}, channel));
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 10, 1)).ok());

  const auto result = recorder.stop();
  CHECK(result.file_state == FileState::Finalized);
  CHECK(result.reason == StopReason::TimingFailure);
  CHECK(result.error.operation == "hash_timing");
  const auto session = make_session(parent, "timing-hash-failure-session");
  CHECK(read_messages(session / "data_0000.mcap").size() == 1);
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("state").as_string() == "failed");
  const auto& timing = manifest.at("files")[0].at("timing");
  CHECK(timing.at("path").as_string() == "data_0000.timing.ndjson");
  CHECK(timing.at("row_count").as_u64() == 1);
  CHECK(timing.at("sha256").is_null());
  CHECK(timing.at("state").as_string() == "unknown");
}

void test_timing_fixture(const std::filesystem::path& root,
                         const std::filesystem::path& fixture_root) {
  const auto parent = root / "timing-fixture";
  reset_directory(parent);
  Recorder recorder(base_options(parent, "timing-fixture-session"));
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(
      {"/sensor/temperature", "json", std::nullopt, PublishTimePolicy::PerMessage}, channel));
  const SourceClock clock{"imu-01", 4107, "tick", "sample", "device_boot"};
  const std::uint64_t log_time = 1794326400000000000ULL;
  CHECK(recorder
            .submit_copy(channel,
                         MessageView{log_time, log_time - 1, nullptr, 0, 0, clock})
            .ok());
  CHECK(recorder
            .submit_copy(channel,
                         MessageView{log_time + 100000000, std::nullopt, nullptr, 0, 0,
                                     std::nullopt})
            .ok());
  CHECK(recorder.stop().ok());

  const auto session = make_session(parent, "timing-fixture-session");
  const auto expected =
      read_file(fixture_root / "01-finalized" / "data_0000.timing.ndjson");
  const auto actual = read_file(session / "data_0000.timing.ndjson");
  CHECK(actual == expected);
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  const auto& timing = manifest.at("files")[0].at("timing");
  CHECK(timing.at("row_count").as_u64() == 2);
  CHECK(timing.at("sha256").as_string() == robot_mcap::sha256_hex(expected));
  CHECK(timing.at("state").as_string() == "finalized");
  const auto fixture_manifest =
      parse_json(read_file(fixture_root / "01-finalized" / "manifest.json"));
  CHECK(timing.at("sha256").as_string() ==
        fixture_manifest.at("files")[0].at("timing").at("sha256").as_string());
}

void test_stats_freeze(const std::filesystem::path& root) {
  const auto parent = root / "stats-freeze";
  reset_directory(parent);
  auto file_system = std::make_shared<TestFileSystem>();
  auto blocking = std::make_shared<BlockingPoint>();
  file_system->on_sync_file = [blocking](const std::filesystem::path& path) {
    if (path.filename() == "data_0000.mcap") {
      blocking->enter();
    }
  };
  RecorderOptions options = base_options(parent, "stats-freeze-session");
  options.durability = DurabilityMode::Sync;
  options.file_system = file_system;
  options.file_options.chunk_size_bytes = 1;

  Recorder recorder(options);
  CHECK_OK(recorder.start());
  ChannelId channel = 0;
  CHECK_OK(recorder.register_channel(raw_channel("/freeze"), channel));
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1))).ok());

  auto future = recorder.stop_async();
  BlockingRelease release(*blocking);
  blocking->wait_entered();
  CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(2))).error ==
        SubmitError::Stopped);
  blocking->release();
  const auto result = future.get();
  CHECK(result.ok());
  CHECK(result.durability_state == DurabilityState::Confirmed);

  const auto session = make_session(parent, "stats-freeze-session");
  const auto manifest = parse_json(read_file(session / "manifest.json"));
  CHECK(manifest.at("statistics").at("accepted").as_u64() == 1);
  CHECK(manifest.at("statistics").at("rejected").as_u64() == 0);
  CHECK(manifest.at("statistics").at("writer_processed").as_u64() == 1);
  CHECK(manifest.at("statistics").at("finalized_messages").as_u64() == 1);
  CHECK(read_messages(session / "data_0000.mcap").size() == 1);
}

void test_sync_targets(const std::filesystem::path& root) {
  const auto parent_root = root / "sync-targets";
  reset_directory(parent_root);
  const std::array<std::string, 2> targets{"recording-config.json", "manifest.json.tmp"};
  const std::array<std::string, 2> session_ids{"sync-config-target",
                                                "sync-manifest-target"};
  for (std::size_t index = 0; index < targets.size(); ++index) {
    const auto parent = parent_root / session_ids[index];
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    const std::string target = targets[index];
    file_system->on_sync_file = [target](const std::filesystem::path& path) {
      if (path.filename().string() == target) {
        throw std::runtime_error("injected sync failure");
      }
    };
    RecorderOptions options = base_options(parent, session_ids[index]);
    options.durability = DurabilityMode::Sync;
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/sync"), channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1))).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.durability_state == DurabilityState::Failed);
    CHECK(result.error.operation == "sync_file");
    CHECK(result.error.path.filename().string() == target);
    const auto session = make_session(parent, session_ids[index]);
    CHECK(std::filesystem::exists(session / "manifest.json"));
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);
  }

  {
    const auto parent = parent_root / "sync-timing-target";
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    file_system->on_sync_file = [](const std::filesystem::path& path) {
      if (path.filename() == "data_0000.timing.ndjson") {
        throw std::runtime_error("injected timing sync failure");
      }
    };
    RecorderOptions options = base_options(parent, "sync-timing-target");
    options.durability = DurabilityMode::Sync;
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(
        {"/timing/sync", "raw", std::nullopt, PublishTimePolicy::PerMessage}, channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 10, 1)).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.durability_state == DurabilityState::Failed);
    CHECK(result.error.operation == "sync_file");
    CHECK(result.error.path.filename() == "data_0000.timing.ndjson");
    const auto session = make_session(parent, "sync-timing-target");
    const auto manifest = parse_json(read_file(session / "manifest.json"));
    CHECK(manifest.at("files")[0].at("timing").at("state").as_string() == "finalized");
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);
  }

  {
    const auto parent = parent_root / "sync-order-parent";
    reset_directory(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    RecorderOptions options = base_options(parent, "sync-order-session");
    options.durability = DurabilityMode::Sync;
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(
        {"/sync/order", "raw", std::nullopt, PublishTimePolicy::PerMessage}, channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1), 10, 1)).ok());
    const auto result = recorder.stop();
    CHECK(result.ok());
    CHECK(result.durability_state == DurabilityState::Confirmed);

    const auto calls = file_system->calls();
    const auto data_sync = call_index(calls, "sync_file:data_0000.mcap");
    const auto timing_sync = call_index(calls, "sync_file:data_0000.timing.ndjson");
    const auto config_sync = call_index(calls, "sync_file:recording-config.json");
    const auto manifest_temp_sync = call_index(calls, "sync_file:manifest.json.tmp");
    const auto manifest_rename =
        call_index(calls, "rename:manifest.json.tmp->manifest.json");
    const auto session = make_session(parent, "sync-order-session");
    const auto session_sync =
        call_index(calls, "sync_directory:" + session.filename().string());
    CHECK(data_sync < timing_sync);
    CHECK(timing_sync < config_sync);
    CHECK(config_sync < manifest_temp_sync);
    CHECK(manifest_temp_sync < manifest_rename);
    CHECK(manifest_rename < session_sync);
    const auto absolute_parent = std::filesystem::absolute(parent);
    std::size_t last_parent_sync = calls.size();
    for (std::size_t index = 0; index < calls.size(); ++index) {
      if (calls[index] ==
          "sync_directory:" + absolute_parent.filename().string()) {
        last_parent_sync = index;
      }
    }
    CHECK(manifest_rename < last_parent_sync);
  }

  {
    const auto parent = parent_root / "sync-parent-target";
    reset_directory(parent);
    const auto absolute_parent = std::filesystem::absolute(parent);
    auto file_system = std::make_shared<TestFileSystem>();
    auto parent_syncs = std::make_shared<std::atomic<int>>(0);
    file_system->on_sync_directory = [absolute_parent, parent_syncs](
                                          const std::filesystem::path& path) {
      if (path == absolute_parent && parent_syncs->fetch_add(1) + 1 == 2) {
        // The first parent sync happens at session creation; fail the post-publish one.
        throw std::runtime_error("injected parent sync failure");
      }
    };
    RecorderOptions options = base_options(parent, "sync-parent-target-session");
    options.durability = DurabilityMode::Sync;
    options.file_system = file_system;
    options.file_options.chunk_size_bytes = 1;
    Recorder recorder(options);
    CHECK_OK(recorder.start());
    ChannelId channel = 0;
    CHECK_OK(recorder.register_channel(raw_channel("/sync"), channel));
    CHECK(recorder.submit_owned(channel, owned_message(sequence_payload(1))).ok());
    const auto result = recorder.stop();
    CHECK(result.file_state == FileState::Finalized);
    CHECK(result.durability_state == DurabilityState::Failed);
    CHECK(result.error.operation == "sync_directory");
    CHECK(result.error.path == absolute_parent);
    CHECK(parent_syncs->load() == 2);
    const auto session = make_session(parent, "sync-parent-target-session");
    CHECK(std::filesystem::exists(session / "manifest.json"));
    CHECK(read_messages(session / "data_0000.mcap").size() == 1);

    const auto calls = file_system->calls();
    const auto manifest_rename =
        call_index(calls, "rename:manifest.json.tmp->manifest.json");
    std::size_t last_parent_sync = calls.size();
    for (std::size_t index = 0; index < calls.size(); ++index) {
      if (calls[index] ==
          "sync_directory:" + absolute_parent.filename().string()) {
        last_parent_sync = index;
      }
    }
    CHECK(manifest_rename < last_parent_sync);
    CHECK(manifest_rename < call_index(
                                calls, "sync_directory:" + session.filename().string()));
  }
}

void test_sha256_vectors() {
  CHECK(robot_mcap::sha256_hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(robot_mcap::sha256_hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // Multi-block input: exercises state chaining and padding past one block.
  CHECK(robot_mcap::sha256_hex(std::string(64, 'a')) ==
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2 && argc != 3) {
      std::cerr << "expected a test output directory and an optional fixture directory\n";
      return 1;
    }
    const std::filesystem::path root = argv[1];
    const std::filesystem::path fixture_root =
        argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path();
    const auto test_root = root / "recorder_lifecycle";
    reset_directory(test_root);
    test_sha256_vectors();
    test_finalized_session(test_root);
    test_empty_session(test_root);
    test_session_validation(test_root);
    test_out_of_order_and_edge_payloads(test_root);
    test_unused_channel(test_root);
    test_concurrent_registration(test_root);
    test_concurrent_same_session(test_root);
    test_queue_full(test_root);
    test_stop_race(test_root);
    test_stop_while_parked(test_root);
    test_stats_freeze(test_root);
    test_write_failure(test_root);
    test_failure_while_stopping(test_root);
    test_finish_failure(test_root);
    test_start_failure_cleanup(test_root);
    test_manifest_publication_failure(test_root);
    test_sync_modes(test_root);
    test_sync_targets(test_root);
    test_timing_sidecar(test_root);
    test_timing_failure(test_root);
    test_timing_close_and_rename_failure(test_root);
    test_timing_hash_failure(test_root);
    if (argc == 3) {
      test_timing_fixture(test_root, fixture_root);
    }
    test_instances_are_isolated(test_root);
    test_destructor_finalizes(test_root);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
