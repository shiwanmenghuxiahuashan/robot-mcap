#pragma once

#include "robot_mcap/types.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace robot_mcap::detail {

// 实例级服务表。注册表与存活的 RegistrationHandle 共享所有权，所以句柄比
// 运行时活得更久时也不会访问已释放的内存。
class ServiceRegistry {
public:
  struct Lookup {
    std::shared_ptr<void> object;
    std::string name;
    std::string provider;
    int version = 1;
  };

  struct Snapshot {
    std::string service;
    std::string name;
    std::string provider;
    int version = 1;
  };

  Status bind(const std::string& service, std::string name, int version,
              std::string provider, std::shared_ptr<void> object,
              std::uint64_t* token = nullptr);
  Status provide(const std::string& service, const std::string& name, int version,
                 const std::string& provider, std::shared_ptr<void> object,
                 std::uint64_t& token);
  std::optional<Lookup> find(const std::string& service) const;
  void revoke(const std::string& service, std::uint64_t token);
  void revoke_provider(const std::string& provider);
  std::vector<Snapshot> snapshot() const;
  void clear();

private:
  struct Entry {
    std::string name;
    std::string provider;
    int version = 0;
    std::shared_ptr<void> object;
    std::uint64_t token = 0;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::uint64_t next_token_ = 1;
};

}  // namespace robot_mcap::detail
