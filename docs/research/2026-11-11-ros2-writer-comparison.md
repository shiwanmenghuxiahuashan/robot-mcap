# 调研：自研 MCAP 写入核心与 ROS 2 / rosbag2 的关系和差距

**日期**　2026-11-11
**范围**　任务 4 完成后的架构复盘与后续改进调研
**结论状态**　调研结论，未改变 v1 契约；改进项供任务 5/6 和后续版本决策

## 1. 摘要

这次比较涉及三层，不能混为一谈：

1. **自研写入核心**：`McapFile`、`Recorder`、会话产物。它是 ROS 无关的存储引擎；ROS 只是可选插件。
2. **自研 ROS 2 数据源插件**：`plugins/ros2_jazzy/ros2_source.cpp`。它负责 rclcpp 发现、订阅、QoS、CDR 字节、源时间和门禁；它把消息交给写入核心的 `IMessageSink`，自己不写 MCAP。
3. **rosbag2 的 MCAP 存储插件**：`rosbag2_storage_mcap`。它是 rosbag2 的存储后端，由 pluginlib 加载；`rosbag2_transport::Recorder` 负责采集，`rosbag2_cpp::Writer` 负责写，`rosbag2_storage_mcap` 负责 MCAP 文件。

因此，**自研写入核心和自研 ROS 插件是上下游关系，不是两个同类写入器**。真正与 `rosbag2_storage_mcap` 对位的是自研写入核心（`McapFile`/`Recorder`），但两者接口和生态定位不同：我们没有实现 rosbag2 的 `StorageInterface`，也没有让 `ros2 bag record` 使用自研写入器。

结论先说：

- **ROS 边界上的实现相似**：都用 rclcpp 序列化订阅、CDR、QoS、类型支持、消息定义和 `metadata.yaml`。
- **存储、队列和生命周期层不同**：自研核心显式提供有界队列、复制/所有权交接、停止边界、失败矩阵、会话清单和时间旁路；rosbag2 把这些分散在 `rosbag2_transport`、`rosbag2_cpp` 和 storage plugin 中。
- **不能笼统说“我们更好”**。自研实现在项目契约、ROS 无关性、失败可观测性和可测试性上更强；rosbag2 在成熟度、MCAP ROS 2 profile/Channel 元数据、压缩/分卷、工具链和生态兼容性上更强。
- 当前最明确的改进项是 **MCAP header profile、Channel QoS/类型哈希和嵌入式 rosbag2 Metadata 记录**；`ros2 bag info/play` 已能读取我们的文件，但这些元数据缺失会影响 Foxglove、MCAP 工具和严格按 ROS 2 profile 消费的下游。

## 2. 调研方法与证据

- 阅读本仓库：`src/mcap_file.cpp`、`src/recorder.cpp`、`src/session_artifacts.cpp`、`include/robot_mcap/recorder.hpp`、`plugins/ros2_jazzy/*`、`tests/ros2_jazzy/record_replay_test.cpp`。
- 阅读 Jazzy 安装包源码：`rosbag2_storage_mcap` 0.26.11 的 `src/mcap_storage.cpp`（ros2/rosbag2 `jazzy` 分支）。
- 在 Ubuntu 24.04 + ROS 2 Jazzy 上执行真实录制，然后运行：
  - `ros2 bag info <session>`
  - `ros2 bag play --delay 1.0 <session>`
  - 独立 `std_msgs/msg/Int32` 订阅端核对数量、类型和载荷。
- 测试结果：Jazzy 构建 4/4 通过，`ros2_record_replay` 连续 3 次通过；录到 10 条消息，覆盖 reliable、best-effort、晚出现、未选中 topic 和类型冲突。

## 3. 数据流对照

### 3.1 自研实现

```text
ROS topics
  → ros2_source（rclcpp GenericSubscription / QoS / CDR / Schema / source time）
  → IMessageSink::submit_copy
  → Recorder 有界队列
  → 写入线程 → McapFile → mcap::McapWriter
  → data_0000.mcap + recording-config.json + manifest.json + timing.ndjson
stop() 后
  → Ros2MetadataWriter 读取最终 MCAP
  → metadata.yaml（rosbag2_storage::MetadataIo）
  → 原子补写 manifest.json 的 ros2_replay / diagnostics
```

### 3.2 rosbag2

