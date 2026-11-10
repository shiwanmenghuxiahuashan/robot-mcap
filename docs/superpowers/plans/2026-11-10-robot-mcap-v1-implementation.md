# Robot MCAP 首版实施计划

> 逐项执行本计划。可使用 `superpowers:executing-plans`，或在明确选择委派后使用 `superpowers:subagent-driven-development`。每项以可复现的验收结果为完成条件。

**目标**　交付可独立构建的 C++ MCAP 录制库、ROS 2 Jazzy 数据源适配、前置门禁、独立后校验与可复现性能基线。

**架构**　数据源在宿主进程把序列化消息交给固定写入核心。插件运行时负责启动装配、依赖和资源管理。后校验进程通过版本化文件契约消费已完成会话。

**技术基线**　Ubuntu 24.04、ROS 2 Jazzy、amd64／arm64。候选工程选择为 C++17、CMake／CTest、上游 MCAP C++ 库、Jazzy 的 rosbag2 元数据 API。

**输入草案**　[插件化录制库技术方案草案 v0.3](<D:/dev/lichonglou/robot/mcap/插件化录制库技术方案草案.md>)。Q1–Q21 是本计划输入。草案第 9 节标为建议的决策、下面的路径与接口均为实施候选。先评审任务 0 的契约，再冻结首版接口。

**仓库起点**　`D:/dev/robot-mcap` 当前目录仅见 Git 元数据，`HEAD` 指向 `main`，未解析到提交。以下路径均相对于仓库根目录。

## 全局约束

- 保留原始消息载荷和源时间戳，派生结果另存。通用核心可脱离 ROS 和插件运行时构建与使用。
- 数据源适配与写入同进程，后校验独立进程。每个录制实例只对应一次会话，队列、状态、文件和插件作用域隔离。
- 提交成功表示接管不可变缓冲。队列按字节有界，满时立即返回 `QueueFull`。核心统计接收、拒绝、底层写入与最终产物状态。
- 同步和异步停止共享收尾结果。区分文件格式收尾、可选持久化同步和后校验结论。写入失败保留部分产物并拒绝后续消息。
- 按启动配置装配原生插件，使用匹配 SDK 和工具链。宿主负责多设备和跨实例调度。
- 性能门槛依据目标硬件实测制定。ROS 回放以真实 Jazzy 订阅端接收到正确消息为验收证据。

## 优先验证的失败场景

| 场景 | 验收 |
| --- | --- |
| 文件正常关闭，持久化同步失败 | 返回明确失败，文件仍可被读回，文件收尾与持久化结果分别可见。任务 2。 |
| 生产线程与停止并发，或单条消息超过队列预算 | 停止边界前成功接收的数据进入收尾流程，边界后拒收，超预算消息立即返回。任务 2。 |
| 插件只初始化了一半，或退出时仍有在途回调 | 撤销已注册服务，等待任务和插件所有缓冲释放后才允许卸载。任务 3。 |
| topic 晚出现、类型改变，或 ROS 兼容元数据生成失败 | 允许配置范围内的晚出现，拒绝混写，标明产物及回放准备状态。任务 4。 |
| 校验子进程中途退出，或源端证据不足 | 保留录制，报告任务执行失败或证据不足。任务 5。 |

## 任务 0：确定 v1 落盘和读取契约

**文件**　`docs/contracts/session-v1.md`、`tests/fixtures/session-v1/`。此任务的设计评审对象是文档、示例清单、检查任务、报告和错误状态样例。

- [ ] 确定目录结构候选。每会话一目录，首版默认一个 `data_0000.mcap`，录制中用 `data_0000.mcap.partial`。同目录存放 `manifest.json`、`recording-config.json`、可选的 `metadata.yaml` 以及 `checks/<run_id>/` 下的任务、规则快照和报告。目录和清单使用稳定会话 ID，清单路径为会话内相对路径，读取端拒绝绝对路径、越界路径和未知主版本。
- [ ] 确定容器候选。采用上游 `mcap::McapWriter`，默认启用分块、消息索引和摘要，不压缩，Chunk 目标 1 MiB，默认开启 Chunk 校验。首版默认不轮转，清单的 `files` 使用数组。所有默认值记录在配置快照，并由任务 6 对照实测。
- [ ] 确定编码和时间候选。数据源声明通道名、消息编码、Schema 或无 Schema、时间来源。通用核心保存原始字节。使用非负纳秒写入 `log_time`。有可信的源发布时间时写 `publish_time`，缺失时用 `log_time` 回填并保留可区分回填的契约。采样时间继续保留在原始载荷中。对载荷外的设备时钟值和逐条缺失标志，明确 v1 的存储位置、文件关联、顺序与失败处理，再开放该类输入。禁止把回填值当作源时间证据。ROS 候选使用 `ros2` profile、原始 `cdr` 及对应 `ros2msg` 或 `ros2idl` 定义，以 Jazzy 回放验证。
- [ ] 定义清单和报告的最小字段及取值。清单含 `schema_version`、`session_id`、会话状态、`files`、通道、统计、诊断和配置快照路径。每个文件含相对路径、完成状态、字节数、记录时间范围。报告含规则与工具版本、输入会话、检查项、指标、证据和结论，使用唯一 `run_id`。将文件完成、持久化请求结果、ROS 回放准备及质检结果分别表达。
- [ ] 明确发布顺序。先收尾并关闭数据，再发布完成清单。普通读取只消费清单中标记完成且存在的文件。同步模式明确文件和目录的同步范围、错误传播及成功通知边界。故障保留已知状态和临时产物。复查创建新 `run_id`。
- [ ] 验收四类样例。完成会话、失败且保留部分产物、ROS 兼容会话、证据不足的报告都可逐字段解释。重点确认无 Schema 策略、额外时间值的表示以及 ROS 产物。任务 1 的测试夹具使用评审后的版本。

