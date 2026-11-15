# Robot MCAP v1 代码审查报告：ROS 2 Jazzy 适配层

- **审查日期**：2026-11-13
- **审查基线**：`main` @ `76ab27c`（任务 1–4 已合并）
- **审查范围**：仓库全部代码；实际发现集中在 `plugins/ros2_jazzy/`
- **审查强度**：max（代码走读 + WSL Jazzy 实测复现 + rosbag2 源码对照）
- **结论**：15 项发现（P1 7 项、P2 5 项、P3 3 项）。未发现核心库的高严重度缺陷，但核心库覆盖不完整（见第 7 节）。P1 问题应在本轮或任务 5 开始前修复。

## 1. 总体结论

本轮审查的主要问题集中在 ROS 2 适配层，按主题可分为四类：

1. **Schema 格式与 rosbag2 规范不一致**（F-01 至 F-04）。手写的 `.msg` 依赖展开逻辑缺少规范的 `MSG:` 行、误跳过有界序列字段、拒绝合法的空定义、把 IDL 标成 `ros2msg`。由于本项目的回放测试使用本机 typesupport，`ros2 bag play` 仍能通过，问题只会在 Foxglove 等外部 MCAP 消费者或严格的 schema 读取方暴露。
2. **元数据收尾可靠性**（F-05 至 F-07）。`metadata.yaml` 写入失败仍标记 `ready`；manifest 中的文件路径未做契约要求的边界校验；manifest 结构异常会从返回 `Status` 的 API 抛出异常。
3. **运行期配置与资源问题**（F-08 至 F-12）。重复 topic 配置导致重复录制；订阅失败诊断每 200 ms 无限追加；QoS 不匹配无检查；订阅失败前已注册 channel；重试写入重复追加诊断。
4. **性能与可维护性**（F-13 至 F-15）。发现定时器长期空转；重复实现 rosbag2 已有的消息定义展开；重复实现核心已有的文件同步接口。

根因很集中：适配层重复实现了 `rosbag2_cpp::LocalMessageDefinitionSource` 和核心 `FileSystem` 已经提供的能力。直接复用官方实现可以同时消除 F-01 至 F-04 一类格式漂移和 F-15。

## 2. 严重度定义

| 级别 | 判定标准 | 处理建议 |
| --- | --- | --- |
| P1 高 | 导致录出的 schema/元数据不符合契约或被外部工具误读；或收尾流程异常、路径越界 | 任务 5 开始前修复 |
| P2 中 | 特定配置或故障下出现重复数据、诊断膨胀、静默丢数据、空 channel | 本轮修复，可与任务 5 并行 |
| P3 低 | 性能开销或可维护性问题，不改变正确性 | 随重构处理 |

## 3. 发现汇总

| ID | 级别 | 位置 | 问题 | 验证方式 |
| --- | --- | --- | --- | --- |
| F-01 | P1 | `plugins/ros2_jazzy/ros2_schema.cpp:138` | 拼接的 ros2msg schema 缺少每段定义前的 `MSG: <pkg>/<type>` 行 | Jazzy 实测 |
| F-02 | P1 | `plugins/ros2_jazzy/ros2_schema.cpp:158` | 含 `=` 的行一律跳过，误伤 `Type[<=N]` 有界序列字段 | Jazzy 实测 |
| F-03 | P1 | `plugins/ros2_jazzy/ros2_adapter.hpp:97` | `ok()` 要求 data 非空，合法的空消息定义（如 Empty）被判不可用 | Jazzy 实测 |
| F-04 | P1 | `plugins/ros2_jazzy/ros2_schema.cpp:207` | `.idl` 回退数据被无条件标记为 `ros2msg` | Jazzy 实测 |
| F-05 | P1 | `plugins/ros2_jazzy/ros2_metadata.cpp:263` | `MetadataIo::write_metadata` 写失败未检测，仍标 `ros2_replay=ready` | rosbag2 源码对照 |
| F-06 | P1 | `plugins/ros2_jazzy/ros2_metadata.cpp:179` | manifest 文件路径未拒绝绝对路径、盘符、`..`、反斜杠 | 代码走读 |
| F-07 | P1 | `plugins/ros2_jazzy/ros2_metadata.cpp:174` | manifest 结构异常时 `json::value()` 抛异常而非返回失败 | 代码走读 |
| F-08 | P2 | `plugins/ros2_jazzy/ros2_source.cpp:86` | 重复 topic 配置不去重，消息被录两遍 | 代码走读 |
| F-09 | P2 | `plugins/ros2_jazzy/ros2_source.cpp:271` | 订阅失败诊断每 200 ms 追加一次，无只报一次守卫 | 代码走读 |
| F-10 | P2 | `plugins/ros2_jazzy/ros2_source.cpp:186` | 预检不检查发布端 QoS，默认 reliable 订阅收不到 best-effort 数据且无警告 | 代码走读 |
| F-11 | P2 | `plugins/ros2_jazzy/ros2_source.cpp:280` | 先注册 channel 再创建订阅，订阅失败留下空 topic | 代码走读 |
| F-12 | P2 | `plugins/ros2_jazzy/ros2_metadata.cpp:275` | 成功/失败路径都追加全部诊断，重试写入会重复 | 代码走读 |
| F-13 | P3 | `plugins/ros2_jazzy/ros2_source.cpp:123` | 发现定时器永不取消，每 200 ms 复制整张 topic 类型图 | 代码走读 |
| F-14 | P3 | `plugins/ros2_jazzy/ros2_schema.cpp:105` | 重复实现 rosbag2 已有的消息定义展开 | 代码走读 |
| F-15 | P3 | `plugins/ros2_jazzy/ros2_metadata.cpp:64` | 重复实现核心已有的文件同步接口，Windows 上为空操作 | 代码走读 |

