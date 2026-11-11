#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace robot_mcap {

// Byte output used by the MCAP writer and by session artifacts.
// Implementations throw std::system_error (or another std::exception) on failure.
// close() finalizes and releases the file; abandon() releases it without reporting errors.
class OutputSink {
public:
  OutputSink(const OutputSink&) = delete;
  OutputSink& operator=(const OutputSink&) = delete;
  virtual ~OutputSink() = default;

  virtual void write(const std::byte* data, std::size_t size) = 0;
  virtual void flush() = 0;
  virtual void close() = 0;
  virtual void abandon() noexcept = 0;
  virtual std::uint64_t size() const = 0;

protected:
  OutputSink() = default;
};

// File operations used by the recorder. Tests inject failures by replacing this seam.
// Methods other than remove() throw std::system_error (or another std::exception) on
// failure. A recorder calls them from its start thread and then its writer thread;
// calls are serialized, so implementations do not need to handle concurrent access.
class FileSystem {
public:
  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;
  virtual ~FileSystem() = default;

  // Creates a new file; fails if the path already exists. The returned sink owns the
  // exclusive creation, so exactly one layer performs it.
  virtual std::unique_ptr<OutputSink> create_exclusive(const std::filesystem::path& path) = 0;
  virtual void sync_file(const std::filesystem::path& path) = 0;
  // POSIX uses fsync on the directory. Windows attempts FlushFileBuffers on a
  // FILE_FLAG_BACKUP_SEMANTICS handle and reports unsupported filesystems as errors.
  virtual void sync_directory(const std::filesystem::path& path) = 0;
  virtual void rename(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
  virtual void remove(const std::filesystem::path& path) noexcept = 0;
  virtual std::uint64_t file_size(const std::filesystem::path& path) = 0;
  virtual std::string file_sha256(const std::filesystem::path& path) = 0;
  virtual void create_directories(const std::filesystem::path& path) = 0;
  // Returns false when the directory already exists.
  virtual bool create_directory_exclusive(const std::filesystem::path& path) = 0;

protected:
  FileSystem() = default;
};

std::shared_ptr<FileSystem> native_file_system();

// Creates an exclusive file, writes all bytes, and closes it. Removes the file on failure.
void write_file(FileSystem& file_system, const std::filesystem::path& path,
                std::string_view bytes);

std::string sha256_hex(std::string_view data);

}  // namespace robot_mcap
