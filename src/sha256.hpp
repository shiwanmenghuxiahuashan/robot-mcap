#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace robot_mcap::detail {

// Incremental SHA-256. finalize() may be called once; the object is not usable afterwards.
class Sha256 {
public:
  Sha256();

  void update(const void* data, std::size_t size);
  std::string finalize();

private:
  void transform(const std::uint8_t* block);

  std::uint32_t state_[8];
  std::uint64_t bit_length_;
  std::uint8_t buffer_[64];
  std::size_t buffer_size_;
};

std::string sha256_hex(std::string_view data);

}  // namespace robot_mcap::detail
