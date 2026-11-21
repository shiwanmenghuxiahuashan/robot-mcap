#include "robot_mcap/io.hpp"

#include "shared_kernel/sha256.hpp"

#include <cerrno>
#include <cstdio>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace robot_mcap {
namespace {

std::FILE *open_read(const std::filesystem::path &path) {
#ifdef _WIN32
  std::FILE *file = nullptr;
  const int error = _wfopen_s(&file, path.c_str(), L"rb");
  if (!file) {
    throw std::system_error(error, std::generic_category(), "open input");
  }
  return file;
#else
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    throw std::system_error(errno, std::generic_category(), "open input");
  }
  return file;
#endif
}

class NativeOutputSink final : public OutputSink {
public:
  NativeOutputSink() = default;
  ~NativeOutputSink() override { abandon(); }

  void open_exclusive(const std::filesystem::path &path) {
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

  void write(const std::byte *data, std::size_t size) override {
    if (size == 0) {
      return;
    }
    if (std::fwrite(data, 1, size, file_) != size) {
      throw std::system_error(errno, std::generic_category(), "write output");
    }
    size_ += size;
  }

  void flush() override {
    if (std::fflush(file_) != 0) {
      throw std::system_error(errno, std::generic_category(), "flush output");
    }
  }

  void close() override {
    std::FILE *file = std::exchange(file_, nullptr);
    if (file && std::fclose(file) != 0) {
      throw std::system_error(errno, std::generic_category(), "close output");
    }
  }

  void abandon() noexcept override {
    if (file_) {
      std::fclose(std::exchange(file_, nullptr));
    }
  }

  std::uint64_t size() const override { return size_; }

private:
  std::FILE *file_ = nullptr;
  std::uint64_t size_ = 0;
};

class NativeFileSystem final : public FileSystem {
public:
  std::unique_ptr<OutputSink>
  create_exclusive(const std::filesystem::path &path) override {
    auto sink = std::make_unique<NativeOutputSink>();
    sink->open_exclusive(path);
    return sink;
  }

  void sync_file(const std::filesystem::path &path) override {
#ifdef _WIN32
    const int descriptor = _wopen(path.c_str(), _O_RDWR | _O_BINARY);
    if (descriptor < 0) {
      throw std::system_error(errno, std::generic_category(), "open for sync");
    }
    const int result = _commit(descriptor);
    const int error = errno;
    _close(descriptor);
    if (result != 0) {
      throw std::system_error(error, std::generic_category(), "sync file");
    }
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
      throw std::system_error(errno, std::generic_category(), "open for sync");
    }
    const int result = ::fsync(descriptor);
    const int error = errno;
    ::close(descriptor);
    if (result != 0) {
      throw std::system_error(error, std::generic_category(), "sync file");
    }
#endif
  }

  void sync_directory(const std::filesystem::path &path) override {
#ifdef _WIN32
    // FlushFileBuffers on a directory handle is the closest Windows equivalent.
    // Some filesystems reject it; surface that as a durability failure rather
    // than silently reporting success.
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      throw std::system_error(static_cast<int>(GetLastError()),
                              std::system_category(),
                              "open directory for sync");
    }
    if (!FlushFileBuffers(handle)) {
      const auto error = GetLastError();
      CloseHandle(handle);
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "sync directory");
    }
    CloseHandle(handle);
#else
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "open directory for sync");
    }
    const int result = ::fsync(descriptor);
    const int error = errno;
    ::close(descriptor);
    if (result != 0) {
      throw std::system_error(error, std::generic_category(), "sync directory");
    }
#endif
  }

  void rename(const std::filesystem::path &from,
              const std::filesystem::path &to) override {
    std::filesystem::rename(from, to);
  }

  void remove(const std::filesystem::path &path) noexcept override {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  std::uint64_t file_size(const std::filesystem::path &path) override {
    return std::filesystem::file_size(path);
  }

  std::string file_sha256(const std::filesystem::path &path) override {
    std::FILE *file = open_read(path);
    detail::Sha256 digest;
    std::vector<std::byte> buffer(64 * 1024);
    try {
      for (;;) {
        const auto read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
          digest.update(buffer.data(), read);
        }
        if (read < buffer.size()) {
          if (std::ferror(file) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "read input");
          }
          break;
        }
      }
    } catch (...) {
      std::fclose(file);
      throw;
    }
    if (std::fclose(file) != 0) {
      throw std::system_error(errno, std::generic_category(), "close input");
    }
    return digest.finalize();
  }

  void create_directories(const std::filesystem::path &path) override {
    std::filesystem::create_directories(path);
  }

  bool create_directory_exclusive(const std::filesystem::path &path) override {
    return std::filesystem::create_directory(path);
  }
};

} // namespace

std::shared_ptr<FileSystem> native_file_system() {
  static const auto instance = std::make_shared<NativeFileSystem>();
  return instance;
}

void write_file(FileSystem &file_system, const std::filesystem::path &path,
                std::string_view bytes) {
  auto sink = file_system.create_exclusive(path);
  try {
    if (!bytes.empty()) {
      sink->write(reinterpret_cast<const std::byte *>(bytes.data()),
                  bytes.size());
    }
    sink->close();
  } catch (...) {
    sink->abandon();
    file_system.remove(path);
    throw;
  }
}

std::string sha256_hex(std::string_view data) {
  return detail::sha256_hex(data);
}

} // namespace robot_mcap