```text
ROS topics
  → rosbag2_transport::Recorder（发现、订阅、QoS、消息交接）
  → rosbag2_cpp::Writer
  → rosbag2_storage_mcap（pluginlib 存储插件）
  → MCAP + metadata.yaml
  → rosbag2_cpp / MetadataIo 写元数据；storage plugin 还写嵌入式 rosbag2 Metadata 记录
```

## 4. 相似点

1. **ROS 数据面**：都用 rclcpp 的序列化/通用订阅，保留 CDR 原始字节，使用 QoS、类型支持和 topic 发现。
2. **MCAP 数据面**：都写 Schema、Channel、Message；都使用 MCAP C++ 写入库（我们锁定 Foxglove 2.2.0，rosbag2 使用其 MCAP C++ 依赖/vendor）。
3. **元数据面**：都生成 rosbag2 version 9 `metadata.yaml`；我们的适配层直接使用 Jazzy 的 `rosbag2_storage::MetadataIo` 和 `BagMetadata`。
4. **回放验收**：`ros2 bag info/play` 都能读取我们的产物；我们的测试用 rosbag2 自己的工具做独立消费验证。

## 5. 主要差异

| 维度 | 自研实现 | rosbag2 / rosbag2_storage_mcap |
| --- | --- | --- |
| 核心依赖 | ROS 无关；ROS 是可加载原生模块 | ROS 原生；依赖 rclcpp、rosbag2、pluginlib |
| 写入职责 | `Recorder` 同时管理队列、写入线程、停止边界、会话产物和持久化 | `rosbag2_transport::Recorder` 采集，`rosbag2_cpp::Writer` 写，storage plugin 落盘 |
| 队列/所有权 | 显式按字节有界；`submit_copy`/`submit_owned`；QueueFull 立即返回 | rosbag2 自己的采集/写入队列和消息交接语义 |
| 停止/失败 | `StopResult` 分离文件状态、持久化状态和原因；失败清单、诊断、部分产物 | rosbag2 的停止/错误模型；以 bag 目录和 metadata 为中心 |
| 会话产物 | `manifest.json`、`recording-config.json`、可选 `timing.ndjson` | `metadata.yaml` + MCAP；由 rosbag2 组织 |
| 时间证据 | `publish_time` 回填/设备时钟可写时间旁路 | 由 storage metadata 和 topic/QoS 元数据表达 |
| MCAP header profile | **当前为空** | `McapWriterOptions("ros2")`，写 `ros2` profile |
| Channel 元数据 | **未写**；QoS 放在 `metadata.yaml` | 写 `offered_qos_profiles` 和 `topic_type_hash` |
| 嵌入式 rosbag2 元数据 | **未写** | 写名为 `rosbag2` 的 MCAP Metadata 记录，含序列化 `BagMetadata` |
| Schema | 从安装的 `.msg` 递归展开；`.idl` 单文件回退 | 使用 rosbag2 的 `MessageDefinition`；支持其完整定义链路 |
| 压缩/分卷 | 无压缩；不分卷；Chunk CRC 默认开启 | 支持 zstd/lz4 预设、分卷等；storage plugin 默认 `noChunkCRC=true` |
| 插件模型 | 自己的 `PluginRuntime`、服务依赖、原生模块 ABI | pluginlib + `rosbag2_storage::StorageInterface` |
| 工具/生态 | 自研测试与清单；用 rosbag2 工具做兼容验收 | 成熟 CLI、存储插件、压缩、分卷、长期维护 |
| 失败可观测性 | 明确的 manifest/diagnostics/failure matrix、可注入 I/O | rosbag2 的日志/元数据/异常语义 |
| 平台验证 | Ubuntu 22.04 + 24.04/Jazzy WSL；无 TSAN on ROS | 广泛平台和发行版验证 |

rosbag2_storage_mcap 的 Jazzy 源码依据（0.26.11）：

- `McapWriterOptions` 构造函数使用 profile `"ros2"`；YAML 配置有意不暴露 `profile`/`library`。
- `create_topic` 把 `offered_qos_profiles` 和 `topic_type_hash` 写入 `mcap::Channel.metadata`。
- 关闭时写 `Metadata{name="rosbag2"}`，其中 `serialized_metadata` 是 `BagMetadata` 的 YAML；读取端从该记录恢复元数据。
- storage plugin 默认 `noChunkCRC = true`、`compression = None`；压缩由 preset/config 覆盖。

