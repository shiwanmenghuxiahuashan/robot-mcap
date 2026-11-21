#include "plugin_runtime/infrastructure/service_registry.hpp"

#include <utility>

namespace robot_mcap::detail {

Status ServiceRegistry::bind(const std::string& service, std::string name, int version,
                             std::string provider, std::shared_ptr<void> object,
                             std::uint64_t* token) {
  if (service.empty() || name.empty() || version <= 0 || !object) {
    return {"service_bind", {}, "service id, name and version are required"};
  }
  std::lock_guard lock(mutex_);
  if (entries_.find(service) != entries_.end()) {
    return {"service_bind", {}, "duplicate provider for " + name};
  }
  Entry entry;
  entry.name = std::move(name);
  entry.provider = std::move(provider);
  entry.version = version;
  entry.object = std::move(object);
  entry.token = next_token_++;
  if (token != nullptr) {
    *token = entry.token;
  }
  entries_.emplace(service, std::move(entry));
  return {};
}

Status ServiceRegistry::provide(const std::string& service, const std::string& name,
                                int version, const std::string& provider,
                                std::shared_ptr<void> object, std::uint64_t& token) {
  return bind(service, name, version, provider, std::move(object), &token);
}

std::optional<ServiceRegistry::Lookup> ServiceRegistry::find(
    const std::string& service) const {
  std::lock_guard lock(mutex_);
  const auto entry = entries_.find(service);
  if (entry == entries_.end()) {
    return std::nullopt;
  }
  return Lookup{entry->second.object, entry->second.name, entry->second.provider,
                entry->second.version};
}

void ServiceRegistry::revoke(const std::string& service, std::uint64_t token) {
  std::lock_guard lock(mutex_);
  const auto entry = entries_.find(service);
  if (entry != entries_.end() && entry->second.token == token) {
    entries_.erase(entry);
  }
}

void ServiceRegistry::revoke_provider(const std::string& provider) {
  std::lock_guard lock(mutex_);
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    if (entry->second.provider == provider) {
      entry = entries_.erase(entry);
    } else {
      ++entry;
    }
  }
}

std::vector<ServiceRegistry::Snapshot> ServiceRegistry::snapshot() const {
  std::lock_guard lock(mutex_);
  std::vector<Snapshot> result;
  result.reserve(entries_.size());
  for (const auto& entry : entries_) {
    result.push_back({entry.first, entry.second.name, entry.second.provider,
                      entry.second.version});
  }
  return result;
}

void ServiceRegistry::clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
}

}  // namespace robot_mcap::detail
