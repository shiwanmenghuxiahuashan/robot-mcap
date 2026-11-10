# Robot MCAP 会话落盘契约 v1

**状态**　任务 0 评审稿。下面的字段、默认值和兼容策略是实施候选。评审通过后作为通用写入、ROS 2 适配和后校验的共同输入。

## 1. 一次会话的产物

```text
<session_id>/
  recording-config.json
  data_0000.mcap
  data_0000.timing.ndjson       # 仅在有额外逐消息时间证据时生成
  metadata.yaml                 # ROS 2 适配产物
  manifest.json
  checks/<run_id>/
    task.json
    rules.json
    report.json
```

- 会话目录用独占创建。同一个 ID 已存在时拒绝开始。会话和检查 ID 使用 1–64 个 ASCII 字母、数字、下划线或连字符，首字符为字母或数字。
- 数据卷从 `0000` 开始，首版每会话一卷，`files` 预留为数组。活动卷和活动时间旁路文件分别带 `.partial` 后缀。旧会话不被覆盖。读取端按清单选择产物，不扫描目录推断完成状态。
- 所有清单路径都是相对于会话根的 UTF-8 路径，分隔符为 `/`。拒绝绝对路径、盘符、空段、`.`、`..`、反斜杠及解析后越过会话根的路径。读取检查任务时也要验证会话 ID、检查 ID 与所在目录一致。
- JSON 使用 UTF-8，`schema_version` 为整数 `1`。读取器拒绝未知主版本、重复字段和必需字段缺失，允许忽略不影响已知语义的附加字段。JSON 计数和字节数使用非负安全整数，64 位时间值用十进制字符串，未知值用 `null`。标为必需的可空字段也必须出现。

## 2. MCAP 容器和消息

| 项目 | 首版候选 |
| --- | --- |
| 写入器 | 上游 C++ `mcap::McapWriter`，配置快照记录构建使用的版本 |
| 通用 profile | 空字符串。各通道声明自己的消息编码 |
| 分块与索引 | 开启分块、消息索引、Chunk 索引和 Summary，Chunk 目标 1,048,576 字节 |
| 压缩与校验 | 默认不压缩，开启 Chunk CRC。ROS 分支只使用 MCAP 自身的 Chunk 压缩配置 |
| 原始载荷 | 写入数据源交给核心的不可变字节。解码、重采样和修正结果由下游另存 |

数据源注册通道时提供 topic、`message_encoding`、Schema 或显式无 Schema、时间策略和时钟描述。通用核心允许 `schema_id = 0`，读取器返回原始字节，依赖解码的检查据此报告证据不足或不适用。ROS 2 模式要求有完整消息定义。同一会话内 MCAP 通道标识只和其所属文件配对。重复注册同一描述可复用通道。已有通道的编码、Schema 或时钟策略改变时拒绝混写，并记录诊断。

MCAP Message 的 `sequence` 在数据源有明确、可表示的原生序号时直接保存，否则写 `0`。业务缺失率使用可信的源端计数或带来源与重启语义的序号证据。单凭 `sequence = 0`、记录端生成的序号或频率估计无法确认源端缺失。

## 3. 时间映射与逐消息证据

- `log_time` 是采集入口的记录时间，单位为自 Unix epoch 起的非负纳秒。该时间用于 MCAP 时间索引和默认间隔检查。原始时间乱序时保留原值，由检查报告异常。
- `publish_time` 保存可按 Unix epoch 非负纳秒表示的真实源发布时间。缺失时按 MCAP 规范填 `log_time`。两者数值相同也可能是两个真实时间值，读取端以通道策略和旁路证据区分。已经在原始载荷里的采样时间继续保留在载荷里。
- 通道的 `publish_time_policy` 固定为 `source_required`、`log_time_only` 或 `per_message`。前者要求每条都有真实且可表示的源发布时间。第二种要求每条都使用回填。第三种允许混合，每一条回填记录都必须在旁路文件标记。通道可以在录制中新增。
- 对于输入附带、载荷中没有的原始设备时间，旁路文件保存原始整数值、时钟 ID、单位、时间含义和纪元描述。保留原始数值和口径。若它不能充当 MCAP 发布时间，仍按上述策略写 `publish_time`。首版接受整数纳秒、微秒、毫秒和 tick，无法无损表达的输入返回参数错误。跨设备对齐由上层完成。