## 6. 自研实现的相对优势

- **ROS 无关的核心**：同一套 `Recorder`/`McapFile` 可在没有 ROS 的进程中使用；ROS 只作为可选原生插件。
- **明确的提交/队列语义**：按字节有界、复制与所有权入口、QueueFull、停止边界内消息排空、故障后拒绝新提交。
- **生命周期和失败矩阵**：文件收尾、持久化、清单发布、时间证据分开表达；失败保留部分产物并发布失败清单。
- **可测试性**：可注入 `OutputSink`/`FileSystem`，有队列/停止/写入/关闭/同步/时间旁路/清单失败的确定性测试。
- **插件运行时**：版本化服务、两阶段启动、作用域清理、在途回调屏障、模块卸载顺序和原生模块测试。
- **时间证据**：`PerMessage` 源时间、回填标记和设备时钟值可以落到时间旁路，供后校验使用。
- **会话清单**：`accepted/rejected/writer_processed/finalized_messages`、诊断和通道快照可直接供独立检查器消费。

## 7. 当前不足（按优先级）

### P0：MCAP ROS 2 profile 与 Channel 元数据

- 现状：MCAP header profile 为空；Channel 未写 `offered_qos_profiles`/`topic_type_hash`；QoS 只在 `metadata.yaml`。
- 影响：`ros2 bag info/play` 已通过，但 Foxglove、MCAP CLI、严格按 ROS 2 profile 读取的下游可能拿不到 QoS/类型哈希/profile。
- 建议：给核心增加最小字段（`FileOptions.profile`、`ChannelSpec.metadata`），ROS 适配层填入 `ros2`、`offered_qos_profiles`、`topic_type_hash`；补 MCAP Reader/CLI 验收。
- 这是把 adapter-only 提升到“ROS 2 profile 完整”的最小改动，不需要改队列/清单架构。

### P0/P1：嵌入式 rosbag2 Metadata 记录

- 现状：只写 `metadata.yaml`；没有 MCAP 内的 `Metadata{name="rosbag2"}`。
- 影响：rosbag2 的读取端可从同名记录恢复完整 `BagMetadata`；缺少时只能依赖 `metadata.yaml`。当前 `ros2 bag info/play` 可用，但某些工具/迁移场景会丢元数据。
- 建议：在 MCAP 关闭前写一条 `rosbag2` Metadata 记录（`serialized_metadata` 为 `BagMetadata` YAML，并含 `ROS_DISTRO`）。需要核心暴露写 Metadata 的能力，或由适配层在最终 MCAP 上追加/重建（后者复杂，优先核心小接口）。

### P1：类型定义和类型哈希完整性

- 现状：Schema 从安装的 `.msg` 递归展开；类型哈希 `type_description_hash` 为空；`.idl` 仅单文件回退。
- 建议：
  - 从 ROS 类型支持获取并保存 REP-2011 类型哈希。
  - 补齐 `.idl` 的 `#include` 依赖展开，或明确限定 `ros2msg`。
  - 与 rosbag2 的 `MessageDefinition` 逐字节对比，添加 golden 测试。

### P1：清单后置补写与最终收尾

- 现状：核心先发布 `manifest.json`，适配层在 `runtime.stop()` 后补写 `ros2_replay`/诊断；存在短窗口，同步模式的补写是适配层自行 fsync。
- 建议：评估是否在核心增加一个明确的 finalize 钩子（在数据关闭后、清单发布前生成 ROS 元数据），或继续 adapter-only 并在 API/文档中把“必须由宿主调用 `Ros2MetadataWriter`”作为契约一部分。任务 5 的检查器必须先确认这个调用已完成。

### P1：压缩、分卷和默认参数

- 现状：无压缩、不分卷、Chunk CRC 开启；rosbag2 storage plugin 默认 `noChunkCRC=true`，支持 zstd/lz4 preset。
- 建议：任务 6 benchmark 对比压缩/分卷/Chunk 大小/CRC 的吞吐和文件大小；再决定默认值。不要在没有实测前承诺与 rosbag2 相同的性能。

### P2：是否需要 rosbag2 storage plugin 适配

