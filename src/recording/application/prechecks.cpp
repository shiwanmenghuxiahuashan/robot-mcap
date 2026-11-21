#include "recording/application/prechecks.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace robot_mcap::detail {
namespace {

// 检查输出目录是否存在、可创建且实际可写。探测文件写入后立即删除。
class OutputWritablePrecheck final : public IPrecheck {
public:
  PrecheckResult run(const StartContext& context) override {
    if (context.output_directory.empty()) {
      return {"", false, "output directory is empty"};
    }
    std::error_code error;
    std::filesystem::create_directories(context.output_directory, error);
    if (error) {
      return {"", false, "cannot create output directory: " + error.message()};
    }
    static std::atomic<std::uint64_t> probe_counter{0};
    const auto probe = context.output_directory /
                       (".robot_mcap_write_probe_" +
                        std::to_string(probe_counter.fetch_add(1) + 1));
    std::ofstream file(probe, std::ios::binary | std::ios::trunc);
    if (!file) {
      return {"", false, "output directory is not writable"};
    }
    file.put('x');
    file.flush();
    const bool wrote = static_cast<bool>(file);
    file.close();
    std::filesystem::remove(probe, error);
    if (!wrote) {
      return {"", false, "writing the output probe failed"};
    }
    return {"", true, {}};
  }
};

// 检查输出所在卷是否至少有调用方要求的剩余空间。
class FreeSpacePrecheck final : public IPrecheck {
public:
  PrecheckResult run(const StartContext& context) override {
    if (context.required_free_bytes == 0) {
      return {"", true, {}};
    }
    std::error_code error;
    const auto space = std::filesystem::space(context.output_directory, error);
    if (error) {
      return {"", false, "cannot determine free space: " + error.message()};
    }
    if (space.available < context.required_free_bytes) {
      return {"", false,
              "insufficient free space: need " +
                  std::to_string(context.required_free_bytes) + " bytes, have " +
                  std::to_string(space.available)};
    }
    return {"", true, {}};
  }
};

}  // namespace

std::vector<RegistrationHandle> add_base_prechecks(PrecheckRegistry& registry) {
  std::vector<RegistrationHandle> handles;
  handles.push_back(
      registry.add("output_writable", std::make_shared<OutputWritablePrecheck>()));
  handles.push_back(
      registry.add("free_space", std::make_shared<FreeSpacePrecheck>()));
  return handles;
}

}  // namespace robot_mcap::detail
