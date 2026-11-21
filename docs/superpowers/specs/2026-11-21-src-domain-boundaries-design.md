# robot-mcap 的模块边界与会话格式（完整设计）

状态　2026-11-21，实施完成。本文把初步设计补全为可执行的模块契约，并记录实施后的源码布局、数据形状、依赖规则、迁移批次与验收证据。

范围　源码组织、模块依赖、会话格式的表示与写出、插件运行时的依赖解析。行为保持 v1 契约不变。

非目标　新增文件格式版本；改变 MCAP 写入参数；改变 ROS 2 适配行为；实现独立后校验进程。

## 0. 结论摘要

- `src/` 按 DDD 分层：`shared_kernel` 与 `infrastructure` 提供共享内核和平台机制；`session_format`、`recording`、`plugin_runtime` 三个限界上下文内部再分 `domain`、`application`、`infrastructure`。`recording/adapters/` 是录制与插件运行时唯一的组合层。
- 解释 v1 会话所需的类型和代码不再依赖录制器、插件或 ROS 2。序列化是纯函数，文件发布顺序集中在 `SessionArtifacts`。
- `include/robot_mcap/` 下的公开头保持原有 include 路径和类型语义。`DurabilityMode` 移到 `types.hpp`，但 `recorder.hpp` 继续包含 `types.hpp`，调用方不受影响。
- 三个现有测试在每次迁移后保持通过，`-Wall -Wextra -Wpedantic -Werror` 构建通过。

## 1. 设计目标与约束

目标：

1. 让只读 v1 数据的程序可以仅凭格式说明、样例和 `session_format` 模块实现读方，不必链接录制器、插件运行时或 ROS 2。
2. 让每个模块只有一个变化理由。录制策略、文件格式、插件生命周期和操作系统 I/O 分开演化。
3. 让格式写出可测试。JSON 文本由纯函数生成，目录创建、同步和重命名由发布类负责，两者不互相渗透。
4. 保持公开 API 和源码兼容。调用方重新编译即可，不需要改 include 或类型名。

约束：

- v1 文件契约（[session-v1.md](../../contracts/session-v1.md)）和样例（`tests/fixtures/session-v1/`）是共同输入。
- 现有 `robot_mcap` 静态库目标和三个测试是兼容性验收项。
- 第一批迁移只移动文件与更新引用；行为调整按批次分开验证。

## 2. 限界上下文与 DDD 分层

每个限界上下文内部按 DDD 的三层组织。`domain` 只表达领域规则和值形状，
`application` 编排用例，`infrastructure` 提供文件、线程、动态库等平台机制。
`adapters` 负责把上下文接到外部接口。

| 限界上下文 | domain | application | infrastructure | adapters |
|---|---|---|---|---|
| `recording` | `message_rules` | `recorder`、`recorder_writer`、`recording_flow`、`prechecks` | 无 | `recording_flow_plugin` |
| `session_format` | `manifest`、`documents` | 无 | `session_artifacts` | 无 |
| `plugin_runtime` | `dependency_resolution`、`descriptor_rules` | `plugin_runtime`、`scope`、`context` | `service_registry`、`native_module` | 无 |
| `shared_kernel` | `sha256`、`text` | 无 | 无 | 无 |
| `infrastructure` | 无 | 无 | `filesystem`、`mcap` | 无 |
| `plugins/ros2_jazzy` | 无 | 无 | 无 | ROS 2 数据源与回放适配 |

职责边界：

| 上下文 | 负责 | 不负责 |
|---|---|---|
| `recording` | 一次录制的预检、通道注册、消息接收、队列、写线程、停止协议和结果判定 | 文件格式文本、插件装配、操作系统调用 |
| `session_format` | 配置快照、清单、时间旁路的数据形状与纯文本序列化；会话目录内的发布顺序 | 录制策略、插件生命周期、MCAP 写入 |
| `plugin_runtime` | 服务注册、描述符校验、依赖排序、apply/activate 生命周期、原生模块装卸 | 录制流程、会话格式、ROS 2 |
| `shared_kernel` | SHA-256 和 UTF-8 校验 | 一切领域知识 |
| `infrastructure` | 原生文件系统、上游 MCAP 写入器、上游版本号 | 任何领域策略 |
| `recording/adapters` | 把 `RecordingFlow` 接入 `PluginRuntime` | 录制实现、插件实现 |

三方边界不变：