## 4. 详细发现

### F-01 ros2msg schema 缺少 `MSG:` 定义行

- **位置**：`plugins/ros2_jazzy/ros2_schema.cpp:138`
- **级别**：P1
- **验证**：Jazzy 环境实测复现，对照 `rosbag2_cpp::LocalMessageDefinitionSource`

**问题**：`collect()` 依赖展开时把每段定义直接拼接：

```cpp
if (out.empty()) {
  out = text;
} else {
  out += kSeparator;
  out += text;
}
```

rosbag2 的消息定义 registry 格式要求每个依赖定义前有 `MSG: <package>/<Type>` 行。当前输出只有 `80 个 =` 的分隔线，没有类型名。

**失败场景**：录制 `geometry_msgs/msg/Twist` 时，依赖段是：

```text
<根定义>
================================================================================
<Vector3 定义>
```

缺少 `MSG: geometry_msgs/Vector3`。Foxglove 等按 MCAP ROS 2 registry 解析的工具无法把该段关联到 `Vector3`，Twist 载荷解码失败。本机 `ros2 bag play` 使用本地 typesupport，所以现有 `ros2_record_replay` 测试不能暴露此问题。

**建议**：在每段依赖前写入 `MSG: <package>/<Type>`；或改用 `LocalMessageDefinitionSource::get_full_text()`（见 F-14）。

### F-02 有界序列字段被误判为常量并跳过

- **位置**：`plugins/ros2_jazzy/ros2_schema.cpp:158`
- **级别**：P1
- **验证**：Jazzy 环境实测复现，对照 `LocalMessageDefinitionSource`

**问题**：依赖收集用“行内是否含 `=`”区分常量和字段：

```cpp
if (line.empty() || line.find('=') != std::string::npos) {
  continue;
}
```

`.msg` 的有界序列语法 `Type[<=N]` 也含 `=`，因此这类字段行被整个跳过，依赖类型永远不会展开。

**失败场景**：实测 `rcl_interfaces/msg/ParameterDescriptor`：

- 适配层输出：**1172 字节**，不含 `FloatingPointRange` / `IntegerRange` 定义；
- rosbag2 官方输出：**3630 字节**，包含两者。

根定义仍然引用这些嵌套类型，外部解码器无法解析。

**建议**：字段解析前先剥离 `[<=N]` 再判断是否含 `=`，并区分“字段行”和“常量行”；推荐直接使用官方实现。

### F-03 合法空消息定义被判为不可用

- **位置**：`plugins/ros2_jazzy/ros2_adapter.hpp:97`
- **级别**：P1
- **验证**：Jazzy 环境实测复现

**问题**：

```cpp
bool ok() const noexcept { return error.empty() && !data.empty(); }
```

`std_msgs/msg/Empty` 安装的 `.msg` 是 0 字节文件，rosbag2 的 `get_full_text()` 返回 `encoding=ros2msg`、长度为 0 的合法定义。当前 `ok()` 要求 data 非空，因此将其判为 schema 不可用。

