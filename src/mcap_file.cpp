#include "robot_mcap/mcap_file.hpp"

#include <mcap/writer.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace robot_mcap {
namespace {

class FileOutput final : public mcap::IWritable {
public:
  ~FileOutput() override { abandon(); }

  void open(const std::filesystem::path& path) {
#ifdef _WIN32
    const int error = _wfopen_s(&file_, path.c_str(), L"wbx");
#else
    file_ = std::fopen(path.c_str(), "wbx");
    const int error = file_ ? 0 : errno;
#endif
    if (!file_) {
      throw std::system_error(error, std::generic_category(), "create output");
    }
  }

  void flush() override {
    if (std::fflush(file_) != 0) {
      throw std::system_error(errno, std::generic_category(), "flush output");
    }
  }

  void end() override {
    if (std::fclose(std::exchange(file_, nullptr)) != 0) {
      throw std::system_error(errno, std::generic_category(), "close output");
    }
  }

  std::uint64_t size() const override { return size_; }

  void abandon() noexcept {
    if (file_) {
      std::fclose(std::exchange(file_, nullptr));
    }
  }

protected:
  void handleWrite(const std::byte* data, std::uint64_t size) override {
    if (size == 0) {
      return;
    }
    if (std::fwrite(data, 1, static_cast<std::size_t>(size), file_) != size) {
      throw std::system_error(errno, std::generic_category(), "write output");
    }
    size_ += size;
  }

private:
  std::FILE* file_ = nullptr;
  std::uint64_t size_ = 0;
};

bool same_schema(const std::optional<SchemaSpec>& first, const std::optional<SchemaSpec>& second) {
  if (!first || !second) {
    return first.has_value() == second.has_value();
  }
  return first->name == second->name && first->encoding == second->encoding &&
         first->data == second->data;
}

}  // namespace

struct McapFile::Impl {
  std::filesystem::path path;
  FileOutput output;
  mcap::McapWriter writer;
  std::vector<ChannelSpec> channels;
  bool used = false;
  bool open = false;

  Status error(const char* operation, std::string detail) const {
    return {operation, path, std::move(detail)};
  }

  void abandon() noexcept {
    writer.terminate();
    output.abandon();
    open = false;
  }

  Status fail(const char* operation, std::string detail) {
    abandon();
    return error(operation, std::move(detail));
  }
};

McapFile::McapFile() : impl_(std::make_unique<Impl>()) {}
McapFile::~McapFile() {
  if (impl_->open) {
    impl_->abandon();
  }
}

Status McapFile::open(const std::filesystem::path& path, const FileOptions& options) {
  if (impl_->used) {
    return {"open", path, "writer already used"};
  }
  impl_->path = path;
  if (path.empty() || options.chunk_size_bytes == 0) {
    return impl_->error("open", "output path and positive chunk size are required");
  }
  try {
    impl_->output.open(path);
    impl_->used = true;
    mcap::McapWriterOptions writer_options("");
    writer_options.chunkSize = options.chunk_size_bytes;
    writer_options.compression = mcap::Compression::None;
    impl_->writer.open(impl_->output, writer_options);
    impl_->open = true;
    return {};
  } catch (const std::exception& error) {
    return impl_->fail("open", error.what());
  }
}

Status McapFile::register_channel(const ChannelSpec& spec, ChannelId& id) {
  if (!impl_->open) {
    return impl_->error("register_channel", "writer is not open");
  }
  constexpr auto max_field_size = std::numeric_limits<std::uint32_t>::max();
  if (spec.topic.empty() || spec.message_encoding.empty() || spec.topic.size() > max_field_size ||
      spec.message_encoding.size() > max_field_size ||
      (spec.schema && (spec.schema->name.empty() || spec.schema->encoding.empty() ||
                       spec.schema->name.size() > max_field_size ||
                       spec.schema->encoding.size() > max_field_size ||
                       spec.schema->data.size() > max_field_size))) {
    return impl_->error("register_channel", "invalid topic, encoding or schema");
  }
  const auto existing = std::find_if(impl_->channels.begin(), impl_->channels.end(),
                                     [&](const auto& entry) { return entry.topic == spec.topic; });
  if (existing != impl_->channels.end()) {
    if (existing->message_encoding != spec.message_encoding ||
        existing->publish_time_policy != spec.publish_time_policy ||
        !same_schema(existing->schema, spec.schema)) {
      return impl_->error("register_channel", "topic already has a different description");
    }
    id = static_cast<ChannelId>(existing - impl_->channels.begin() + 1);
    return {};
  }
  if (impl_->channels.size() == std::numeric_limits<ChannelId>::max()) {
    return impl_->error("register_channel", "file has reached the channel ID limit");
  }
  try {
    mcap::SchemaId schema_id = 0;
    if (spec.schema) {
      mcap::Schema schema(spec.schema->name, spec.schema->encoding, spec.schema->data);
      impl_->writer.addSchema(schema);
      schema_id = schema.id;
    }
    mcap::Channel channel(spec.topic, spec.message_encoding, schema_id);
    impl_->writer.addChannel(channel);
    impl_->channels.push_back(spec);
    id = channel.id;
    return {};
  } catch (const std::exception& error) {
    return impl_->fail("register_channel", error.what());
  }
}

Status McapFile::write(ChannelId channel, MessageView message) {
  if (!impl_->open || channel == 0 || channel > impl_->channels.size()) {
    return impl_->error("write", "writer is closed or channel is unregistered");
  }
  if (!message.data && message.size != 0) {
    return impl_->error("write", "payload pointer is required for nonempty messages");
  }
  const auto policy = impl_->channels[channel - 1].publish_time_policy;
  if ((policy == PublishTimePolicy::SourceRequired && !message.publish_time_ns) ||
      (policy == PublishTimePolicy::LogTimeOnly && message.publish_time_ns)) {
    return impl_->error("write", "publish time does not match the channel policy");
  }
  static constexpr std::byte empty_payload{};
  try {
    const auto result = impl_->writer.write({
      channel, message.sequence, message.log_time_ns,
      message.publish_time_ns.value_or(message.log_time_ns), message.size,
      message.size == 0 ? &empty_payload : message.data,
    });
    return result.ok() ? Status{} : impl_->fail("write", result.message);
  } catch (const std::exception& error) {
    return impl_->fail("write", error.what());
  }
}

Status McapFile::finish() {
  if (!impl_->open) {
    return impl_->error("finish", "writer is not open");
  }
  try {
    impl_->writer.close();
    impl_->open = false;
    return {};
  } catch (const std::exception& error) {
    return impl_->fail("finish", error.what());
  }
}

}  // namespace robot_mcap