- 录制器产生会话，决定何时结束。
- ROS 2 适配器通过磁盘契约补写 `metadata.yaml` 和 `manifest.json` 中的 `ros2_replay`。
- 未来的独立后校验进程读取已完成的会话。它应以格式说明和样例为输入，而不是依赖本库的录制器实现。

## 3. 依赖规则

DDD 依赖方向：

```text
adapters ──> application ──> domain
                 │
                 └────────> infrastructure（仅组合根）
infrastructure ──> domain
```

- `domain` 只包含公开值类型和标准库，不包含 I/O、文件系统、MCAP、ROS 2。
- `application` 可以包含本上下文的 `domain`。在组合根（`Recorder`、`PluginRuntime`）上可以直接使用 `infrastructure`，因为这里只有一个实现；不为单一实现引入接口层。
- `infrastructure` 可以包含 `domain` 和公开接口，但不包含 `application`。
- `adapters` 可以包含 `application` 和 `domain`，是跨上下文组合的唯一位置。
- `shared_kernel` 不包含任何项目内头文件。
- 各上下文的 `domain` 不互相包含。`session_format/domain` 不包含 `recorder.hpp`、插件或 MCAP 头。
- `recording/adapters` 是唯一同时包含 `robot_mcap/recording_flow.hpp` 和插件 SDK 类型的实现文件。
- 私有头文件放在所属层目录，通过 `target_include_directories(robot_mcap PRIVATE src)` 以 `上下文/层/文件.hpp` 形式包含。每个私有头可以单独编译。

公开头与私有头的边界：

- `include/robot_mcap/types.hpp` 是共享内核，只依赖标准库。
- `include/robot_mcap/{recorder,recording_flow,plugin_runtime,mcap_file,io}.hpp` 是对外 API。
- `src/**/*.hpp` 是实现细节，不承诺兼容性。

## 4. 源码布局

```text
src/
  shared_kernel/
    sha256.hpp
    sha256.cpp
    text.hpp
    text.cpp

  infrastructure/
    filesystem/
      io.cpp
    mcap/
      mcap_file.cpp
      mcap_vendor.cpp
      version.hpp

  session_format/
    domain/
      manifest.hpp            # 持久化数据形状
      documents.hpp           # 配置快照与纯渲染器
      documents.cpp
    infrastructure/
      session_artifacts.hpp   # 目录与发布顺序
      session_artifacts.cpp

  plugin_runtime/
    domain/
      dependency_resolution.hpp/cpp  # 纯依赖排序
      descriptor_rules.hpp/cpp       # 描述符校验
    application/
      plugin_runtime.cpp             # 运行时编排：start、apply/activate、关闭
      scope.cpp                      # RegistrationHandle 与 Scope 生命周期动作
      context.cpp                    # 插件装配视图
    infrastructure/
      service_registry.hpp/cpp       # 实例级服务表
      native_module.hpp/cpp          # 原生模块加载、ABI 校验与卸载

  recording/
    domain/
      message_rules.hpp/cpp          # 纯校验与时间证据判定
    application/
      recorder_impl.hpp              # 控制路径与写入路径共享的私有状态
      recorder.cpp                   # 公开 API、启动、注册、提交、停止
      recorder_writer.cpp            # 队列、写线程、失败记录、收尾
      recording_flow.cpp             # 预检、启动、停止编排
      prechecks.hpp/cpp              # 预检注册表与内置检查
    adapters/
      recording_flow_plugin.cpp
```

构建清单按模块分组，`robot_mcap` 仍是唯一库目标：

```cmake
add_library(robot_mcap STATIC
  src/shared_kernel/sha256.cpp
  src/shared_kernel/text.cpp
  src/infrastructure/filesystem/io.cpp
  src/infrastructure/mcap/mcap_file.cpp
  src/infrastructure/mcap/mcap_vendor.cpp
  src/session_format/domain/documents.cpp
  src/session_format/infrastructure/session_artifacts.cpp
  src/plugin_runtime/domain/dependency_resolution.cpp
  src/plugin_runtime/domain/descriptor_rules.cpp
  src/plugin_runtime/application/plugin_runtime.cpp
  src/plugin_runtime/application/scope.cpp
  src/plugin_runtime/application/context.cpp
  src/plugin_runtime/infrastructure/service_registry.cpp
  src/plugin_runtime/infrastructure/native_module.cpp
  src/recording/domain/message_rules.cpp
  src/recording/application/recorder.cpp
  src/recording/application/recorder_writer.cpp
  src/recording/application/recording_flow.cpp
  src/recording/application/prechecks.cpp
  src/recording/adapters/recording_flow_plugin.cpp
)
target_include_directories(robot_mcap PUBLIC include PRIVATE src)
```