旁路文件以同卷 MCAP 的 **物理 Message 顺序**建立关联。序号 `message_ordinal` 从 0 开始，跨通道递增，排除非 Message 记录，不受时间排序影响。每行一个 UTF-8 JSON 对象，以 LF 结束，行按序号严格递增。每行必有 `message_ordinal`、`channel_id`、`publish_time_fallback` 和可空的 `source_clock`。时钟对象的字段为 `clock_id`、`value`、`unit`、`meaning`、`epoch`。仅在需要逐条标记回填或保存独立原始时钟值时写行。序号和 Channel 必须对得上同卷原始文件。改写或过滤 MCAP 时，如果派生产物继续引用旁路，必须重建关联。

`timing` 清单条目含 `path`、`row_count`、`sha256` 和 `state`。没有任何行时为 `null`。完成文件的旁路条目必须处于 `finalized`，行数与摘要匹配。记录线程在同一串行写入路径分配序号并追加关联。写 MCAP 或必需时间旁路失败时停止接收，保留产物并记录故障。已完成 MCAP 但时间证据不完整时，会话为 `failed`。普通检查不消费该会话。显式诊断读取可以报告已完成数据，并将时间证据标为不可用。

资料：[MCAP Message 记录](https://mcap.dev/spec#message-op0x05) · [MCAP 编码注册表](https://mcap.dev/spec/registry)

## 4. 清单、统计和发布边界

下表列出 `manifest.json` 的必需字段。检查规则和后校验状态独立保存。

| 字段 | 类型与语义 |
| --- | --- |
| `schema_version`、`session_id` | 版本和稳定会话 ID |
| `state` | `finalized` 或 `failed`。活动录制不发布终态清单 |
| `files[]` | 每项含 `path`、`state`、`bytes`、`message_count`、`first_log_time_ns`、`last_log_time_ns`、`timing`。文件状态为 `finalized`、`partial` 或 `unknown`。首尾时间是最小和最大记录时间。空文件及未验证卷的未知统计填 `null` |
| `channels[]` | 每项含 `file_path`、`channel_id`、`topic`、`message_encoding`、`schema`、`publish_time_policy`。有 Schema 时包含 `name`、`encoding`、原始 Schema 数据的 `sha256`。无 Schema 用 `null` |
| `statistics` | `accepted`、`rejected`、`writer_processed`、`finalized_messages`。写入处理计数可能含仍在缓存的消息。终态确认前或故障导致总量未知时 `finalized_messages` 为 `null` |
| `durability` | `requested_mode` 为 `close` 或 `sync`。`confirmation` 在清单中是 `not_requested` 或 `see_stop_result` |
| `ros2_replay` | 含 `state` 和 `metadata_path`。状态为 `not_requested`、`ready` 或 `failed`。`ready` 才有元数据路径 |
| `config_path`、`diagnostics[]` | 配置快照相对路径，以及含 `code`、`severity`、`topic`、`log_time_ns`、`detail`、`action` 的诊断对象 |

配置快照记录 profile、索引、Chunk、压缩、校验、轮转、持久化模式、写入器版本，以及适配插件实际使用的 topic 和 QoS 配置。清单里的通道是实际注册结果，可能包含录制启动后才发现的通道。

正常收尾先停止接收、排空队列、完成 MCAP 和旁路结构并关闭，随后发布已完成文件。ROS 适配层生成相应回放元数据，再以同目录临时文件写出清单并重命名。若只在 ROS 适配步骤失败，MCAP 会话仍可为 `finalized`，`ros2_replay.state = failed`。诊断记录错误。普通质检只接受终态清单中已完成、存在且必要关联完整的产物。

`sync` 模式在发布前同步数据卷、旁路、配置、生成的元数据和清单临时文件，发布后同步会话目录，并在新建目录时同步其父目录。停止返回 `file_state`、`durability_state` 和错误，持久化状态取 `not_requested`、`confirmed` 或 `failed`。任何要求的同步失败都使同步模式停止返回失败，即使 MCAP 已格式完整。清单的发布发生在最后一次目录同步确认之前，因此文件存在不能作为 `confirmed` 的证明。若调用者需要保存同步结果，将停止结果另行记录。

故障时尽力发布失败清单和已知状态，保留已有文件。进程被终止、磁盘写满等场景可能只留下 `.partial` 和配置，甚至没有清单。这类目录由诊断工具显式处理。恢复输出使用新目录，保留原文件供追溯。

## 5. ROS 2 Jazzy 回放约定

- ROS 会话使用 `ros2` profile、原始 `cdr` 载荷和完整的 `ros2msg` 或 `ros2idl` Schema，保存 ROS 类型全名。发现类型变化时按草案拒绝混写。
- 适配层使用安装的 Jazzy 元数据 API 写 `metadata.yaml`。其中的 storage ID、相对文件、时间范围、topic 类型、序列化格式、消息数和已获取的 QoS 与 MCAP 一致。MCAP 内的 Channel QoS、类型哈希以及适用版本的嵌入式 rosbag2 元数据使用相同的 Jazzy 约定。
- `ready` 表示该会话的适配产物齐备。任务 4 的发布验收另行在 Jazzy 运行 `ros2 bag info`、`ros2 bag play`，用独立订阅端核对消息类型、数量和字节。通用核心可以单独构建和读取 ROS 文件的结构与原始字节。

资料：[ROS 2 profile](https://mcap.dev/spec/registry#ros2) · [Jazzy 元数据类型](https://github.com/ros2/rosbag2/blob/jazzy/rosbag2_storage/include/rosbag2_storage/bag_metadata.hpp) · [Jazzy MCAP 插件](https://github.com/ros2/rosbag2/blob/jazzy/rosbag2_storage_mcap/src/mcap_storage.cpp)

## 6. 独立检查任务和报告

- `task.json` 必需字段为 `schema_version`、`session_id`、`run_id`、`manifest_path`、`rules_path`。任务通过文件传递，后校验进程根据路径边界读取同一会话。每次复查使用新的 `run_id` 和规则快照。
- `rules.json` 含 `schema_version`、`ruleset_version`、`checks[]`，每项含 `id`、`required` 和 `parameters`。检查器记录本次参数、时间口径及工具版本。
- `report.json` 含 `schema_version`、`session_id`、`run_id`、`manifest_path`、`ruleset_version`、`tool_version`、`overall` 和 `checks[]`。每个结果含 `id`、`rule_version`、`target`、`parameters`、`time_basis`、`status`、`metrics`、`evidence` 和 `reason`。
- 检查项状态为 `pass`、`fail`、`execution_error`、`not_applicable` 或 `insufficient_evidence`。整体结论为 `pass`、`fail` 或 `undetermined`。必需项有明确失败时可判失败。必需项无法执行、证据不足或不适用时禁止判通过。未启动检查用“未请求”表示，无报告。
- 报告以临时文件写入并重命名。进程异常退出且没有完整报告时，宿主按检查执行错误处理。录制清单和原文件保持可用。

## 7. 评审样例

仓库内的 `tests/fixtures/session-v1/` 提供四组文本样例。它们展示落盘结构、时间旁路、部分失败和检查报告，引用的 MCAP 由后续写入、故障及回放任务生成。

| 样例 | 验证的契约 |
| --- | --- |
| `01-finalized` | 两条消息，共用一个通道。一条携带独立设备时钟值，另一条缺发布时间并明确回填 |
| `02-partial` | 底层写入失败，保留 `.partial`，区分已接收、已处理和最终落盘未知 |
| `03-ros2` | ROS 2 适配清单和 `metadata.yaml` 中的 topic、类型、消息数及时间范围一致 |
| `04-evidence-insufficient` | 容器完成，缺少可信源计数导致必需缺失率检查无法下结论 |