**失败场景**：

- 必需 topic 为 `std_msgs/msg/Empty` 时，预检返回 `message definition unavailable for /trigger: `（detail 为空），录制无法启动；
- 作为可选 topic 时永远不订阅，并周期性产生 `ros2_schema_error` 诊断（与 F-09 叠加）。

**建议**：判断“schema 是否可用”时不要求 data 非空，只要求加载成功、错误为空；空定义是合法输入。

### F-04 IDL 定义被标记为 ros2msg

- **位置**：`plugins/ros2_jazzy/ros2_schema.cpp:207`
- **级别**：P1
- **验证**：Jazzy 环境实测复现

**问题**：`.idl` 回退路径在 `collect()` 中返回原始单文件 IDL 文本（`idl = true`），但 `load_message_schema()` 无条件执行：

```cpp
schema.encoding = "ros2msg";
```

**失败场景**：只安装 `.idl` 的接口包会记录为 `ros2msg` + IDL 文本。规范消费者按 `.msg` 语法解析失败。正确格式应为 `ros2idl`，且每段定义前（包括根定义）写入 `IDL: <package>/<Type>` 行，分隔线仍为 80 个 `=`。

**建议**：保留 `idl` 标记并设置 `encoding = "ros2idl"`，同时补 `IDL:` 行；或改用官方实现。

### F-05 metadata.yaml 写失败仍标记 ready

- **位置**：`plugins/ros2_jazzy/ros2_metadata.cpp:263`（成功标记在 271 行）
- **级别**：P1
- **验证**：对照 Jazzy 的 `rosbag2_storage/metadata_io.cpp` 源码

**问题**：Jazzy 的 `MetadataIo::write_metadata` 实现大意是：

```cpp
YAML::Node node;
node["rosbag2_bagfile_information"] = metadata;
std::ofstream fout(uri);
fout << node;
```

没有检查 `ofstream` 是否成功打开、写入或关闭。适配层直接调用：

```cpp
rosbag2_storage::MetadataIo io;
io.write_metadata(session_directory.string(), metadata);
```

随后无条件写入 `ros2_replay.state = "ready"`。

**失败场景**：磁盘满、配额限制、写入中断（进程被杀）导致 `metadata.yaml` 缺失或截断时，manifest 仍显示 `ready` 且 `metadata_path=metadata.yaml`。默认 `close` durability 模式不会额外 `fsync`，因此也没有存在性检查。消费方拿到“标记可用但没有可用元数据”的会话。

**建议**：写入后校验 `metadata.yaml` 存在且能被重新解析（例如重新读取 YAML 或检查非零大小），失败则走 `fail()` 路径；`sync` 模式保留现有的 fsync 检查。

### F-06 manifest 文件路径未做边界校验

- **位置**：`plugins/ros2_jazzy/ros2_metadata.cpp:179`（拼接在 183 行）
- **级别**：P1
- **验证**：代码走读，对照 `docs/contracts/session-v1.md:22`

**问题**：

```cpp
const std::string relative_path = file.value("path", "");
...
const auto data_path = session_directory / relative_path;
```

没有拒绝绝对路径、盘符、空段、`.`、`..`、反斜杠或解析后越过会话根的路径。契约明确要求读取方拒绝这些形式。

**失败场景**：被篡改或损坏的 manifest 写入 `../../other.mcap` 或绝对路径时，`std::filesystem::exists`、`file_size` 和 `mcap::McapReader::open` 会访问会话目录外的文件，并把该路径写入 `metadata.yaml` 的 `relative_file_paths`；下游 rosbag2 读取方可能继续跟随到会话外。

**建议**：在适配层复用契约规定的路径校验（相对、`/` 分隔、无空段/`.`/`..`/反斜杠，规范化后仍在会话根内），校验失败返回 `failed`。

### F-07 manifest 结构异常时抛出异常

- **位置**：`plugins/ros2_jazzy/ros2_metadata.cpp:174`（另见 151–152、179–180 行）
- **级别**：P1
- **验证**：代码走读

**问题**：`write()` 返回 `Status`，但多处 `nlohmann::json` 取值在 `try` 块之外：

```cpp
const bool sync = manifest.contains("durability") &&
                  manifest["durability"].value("requested_mode", "close") == "sync";
...
if (manifest.value("state", "failed") != "finalized" || ...)
...
const auto& file = manifest["files"][0];
const std::string relative_path = file.value("path", "");
if (relative_path.empty() || file.value("state", "partial") != "finalized") ...
```