## 5. 数据形状

先定数据，再写逻辑。跨越模块边界的数据只有三类：共享值类型、持久化形状、纯函数输入输出。

### 5.1 共享值类型

`include/robot_mcap/types.hpp` 保持轻量，不包含其他模块头：

```cpp
enum class DurabilityMode { Close, Sync };
enum class PublishTimePolicy { SourceRequired, LogTimeOnly, PerMessage };

struct ChannelSpec {
  std::string topic;
  std::string message_encoding;
  std::optional<SchemaSpec> schema;
  PublishTimePolicy publish_time_policy = PublishTimePolicy::LogTimeOnly;
};

struct MessageView { /* log/publish time, payload view, sequence, source clock */ };
struct FileOptions { std::uint64_t chunk_size_bytes; std::shared_ptr<FileSystem> file_system; };
```

`DurabilityMode` 原来在 `recorder.hpp`。它同时属于录制结果和会话格式，因此上移到共享内核。`recorder.hpp` 仍包含 `types.hpp`，`robot_mcap::DurabilityMode` 对调用方保持可见。

### 5.2 持久化形状

`session_format/domain/manifest.hpp` 只包含公开类型和标准库：

```cpp
struct Diagnostic { std::string code; std::string severity; /* ... */ };
struct FileFacts { std::filesystem::path path; bool finalized; bool present; /* ... */ };
struct TimingFacts { bool present; std::filesystem::path path; std::uint64_t row_count; /* ... */ };
struct Statistics { /* accepted, rejected, writer_processed, finalized_messages */ };
struct ManifestInput { /* session_id, data, timing, channels, statistics, durability, diagnostics */ };
struct PublishResult { bool manifest_published; Status error; };
inline constexpr const char* kConfigFileName = "recording-config.json";
```

录制器在停止时填充 `ManifestInput`。格式模块把它渲染成文本。读方可以从磁盘重建同一形状，不需要链接录制器。

### 5.3 配置快照与纯渲染器

`session_format/domain/documents.hpp`：

```cpp
struct ConfigSnapshot {
  std::string session_id;
  std::uint64_t chunk_size_bytes;
  std::uint64_t queue_byte_limit;
  DurabilityMode durability_mode;
  std::string writer_version;   // 由 infrastructure/mcap 提供，格式模块只格式化
};

std::string render_config_json(const ConfigSnapshot&);
std::string render_manifest_json(const ManifestInput&);
std::string render_timing_row(std::uint64_t ordinal, ChannelId channel,
                              bool publish_time_fallback,
                              const std::optional<SourceClock>& source_clock);
```

`writer_version` 来自 `infrastructure/mcap/version.hpp` 的 `mcap_writer_version()`，由录制器在构造 `SessionArtifacts` 时传入。`session_format` 因此不包含 `<mcap/...>`。

### 5.4 录制纯规则

`recording/domain/message_rules.hpp`：

```cpp
bool valid_session_id(std::string_view);
bool valid_channel_spec(const ChannelSpec&);
bool valid_source_clock(const SourceClock&);
bool requires_timing_row(PublishTimePolicy policy, bool has_publish_time,
                         bool has_source_clock);
```

这些函数只判断跨越 API 边界的值。队列预算、生命周期和计数器仍留在 `Recorder::Impl`，因为它们需要状态。

### 5.5 插件依赖解析

`plugin_runtime/domain/dependency_resolution.hpp`：

```cpp
struct ServiceAvailability { std::string service, name, provider; int version; };

Status resolve_start_order(const std::vector<PluginDescriptor>& descriptors,
                           const std::vector<ServiceAvailability>& host_services,
                           std::vector<std::size_t>& order);
```

输入是所选插件的描述符和宿主服务版本，输出是描述符下标顺序或错误。函数不构造插件、不执行插件代码。可选依赖缺失或不兼容时不加边，`find<T>()` 返回空。

## 6. 关键流程

### 6.1 一次录制的生命周期

