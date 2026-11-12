# Robot MCAP 开发交接

交接日期为 2026 年 11 月 11 日。**任务 2、3 已提交，任务 4 已在 Jazzy 环境完成并通过真实录制/回放验证；从任务 5 接手。**

## 仓库、分支和环境

- 主仓库在 `D:/dev/robot-mcap`，远端为 [shiwanmenghuxiahuashan/robot-mcap](https://github.com/shiwanmenghuxiahuashan/robot-mcap)。
- `feature/task-2-recorder` 提交 `ca238da`；`feature/task-3-plugin-runtime` 提交 `e71b25a`。任务 4 的改动在 `feature/task-4-ros2-jazzy` 的**工作树中，尚未提交**。
- 任务 4 使用独立 WSL 发行版 `Ubuntu-24.04-Jazzy`（root，Ubuntu 24.04.5），安装 ROS 2 Jazzy、`ros2bag`、`rosbag2-storage-mcap`、`geometry_msgs` 和构建工具。仓库通过 `/mnt/d/dev/robot-mcap` 访问，Jazzy 构建目录为 `/root/robot-mcap-build/jazzy`（ext4）。
- 本机 WSL 使用 `~/.wslconfig` 的 `networkingMode=mirrored` 并通过 Windows 代理访问镜像；Ubuntu/ROS apt 源使用清华镜像，ROS key 来自清华 rosdistro 镜像。现有 Ubuntu 22.04 核心构建仍保留。
- 现有核心在 Ubuntu 22.04 和 Ubuntu 24.04/Jazzy 上均通过 3 个核心测试。

## 已有交付

| 范围 | 状态 |
| --- | --- |
| 任务 0 | v1 会话契约和四组文本夹具 |
| 任务 1 | 独立 C++17 MCAP 写入器与读回测试 |
| 任务 2 | **已提交**：录制会话、有界队列、收尾、时间旁路、失败矩阵 |
| 任务 3 | **已提交**：插件运行时、服务依赖/作用域、录制流程、门禁、原生模块 |
| 任务 4 | **已完成（工作树）**：ROS 2 Jazzy 数据源、QoS/时间/Schema、Jazzy 元数据、真实 bag info/play |
| 任务 5–6 | 独立后校验与报告、benchmark 和性能验收 |

## 任务 4 交付

新增文件：

- `plugins/ros2_jazzy/ros2_adapter.hpp`、`ros2_adapter.cpp`：`Ros2SourceOptions`/`Ros2Qos`/诊断与计数服务、`load_message_schema`、`Ros2MetadataWriter`。
- `plugins/ros2_jazzy/ros2_schema.cpp`：从安装的 `share/<pkg>/msg/*.msg` 读取主定义并递归展开依赖；`.idl` 作为单文件回退。
- `plugins/ros2_jazzy/ros2_source.cpp`：`ros2_source` 原生模块插件；配置发现、GenericSubscription、QoS、时间、Schema、门禁和诊断。
- `plugins/ros2_jazzy/ros2_metadata.cpp`：读取最终 MCAP，写 Jazzy `metadata.yaml`，并原子补写 `manifest.json` 的 `ros2_replay`/`diagnostics`。
- `plugins/ros2_jazzy/CMakeLists.txt`、顶层 `ROBOT_MCAP_WITH_ROS2` 选项、`tests/ros2_jazzy/record_replay_test.cpp`。

已实现的关键语义：

1. **源插件**：任务 3 的 `PluginRuntime` 加载同工具链原生模块。插件依赖 `IRos2SourceConfig`、`IRos2Diagnostics`、`IRos2SourceStats`（宿主提供）以及 `IMessageSink`、`IRecordingStatus`、`IPrecheckRegistry`（流程提供）。
2. **发现与订阅**：按配置发现选中 topic，使用 `rclcpp::GenericSubscription` 接收序列化 CDR；未选中 topic 不订阅；允许晚出现的 topic 由 200ms 发现定时器补订阅并记录诊断；同名 topic 类型不唯一或不匹配时记录 `ros2_topic_type_conflict` 并拒绝订阅，不混写。
3. **QoS 与时间**：每 topic 可配置 depth/reliability/transient_local，订阅和 `metadata.yaml` 使用同一 QoS；源时间戳非零时写 `publish_time`，否则按 `PerMessage` 回填并生成时间旁路；`log_time` 使用 RMW 接收时间戳（无则系统时钟）；sequence 保持 0。
4. **Schema/门禁**：Channel 使用 `cdr` 和 `ros2msg` Schema；必需 topic 在启动前预检（等待最多 2s），类型支持加载失败、Schema 缺失、必需 topic 缺失/类型冲突都会阻止启动；晚出现和可选 topic 的问题只记录诊断。
5. **元数据（adapter-only）**：`PluginRuntime::stop()` 返回后，`Ros2MetadataWriter` 读取最终 MCAP 的 Summary/Statistics，用 Jazzy `rosbag2_storage::BagMetadata` + `MetadataIo` 写 version 9 `metadata.yaml`（storage `mcap`、相对文件、时间范围、topic 类型/QoS、消息数），再原子重写 `manifest.json`：设置 `ros2_replay.state=ready`/`metadata_path`，合并源端诊断。元数据失败时写 `failed`、保留 MCAP 并追加 `ros2_metadata_error`；同步模式补做 metadata/manifest/目录 fsync。
6. **原生模块生命周期**：模块导出 `robot_mcap_plugin_module_v1`；源节点在 apply 创建、activate 启动单线程 executor/发现定时器，stop 取消、wait join，cleanup 释放；沿用任务 3 的模块卸载屏障。

测试覆盖：

- `load_message_schema("geometry_msgs/msg/Twist")` 展开 `Vector3` 依赖；缺失包失败。
- 必需 topic 缺失：启动失败，核心未启动、无会话目录。
- 主录制：`/test/int` 5 条（reliable）、`/test/late` 3 条（required+allow_late）、`/test/best_effort` 2 条（best_effort QoS）、`/test/unselected` 不订阅、`/test/conflict` 类型冲突诊断；共 10 条消息。
- 元数据：`metadata.yaml` 的 storage/message_count/topics/QoS 正确；`ros2 bag info` 显示 10 条和 topic 计数；`ros2 bag play --delay 1.0` 后独立订阅端收到 5/3/2 条并核对载荷；未选中 topic 不在清单/元数据/回放中。
- 元数据失败注入：manifest `ros2_replay=failed`、诊断可见、已完成 MCAP 仍可读。

## 验证记录

在 `Ubuntu-24.04-Jazzy` 中：

```sh
source /opt/ros/jazzy/setup.bash
cmake -S /mnt/d/dev/robot-mcap -B /root/robot-mcap-build/jazzy \
  -DBUILD_TESTING=ON -DROBOT_MCAP_WITH_ROS2=ON \
  -DFETCHCONTENT_SOURCE_DIR_FOXGLOVE_MCAP=/mnt/c/Users/lichonglou/.codex/visualizations/2026/11/09/01a1c069-bcb6-7300-beff-51bfe8000ff9/mcap-cpp-2.2.0-source
cmake --build /root/robot-mcap-build/jazzy -j 4
ctest --test-dir /root/robot-mcap-build/jazzy --output-on-failure
```

结果：`mcap_roundtrip`、`recorder_lifecycle`、`plugin_runtime`、`ros2_record_replay` **4/4 通过**；`ros2_record_replay` 连续重复 3 次通过。核心在 Ubuntu 24.04/GCC 13 和 Ubuntu 22.04/GCC 11.4 上均 3/3 通过。任务 2/3 的 TSAN、ASan/UBSan 和 `-Werror` 记录仍有效；ROS 适配层依赖 Jazzy 运行库，未单独跑 TSAN。

## 复核留项 / adapter-only 约束

- 与 rosbag2 / `rosbag2_storage_mcap` 的分层、能力对比和改进 backlog 已整理为
  [ROS 2 writer comparison](<D:/dev/robot-mcap/docs/research/2026-11-11-ros2-writer-comparison.md>)。
  最优先的改进是 MCAP ROS 2 profile、Channel QoS/类型哈希和嵌入式 rosbag2 Metadata。
- **MCAP 容器**：header profile 仍为空，Channel QoS/类型哈希未写入 MCAP，而放在 `metadata.yaml`；如果下游要求 MCAP ROS 2 profile 或嵌入式 rosbag2 元数据，需要最小核心补丁（profile + channel metadata），再按相同流程验证。
- **清单后置补写**：核心先发布 `manifest.json`（`ros2_replay=not_requested`），适配层随后原子改写为 `ready`/`failed`。宿主必须在 `stop()` 之后、把会话交给读取方之前调用 `Ros2MetadataWriter`；短暂窗口内读者会看到未补写状态。同步模式下补写会额外 fsync metadata/manifest/目录。
- **Schema**：`.msg` 依赖递归展开已实现；`.idl` 仅读取单文件，不展开 `#include`。非标准/未安装消息包会记录 `ros2_schema_error`。
- **时间**：GenericSubscription 使用 `rmw_message_info_t` 的 source/received timestamp；发布端未设置源时间时走 `log_time` 回填旁路。sequence 固定 0；source 计数证据不在本任务范围。
- **类型冲突检测**基于 ROS graph 的类型集合；已订阅类型之后发生变化的发布者不会被旧订阅接收，诊断会记录冲突。
- 回放测试使用 `ros2 bag play --delay 1.0` 和发现轮询；在慢速/共享 CI 上可适当增大延迟和超时。
- WSL 的 mirrored 网络、Windows 代理和清华镜像属于本机环境配置；换机器需重新准备 Jazzy 环境。

## 任务 5 接手点

1. 新增 `apps/robot_mcap_check/main.cpp`、`src/checks/mcap_checks.cpp`、`src/checks/check_report.cpp`、`tests/check_process_test.cpp`。
2. `robot-mcap-check --task <task.json>` 读取契约定义的 task/rules/manifest；检查器显式依赖读取、解码和统计服务，采用有界内存扫描。
3. 覆盖完好文件、结构异常、必需通道缺失、最大/最小间隔、缺少可信源计数（`insufficient_evidence`）、输入损坏和子进程被终止；报告使用新 `run_id`，临时文件+重命名，整体结论 `pass`/`fail`/`undetermined`。
4. 运行 `ctest -R '^check_process$'`；对任务 4 的真实会话目录运行检查子进程，验证重跑产生新报告、退出异常可被宿主识别、录制清单和原文件保持可用。
5. 同步更新 README 和本交接文件。

阅读入口：[实施计划的任务 5](<D:/dev/robot-mcap/docs/superpowers/plans/2026-11-10-robot-mcap-v1-implementation.md:90>) · [会话落盘契约](<D:/dev/robot-mcap/docs/contracts/session-v1.md>) · [ROS 适配层接口](<D:/dev/robot-mcap/plugins/ros2_jazzy/ros2_adapter.hpp>) · [回放测试](<D:/dev/robot-mcap/tests/ros2_jazzy/record_replay_test.cpp>) · [任务 3 插件运行时](<D:/dev/robot-mcap/include/robot_mcap/plugin_runtime.hpp>)