参考：[MCAP 规范](https://mcap.dev/spec) · [MCAP ROS 2 profile](https://mcap.dev/spec/registry#ros2) · [Jazzy BagMetadata](https://github.com/ros2/rosbag2/blob/jazzy/rosbag2_storage/include/rosbag2_storage/bag_metadata.hpp)

## 任务 1：实现无 ROS 的最小写入闭环

**文件**　`CMakeLists.txt`、`cmake/Dependencies.cmake`、`include/robot_mcap/types.hpp`、`src/mcap_file.cpp`、`tests/mcap_roundtrip_test.cpp`。

**接口候选**　内部 `McapFile::open(path, options)`、`register_channel(ChannelSpec)`、`write(ChannelId, MessageView)`、`finish()`。状态或返回值保留路径、操作和底层错误。

- [ ] 用 CTest 建立 `mcap_roundtrip`。写入一条有 Schema 的合成消息，使用上游 Reader 验证通道名、Schema、原始字节、纳秒时间及索引；用任务 0 的规则覆盖缺时间和无 Schema 的行为。
- [ ] 运行核心构建和 `ctest --test-dir build/core -R '^mcap_roundtrip$' --output-on-failure`，先观察缺少写入实现导致的失败。
- [ ] 封装上游 writer。锁定实际使用的上游版本及压缩依赖，在 CMake 中分离核心和 ROS 目标。检查核心构建与消费测试可以在未加载 ROS 环境中通过。
- [ ] 重跑测试，使用独立 CLI 抽查文件和索引，保存版本和命令。

## 任务 2：实现录制会话、有界队列和收尾

**文件**　`include/robot_mcap/recorder.hpp`、`src/recorder.cpp`、`src/session_artifacts.cpp`、`tests/recorder_lifecycle_test.cpp`。

**接口候选**　`Recorder::start()`、`register_channel(ChannelSpec)`、`submit_copy(ChannelId, MessageView)`、`submit_owned(ChannelId, OwnedMessage)`、`stop_async()` 返回共享的停止结果，`stop()` 等待同一结果。

- [ ] 新增 `recorder_lifecycle` 测试。用屏障而非延时固定队列满、停止与提交竞争、写入／关闭／同步失败、两实例互不影响。复制入口覆盖调用者立即复用输入，所有权入口覆盖停止前缓冲保持有效。
- [ ] 运行测试并确认因尚未实现队列与收尾而失败。
- [ ] 将状态、接收边界、通道注册和队列预算纳入同一同步协议。写入线程按队列顺序调用任务 1 接口。按任务 0 发布产物与统计。用可注入文件操作覆盖 I/O 故障。
- [ ] 重跑 `ctest --test-dir build/core -R 'mcap_roundtrip|recorder_lifecycle' --output-on-failure`。核对成功录制的消息数与读取器结果，故障下分别核对接收、拒绝、写入和部分产物。

## 任务 3：实现插件运行时和录制流程

**文件**　`include/robot_mcap/plugin_runtime.hpp`、`src/plugin_runtime.cpp`、`src/recording_flow.cpp`、`tests/plugin_runtime_test.cpp`。

**接口候选**　`IPlugin::apply(Context&, Scope&)`、带版本的服务描述、`IRecordingSink`、`IPrecheck::run(const StartContext&)`。宿主注入固定核心。热路径直接调用已解析的 sink。

- [ ] 新增 `plugin_runtime` 测试。覆盖缺依赖、重复提供者、版本不符、循环、中途失败、在途回调、跨实例隔离和未请求后校验。用测试插件验证卸载前停止生产、等待回调、收尾、释放缓冲及服务引用的顺序。
- [ ] 实现静态配置驱动的装配、依赖排序、类型检查和作用域清理。先用内置工厂跑通，再验证同工具链构建的原生模块从启动到关停的生命周期。基础门禁负责输出可写和空间阈值，并允许 ROS 插件注册附加检查。
- [ ] 运行 `ctest --test-dir build/core -R 'plugin_runtime|recorder_lifecycle' --output-on-failure`。确认无插件方式仍可完成核心录制。

## 任务 4：实现 ROS 2 Jazzy 录制与实际回放

**文件**　`plugins/ros2_jazzy/CMakeLists.txt`、`plugins/ros2_jazzy/ros2_source.cpp`、`plugins/ros2_jazzy/bag_metadata.cpp`、`tests/ros2_jazzy/record_replay_test.cpp`。

**接口**　源插件调用 `IRecordingSink`，ROS 适配层负责消息定义、QoS 和回放元数据。使用 Jazzy 类型与工具链构建。

- [ ] 新增 `ros2_record_replay` 测试。发布一个已知消息流，覆盖必需 topic 缺失、选择范围外 topic、允许晚出现和类型冲突。录完执行 `ros2 bag info`、`ros2 bag play`，用独立订阅端核对数量、类型和载荷。回放测试先建立订阅和发现条件，避免把测试自身的启动竞争当成丢包。
- [ ] 按配置发现并订阅选中的序列化 topic，接入 QoS 覆盖与类型支持门禁。保留字节和真实可取得的时间信息。记录晚出现与类型冲突，拒绝不兼容消息。
- [ ] 在适配层生成与实际 MCAP 文件和统计相符的 Jazzy 元数据。模拟元数据生成失败，验证错误可见且已完成 MCAP 可独立读取。
- [ ] 在已加载 Jazzy 环境的构建中运行 `ctest --test-dir build/jazzy -R '^ros2_record_replay$' --output-on-failure`，保存真实订阅证据。

## 任务 5：实现独立后校验和版本化报告

**文件**　`apps/robot_mcap_check/main.cpp`、`src/checks/mcap_checks.cpp`、`src/checks/check_report.cpp`、`tests/check_process_test.cpp`。

**接口候选**　`robot-mcap-check --task <task.json>` 读取任务 0 定义的文件清单与规则快照。读、解码、统计服务由检查器显式依赖。

- [ ] 新增 `check_process` 测试。覆盖完好文件、结构异常、必需通道缺失、最大／最小间隔、缺少可信源计数、输入损坏与子进程被终止。验证同输入和规则的指标与结论可重复。
- [ ] 实现有界内存扫描。明确统计时间口径和跨块间隔计算，缺少源计数报告证据不足。逐项输出证据，由流程按配置决定整体验收。
- [ ] 运行 `ctest --test-dir build/core -R '^check_process$' --output-on-failure`。对任务 4 录制目录运行真实检查子进程，验证重跑产生新报告，退出异常可被宿主识别。

## 任务 6：建立 benchmark 和交付验收

**文件**　`benchmarks/core_bench.cpp`、`benchmarks/ros2_bench.cpp`、`benchmarks/README.md`、`docs/validation/v1.md`。

- [ ] 建立可复现输入。分别运行小消息高频、大消息和混合流，记录消息大小、速率、生产者、持续时长和实例数。核心场景分别测复制与所有权交接，ROS 场景纳入实际发现与订阅。
- [ ] 测吞吐、提交延迟分位数、停止耗时、CPU、峰值内存、队列和拒收计数。覆盖长跑、突发、过载、压缩、不同队列预算、多实例和同步落盘。将后校验的时间、内存单独统计。
- [ ] 对照生产端可观测计数、拒收、写入和最终文件计数。记录目标机器、架构、文件系统、磁盘、SDK、ROS 版本与配置。根据实测确定默认参数与性能门槛。
- [ ] 在目标环境运行核心与 Jazzy 全量 CTest，复跑真实录制、回放和独立校验。按平台记录已验证的覆盖和缺项。

## 执行顺序

按 0 → 1 → 2 推进，首先用合成数据交付可读回的通用核心。之后完成 3 → 4 → 5，用一份真实 Jazzy 会话贯通录制、回放和报告。任务 6 完成后再确定性能验收阈值。

核心构建命令以 `cmake -S . -B build/core -DROBOT_MCAP_WITH_ROS2=OFF -DBUILD_TESTING=ON` 为计划约定，随后执行 `cmake --build build/core`。Jazzy 构建在已加载 ROS 环境中将选项改为 `ON`，使用独立的 `build/jazzy` 目录。
