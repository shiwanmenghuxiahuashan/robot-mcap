# Robot MCAP 开发交接

交接日期为 2026 年 11 月 11 日。**任务 2 已提交，任务 3 已在工作树完成并通过验证；从任务 4 接手。**

## 仓库和分支

- 主仓库在 `D:/dev/robot-mcap`。GitHub 远端为 [shiwanmenghuxiahuashan/robot-mcap](https://github.com/shiwanmenghuxiahuashan/robot-mcap)。
- 任务 2 已提交为 `ca238da`（分支 `feature/task-2-recorder`），基于 `9341eef`。任务 3 的改动在 `feature/task-3-plugin-runtime` 的**工作树中，尚未提交**，请勿覆盖。
- 本机验证环境为 Ubuntu 22.04 WSL、GCC 11.4、CMake 3.22；Windows 原生构建仍未验证。

## 已有交付

| 范围 | 状态 |
| --- | --- |
| 任务 0 | v1 落盘契约和四组文本夹具 |
| 任务 1 | 独立 C++17 写入器与读回测试 |
| 任务 2 | **已提交**：录制会话、有界队列、收尾、时间旁路、失败矩阵和生命周期测试 |
| 任务 3 | **已完成（工作树）**：服务注册表、依赖解析、插件作用域、录制流程、前置门禁、原生模块和测试 |
| 任务 4–6 | Jazzy 录制回放、独立后校验、性能验证 |

## 任务 3 交付

新增或修改的文件：

- `include/robot_mcap/plugin_runtime.hpp`、`src/plugin_runtime.cpp`：版本化服务描述、`Context`/`Scope`/`RegistrationHandle`、`IPlugin`、`PluginRuntime`、原生模块 C ABI。
- `include/robot_mcap/recording_flow.hpp`、`src/recording_flow.cpp`：`IRecordingSink`/`IMessageSink`、`CoreRecordingSink`、`IRecordingStatus`、`IPrecheck`/`IPrecheckRegistry`、`RecordingFlow`、`RecordingFlowPlugin`。
- `include/robot_mcap/recorder.hpp`、`src/recorder.cpp`：新增 `Recorder::parent_directory()`，供前置门禁校验目标目录。
- `tests/plugin_runtime_test.cpp`、`tests/plugin_test_module.cpp`、`tests/plugin_test_support.hpp`：内置插件和同工具链原生模块测试。
- `CMakeLists.txt`：新增库源文件、`POSITION_INDEPENDENT_CODE`、`${CMAKE_DL_LIBS}`、测试模块和 `plugin_runtime` 目标。
- `README.md`、实施计划任务 3 勾选。

已实现的关键语义：

1. **装配与依赖**：一个 `PluginRuntime` 对应一个实例。宿主先绑定服务、注册工厂/原生模块，再调用 `start(plugin_ids)`。启动前检查缺工厂、重复选择、描述符非法、缺必需依赖、重复提供者、版本不满足和依赖循环；错误包含插件/服务名。插件按依赖拓扑排序，选择顺序只用于同层稳定排序。
2. **两阶段启动**：所有插件先按拓扑顺序 `apply()`，每个提供者的声明服务在依赖者应用前验证；全部 apply 成功后再按同一顺序 `activate()`。源插件在 activate 中注册通道并开始提交，确保录制流程已经通过门禁并启动核心。
3. **服务与作用域**：服务 ID 由 `service_id<T>()`（实现相关的 `typeid(T).name()`）生成，带名称和版本；同一 ID 只允许一个提供者。`Context::provide<T>()` 返回 RAII `RegistrationHandle`，必须由插件 `Scope::own()` 持有。`Scope` 记录 stop/wait/finalize/cleanup 动作和注册句柄；运行时按**逆初始化顺序**执行所有 stop、所有 wait、所有 finalize、所有 cleanup/句柄释放。回调不持有运行时锁，异常被收集且后续动作继续执行。
4. **回滚**：apply/activate 失败时，运行时对已应用和失败的插件执行完整四阶段清理，强制撤销插件提供但未由 Scope 释放的服务，销毁所有插件对象后再卸载模块；注册句柄幂等，支持“提供后抛异常”的半初始化插件。
5. **录制流程**：`RecordingFlow` 由宿主持有，`RecordingFlowPlugin` 在 apply 中取得 `IRecordingSink`、校验前置目录、提供 `IMessageSink`/`IPrecheckRegistry`/`IRecordingStatus`，在 activate 中运行门禁并启动核心，在 finalize 中停止核心；stop 释放 sink 引用和插件预检，避免宿主持有的 flow 在模块卸载后仍访问原生物件。`FlowStartState`、`start_error`、`prechecks` 和可空 `stop` 分别表达启动结果与核心收尾结果；未配置后校验时 `post_check` 为 `NotRequested`。门禁运行时不持有 flow 锁，预检可安全读取 `IRecordingStatus::result()`。
6. **前置门禁**：`RecordingFlow` 构造时注册输出目录可写和剩余空间阈值检查；`attach_sink` 校验 `StartContext.output_directory` 与 `IRecordingSink::output_directory()` 一致。其他插件可在 apply 中通过 `IPrecheckRegistry` 追加检查；检查注册表在首次运行后关闭，重复名称和运行后追加会被拒绝，检查异常转为命名失败结果。
7. **能力拆分**：数据源只取得 `IMessageSink`（注册通道、提交消息），不能启动/停止核心；`IRecordingSink`（start/stop/输出目录）只提供给录制流程插件。`CoreRecordingSink` 持有 `shared_ptr<Recorder>`，保证生命周期。
8. **原生模块**：模块导出 `robot_mcap_plugin_module_v1`（ABI 1），提供 `plugin_id`、create/destroy；运行时用 `dlopen`/`LoadLibraryW` 加载，校验 ABI，工厂 deleter 持有模块直到插件销毁，插件对象全部销毁后才卸载。模块必须与宿主使用相同工具链和头文件（Q16）。

测试覆盖：缺依赖、重复提供者、版本不符、循环、缺工厂、工厂返回空、describe 抛异常、中途 apply 失败与回滚、回滚中的线程 join 和下游插件抑制、activate 失败、stop 动作抛异常仍继续 wait/finalize/cleanup、句柄在 cleanup 中重置、在途线程 join 屏障、跨实例隔离与失败隔离、可选依赖缺失/旧版本、宿主服务重复/空/版本/启动后绑定、预检重复/异常/关闭后追加、预检重入读取状态、预检失败不启动核心、核心 start 失败不调用 stop、源插件先选择但流程仍先启动、finalize 阶段提交最后一条消息并被核心排空、无插件核心录制、原生模块加载/坏 ABI/服务生命周期/双实例。

## 验证记录

从仓库根目录在 WSL 运行：

```sh
cmake -S . -B build/core -DBUILD_TESTING=ON \
  -DFETCHCONTENT_SOURCE_DIR_FOXGLOVE_MCAP=/mnt/c/Users/lichonglou/.codex/visualizations/2026/11/09/01a1c069-bcb6-7300-beff-51bfe8000ff9/mcap-cpp-2.2.0-source
cmake --build build/core -j 2
ctest --test-dir build/core --output-on-failure
```

结果：`mcap_roundtrip`、`recorder_lifecycle`、`plugin_runtime` **3/3 通过**；`plugin_runtime` 连续 10 次通过。另完成：

- ThreadSanitizer（`setarch $(uname -m) -R`）无报告。
- AddressSanitizer + UBSan 无报告。
- `-Wall -Wextra -Wpedantic -Werror` 构建通过。

## 复核留项

- 任务 4 的 ROS 插件需要通过 `IPrecheckRegistry` 注册必需 topic/类型检查，并通过 `IMessageSink` 提交 CDR 载荷。
- 任务 5 的后校验会扩展 `PostCheckState`；当前只有 `NotRequested`。
- 在 `PluginRuntime` 模式下停止必须走 `PluginRuntime::stop()`；`RecordingFlow::stop()` 只用于无运行时核心录制或流程插件 finalize。
- 运行时是一次性的：失败或停止后不能重新 `start()`；需要重新录制请新建 Recorder/Flow/Runtime。
- 核心启动后、后续插件 activate 失败时，回滚会停止核心并留下一个干净 finalized 的空会话；运行时 start 同时返回失败。调用方应同时检查两者。
- 原生模块 ABI 不承诺长期二进制兼容；同工具链/头文件是 v1 前提。
- 前置门禁使用原生文件系统探测（未走 `FileSystem` 注入），空间阈值边界当前用超大阈值和不可写路径验证。
- Windows 原生模块加载和目录同步仍未在目标环境验证。

## 任务 4 接手点

1. 新增 `plugins/ros2_jazzy/CMakeLists.txt`、`ros2_source.cpp`、`bag_metadata.cpp`、`tests/ros2_jazzy/record_replay_test.cpp`；使用 Jazzy 类型与工具链构建。
2. ROS 数据源插件声明依赖 `IMessageSink` 与 `IRecordingStatus`，在 `activate()` 中按配置发现并订阅选中序列化 topic，保留原始 CDR 和真实时间；晚出现、类型/定义变化、QoS 覆盖和必需 topic 门禁按 Q19 处理，诊断进入会话记录。
3. 适配层用 Jazzy 元数据 API 生成与实际 MCAP 和统计一致的 `metadata.yaml`；元数据生成失败可见，已完成 MCAP 仍可独立读取。
4. 测试先建立独立订阅端和发现条件，再发布已知消息流；录完执行 `ros2 bag info`、`ros2 bag play`，核对数量、类型和载荷。运行 `ctest --test-dir build/jazzy -R '^ros2_record_replay$' --output-on-failure`。
5. 同步更新 README 和本交接文件。

阅读入口：[实施计划的任务 4](<D:/dev/robot-mcap/docs/superpowers/plans/2026-11-10-robot-mcap-v1-implementation.md:79>) · [会话落盘契约](<D:/dev/robot-mcap/docs/contracts/session-v1.md>) · [插件运行时接口](<D:/dev/robot-mcap/include/robot_mcap/plugin_runtime.hpp>) · [录制流程接口](<D:/dev/robot-mcap/include/robot_mcap/recording_flow.hpp>) · [插件测试](<D:/dev/robot-mcap/tests/plugin_runtime_test.cpp>)