- 现状：没有实现 `rosbag2_storage::StorageInterface`，`ros2 bag record` 不会使用自研写入器。
- 建议：先明确产品需求。若需要“让 rosbag2 生态直接使用自研写入核心”，再实现 storage plugin；否则保持“自研 SDK/核心 + 自研 ROS 数据源”的路线，避免维护两套插件接口。

### P2：ROS 层测试和平台覆盖

- 现状：真实 Jazzy 录制/回放通过；ROS 层未单独跑 TSAN/ASan；Windows 未验证。
- 建议：为 ROS 适配层增加 ASan/TSAN（可能受 ROS 运行库影响，需基线）、DDS 实现矩阵（CycloneDDS/Fast DDS）、Ubuntu 24.04 原生和 arm64 验证。

## 8. 评价：谁更好

**如果只比较“自研写入核心 vs 自研 ROS 插件”**：两者不是同类实现。写入核心是存储引擎，ROS 插件是数据源适配；相似点只在 MCAP/ROS 数据边界。不能用一个替代另一个。

**如果比较“自研整体 vs rosbag2 / rosbag2_storage_mcap”**：

- 自研更适合本项目当前目标：ROS 无关核心、显式队列/所有权/停止/失败契约、可注入 I/O、会话清单和时间证据、插件运行时隔离。
- rosbag2 更适合标准化和生态：MCAP ROS 2 profile、Channel QoS/类型哈希、嵌入式元数据、压缩/分卷、成熟 CLI、广泛平台验证和长期维护。
- 我们当前**不是 rosbag2 的替代品**，也不应在文档或汇报中声称整体更优；`ros2 bag info/play` 通过只证明已覆盖场景的互操作性。
- 如果下游明确要求 MCAP ROS 2 profile 或嵌入式 rosbag2 元数据，优先完成 P0/P1 的三项核心/适配层改进；如果目标只是按本项目契约完成内部录制和独立检查，当前 adapter-only 实现已满足任务 4 验收。

## 9. 对后续任务的建议

- **任务 5（独立后校验）**：
  - 检查器按清单消费会话；在读取前确认宿主已调用 `Ros2MetadataWriter`，否则把 `ros2_replay` 视为未完成。
  - 不要把 `ros2 bag info/play` 通过当作质量合格的充分条件；仍要检查结构、通道、间隔和证据。
- **任务 6（benchmark）**：
  - 在相同 topic/负载/硬件上同时测自研核心和 rosbag2 storage plugin；分别记录提交延迟、QueueFull、停止耗时、文件大小、CPU/内存。
  - 单独测压缩/分卷/Chunk 大小和 CRC；用实测决定默认参数，不沿用 rosbag2 的默认值假设。
- **版本演进**：
  - 先做 P0 的 profile/Channel 元数据（最小核心补丁），再决定嵌入式 Metadata 和类型哈希。
  - 只有明确需要 `ros2 bag record` 使用自研写入器时，才评估实现 rosbag2 storage plugin。

## 10. 参考资料

- 本仓库：
  - [会话契约](../contracts/session-v1.md)
  - [实施计划](../superpowers/plans/2026-11-10-robot-mcap-v1-implementation.md)
  - [Recorder 接口](../../include/robot_mcap/recorder.hpp)
  - [ROS 适配层](../../plugins/ros2_jazzy/ros2_adapter.hpp)
  - [ROS 数据源](../../plugins/ros2_jazzy/ros2_source.cpp)
  - [元数据写入](../../plugins/ros2_jazzy/ros2_metadata.cpp)
  - [回放测试](../../tests/ros2_jazzy/record_replay_test.cpp)
- rosbag2：
  - [rosbag2_storage_mcap（Jazzy 分支）](https://github.com/ros2/rosbag2/blob/jazzy/rosbag2_storage_mcap/src/mcap_storage.cpp)
  - `McapWriterOptions("ros2")`、`offered_qos_profiles`、`topic_type_hash`、`Metadata{name="rosbag2"}` 的实现见上文件。
  - Jazzy `BagMetadata`/`MetadataIo`：`/opt/ros/jazzy/include/rosbag2_storage/rosbag2_storage/`。
- MCAP：
  - [MCAP 规范](https://mcap.dev/spec)
  - [MCAP ROS 2 profile](https://mcap.dev/spec/registry#ros2)
  - Foxglove MCAP C++ 2.2.0（本仓库锁定 `9b4d3e36c7f7742541df33e0cff35c95de6af2a1`）。