1. `Recorder::start()` 校验会话 ID、父目录、队列预算和分块大小。
2. 构造 `SessionArtifacts`，独占创建 `<parent>/<session_id>`，写入配置快照。
3. 打开 `data_0000.mcap.partial`。启动写线程。状态从 `Created` 进入 `Running`。
4. `register_channel()` 在 `writer_mutex` 下注册 MCAP 通道，再在状态锁下记录 `ChannelSpec`。
5. `submit_copy()`/`submit_owned()` 在状态锁下校验、入队、记账；队列按载荷字节数限额。
6. 写线程按提交顺序取出消息，在 `writer_mutex` 下写 MCAP；随后分配物理序号，按规则决定是否追加时间旁路行。
7. `stop_async()` 停止接收并返回共享 future；写线程排空队列、`finish()` MCAP、关闭并重命名数据文件、关闭并重命名时间旁路、发布清单。
8. 停止结果同时携带文件状态、持久化状态、停止原因和首个错误。写线程自身失败时也会自主收尾，future 无需 `stop()` 即可解析。

### 6.2 会话格式的发布顺序

`SessionArtifacts` 是唯一知道临时文件和重命名顺序的类：

1. 创建会话目录，独占。`Sync` 模式额外同步父目录。
2. 配置快照写入 `recording-config.json.tmp`，重命名为 `recording-config.json`。
3. 数据文件和旁路文件以 `.partial` 后缀活动。
4. 停止时先完成 MCAP，再重命名数据文件；旁路只在数据文件完成且内容完整时重命名。
5. 清单写入 `manifest.json.tmp`。`Sync` 模式依次同步数据、旁路、配置和清单临时文件，然后重命名清单，再同步会话目录和父目录。
6. 任一步失败都保留已有产物并返回带操作名、路径和详情的 `Status`。发布失败优先于更早的持久化错误。

文本生成全部在 `documents.cpp`，文件系统操作全部在 `session_artifacts.cpp`。这样可以用样例逐字节检查 JSON，而不需要真实磁盘。

### 6.3 插件解析与生命周期

1. `PluginRuntime::start(plugin_ids)` 校验选择列表、创建插件实例、调用 `describe()` 并校验描述符。
2. 收集宿主服务快照，交给 `resolve_start_order()`。解析器检查重复提供者、缺失依赖、版本不匹配和循环依赖。
3. 按返回顺序对每个插件调用 `apply()`。每提供一个服务后立即验证声明与实际注册一致。
4. 再按同一顺序调用 `activate()`。数据源只在此后开始生产，录制流程只在此后启动核心。
5. 关闭时逆序执行四阶段：所有 `stop`、所有 `wait`、所有 `finalize`、所有清理和注册撤销。先撤销插件服务，再销毁插件对象，最后卸载原生模块。

## 7. 并发与错误模型

- `Recorder` 的状态机为 `Created -> Running -> Stopping -> Stopped`。`start_attempted` 保证只尝试一次。
- 锁顺序固定为 `writer_mutex` 再 `mutex`。写线程只持有 `writer_mutex` 调 MCAP；状态、队列、统计和诊断只由 `mutex` 保护。
- 队列字节数在入队前预留，在出队时扣减。被拒绝的消息不改变调用方缓冲区，也不增加已接收计数。
- 停止结果通过 `std::shared_future<StopResult>` 共享。多个调用者观察同一结果，不会各自触发一次收尾。
- `SessionArtifacts` 在 `start()` 阶段由调用线程使用，之后由写线程使用；两者不会并发访问。发布在写线程的 `finalize()` 中完成。
- `PluginRuntime` 明确不是线程安全的。宿主负责串行化 `start()`、`stop()` 和注册调用；插件可以拥有自己的线程，并通过 `Scope` 参与停止顺序。
- 所有跨模块错误都用 `Status { operation, path, detail }`。纯函数返回 `Status` 或布尔值，不抛异常；需要在构造/注册阶段失败的公开 API 可以用异常表达调用方错误。

## 8. 迁移批次与验收

| 批次 | 改动 | 验收 | 结果 |
|---|---|---|---|
| 1. 源码归位 | 移动 9 个实现文件和 3 个私有头；更新构建清单和私有引用；`PRIVATE src` 提供模块根 | 搬迁前后分别通过 `mcap_roundtrip`、`recorder_lifecycle`、`plugin_runtime`；公开头不变 | 通过 |
| 2. 明确会话格式输入 | `DurabilityMode` 上移；`session_format` 不再包含 `recorder.hpp`；抽出 `manifest.hpp`、`documents.*`；上游版本以数据传入；`SessionArtifacts` 只负责目录与发布 | 对照 v1 样例核对清单字段、配置快照和时间旁路字节；生命周期测试覆盖已完成数据但时间证据失败、写入失败保留部分文件、同步失败返回持久化失败 | 通过 |
| 3. 拆插件接入与依赖解析 | `RecordingFlowPlugin` 与工厂移到 `recording/adapters/`；从运行时启动流程抽出 `resolve_start_order()` | 现有插件测试覆盖可选依赖、版本冲突、循环依赖、先全部 apply 再逐个 activate、回滚与卸载顺序 | 通过 |
| 4. 局部整理录制核心 | 分出 `message_rules.*`（消息校验与时间证据判定）；队列、写线程和停止协议仍作为一个并发单元审阅 | `recorder_lifecycle` 核对队列满时的所有权、停止时的接收边界、同步与异步停止共享结果；`mcap_roundtrip` 验证单文件写入 | 通过 |
| 5. DDD 分层与长文件拆分 | 目录按 `domain`、`application`、`infrastructure`、`adapters` 重排；`recorder.cpp` 拆为控制路径和写入路径；预检从 `recording_flow.cpp` 拆出；描述符校验移到 `plugin_runtime/domain` | 每次拆分后三个测试全绿；全新构建、`-Werror`、ASan/UBSan 通过；私有头自包含 | 通过 |

