#include "robot_mcap/mcap_file.hpp"

#include <mcap/reader.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* detail) {
  if (!condition) {
    throw std::runtime_error(detail);
  }
}

void require(const robot_mcap::Status& status) {
  if (!status.ok()) {
    throw std::runtime_error(status.operation + ": " + status.detail);
  }
}

void test_roundtrip(const std::filesystem::path& directory) {
  using namespace robot_mcap;
  const auto path = directory / "roundtrip.mcap";
  std::filesystem::remove(path);
  McapFile writer;
  require(writer.open(path));
  constexpr std::string_view schema_data = R"({"type":"object"})";
  constexpr std::string_view payload = R"({"celsius":23.5})";
  const ChannelSpec temperature{
    "/sensor/temperature", "json",
    SchemaSpec{"Temperature", "jsonschema", std::string(schema_data)},
    PublishTimePolicy::SourceRequired,
  };
  ChannelId temperature_id = 0;
  require(writer.register_channel(temperature, temperature_id));
  ChannelId duplicate_id = 0;
  require(writer.register_channel(temperature, duplicate_id));
  require(duplicate_id == temperature_id, "identical channel must reuse its ID");
  auto changed = temperature;
  changed.message_encoding = "raw";
  require(!writer.register_channel(changed, duplicate_id).ok(),
          "changed encoding must not reuse the topic");

  ChannelId raw_id = 0;
  require(writer.register_channel({"/sensor/raw", "raw", std::nullopt}, raw_id));
  constexpr std::uint64_t log_time = 1794326400000000000ULL;
  constexpr std::uint64_t publish_time = 1794326399999999999ULL;
  const auto* data = reinterpret_cast<const std::byte*>(payload.data());
  require(!writer.write(temperature_id, {log_time, std::nullopt, data, payload.size()}).ok(),
          "source-required channel accepted missing publish time");
  require(writer.write(temperature_id, {log_time, publish_time, data, payload.size(), 42}));
  const std::array raw{std::byte{0x00}, std::byte{0xff}};
  require(!writer.write(raw_id, {log_time + 1, publish_time, raw.data(), raw.size()}).ok(),
          "log-only channel accepted source publish time");
  require(writer.write(raw_id, {log_time + 1, std::nullopt, raw.data(), raw.size()}));
  require(writer.finish());

  mcap::McapReader reader;
  require(reader.open(path.string()).ok(), "upstream Reader could not open the MCAP");
  require(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok(), "summary missing");
  require(reader.chunkIndexes().size() == 1, "indexed chunk missing");
  require(reader.chunkIndexes().front().messageIndexOffsets.size() == 2,
          "per-channel message index missing");
  require(reader.chunkIndexes().front().compression.empty(), "default chunk was compressed");

  auto messages = reader.readMessages();
  auto it = messages.begin();
  require(it != messages.end(), "temperature message missing");
  require(it->channel->topic == "/sensor/temperature" && it->channel->messageEncoding == "json",
          "temperature channel changed");
  require(it->schema && it->schema->name == "Temperature" && it->schema->encoding == "jsonschema",
          "schema description changed");
  require(std::string_view(reinterpret_cast<const char*>(it->schema->data.data()),
                           it->schema->data.size()) == schema_data, "schema bytes changed");
  require(it->message.logTime == log_time && it->message.publishTime == publish_time,
          "nanosecond timestamps changed");
  require(it->message.sequence == 42, "source sequence changed");
  require(std::string_view(reinterpret_cast<const char*>(it->message.data),
                           it->message.dataSize) == payload, "JSON bytes changed");

  ++it;
  require(it != messages.end(), "raw message missing");
  require(it->channel->topic == "/sensor/raw" && it->channel->schemaId == 0 && !it->schema,
          "schemaless channel was changed");
  require(it->message.logTime == log_time + 1 && it->message.publishTime == log_time + 1,
          "publish time fallback changed");
  require(it->message.dataSize == raw.size() && it->message.data[0] == std::byte{0x00} &&
          it->message.data[1] == std::byte{0xff}, "opaque bytes changed");
  require(++it == messages.end() && messages.status().ok(), "unexpected messages or read error");
}

void test_existing_file(const std::filesystem::path& directory) {
  const auto path = directory / "roundtrip.mcap";
  const auto bytes_before = std::filesystem::file_size(path);
  robot_mcap::McapFile writer;
  const auto status = writer.open(path);
  require(!status.ok() && status.operation == "open" && status.path == path,
          "existing output was not rejected with a path");
  require(std::filesystem::file_size(path) == bytes_before, "existing output was overwritten");
}

void test_unfinished_file(const std::filesystem::path& directory) {
  const auto path = directory / "unfinished-test.mcap";
  std::filesystem::remove(path);
  {
    robot_mcap::McapFile writer;
    require(writer.open(path));
    robot_mcap::ChannelId id = 0;
    require(writer.register_channel({"/unfinished", "raw", std::nullopt}, id));
    require(!writer.write(id, {42, std::nullopt, nullptr, 1}).ok(),
            "null payload with a nonzero length was accepted");
    require(writer.write(id, {42, std::nullopt, nullptr, 0}));
  }
  mcap::McapReader reader;
  require(!reader.open(path.string()).ok() ||
          !reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok(),
          "destructor unexpectedly published a complete file");
}

void test_mcap_strings(const std::filesystem::path& directory) {
  const auto path = directory / "utf8-test.mcap";
  std::filesystem::remove(path);
  robot_mcap::McapFile writer;
  require(writer.open(path));
  robot_mcap::ChannelId id = 0;
  for (const auto* invalid : {"\x80", "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80",
                              "\xf4\x90\x80\x80", "\xe4\xb8"}) {
    require(!writer.register_channel({invalid, "raw", std::nullopt}, id).ok(),
            "invalid UTF-8 topic was accepted");
  }
  require(!writer.register_channel({"/raw", "\xff", std::nullopt}, id).ok(),
          "invalid UTF-8 message encoding was accepted");
  require(!writer.register_channel({"/raw", "raw", robot_mcap::SchemaSpec{"\xff", "raw", ""}},
                                   id).ok(), "invalid UTF-8 schema name was accepted");
  require(!writer.register_channel({"/raw", "raw", robot_mcap::SchemaSpec{"Raw", "\xff", ""}},
                                   id).ok(), "invalid UTF-8 schema encoding was accepted");
  require(writer.register_channel(
      {u8"/\u6e29\u5ea6", "raw", robot_mcap::SchemaSpec{u8"\U0001f321", "raw", "\xff"}}, id));
  require(id == 1, "invalid registrations consumed channel IDs");
  require(writer.finish());
}

void test_embedded_nul_path(const std::filesystem::path& directory) {
  const auto prefix = directory / "nul-test.mcap";
  std::filesystem::remove(prefix);
  auto invalid = prefix.native();
  invalid.push_back(std::filesystem::path::value_type{});
  invalid += std::filesystem::path(".unexpected").native();
  robot_mcap::McapFile writer;
  require(!writer.open(std::filesystem::path(invalid)).ok(), "embedded NUL in path was accepted");
  require(!std::filesystem::exists(prefix), "writer created a file at the truncated path");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "expected a test output directory");
    test_roundtrip(argv[1]);
    test_existing_file(argv[1]);
    test_unfinished_file(argv[1]);
    test_mcap_strings(argv[1]);
    test_embedded_nul_path(argv[1]);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
