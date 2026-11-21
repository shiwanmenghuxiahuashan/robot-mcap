#pragma once

#include "robot_mcap/recording_flow.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace robot_mcap::detail {

// 实例级预检注册表。插件在 apply() 阶段注册检查，RecordingFlow 在
// activate 阶段运行并关闭注册表；关闭后的新增会抛异常，而不是被静默忽略。
class PrecheckRegistry final : public IPrecheckRegistry,
                               public std::enable_shared_from_this<PrecheckRegistry> {
public:
  RegistrationHandle add(std::string name,
                         std::shared_ptr<IPrecheck> precheck) override {
    if (name.empty() || !precheck) {
      throw std::invalid_argument("precheck name and object are required");
    }
    std::lock_guard lock(mutex_);
    if (closed_) {
      throw std::runtime_error("precheck registry is closed");
    }
    for (const auto& entry : entries_) {
      if (entry.name == name) {
        throw std::runtime_error("duplicate precheck name: " + name);
      }
    }
    const auto id = next_id_++;
    entries_.push_back({id, std::move(name), std::move(precheck)});
    auto self = shared_from_this();
    return RegistrationHandle([self, id] { self->remove(id); });
  }

  void clear() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    entries_.clear();
  }

  std::vector<PrecheckResult> run_all(const StartContext& context) override {
    std::vector<Entry> entries;
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      entries = entries_;
    }
    std::vector<PrecheckResult> results;
    results.reserve(entries.size());
    for (auto& entry : entries) {
      try {
        PrecheckResult result = entry.precheck->run(context);
        result.name = entry.name;
        results.push_back(std::move(result));
      } catch (const std::exception& exception) {
        results.push_back({entry.name, false, exception.what()});
      } catch (...) {
        results.push_back({entry.name, false, "unknown precheck error"});
      }
    }
    return results;
  }

private:
  struct Entry {
    std::uint64_t id = 0;
    std::string name;
    std::shared_ptr<IPrecheck> precheck;
  };

  void remove(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
      if (entry->id == id) {
        entries_.erase(entry);
        return;
      }
    }
  }

  std::mutex mutex_;
  std::vector<Entry> entries_;
  std::uint64_t next_id_ = 1;
  bool closed_ = false;
};

// 注册内置的输出可写和剩余空间检查。返回的句柄必须由 RecordingFlow 持有，
// 否则检查会在句柄析构时被撤销。
std::vector<RegistrationHandle> add_base_prechecks(PrecheckRegistry& registry);

}  // namespace robot_mcap::detail
