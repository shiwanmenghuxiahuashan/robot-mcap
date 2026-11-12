#include "ros2_adapter.hpp"

#include <mcap/reader.hpp>

#include <nlohmann/json.hpp>
#include <rosbag2_storage/bag_metadata.hpp>
#include <rosbag2_storage/metadata_io.hpp>
#include <rosbag2_storage/topic_metadata.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace robot_mcap::ros2 {
namespace {

using Json = nlohmann::json;
using TimePoint = std::chrono::time_point<std::chrono::high_resolution_clock>;

Status error(std::string detail) {
  return {"ros2_metadata", {}, std::move(detail)};
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path.string());
  }
  std::string contents((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
  if (file.bad()) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return contents;
}

void write_text_file(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    throw std::runtime_error("cannot create " + path.string());
  }
  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  file.close();
  if (!file) {
    throw std::runtime_error("cannot write " + path.string());
  }
}

void sync_file(const std::filesystem::path& path) {
#ifndef _WIN32
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "open for sync");
  }
  const int result = ::fsync(descriptor);
  const int saved = errno;
  ::close(descriptor);
  if (result != 0) {
    throw std::system_error(saved, std::generic_category(), "sync file");
  }
#else
  (void)path;
#endif
}

void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::system_error(errno, std::generic_category(), "open directory for sync");
  }
  const int result = ::fsync(descriptor);
  const int saved = errno;
  ::close(descriptor);
  if (result != 0) {
    throw std::system_error(saved, std::generic_category(), "sync directory");
  }
#else
  (void)path;
#endif
}

Json diagnostic_json(const Ros2Diagnostic& diagnostic) {
  Json item;
  item["code"] = diagnostic.code;
  item["severity"] = diagnostic.severity;
  item["topic"] = diagnostic.topic ? Json(*diagnostic.topic) : Json(nullptr);
  item["log_time_ns"] = diagnostic.log_time_ns
                            ? Json(std::to_string(*diagnostic.log_time_ns))
                            : Json(nullptr);
  item["detail"] = diagnostic.detail;
  item["action"] = diagnostic.action;
  return item;
}

Status rewrite_manifest(const std::filesystem::path& session_directory, Json& manifest,
                        bool sync) {
  if (!manifest.contains("diagnostics") || !manifest["diagnostics"].is_array()) {
    manifest["diagnostics"] = Json::array();
  }
  const auto manifest_path = session_directory / "manifest.json";
  const auto temp_path = session_directory / "manifest.json.tmp";
  try {
    write_text_file(temp_path, manifest.dump(2) + "\n");
    if (sync) {
      sync_file(temp_path);
    }
    std::filesystem::rename(temp_path, manifest_path);
    if (sync) {
      sync_directory(session_directory);
    }
    return {};
  } catch (const std::exception& exception) {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    return error(std::string("manifest rewrite failed: ") + exception.what());
  }
}

}  // namespace

Ros2MetadataWriter::Ros2MetadataWriter(Ros2MetadataOptions options)
    : options_(std::move(options)) {}