**失败场景**：manifest 语法合法但形状不合法（`files[0]` 不是对象、`state`/`durability` 不是字符串、`manifest` 不是对象）时，`value()` 抛出 `nlohmann::json::type_error`。API 约定是返回失败 `Status`，宿主的收尾逻辑不会得到 `ros2_metadata_error`，也不会写出失败的 manifest。

**建议**：把所有 JSON 形状访问移入 `try`，或先做 `is_object()` / `is_array()` / `is_string()` 校验，失败时返回 `fail(...)`。

### F-08 重复 topic 配置导致重复录制

- **位置**：`plugins/ros2_jazzy/ros2_source.cpp:86`
- **级别**：P2
- **验证**：代码走读

**问题**：`apply()` 直接遍历配置中的 topic 列表：

```cpp
for (const auto& option : config_->options().topics) {
  TopicState state;
  state.option = option;
  ...
  topics_.push_back(std::move(state));
}
```

重复条目既不拒绝也不去重。每个条目生成独立的 `TopicState` 和 `GenericSubscription`；`register_channel` 按契约“重复注册同一描述可复用通道”（`docs/contracts/session-v1.md:35`），因此两个订阅写入同一个 channel。

**失败场景**：

- 配置把 `/imu` 列两次：每条消息写入 MCAP 两次，消息计数翻倍；
- 两条 QoS 不同时，metadata 的 `qos_by_topic` 采用最后一条（`ros2_metadata.cpp:226–229`），与实际录制使用的 QoS 不一致。

**建议**：配置校验阶段拒绝重复 topic，或按 topic 名合并并显式记录诊断。

### F-09 订阅失败诊断每 200 ms 无限追加

- **位置**：`plugins/ros2_jazzy/ros2_source.cpp:271`（订阅异常在 296 行）
- **级别**：P2
- **验证**：代码走读

**问题**：`discover()` 由 200 ms 定时器反复调用；对未订阅的 topic 会再次进入 `create_subscription()`：

- schema 不可用时，每次调用追加一条 `ros2_schema_error`（271 行）；
- `create_generic_subscription()` 抛异常时，每次调用追加一条 `ros2_subscription_failed`（296 行）。

这两处没有像 `late_reported` / `conflict_reported` / `queue_full_reported` 那样的只报一次守卫。

**失败场景**：一个 schema 损坏或 typesupport 缺失的可选 topic 每秒产生 5 条相同诊断；一小时会话约 1.8 万条，全部保留在内存和最终 `manifest.json` 中。

**建议**：为每类订阅失败增加 TopicState 级的一次性标记；真正不可用的 topic 在预检后标记为 `skip`，不再进入发现循环。

### F-10 预检不检查发布端 QoS，best-effort 数据静默丢失

- **位置**：`plugins/ros2_jazzy/ros2_source.cpp:186`（QoS 默认值在 `plugins/ros2_jazzy/ros2_adapter.hpp:20–24`）
- **级别**：P2
- **验证**：代码走读

**问题**：`precheck()` 只检查 topic 是否存在、类型是否匹配、typesupport 是否可加载，不检查发布端 QoS。默认 `Ros2Qos` 是 `reliable = true`、`transient_local = false`。DDS 中 reliable 订阅无法匹配 best-effort 发布者。

**失败场景**：录制典型的 best-effort 传感器 topic 时，预检通过、订阅创建成功，但收到 0 条消息且没有任何诊断；只有任务 5 对账时才会发现数据缺失。`metadata.yaml` 还会把配置的 reliable QoS 当作 offered profile 写入。

**建议**：发现阶段读取发布端 QoS 并与订阅 QoS 做兼容性检查；不兼容时对必需 topic 阻止启动或对可选 topic 记录明确诊断。至少为“0 个匹配发布者”提供警告。

### F-11 订阅创建失败前已注册 channel

- **位置**：`plugins/ros2_jazzy/ros2_source.cpp:280`（订阅创建在 289 行）
- **级别**：P2
- **验证**：代码走读

**问题**：`create_subscription()` 先调用 `sink_->register_channel(spec, state.channel)`，再调用 `node_->create_generic_subscription()`。后者抛异常时 channel 已注册且不会撤销。后续每 200 ms 重试会复用同一 channel（契约规定重复描述复用），但 topic 始终没有消息。

