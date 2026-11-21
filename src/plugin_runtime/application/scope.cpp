#include "robot_mcap/plugin_runtime.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace robot_mcap {

// RegistrationHandle 的共享状态。句柄可能被移动或提前 reset()，共享状态保证
// 撤销动作最多执行一次。
struct RegistrationHandle::State {
  std::mutex mutex;
  std::function<void()> revoke;
  std::atomic<bool> active{true};
};

RegistrationHandle::RegistrationHandle(std::function<void()> revoke)
    : state_(std::make_shared<State>()) {
  state_->revoke = std::move(revoke);
}

RegistrationHandle::~RegistrationHandle() { reset(); }

RegistrationHandle::RegistrationHandle(RegistrationHandle&& other) noexcept
    : state_(std::move(other.state_)) {}

RegistrationHandle& RegistrationHandle::operator=(RegistrationHandle&& other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
  }
  return *this;
}

void RegistrationHandle::reset() noexcept {
  if (!state_) {
    return;
  }
  bool expected = true;
  if (!state_->active.compare_exchange_strong(expected, false)) {
    return;
  }
  std::function<void()> revoke;
  {
    std::lock_guard lock(state_->mutex);
    revoke = std::move(state_->revoke);
  }
  if (revoke) {
    try {
      revoke();
    } catch (...) {
      // 撤销注册是尽力而为，析构路径不能抛出异常。
    }
  }
}

RegistrationHandle::operator bool() const noexcept {
  return state_ && state_->active.load();
}

struct Scope::Impl {
  std::vector<std::function<void()>> stop_actions;
  std::vector<std::function<void()>> wait_actions;
  std::vector<std::function<void()>> finalize_actions;
  std::vector<std::function<void()>> cleanup_actions;
  std::vector<RegistrationHandle> handles;
  bool stop_ran = false;
  bool wait_ran = false;
  bool finalize_ran = false;
  bool released = false;
};

namespace {

std::vector<Status> run_actions(std::vector<std::function<void()>>& actions,
                                const char* operation, const std::string& plugin_id) {
  std::vector<Status> errors;
  // 逆序执行，让后注册的清理先发生。单个动作失败不阻止其余动作。
  for (auto action = actions.rbegin(); action != actions.rend(); ++action) {
    try {
      if (*action) {
        (*action)();
      }
    } catch (const std::exception& exception) {
      errors.push_back(
          {operation, {}, "plugin " + plugin_id + ": " + exception.what()});
    } catch (...) {
      errors.push_back({operation, {}, "plugin " + plugin_id + ": unknown error"});
    }
  }
  return errors;
}

std::vector<Status> scope_phase(std::vector<std::function<void()>>& actions, bool& ran,
                                const char* operation, const std::string& plugin_id) {
  // 每个阶段只运行一次；重复调用返回空结果。
  if (ran) {
    return {};
  }
  ran = true;
  return run_actions(actions, operation, plugin_id);
}

}  // namespace

Scope::Scope() : impl_(std::make_unique<Impl>()) {}

Scope::~Scope() {
  if (impl_ && !impl_->released) {
    try {
      release({});
    } catch (...) {
    }
  }
}

void Scope::on_stop(std::function<void()> action) {
  impl_->stop_actions.push_back(std::move(action));
}

void Scope::on_wait(std::function<void()> action) {
  impl_->wait_actions.push_back(std::move(action));
}

void Scope::on_finalize(std::function<void()> action) {
  impl_->finalize_actions.push_back(std::move(action));
}

void Scope::own(RegistrationHandle handle) {
  impl_->handles.push_back(std::move(handle));
}

void Scope::on_cleanup(std::function<void()> action) {
  impl_->cleanup_actions.push_back(std::move(action));
}

std::vector<Status> Scope::run_stop(const std::string& plugin_id) {
  return scope_phase(impl_->stop_actions, impl_->stop_ran, "plugin_stop", plugin_id);
}

std::vector<Status> Scope::run_wait(const std::string& plugin_id) {
  return scope_phase(impl_->wait_actions, impl_->wait_ran, "plugin_wait", plugin_id);
}

std::vector<Status> Scope::run_finalize(const std::string& plugin_id) {
  return scope_phase(impl_->finalize_actions, impl_->finalize_ran, "plugin_finalize",
                     plugin_id);
}

std::vector<Status> Scope::release(const std::string& plugin_id) {
  if (impl_->released) {
    return {};
  }
  impl_->released = true;
  std::vector<Status> errors =
      run_actions(impl_->cleanup_actions, "plugin_cleanup", plugin_id);
  // 最后逆序撤销注册句柄，确保清理动作仍能使用已注册的服务。
  for (auto handle = impl_->handles.rbegin(); handle != impl_->handles.rend(); ++handle) {
    handle->reset();
  }
  impl_->handles.clear();
  return errors;
}

}  // namespace robot_mcap
