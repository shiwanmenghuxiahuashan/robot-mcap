# Robot MCAP 开发交接

交接日期为 2026 年 11 月 11 日。**任务 2 已在工作树完成并通过 WSL 验证；从任务 3 接手。**

## 仓库和分支

- 主仓库在 `D:/dev/robot-mcap`。GitHub 远端为 [shiwanmenghuxiahuashan/robot-mcap](https://github.com/shiwanmenghuxiahuashan/robot-mcap)。
- 任务 2 的实现基于 `9341eef40bc61089fd3e2cb7c680ca32747c2782`（本地 `main` 与 `origin/main` 在该提交一致）。**这些改动仍在工作树中，尚未提交**，请勿覆盖或重置。
- 本机验证环境为 Ubuntu 22.04 WSL、GCC 11.4、CMake 3.22；Windows 原生构建仍未验证。

## 已有交付

| 范围 | 状态 |
| --- | --- |
| 任务 0 | v1 落盘契约和四组文本夹具 |
| 任务 1 | 独立 C++17 写入器与读回测试，含 UTF-8 和路径校验 |
| 任务 2 | **已完成（工作树）**：录制会话、有界异步队列、收尾、时间旁路、失败矩阵和生命周期测试 |
| 任务 3–6 | 插件运行时、Jazzy 录制回放、独立后校验、性能验证 |

## 任务 2 交付

新增或修改的文件：

- `include/robot_mcap/recorder.hpp`：`Recorder`、`RecorderOptions`、`OwnedMessage`、`SubmitResult`、`StopResult`。
- `include/robot_mcap/io.hpp`、`src/io.cpp`：`OutputSink` 和 `FileSystem` 可注入 I/O 接口及原生实现。
- `src/recorder.cpp`：状态机、有界队列、写入线程、停止结果和失败收尾。
- `src/session_artifacts.{hpp,cpp}`：会话目录、配置快照、时间旁路、清单发布、同步和 SHA-256 使用。
- `src/sha256.{hpp,cpp}`、`src/text.{hpp,cpp}`：内部哈希与 UTF-8 校验。
- `tests/recorder_lifecycle_test.cpp`：屏障/门控故障注入和契约断言。
- `CMakeLists.txt`、`README.md`、`tests/mcap_roundtrip_test.cpp`：构建、文档和新增字段的初始化修正。

已实现的关键语义：

1. `start()` 独占创建 `<parent>/<session_id>`，写入配置快照，打开 `data_0000.mcap.partial`，启动单一写入线程。会话 ID 按契约校验；启动失败尽量清理本次创建的目录和文件。
2. 队列按 **载荷字节** 有界。`submit_copy` 在返回前复制；`submit_owned` 接收右值，仅在成功时转移所有权。队列满或单条超过预算立即返回 `QueueFull`。
3. `stop_async()` 同步停止接收并返回共享 future；`stop()` 等待同一结果。停止边界前已接收的消息由写入线程排空并写入；故障路径丢弃排队消息并拒绝后续提交。写入失败会自动收尾，`stop_async()` 可在不调用 `stop()` 时观察结果；析构函数会停止并 join。
4. 成功时 `data_0000.mcap.partial` 和可选的 `data_0000.timing.ndjson.partial` 重命名为最终文件，清单通过临时文件发布。失败时保留 `.partial` 和已知产物，并尽力发布失败清单。
5. 失败矩阵分别表达：MCAP 写入/关闭失败（文件 partial、最终消息数 null）；MCAP 完成但时间旁路失败（文件 finalized、会话 failed、`finalized_messages` 为已知的 `writer_processed`、timing state 为 partial/unknown）；仅同步失败（文件 finalized、durability failed）；清单发布失败（数据仍可读，stop 返回错误）。
6. 时间旁路按契约生成稀疏行（`source_clock` 存在，或通道策略为 `per_message` 且缺少发布时间）。`message_ordinal` 统计成功写入的物理 Message，跨通道递增；序号 2 没有行时后续行仍使用 3/4。行格式与 `tests/fixtures/session-v1/01-finalized/data_0000.timing.ndjson` **逐字节一致**，清单中的 sha256 与夹具一致。
7. `DurabilityMode::Sync` 在清单改名之前同步数据、时间旁路、配置和清单临时文件；之后同步会话目录和父目录。创建会话目录时即尝试同步父目录。任何要求的同步失败都会使同步模式 stop 失败，但已完成的 MCAP 仍可读。
8. `StopResult.reason` 区分 `Requested`、`WriterFailure`、`TimingFailure`、`PublishFailure`、`StartFailure`、`NotStarted`；`file_state` 与 `durability_state` 独立表达。

测试覆盖：正常/空会话、队列满、停止竞争、门控写入期间停止、复制缓冲复用、所有权拒绝时保留、写入/关闭/时间旁路/清单/同步失败、两实例隔离和失败隔离、并发注册、同 ID 并发启动、启动失败清理、乱序 log_time、零长度载荷、未使用通道、时间旁路 golden 字节和统计冻结。

## 验证记录

从仓库根目录在 WSL 运行：

```sh
cmake -S . -B build/core -DBUILD_TESTING=ON \
  -DFETCHCONTENT_SOURCE_DIR_FOXGLOVE_MCAP=/mnt/c/Users/lichonglou/.codex/visualizations/2026/11/09/01a1c069-bcb6-7300-beff-51bfe8000ff9/mcap-cpp-2.2.0-source
cmake --build build/core -j 2
ctest --test-dir build/core --output-on-failure
```

结果：`mcap_roundtrip` 与 `recorder_lifecycle` **2/2 通过**。另完成：

- ThreadSanitizer（`setarch $(uname -m) -R` 规避 WSL 内存布局）无报告。
- AddressSanitizer + UBSan 无报告。
- `-Wall -Wextra -Wpedantic -Werror` 构建通过。

## 复核留项

- 时间旁路的 ordinal/物理顺序保证仅对 finalized 文件成立；partial 文件只用于显式诊断，普通检查不消费。
- `recording-config.json` 目前直接记录写入器固定设置（profile 为空、无压缩、CRC/索引/摘要开启）。任务 4 引入 ROS profile 时，应让 `McapFile` 的有效设置成为配置快照的单一来源。
- 队列预算按队等待载荷字节计算，写入中的一条消息不计入，因此内存上界为预算加一条在途消息。零长度消息不受消息计数上限约束。
- `UINT64_MAX` 时间戳的上游索引边界仍是后续边界用例；`log_time = 0` 已覆盖。
- Windows 目录同步使用 `FlushFileBuffers`，不支持的卷会返回失败；Windows 构建和 `ros2` 目标尚未在目标环境验证。
- Windows 保留设备名（如 `con`）通过会话 ID 语法校验，但目录创建会失败；如需可加显式拒绝。
- 当前 API 没有独立的 `termination()`；写入故障后调用 `stop_async()` 即可等待自主收尾结果。

## 任务 3 接手点

1. 先写 `plugin_runtime` 测试，覆盖缺依赖、重复提供者、版本不符、循环、中途失败、在途回调、跨实例隔离和未请求后校验；使用屏障固定卸载顺序。
2. 实现 `IPlugin::apply(Context&, Scope&)`、带版本的服务描述、`IRecordingSink` 和 `IPrecheck::run(const StartContext&)`；宿主注入任务 2 的 `Recorder`。热路径直接调用已解析的 sink。
3. 基础门禁检查输出可写和空间阈值，允许 ROS 插件注册附加检查；插件卸载前停止生产、等待回调、收尾、释放缓冲和服务引用。
4. 运行 `ctest --test-dir build/core -R 'plugin_runtime|recorder_lifecycle' --output-on-failure`，确认无插件方式仍可完成核心录制。
5. 同步更新 README 和本交接文件。

阅读入口：[实施计划的任务 3](<D:/dev/robot-mcap/docs/superpowers/plans/2026-11-10-robot-mcap-v1-implementation.md:69>) · [会话落盘契约](<D:/dev/robot-mcap/docs/contracts/session-v1.md>) · [Recorder 接口](<D:/dev/robot-mcap/include/robot_mcap/recorder.hpp>) · [生命周期测试](<D:/dev/robot-mcap/tests/recorder_lifecycle_test.cpp>)