**失败场景**：typesupport 缺失、QoS 参数非法等情况让订阅持续失败；最终 MCAP summary、`manifest.json` 和 `metadata.yaml` 都列出从未收到消息的 topic，只有一条 warning 诊断说明原因。

**建议**：先创建订阅，成功后再注册 channel；或注册失败时从写入器回滚（若核心提供该能力）。若无法回滚，至少在最终元数据中排除零消息的失败 topic。

### F-12 重试写入重复追加诊断

- **位置**：`plugins/ros2_jazzy/ros2_metadata.cpp:275`（失败路径在 163–165 行）
- **级别**：P2
- **验证**：代码走读

**问题**：成功和失败路径都无条件把 `diagnostics.snapshot()` 追加到 `manifest["diagnostics"]`。若宿主对同一次录制重试 `Ros2MetadataWriter::write()`，每次重试都会再追加一遍。

**失败场景**：首次元数据写入失败后重试，`manifest.json` 中每条 `ros2_*` 诊断出现两次；多次重试持续膨胀，影响诊断计数类检查。

**建议**：追加前记录/去重（按 code + topic + log_time），或让写入操作幂等：先移除本写入器之前追加的诊断，再写入当前快照。

### F-13 发现定时器长期空转

- **位置**：`plugins/ros2_jazzy/ros2_source.cpp:123`
- **级别**：P3
- **验证**：代码走读

**问题**：200 ms 发现定时器在 `activate()` 中无条件创建，所有配置 topic 都已订阅后仍不取消；`discover()` 每次调用都执行 `node_->get_topic_names_and_types()`（完整图快照拷贝），即使 `topics_` 为空也照跑。

**失败场景**：话题数量数百的机器人上，整个录制期间每秒 5 次全图拷贝和 executor 唤醒，纯开销，且增加实时线程的调度抖动。

**建议**：当所有 topic 已订阅且没有待等待的晚出现/可选 topic 时取消定时器；或改用 graph 事件，仅查询配置中尚未订阅的 topic。

### F-14 重复实现 rosbag2 已有的消息定义展开

- **位置**：`plugins/ros2_jazzy/ros2_schema.cpp:105`
- **级别**：P3（但修复收益最高）
- **验证**：代码走读

**问题**：适配层自行实现 `.msg` 依赖递归展开、分隔符拼接和类型解析（约 150 行），而 Jazzy 的 `rosbag2_cpp::LocalMessageDefinitionSource::get_full_text()` 实现了同样功能且经过 rosbag2 自身使用验证。

**失败场景**：手写实现已经与规范产生 F-01、F-02、F-03、F-04 四处偏差。继续自行维护还会引入新的格式漂移。

**建议**：`plugins/ros2_jazzy/CMakeLists.txt` 增加 `rosbag2_cpp` 依赖，用 `LocalMessageDefinitionSource` 取代手写展开；保留一个薄封装返回 `SchemaText`。注意它同时覆盖 `.msg` 与 `.idl` 的正确编码和定义行。

### F-15 重复实现文件同步接口

- **位置**：`plugins/ros2_jazzy/ros2_metadata.cpp:64`
- **级别**：P3
- **验证**：代码走读

**问题**：适配层手写 POSIX `sync_file` / `sync_directory`，Windows 分支为空操作：

```cpp
void sync_file(const std::filesystem::path& path) {
#ifndef _WIN32
  ...
#else
  (void)path;
#endif
}
```

核心的 `robot_mcap::FileSystem`（`include/robot_mcap/io.hpp:35–58`）已提供同一接口，Windows 上尝试 `FlushFileBuffers`，并支持测试故障注入。

**失败场景**：适配层在 Windows 上同步 durability 静默失效；同时 `Ros2MetadataWriter` 无法复用现有故障注入测试，F-05 这类问题缺少测试手段。

**建议**：让 `Ros2MetadataWriter` 接受 `FileSystem&`（默认 `native_file_system()`），复用 `sync_file` / `sync_directory`；顺带获得与核心一致的 Windows 行为和失败注入。

## 5. 根因分析

`plugins/ros2_jazzy` 在三个地方重复实现了已有能力：

1. **消息定义展开**：rosbag2 已提供 `LocalMessageDefinitionSource`（F-01 至 F-04、F-14）；
2. **文件同步与原子写入**：核心已提供 `FileSystem`（F-15，并为 F-05 提供测试手段）；
3. **manifest 路径校验**：契约已规定完整规则，核心读取路径已有实现可参考（F-06）。