Status Ros2MetadataWriter::write(const std::filesystem::path& session_directory,
                                 const Ros2SourceOptions& source_options,
                                 const IRos2Diagnostics& diagnostics) const {
  const auto manifest_path = session_directory / "manifest.json";
  Json manifest;
  try {
    manifest = Json::parse(read_text_file(manifest_path));
  } catch (const std::exception& exception) {
    return error(std::string("cannot read manifest: ") + exception.what());
  }

  const bool sync = manifest.contains("durability") &&
                    manifest["durability"].value("requested_mode", "close") == "sync";

  auto fail = [&](const Status& cause) -> Status {
    manifest["ros2_replay"] = {{"state", "failed"}, {"metadata_path", nullptr}};
    Json root_diagnostics = Json::array();
    if (manifest.contains("diagnostics") && manifest["diagnostics"].is_array()) {
      root_diagnostics = manifest["diagnostics"];
    }
    root_diagnostics.push_back(diagnostic_json(
        {"ros2_metadata_error", "error", std::nullopt, std::nullopt, cause.detail,
         "retain_mcap_and_report_failed_metadata"}));
    for (const auto& diagnostic : diagnostics.snapshot()) {
      root_diagnostics.push_back(diagnostic_json(diagnostic));
    }
    manifest["diagnostics"] = std::move(root_diagnostics);
    const Status rewritten = rewrite_manifest(session_directory, manifest, sync);
    return rewritten.ok() ? cause : rewritten;
  };

  if (options_.fail_for_test) {
    return fail(error("injected metadata failure"));
  }
  if (manifest.value("state", "failed") != "finalized" || !manifest.contains("files") ||
      !manifest["files"].is_array() || manifest["files"].empty()) {
    return fail(error("session is not finalized"));
  }
  const auto& file = manifest["files"][0];
  const std::string relative_path = file.value("path", "");
  if (relative_path.empty() || file.value("state", "partial") != "finalized") {
    return fail(error("data file is not finalized"));
  }
  const auto data_path = session_directory / relative_path;
  if (!std::filesystem::exists(data_path)) {
    return fail(error("data file is missing: " + data_path.string()));
  }

  rosbag2_storage::BagMetadata metadata;
  try {
    mcap::McapReader reader;
    const auto opened = reader.open(data_path.string());
    if (!opened.ok()) {
      return fail(error("cannot open MCAP: " + opened.message));
    }
    const auto summary = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
    if (!summary.ok()) {
      return fail(error("cannot read MCAP summary: " + summary.message));
    }
    const auto statistics = reader.statistics();
    if (!statistics) {
      return fail(error("MCAP statistics are unavailable"));
    }

    const std::uint64_t start_ns = statistics->messageStartTime;
    const std::uint64_t end_ns = statistics->messageEndTime;
    const std::uint64_t duration_ns =
        statistics->messageCount == 0 ? 0 : end_ns - start_ns;
    const TimePoint starting_time{std::chrono::nanoseconds(static_cast<std::int64_t>(start_ns))};
    const std::chrono::nanoseconds duration{static_cast<std::int64_t>(duration_ns)};

    metadata.version = 9;
    metadata.bag_size = std::filesystem::file_size(data_path);
    metadata.storage_identifier = "mcap";
    metadata.relative_file_paths = {relative_path};
    metadata.files.push_back(
        {relative_path, starting_time, duration,
         static_cast<std::size_t>(statistics->messageCount)});
    metadata.duration = duration;
    metadata.starting_time = starting_time;
    metadata.message_count = statistics->messageCount;
    metadata.compression_format = "";
    metadata.compression_mode = "";
    metadata.custom_data = {};
    metadata.ros_distro = "jazzy";

    std::map<std::string, Ros2Qos> qos_by_topic;
    for (const auto& topic : source_options.topics) {
      qos_by_topic[topic.topic] = topic.qos;
    }

    auto channels = reader.channels();
    auto schemas = reader.schemas();
    std::vector<std::pair<std::uint16_t, mcap::ChannelPtr>> ordered_channels(
        channels.begin(), channels.end());
    std::sort(ordered_channels.begin(), ordered_channels.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& [channel_id, channel] : ordered_channels) {
      rosbag2_storage::TopicMetadata topic;
      topic.id = channel_id;
      topic.name = channel->topic;
      topic.serialization_format = channel->messageEncoding;
      const auto schema = schemas.find(channel->schemaId);
      if (schema != schemas.end() && schema->second) {
        topic.type = schema->second->name;
      }
      const auto qos = qos_by_topic.find(channel->topic);
      topic.offered_qos_profiles.push_back(
          qos == qos_by_topic.end() ? Ros2Qos{}.to_rclcpp() : qos->second.to_rclcpp());
      std::size_t count = 0;
      const auto count_entry = statistics->channelMessageCounts.find(channel_id);
      if (count_entry != statistics->channelMessageCounts.end()) {
        count = static_cast<std::size_t>(count_entry->second);
      }
      metadata.topics_with_message_count.push_back({std::move(topic), count});
    }
    reader.close();
  } catch (const std::exception& exception) {
    return fail(error(std::string("metadata generation failed: ") + exception.what()));
  }

  try {
    rosbag2_storage::MetadataIo io;
    io.write_metadata(session_directory.string(), metadata);
    if (sync) {
      sync_file(session_directory / "metadata.yaml");
    }
  } catch (const std::exception& exception) {
    return fail(error(std::string("metadata write failed: ") + exception.what()));
  }

  manifest["ros2_replay"] = {{"state", "ready"}, {"metadata_path", "metadata.yaml"}};
  if (!manifest.contains("diagnostics") || !manifest["diagnostics"].is_array()) {
    manifest["diagnostics"] = Json::array();
  }
  for (const auto& diagnostic : diagnostics.snapshot()) {
    manifest["diagnostics"].push_back(diagnostic_json(diagnostic));
  }
  return rewrite_manifest(session_directory, manifest, sync);
}

}  // namespace robot_mcap::ros2