内部函数名称、私有头数量和每次移动的文件数在实施时确定。本轮交付范围是源码组织与现有行为的兼容。

## 9. 验证

构建与测试：

```sh
cmake -S . -B build/core -DBUILD_TESTING=ON
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

结果：`mcap_roundtrip`、`recorder_lifecycle`、`plugin_runtime` 三个测试全部通过。

严格警告构建：

```sh
cmake -S . -B build/warn -DBUILD_TESTING=ON
cmake --build build/warn
ctest --test-dir build/warn --output-on-failure
```

`-Wall -Wextra -Wpedantic -Werror` 下通过。

私有头自包含检查：

```sh
g++ -std=c++17 -Iinclude -Isrc -fsyntax-only -x c++ src/.../*.hpp
```

`manifest.hpp`、`documents.hpp`、`session_artifacts.hpp`、`dependency_resolution.hpp`、`message_rules.hpp`、`version.hpp`、`sha256.hpp`、`text.hpp` 均可单独编译。

ROS 2 适配器在 Jazzy 环境另行验证：

```sh
cmake -S . -B build/jazzy -DBUILD_TESTING=ON -DROBOT_MCAP_WITH_ROS2=ON
ctest --test-dir build/jazzy -R '^ros2_record_replay$' --output-on-failure
```

## 10. 取舍、风险与后续

- **适配器拆分只改翻译单元边界。** `RecordingFlowPlugin` 的声明仍在公开头，实现移到 `recording/adapters/`。二进制布局和公开 API 不变，但静态库使用方需要重新编译。
- **解析器错误字符串是测试契约。** `missing dependency`、`version mismatch`、`duplicate provider`、`dependency cycle` 被现有插件测试断言。后续改动应保留这些措辞，或同步修改测试。
- **格式模块不包含 MCAP 头。** 上游版本作为字符串传入。如果未来需要记录更多上游常量，仍应通过 `infrastructure/mcap` 的值对象传入，避免格式模块反向依赖存储实现。
- **后校验是未来消费者。** 它应依赖 `docs/contracts/session-v1.md` 和 `tests/fixtures/session-v1/`，不依赖录制器。当前 `session_format` 的纯渲染器和数据形状可以作为参考实现，但不是跨进程 ABI。
- **并发模型没有拆成多个类。** `Recorder::Impl` 仍是单一状态单元。拆分只把控制路径（`recorder.cpp`）和写入路径（`recorder_writer.cpp`）放到两个翻译单元，两者共享 `recorder_impl.hpp`，锁顺序和停止协议不变。
- **插件运行时按职责再拆。** `plugin_runtime/application/plugin_runtime.cpp` 只保留运行时编排；服务表、注册句柄与作用域、装配视图、原生模块装卸各有独立翻译单元。公开头 `include/robot_mcap/plugin_runtime.hpp` 不变，插件作者的编译契约不变。
- **唯一超过 400 行的生产文件是 `plugins/ros2_jazzy/ros2_source.cpp`（406 行）。** 它是一个内聚的 ROS 2 数据源插件类，拆开只会把一个类的声明和实现分开，不产生新的领域边界，因此保留。
- **测试文件没有按 DDD 拆分。** `tests/recorder_lifecycle_test.cpp` 和 `tests/plugin_runtime_test.cpp` 是场景集合，不是分层代码。按行数拆需要先抽出共享测试支撑头文件，属于独立任务。
- **ROS 2 适配行为未变。** 它继续通过磁盘契约更新清单，下一轮若需要进程内发布，应在 `session_format` 增加显式的清单更新接口，而不是让适配器直接拼 JSON。