任务 4 的 handoff 把该层定位为“adapter-only”，这解释了为什么选择最小实现；但 schema 和 metadata 是外部消费方直接依赖的边界，最小实现不能偏离规范。建议在任务 5 前把这三处收敛到既有实现。

## 6. 修复优先级建议

| 批次 | 内容 | 说明 |
| --- | --- | --- |
| A（先做） | F-01、F-02、F-03、F-04、F-14 | 直接接入 `LocalMessageDefinitionSource::get_full_text()`，删除手写展开；补 Twist、ParameterDescriptor、Empty、仅 IDL 包四类测试，并断言 `MSG:`/`IDL:` 行存在 |
| B（先做） | F-05、F-06、F-07 | 元数据写后校验；manifest 路径按契约校验；JSON 访问纳入 `try`/形状检查；复用 `FileSystem` 获取故障注入能力 |
| C（可与任务 5 并行） | F-08、F-09、F-10、F-11、F-12 | 配置去重、诊断一次性守卫、QoS 兼容检查、订阅成功后再注册 channel、诊断幂等 |
| D（随重构） | F-13、F-15 | 定时器取消条件；复用核心同步接口 |

建议 A + B 完成后重跑 `ros2_record_replay`，并用 Foxglove 或第三方 MCAP 读取器做一次外部解码验证——现有测试只证明本机 `ros2 bag play` 可用，不能证明 schema 兼容。

## 7. 覆盖与限制

- 本轮审查由协调器直接完成代码走读；并行的 finder/verifier 子代理在审查窗口内未返回结果，因此**核心库**（`src/recorder.cpp`、`src/plugin_runtime.cpp`、`src/io.cpp`、`src/session_artifacts.cpp` 等）**没有产出 finding，但覆盖不完整**，不能视为已全面审查。建议后续补一轮核心库专项审查。
- F-01、F-02、F-03、F-04 在 WSL Jazzy 环境用复现程序与 rosbag2 的 `LocalMessageDefinitionSource` 输出做过对照；F-05 对照了 Jazzy 的 `metadata_io.cpp` 源码；其余为源码走读。走读类结论建议在修复时用测试固定行为。
- 现有 `ros2_record_replay` 通过并不能反驳 F-01 至 F-04：`ros2 bag play` 使用本机 typesupport，不依赖 MCAP 中的 schema 文本。
- 审查基线为 `main` @ `76ab27c`；若后续提交改动上述文件，行号可能偏移。

## 8. 附录：验证环境与复现线索

- **环境**：WSL 发行版 `Ubuntu-24.04-Jazzy`（Ubuntu 24.04、GCC 13、ROS 2 Jazzy、`rosbag2_storage_mcap`）
- **构建**：

```sh
source /opt/ros/jazzy/setup.bash
cmake -S /mnt/d/dev/robot-mcap -B /root/robot-mcap-build/jazzy \
  -DBUILD_TESTING=ON -DROBOT_MCAP_WITH_ROS2=ON
cmake --build /root/robot-mcap-build/jazzy -j 4
ctest --test-dir /root/robot-mcap-build/jazzy -R '^ros2_record_replay$' --output-on-failure
```

- **F-01/F-02/F-03/F-04 复现思路**：
  1. 调用 `load_message_schema("geometry_msgs/msg/Twist")`，检查依赖段前是否有 `MSG: geometry_msgs/Vector3`；
  2. 调用 `load_message_schema("rcl_interfaces/msg/ParameterDescriptor")`，比较输出字节数与 `LocalMessageDefinitionSource::get_full_text()`（实测 1172 vs 3630）；
  3. 调用 `load_message_schema("std_msgs/msg/Empty")`，对比 `get_full_text()` 的 `encoding=ros2msg`、长度 0；
  4. 找一个只安装 `.idl` 的接口包，检查返回的 `encoding` 与 `IDL:` 行。
- **F-05 复现思路**：在只读目录或磁盘配额受限的目录写 metadata，观察 `write_metadata` 返回后 manifest 是否仍为 `ready`。
- **相关规范**：
  - `docs/contracts/session-v1.md:22`（路径边界规则）
  - `docs/contracts/session-v1.md:35`（通道重复注册复用）
  - `docs/contracts/session-v1.md:77`（ROS Schema 编码要求）
