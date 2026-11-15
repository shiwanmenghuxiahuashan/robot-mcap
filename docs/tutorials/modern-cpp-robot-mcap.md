# 分章节教程：用 robot-mcap 学现代 C++

> 生成日期：2026-11-15。行号基于当前工作树，修改代码后可能漂移。
>
> **适用对象**：会 C/C++ 基础语法，但没有系统用过 C++11/14/17，或者能写代码但读不懂这个项目的人。  
> **教材**：当前工作树（约 9400 行）。  
> **主线**：项目标准是 C++17；C++11/14 是它依赖的地基。  
> **学法**：每章按“读接口 → 读实现 → 回答问题 → 动手改”推进，不要一上来就通读 `src/`。

---

## 课程目录

- [第 0 章 课程地图与阅读方法](#第-0-章-课程地图与阅读方法)
- [第 1 章 构建系统与 C++ 标准](#第-1-章-构建系统与-c-标准)
- [第 2 章 类型、值语义与接口契约](#第-2-章-类型值语义与接口契约)
- [第 3 章 RAII 与所有权](#第-3-章-raii-与所有权)
- [第 4 章 移动语义与特殊成员函数](#第-4-章-移动语义与特殊成员函数)
- [第 5 章 异常、错误码与错误模型](#第-5-章-异常错误码与错误模型)
- [第 6 章 `std::filesystem`](#第-6-章-stdfilesystem)
- [第 7 章 `std::optional`](#第-7-章-stdoptional)
- [第 8 章 `std::string_view`](#第-8-章-stdstring_view)
- [第 9 章 `std::byte`](#第-9-章-stdbyte)
- [第 10 章 模板、类型擦除与插件 ABI](#第-10-章-模板类型擦除与插件-abi)
- [第 11 章 并发编程](#第-11-章-并发编程)
- [第 12 章 C++17 语言特性](#第-12-章-c17-语言特性)
- [第 13 章 综合练习与代码审查](#第-13-章-综合练习与代码审查)
- [附录 A 标准版本速查](#附录-a-标准版本速查)
- [附录 B 文件地图](#附录-b-文件地图)
- [附录 C 术语表](#附录-c-术语表)

---

## 第 0 章 课程地图与阅读方法

### 本章目标

- 知道项目由哪些层组成；
- 知道一次录制从调用到落盘经过哪些对象；
- 掌握“先接口、后实现、最后分支”的阅读顺序。

### 0.1 项目分层

```
include/robot_mcap/*.hpp     公共接口：类型、I/O 接缝、Recorder、插件运行时
src/io.cpp                   文件系统/输出流的平台实现
src/mcap_file.cpp            MCAP writer 适配
src/recorder.cpp             录制核心：有界队列 + 写线程 + 停止协议
src/session_artifacts.cpp    会话产物：config/manifest/timing
src/plugin_runtime.cpp       插件运行时：服务注册、依赖排序、加载 .dll/.so
plugins/ros2_jazzy/*         ROS 2 适配器与 source 插件
tests/*                      行为测试（也是最好的用法示例）
```

### 0.2 一次录制的数据流

```
调用方
  └─ Recorder::register_channel()      注册 channel（拿 ChannelId）
  └─ Recorder::submit_copy/owned()     入队（有界队列，按字节计数）
        └─ 写线程 run()                从队列取 Item
              └─ McapFile::write()     写 MCAP 数据
                    └─ OutputSink      OutputSink 是 I/O 接缝
  └─ Recorder::stop_async()            停止接收，返回 shared_future<StopResult>
        └─ finalize()                  排空队列、关闭文件、rename .partial
              └─ SessionArtifacts      写 timing sidecar、manifest.json
```

读代码时始终沿着这条数据流走：**谁产生数据 → 谁排队 → 谁写盘 → 谁收尾**。

### 0.3 读每个类型时问四个问题

1. **谁拥有它？** `unique_ptr`、`shared_ptr`、栈对象，还是裸指针/引用？
2. **失败怎么表达？** 异常、`Status`、`SubmitResult`、`StopResult`，还是 `error_code`？
3. **生命周期到哪结束？** 函数返回、作用域结束、`stop()`、还是模块卸载？
4. **哪个线程访问？** 调用方线程、写线程、插件线程，还是运行时线程？

这四个问题能解释项目中 80% 的设计。

### 0.4 环境与自检

```sh
cmake -S . -B build/core -DBUILD_TESTING=ON
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

第一次构建会拉取 Foxglove MCAP 的上游代码；离线构建可设置 `FETCHCONTENT_SOURCE_DIR_FOXGLOVE_MCAP`。

**本章作业**：画出上面那条数据流，并标注每一步的所有权和线程。

---

## 第 1 章 构建系统与 C++ 标准

### 本章目标

- 理解 `target_compile_features(... cxx_std_17)` 到底做了什么；
- 理解为什么“能编过”不等于“严格 C++17”；
- 会自己把项目切成严格 C++17 模式。

### 阅读材料

- `CMakeLists.txt`
- `plugins/ros2_jazzy/CMakeLists.txt`
- `cmake/Dependencies.cmake`

### 1.1 目标模型，而不是全局变量

`CMakeLists.txt:9-24`：

```cmake
add_library(robot_mcap STATIC ...)
target_compile_features(robot_mcap PUBLIC cxx_std_17)
target_include_directories(robot_mcap PUBLIC include)
set_target_properties(robot_mcap PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(robot_mcap PRIVATE
  robot_mcap_upstream Threads::Threads ${CMAKE_DL_LIBS})
```

三个关键词：

- **`PUBLIC`**：这个要求既用于编译 `robot_mcap` 自己，也写进它的接口，链接它的测试目标会自动继承；
- **`PRIVATE`**：只用于自己编译，不传播（例如上游 MCAP 头文件、线程库、`dl`）；
- **`INTERFACE`**：自己不编译，只传播给使用方（本项目没直接用）。

`cxx_std_17` 是“**至少** C++17”，不是“锁定 `-std=c++17`”。如果编译器默认标准已经满足，CMake 可能一个 `-std` 都不传。本机 GCC 11.4 默认 `gnu++17`，所以 `build/*/flags.make` 里可能只有 `-fPIC`——看起来没设标准也能编，换编译器就可能崩。

### 1.2 想要严格 C++17，应这样写

顶层：

```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

或目标级：

```cmake
set_target_properties(robot_mcap PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
```

区别：

- `CXX_EXTENSIONS OFF` 决定用 `-std=c++17` 还是 `-std=gnu++17`；
- `CXX_STANDARD_REQUIRED ON` 防止编译器不支持时静默降级；
- `CXX_EXTENSIONS` **不会**沿链接关系传播，插件子目录要单独设置，或用全局变量；
- `-Wall -Wextra -Wpedantic -Werror` **不能**替代严格模式；它们不是标准开关；
- `.vscode/c_cpp_properties.json` 里的 `cppStandard: c++17` 只影响 IntelliSense，不参与真实编译。

### 1.3 为什么这个项目“必须”是 C++17

因为公共头文件直接暴露了 C++17 类型：

```cpp
// types.hpp:18
std::filesystem::path path;

// types.hpp:48
std::optional<SchemaSpec> schema;

// io.hpp:21
virtual void write(const std::byte* data, std::size_t size) = 0;
```

任何包含 `types.hpp`/`io.hpp` 的使用方都必须按 C++17 编译。这不是内部实现细节，而是公共契约。

**常见误区**：

- “CMake 里写了 `cxx_std_17`，就一定是 `-std=c++17`”——不一定；
- “GCC 8 能编过就不用加链接库”——GCC 8 及更早链接 `<filesystem>` 需要 `-lstdc++fs`；
- “VS Code 里显示 C++17 就是 C++17”——IDE 配置不约束编译。

**练习**：

1. 构建后查看 `build/core/CMakeFiles/robot_mcap.dir/flags.make`，确认有没有 `-std`；
2. 用 `-std=c++14` 编译，记录前三个报错：`std::filesystem`、`namespace a::b`、`std::lock_guard lock(m)`；
3. 改成严格模式后重复上一步，确认报错变成“标准不支持”而不是“编译器默认值不同”。

**小结**：构建层是 C++ 标准的第一道契约。先确认标准，再读语法。

---

## 第 2 章 类型、值语义与接口契约

### 本章目标

- 学会用类型表达语义：`enum class`、`optional`、`path`、`byte`；
- 理解值语义、默认成员初始化器（NSDMI）、聚合初始化；
- 学会从公共头文件读出接口契约。

### 阅读材料

- `include/robot_mcap/types.hpp`（全文，67 行）
- `include/robot_mcap/recorder.hpp`（第 19-83 行）
- `include/robot_mcap/plugin_runtime.hpp`（第 31-59 行）

### 2.1 `Status`：用值表达“成功/失败 + 上下文”

`types.hpp:16-22`：

```cpp
struct Status {
  std::string operation;
  std::filesystem::path path;
  std::string detail;

  bool ok() const noexcept { return operation.empty(); }
};
```

读法：

- `operation` 为空表示成功，这是**约定**，不是类型系统保证；
- `path` 用 `std::filesystem::path`，失败时能带上具体路径；
- `ok()` 是 `const noexcept`：查询函数不修改对象，也不抛异常。

这套设计比返回 `int` 或 `bool` 多保留了上下文，又比抛异常更适合公共 API 的常规失败。

### 2.2 `enum class`：强类型枚举

```cpp
// types.hpp:30
enum class PublishTimePolicy { SourceRequired, LogTimeOnly, PerMessage };

// recorder.hpp:27
enum class DurabilityMode { Close, Sync };

// recorder.hpp:48
enum class SubmitError { None, QueueFull, Stopped, WriterFailed, InvalidArgument };
```

相比旧式 `enum`：

- 名字不泄漏到外层作用域，必须写 `SubmitError::QueueFull`；
- 不会隐式转成 `int`，日志/序列化要显式 `static_cast`；
- 不同枚举不能互相比较。

**坑**：需要按位组合时要自己重载 `operator|`/`&`；前置声明必须写底层类型（`enum class X : std::uint8_t;`），底层类型影响 ABI。

### 2.3 NSDMI 与聚合初始化

```cpp
// types.hpp:49
PublishTimePolicy publish_time_policy = PublishTimePolicy::LogTimeOnly;

// recorder.hpp:36
std::uint64_t queue_byte_limit = 64ULL * 1024 * 1024;

// plugin_runtime.hpp:34-35
int min_version = 1;
bool optional = false;
```

默认成员初始化器（NSDMI）让字段有确定默认值。C++11 里带 NSDMI 的类不再是聚合；C++14 放宽后，下面这种花括号构造才合法（`plugin_runtime.hpp:47`）：

```cpp
return {service_id<T>(), std::move(name), min_version, optional};
```

**坑**：

- 花括号按成员声明顺序初始化，字段增删/换序会静默错位；
- 指定初始化器 `{.name = ...}` 是 C++20，不是 C++17；
- 花括号里的窄化转换是错误。

### 2.4 从接口读出契约

以 `Recorder` 为例（`recorder.hpp:85-112`）：

```cpp
class Recorder {
public:
  explicit Recorder(RecorderOptions options);
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  Status start();
  Status register_channel(const ChannelSpec& spec, ChannelId& id);
  SubmitResult submit_copy(ChannelId channel, MessageView message);
  SubmitResult submit_owned(ChannelId channel, OwnedMessage&& message);
  std::shared_future<StopResult> stop_async();
  StopResult stop();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

从声明就能读出：

- **不可拷贝**（`= delete`），资源对象只能有一个所有者；
- `start()`/`register_channel()` 返回 `Status`；
- `submit_*` 返回 `SubmitResult`，因为提交失败还有分类（`QueueFull`/`Stopped`/...）；
- `submit_owned` 收右值，表示所有权转移；
- `stop_async` 返回 `shared_future`，说明结果可被多个等待者共享；
- `Impl` 前向声明 + `unique_ptr`，说明实现被隐藏（PImpl）。

**练习**：

1. 给 `ChannelSpec` 加一个 `std::optional<std::string> notes;` 字段，并用 NSDMI 给它默认 `std::nullopt`；
2. 给 `Status` 加一个 `bool has_path() const noexcept`，思考为什么不应该把 `path` 改成 `std::string`；
3. 用一句话解释 `SubmitError` 为什么不用 `int`。

**小结**：接口设计先于实现。先读头文件，能读出所有权、错误模型和线程模型。

---

## 第 3 章 RAII 与所有权

### 本章目标

- 理解 RAII：资源生命周期绑定对象生命周期；
- 分清 `unique_ptr`、`shared_ptr`、`weak_ptr`、裸指针/引用的语义；
- 理解 PImpl 与不完整类型。

### 阅读材料

- `include/robot_mcap/io.hpp`
- `src/io.cpp`（第 40-110 行）
- `include/robot_mcap/recorder.hpp`（第 114-117 行）
- `src/plugin_runtime.cpp`（第 872-884 行）
- `tests/plugin_runtime_test.cpp`（第 1073-1097 行）

### 3.1 RAII 的最小例子

`std::lock_guard` 是最常见的 RAII：

```cpp
void f() {
  std::lock_guard lock(mutex);  // 构造：加锁
  // ... 临界区，任何 return/throw 都会先析构 lock
}                                 // 析构：解锁
```

项目里所有锁都这样写（`recorder.cpp:158` 等）。同样的思想用于文件、线程、智能指针、插件模块。

### 3.2 `unique_ptr`：独占所有权

`io.hpp:43`：

```cpp
virtual std::unique_ptr<OutputSink> create_exclusive(const std::filesystem::path& path) = 0;
```

工厂把 sink 的**唯一所有权**交给调用方。`mcap_file.cpp:22` 再把它移进 `SinkWritable`：

```cpp
explicit SinkWritable(std::unique_ptr<OutputSink> sink) : sink_(std::move(sink)) {}
```

所有权链：

```
FileSystem 工厂 → unique_ptr<OutputSink> → SinkWritable::sink_ → McapFile 生命周期结束
```

没有人能拷贝这个 sink；`unique_ptr` 的拷贝构造是删除的，按值传递必须 `std::move`。

### 3.3 PImpl：把实现藏起来

`mcap_file.hpp:30`、`recorder.hpp:115`、`plugin_runtime.hpp:245`：

```cpp
struct Impl;
std::unique_ptr<Impl> impl_;
```

规则：

- 头文件只前向声明 `Impl`，不暴露成员；
- 析构函数必须在 `.cpp` 中定义（`Recorder::~Recorder() = default;` 在 `recorder.cpp:704`），因为 `unique_ptr<Impl>` 的析构需要**完整类型**；
- 移动构造/赋值同理，要在 `Impl` 完整处定义；
- 代价是每个对象多一次堆分配和指针间接。

**坑**：如果在头文件里写 `~Recorder() = default;`，编译器会在 `Impl` 仍不完整处实例化 `unique_ptr` 的析构，触发 `static_assert`。

### 3.4 `shared_ptr`：共享所有权

`types.hpp:64`：

```cpp
std::shared_ptr<FileSystem> file_system;
```

这是 I/O 注入接缝：`Recorder` 和 `SessionArtifacts` 共享同一个 `FileSystem`，测试可以传入假的实现。`io.hpp:60` 的 `native_file_system()` 返回 `shared_ptr`，用函数内 `static` 保证只创建一次。

**坑**：

- 循环引用会泄漏；用 `weak_ptr` 打破环；
- 引用计数是原子操作，热路径频繁拷贝有开销；
- 跨 DLL 释放要求同一堆/CRT。

### 3.5 `weak_ptr`：只观察，不续命

测试里（`tests/plugin_runtime_test.cpp:1075-1082`）：

```cpp
explicit ReentrantPrecheck(std::weak_ptr<robot_mcap::IRecordingStatus> status)
    : status_(std::move(status)) {}

void run() {
  if (auto status = status_.lock()) {
    (void)status->result();
    (void)status->recording();
  }
}
```

`weak_ptr` 不增加引用计数；`lock()` 原子地完成“检查 + 提升”，返回临时 `shared_ptr`。不要先 `expired()` 再 `lock()`，中间对象可能被释放。

对照 `enable_shared_from_this`（`recording_flow.cpp:16/34`）：需要延长生命周期时用它；只观察时用 `weak_ptr`。

### 3.6 自定义 deleter：模块和对象一起活

`plugin_runtime.cpp:875-882`：

```cpp
return std::shared_ptr<IPlugin>(plugin, [native](IPlugin* value) {
  native->module->destroy(value);
});
```

插件的 `IPlugin` 必须由**模块内部**的代码销毁；自定义 deleter 把 `native`（模块句柄）一起捕获，保证对象销毁前模块不会被卸载。

**练习**：

1. 画出 `FileSystem` 的共享关系图和 `McapFile` 的独占关系图；
2. 解释为什么 `FileSystem` 用 `shared_ptr`，而 writer 用 `unique_ptr`；
3. 如果把 `Recorder::Impl` 改成 `shared_ptr<Impl>`，会多出什么语义？为什么项目不这么做？

**小结**：所有权是 C++ 最重要的设计维度。先问“谁拥有”，再问“怎么释放”。

---

## 第 4 章 移动语义与特殊成员函数

### 本章目标

- 理解左值/右值、`std::move` 只是类型转换；
- 理解 `= delete`、`= default`、移动构造/赋值、`explicit operator bool`；
- 理解 `submit_copy` 与 `submit_owned` 的成本差异。

### 阅读材料

- `include/robot_mcap/recorder.hpp`（第 19-25、100-103 行）
- `src/recorder.cpp`（第 330-370 行）
- `include/robot_mcap/plugin_runtime.hpp`（第 61-80 行）

### 4.1 `submit_copy` vs `submit_owned`

```cpp
// recorder.hpp:100-103
SubmitResult submit_copy(ChannelId channel, MessageView message);
SubmitResult submit_owned(ChannelId channel, OwnedMessage&& message);
```

- `submit_copy` 在返回前复制 payload，调用方可以立刻复用缓冲；
- `submit_owned` 接收右值，成功入队后转移所有权，避免大 payload 拷贝；
- 注释明确：**只有成功时才转移**，拒绝时调用方数据仍完整。

实现里（`recorder.cpp:349-363`）先 `emplace_back` 占位，再 `std::move`：

```cpp
queue.push_back(std::move(item));
...
item.source_clock = std::move(message.source_clock);
item.data = std::move(message.data);
```

这样即使分配失败，调用方的数据也还没被移走——异常安全的关键顺序。

### 4.2 特殊成员函数规则

- 声明移动构造/赋值会让拷贝构造/赋值被隐式删除；
- 只 `delete` 拷贝而没写移动，类就不可移动；
- 析构函数一旦用户声明，隐式移动也不会生成；
- `= default` 让编译器生成，`= delete` 把错误提前到编译期。

项目里两类对象：

- **资源对象**：`Recorder`、`McapFile`、`Scope`、`Context`、`RecordingFlow`、`PluginRuntime` 删除拷贝，有意不可移动；
- **句柄对象**：`RegistrationHandle` 显式实现移动构造/赋值，转移注册所有权，并用 `explicit operator bool` 判断有效性。

### 4.3 `explicit operator bool`

```cpp
// plugin_runtime.hpp:75
explicit operator bool() const noexcept;
```

`explicit` 防止 `int x = handle;` 这种隐式转换，但 `if (handle)` 仍然可用。

### 4.4 moved-from 状态

被 move 的对象仍然“有效但未指定”：

- 可以析构；
- 可以重新赋值；
- 不要依赖它的具体值（例如 `message.data.size()`）。

**练习**：

1. 写一个 move-only 的 `Buffer` 类：删除拷贝，移动构造/赋值标 `noexcept`，析构释放内存；
2. 解释为什么 `std::vector` 扩容时更愿意用移动构造（提示：`noexcept`）；
3. 在 `submit_owned` 里，如果把 `std::move` 提到 `emplace_back` 之前，异常时会发生什么？

**小结**：移动语义是“允许窃取资源”的契约，不是“移动了内存”。先保证异常安全，再 move。

---

## 第 5 章 异常、错误码与错误模型

### 本章目标

- 理解异常、`std::error_code`、`noexcept` 的分工；
- 学会读一个函数的错误契约；
- 理解为什么同一个操作需要“抛异常版”和“`error_code` 版”。

### 阅读材料

- `src/io.cpp`（第 20-90、160-180 行）
- `include/robot_mcap/io.hpp`（第 12-58 行）
- `include/robot_mcap/types.hpp`（第 16-22 行）
- `src/recorder.cpp`（第 483-522 行）

### 5.1 三层错误模型

项目把错误分成三层：

| 层 | 机制 | 例子 |
|---|---|---|
| OS 边界 | `throw std::system_error` | `io.cpp:30/64/139` |
| 公共 API | 返回 `Status`/`SubmitResult`/`StopResult` | `recorder.hpp:95-112` |
| 清理/`noexcept` 路径 | `error_code` 重载，静默失败 | `io.cpp:167-173` |

### 5.2 `std::system_error`

```cpp
// io.cpp:30
throw std::system_error(error, std::generic_category(), "open input");
```

`system_error` 把“错误值 + 类别 + 上下文”打包。`errno` 用 `std::generic_category()`，Win32 错误用 `std::system_category()`。失败后、构造异常前不能再调用可能改写 `errno` 的函数；项目在 `_commit`/`fsync` 后立刻保存 `errno`，再关闭。

### 5.3 `error_code` 重载与 `noexcept`

```cpp
// io.cpp:167-173
void remove(const std::filesystem::path& path) noexcept override {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}
```

`remove(path)` 失败会抛 `filesystem_error`；在 `noexcept` 函数里逃逸会直接 `std::terminate`。所以清理路径必须用 `(path, ec)` 重载。`ignored` 是刻意的：清理失败不应覆盖正在传播的原始错误。

### 5.4 `noexcept` 的真实含义

`recorder.cpp:483`：

```cpp
void Recorder::Impl::run() noexcept {
```

线程入口一旦有异常逃逸就会 `std::terminate`，所以 `run()` 标 `noexcept` 并在内部 `catch(...)`，把异常转成 `record_session_error`。`finalize()` 同样 `noexcept`，把失败写进 `StopResult` 并通过 promise 发布。

**坑**：`noexcept` 不是“检查”，是承诺。`promise::set_value` 可能抛 `future_error` 或 `bad_alloc`；在 `noexcept` 函数里任何可抛调用都要审计整条路径。

**练习**：

1. 用 `grep -n "throw\|catch\|noexcept\|error_code" src include` 列出所有错误处理点，按三层分类；
2. 把 `file_size` 的抛异常调用改成 `error_code` 版本，写出两条分支；
3. 解释为什么 `Status::ok()` 标 `noexcept`，而 `Recorder::start()` 不标。

**小结**：异常用于“主流程的意外失败”，`error_code` 用于“清理路径的静默失败”，`Status` 用于“公共 API 的可预期失败”。

---

## 第 6 章 `std::filesystem`

### 本章目标

- 掌握 `path` 的构造、拼接、查询和字符串转换；
- 理解抛异常版与 `error_code` 版的分工；
- 理解“临时文件 + rename”的原子发布协议；
- 知道 `filesystem` 不做什么。

### 阅读材料

- `include/robot_mcap/io.hpp`（第 31-58 行）
- `src/io.cpp`（第 40-238 行）
- `src/recorder.cpp`（第 164-200 行）
- `src/session_artifacts.cpp`（第 130-180、300-330 行）
- `plugins/ros2_jazzy/ros2_metadata.cpp`（第 110-135、170-215 行）

### 6.1 `path` 是值类型

```cpp
// types.hpp:18
std::filesystem::path path;

// recorder.cpp:188-189
session_directory =
    std::filesystem::absolute(options.parent_directory) / options.session_id;
```

`operator/` 自动补分隔符，`absolute()` 取绝对路径。注意 `absolute()` 不解析符号链接，也不规范化 `..`；`canonical()` 才解析，`lexically_normal()` 只做词法折叠。

### 6.2 六个常用操作

| 操作 | 项目位置 | 注意 |
|---|---|---|
| `create_directories` | `io.cpp:203` | 已存在返回 `false`，不报错 |
| `create_directory` | `io.cpp:207` | 返回 `true` 才表示本次新建 |
| `rename` | `io.cpp:163` | 只在同一文件系统内原子 |
| `remove` | `io.cpp:167` | `noexcept` 路径用 `error_code` 版 |
| `file_size` | `io.cpp:172` | 文件不存在会抛异常 |
| `exists` | `ros2_metadata.cpp:184` | 先 `exists` 再 `file_size` 有 TOCTOU |

### 6.3 原子发布协议

```cpp
// session_artifacts.cpp:158-161
const auto config_temp = directory_ / "recording-config.json.tmp";
...
file_system_->rename(config_temp, config_);

// session_artifacts.cpp:306
file_system_->rename(manifest_temp_, manifest_);
```

流程：写 `.tmp`/`.partial` → 按需 flush/fsync → `rename` 到最终名。这样读取方不会看到半成品。

**坑**：

- `rename` 只保证“改名”原子，不保证数据落盘；`DurabilityMode::Sync` 下项目还调用 `_commit`/`fsync`/`FlushFileBuffers`；
- 临时文件必须和目标同目录，否则跨卷；
- Windows 上目标文件被打开时 `rename` 可能失败。

### 6.4 三种字符串转换

| 方法 | 用途 | 项目位置 |
|---|---|---|
| `string()` | 交给 OS/外部库（原生窄编码） | `ros2_metadata.cpp:263` |
| `native()` | 平台原生字符串（Windows 是 `wstring`） | `mcap_file.cpp:113`、`recorder.cpp:172` |
| `generic_string()` | 统一 `/` 分隔，写 manifest/JSON | `session_artifacts.cpp:324` |

`generic_string()` **只统一分隔符，不解决编码**。C++17 的 `u8string()` 返回 `std::string`，C++20 返回 `std::u8string`。

### 6.5 `filesystem` 不做什么

- 不提供 `fsync`/持久化；
- 不提供文件独占创建（只有目录 `create_directory`）；
- 不提供安全校验。`path::operator/` 对绝对路径会替换左侧，`..` 原样保留。

`ros2_metadata.cpp:178-185` 是真实反例：只检查 `empty()` 和 `finalized`，没有校验 `relative_path` 是否相对、是否含 `..`/盘符/反斜杠。仓库审查记录已把这条列为 P1 F-06。**信任边界上的路径必须自己校验。**

**练习**：

1. 写一个 `safe_join(base, relative)`：拒绝绝对路径、盘符、空段、`.`、`..`、反斜杠；
2. 给 `ros2_metadata.cpp` 的 manifest 读取加一个 `../outside.mcap` 测试；
3. 解释 `absolute()`、`lexically_normal()`、`canonical()` 的区别。

**小结**：`filesystem` 负责“路径表示”和“文件操作”，不负责“持久化”和“安全”。

---

## 第 7 章 `std::optional`

### 本章目标

- 用 `optional` 表达“值可能不存在”；
- 区分“没有值”和“值为 0/空”；
- 理解 `optional<StopResult>` 的三态。

### 阅读材料

- `include/robot_mcap/types.hpp`（第 45-59 行）
- `include/robot_mcap/recording_flow.hpp`（第 87-97 行）
- `src/mcap_file.cpp`（第 180-195 行）
- `src/plugin_runtime.cpp`（第 74-81 行）

### 7.1 项目里的四类用法

```cpp
// 1. 可选 schema
std::optional<SchemaSpec> schema;            // types.hpp:48

// 2. 可选发布时间
std::optional<std::uint64_t> publish_time_ns; // types.hpp:54

// 3. 可选停止结果
std::optional<StopResult> stop;              // recording_flow.hpp:94

// 4. 服务查找结果
std::optional<Lookup> find(const std::string& service) const; // plugin_runtime.cpp:74
```

`mcap_file.cpp:190` 用 `value_or` 给缺省值：

```cpp
message.publish_time_ns.value_or(message.log_time_ns), message.size,
```

### 7.2 三态：`optional<StopResult>`

- `nullopt`：核心没启动或没停过；
- `StopResult` 且 `ok()`：停止成功；
- `StopResult` 且 `!ok()`：停止了但失败。

`recording_flow.cpp:386`：

```cpp
if (result.stop && !result.stop->ok()) {
```

**坑**：

- `if (opt)` 判断“有没有值”，不是“值是否为真”；`optional<uint64_t>(0)` 仍为 true；
- 空 `optional` 上 `*opt`/`opt->` 是 UB；`value()` 才抛 `bad_optional_access`；
- `optional` 只表达缺失，不表达失败原因；错误用 `Status`；
- C++23 的 `and_then`/`transform`/`std::expected` 不是 C++17。

**练习**：

1. 给 `RecordingFlowResult` 加一个 `std::optional<std::string> diagnostics`，并写出它的三态含义；
2. 把 `StopResult` 的处理改成先 `has_value()` 再 `->ok()`，比较两种写法的可读性；
3. 用 `optional<int>` 写一个例子，证明 `optional<int>(0)` 在 `if` 里为真。

**小结**：`optional` 把“有没有”放进类型，但不要用它表达“为什么失败”。

---

## 第 8 章 `std::string_view`

### 本章目标

- 理解“指针 + 长度”的非拥有视图；
- 学会在接口里用 `string_view`，在存储/异步边界用 `std::string`；
- 避免悬垂和 NUL 结尾问题。

### 阅读材料

- `include/robot_mcap/io.hpp`（第 62-66 行）
- `src/io.cpp`（第 219-238 行）
- `src/recorder.cpp`（第 29-39 行）
- `src/text.hpp`
- `plugins/ros2_jazzy/ros2_schema.cpp`（第 27-55 行）

### 8.1 项目里的用法

```cpp
// io.hpp:63-64
void write_file(FileSystem& file_system, const std::filesystem::path& path,
                std::string_view bytes);

// io.hpp:66
std::string sha256_hex(std::string_view data);

// recorder.cpp:29
bool valid_session_id(std::string_view value);
```

这些函数都是**同步、只读**的：调用期间数据必须存活，函数不保存 view。

### 8.2 与 `const std::string&` 的对比

| 写法 | 字面量/`char*` 传入 | 是否拥有 | 能否表示内嵌 NUL |
|---|---|---|---|
| `const std::string&` | 可能构造临时 `std::string`（分配） | 否 | 能 |
| `const char*` | 不分配 | 否 | 不能（依赖 NUL） |
| `std::string_view` | 不分配 | 否 | 能 |

`std::string` 可以隐式转成 `string_view`；反向必须显式构造。

### 8.3 坑

- 不能返回指向局部 `std::string` 的 view；
- `data()` 不保证 NUL 结尾，空 view 的 `data()` 可能是 `nullptr`；
- 不能直接和 `std::string` 用 `operator+`；
- C++17 给 `std::string` 增加了 `find(string_view)` 等重载，测试 `tests/recorder_lifecycle_test.cpp:778` 用了它。

**练习**：

1. 把 `valid_session_id` 改成接收 `const std::string&`，比较调用点的临时对象成本；
2. 写一个 `std::string trim(std::string_view)`，解释为什么返回 `std::string` 而不是 `string_view`；
3. 找出项目中所有 `string_view` 参数，确认它们都没有被存进成员或异步任务。

**小结**：`string_view` 是借用，不是拥有。接口参数可以用，存储和跨线程必须转 `std::string`。

---

## 第 9 章 `std::byte`

### 本章目标

- 理解 `std::byte` 与 `char`/`uint8_t`/`void*` 的语义差异；
- 学会用 `std::to_integer` 和 `reinterpret_cast`；
- 理解字节视图的生命周期和 aliasing 规则。

### 阅读材料

- `include/robot_mcap/types.hpp`（第 52-59 行）
- `include/robot_mcap/recorder.hpp`（第 19-25 行）
- `include/robot_mcap/io.hpp`（第 15-29 行）
- `src/io.cpp`（第 55-90、175-230 行）
- `plugins/ros2_jazzy/ros2_source.cpp`（第 315-330 行）

### 9.1 为什么不是 `char`

```cpp
// types.hpp:55
const std::byte* data;

// recorder.hpp:24
std::vector<std::byte> data;

// io.hpp:21
virtual void write(const std::byte* data, std::size_t size) = 0;
```

- `char` 暗示文本，且符号性依平台；
- `uint8_t` 是整数，会让人想做算术；
- `void*` 丢失长度和类型；
- `std::byte` 是 `enum class`，只支持位运算，语义就是“对象表示”。

### 9.2 转换

```cpp
// io.cpp:224
sink->write(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());

// ros2_source.cpp:324
reinterpret_cast<const std::byte*>(serialized.buffer),
```

取值：

```cpp
std::byte b{0xFF};
auto value = std::to_integer<unsigned>(b);  // 255
```

`byte + 1` 编译不过；`byte` 也不能隐式转 `char`/`int`。

### 9.3 生命周期

`reinterpret_cast` 只改视图，不复制、不延长生命周期。`bytes`/`serialized` 必须活到 `write` 结束。标准允许通过 `char`/`unsigned char`/`std::byte` 观察任意对象的字节表示，所以这种转换合规；但它不处理端序和对齐，CDR 的字节序由编码层负责。

**练习**：

1. 写 `void append_bytes(std::vector<std::byte>& out, std::string_view data)`；
2. 写一个函数把 `std::vector<std::byte>` 的前 4 字节按小端转成 `uint32_t`；
3. 解释为什么 `MessageView` 用 `const std::byte*` + `size`，而不是 `std::vector<std::byte>`。

**小结**：`std::byte` 把“原始内存”写进类型；它不替你做端序、对齐和生命周期管理。

---

## 第 10 章 模板、类型擦除与插件 ABI

### 本章目标

- 理解函数模板如何把类型变成运行时字符串；
- 理解 `shared_ptr<void>` + `static_pointer_cast` 的类型擦除；
- 理解插件 ABI：C 入口、`inline constexpr`、动态加载、同工具链要求。

### 阅读材料

- `include/robot_mcap/plugin_runtime.hpp`（全文，249 行）
- `src/plugin_runtime.cpp`（第 40-120、180-260、820-890 行）
- `tests/plugin_runtime_test.cpp`（重点看服务注册/依赖/回滚）

### 10.1 `service_id<T>`：类型 → 字符串

```cpp
// plugin_runtime.hpp:26-29
template <typename T>
std::string service_id() {
  return typeid(T).name();
}
```

`require_service<T>()`/`provide_service<T>()` 用它构造服务描述：

```cpp
// plugin_runtime.hpp:44-53
template <typename T>
ServiceRequirement require_service(std::string name, int min_version = 1,
                                   bool optional = false) {
  return {service_id<T>(), std::move(name), min_version, optional};
}
```

**注意**：函数模板本身是 C++98；这里的“新”是 `std::move`（C++11）和 NSDMI 聚合返回（C++14）。`typeid(T).name()` 是实现定义的修饰名，跨编译器/标准库可能不同，所以项目要求宿主和插件同工具链。

### 10.2 类型擦除

- 注册表键：`service_id<T>()`；
- 存储值：`std::shared_ptr<void>`；
- 还原：`std::static_pointer_cast<T>`（`plugin_runtime.hpp:128`）。

```cpp
template <typename T>
std::shared_ptr<T> require() {
  return std::static_pointer_cast<T>(require_erased(service_id<T>()));
}
```

为什么不用 `std::any`：

- `any` 存的是值，要求可拷贝；
- `any_cast<T>` 要求精确类型，不能把 `shared_ptr<Derived>` 转成 `shared_ptr<IBase>`；
- `shared_ptr<void>` 保留共享所有权，`static_pointer_cast` 能还原接口指针。

**坑**：`static_pointer_cast` 不做运行时检查，类型不匹配是 UB；提供服务时必须写接口类型 `context.provide<IBase>(ptr)`，不能传派生类指针。

### 10.3 插件 ABI

`plugin_runtime.hpp:195-206`：

```cpp
struct RobotMcapPluginModuleV1 {
  std::uint32_t abi_version;
  const char* plugin_id;
  IPlugin* (*create)();
  void (*destroy)(IPlugin*);
};
using RobotMcapPluginModuleV1Function = const RobotMcapPluginModuleV1* (*)();
inline constexpr std::uint32_t kPluginModuleAbiVersion = 1;
inline constexpr const char* kPluginModuleEntryPoint = "robot_mcap_plugin_module_v1";
```

宿主用 `LoadLibraryW`/`dlopen` 打开模块，用 `GetProcAddress`/`dlsym` 找入口（`plugin_runtime.cpp:847-867`），再校验 `abi_version`。

**为什么用 C 风格函数指针表**：C++ 没有稳定 ABI，不同编译器/标准库的类布局、异常、RTTI 都可能不同。用 C 结构体 + 函数指针把边界压到最小，再要求同工具链。

### 10.4 生命周期：`RegistrationHandle` 与 `Scope`

- `RegistrationHandle` 是 move-only RAII：`reset()` 最多注销一次，析构时自动注销；
- `Scope` 收集 stop/wait/finalize/cleanup 四个阶段的动作，关闭时按逆序执行；
- `PluginRuntime::stop()` 先跑所有 stop，再 wait，再 finalize，最后 cleanup。

**练习**：

1. 新增一个 `ITestService` 接口，在测试插件里 `provide<ITestService>`，宿主 `require<ITestService>`；
2. 解释为什么 `provide(derived_ptr)` 会导致 `require<IBase>()` 找不到；
3. 画出 `PluginRuntime::start()` 的 apply/activate 顺序和失败回滚路径。

**小结**：模板负责编译期类型，类型擦除负责运行期解耦，C ABI 负责跨模块边界；三者合起来才是插件系统。

---

## 第 11 章 并发编程

### 本章目标

- 理解线程生命周期、`join`/`terminate`；
- 理解 `mutex`/`lock_guard`/`unique_lock`/`condition_variable` 的配合；
- 理解 `promise`/`shared_future` 的一次性结果发布；
- 能读懂有界队列和停止协议。

### 阅读材料

- `src/recorder.cpp`（第 60-230、300-380、430-540、660-710 行）
- `include/robot_mcap/recorder.hpp`（第 85-112 行）

### 11.1 线程生命周期

```cpp
// recorder.cpp:152
std::thread thread;

// recorder.cpp:207
thread = std::thread([this] { run(); });

// recorder.cpp:98
thread.join();
```

规则：

- `std::thread` 析构时仍 `joinable()` 会 `std::terminate`；
- 必须 `join()` 或 `detach()`；
- `join()` 只能调用一次；
- 线程函数里逃逸的异常也会 `terminate`，所以 `run()` 标 `noexcept` 并内部捕获。

### 11.2 锁与锁序

```cpp
// recorder.cpp:130-131
std::mutex mutex;
std::condition_variable condition;

// recorder.cpp:222-224
// Lock order: writer_mutex before mutex. ...
```

`register_channel` 同时持有两把锁时严格按 `writer_mutex` → `mutex` 的顺序。`std::mutex` 不可递归，同一线程重复加锁是 UB。

`lock_guard` 不能提前解锁；`condition_variable::wait` 需要 `unique_lock`（等待期间解锁、唤醒后重新加锁）。

### 11.3 条件变量

```cpp
// recorder.cpp:488-491
std::unique_lock lock(mutex);
condition.wait(lock, [this] {
  return !queue.empty() || stop_requested || writer_failed || timing_failed;
});
```

必须带 predicate，防虚假唤醒；predicate 读取的标志必须在同一 `mutex` 下修改，否则是数据竞争或丢失唤醒。

### 11.4 `promise` / `shared_future`

```cpp
// recorder.cpp:68-73
std::shared_future<StopResult> ready_future(StopResult result) {
  std::promise<StopResult> promise;
  auto future = promise.get_future().share();
  promise.set_value(result);
  return future;
}
```

- `future` 不可拷贝、`get()` 只能一次；
- `shared_future` 可拷贝、可多次 `get()`，适合多个等待者；
- `promise` 未 `set_value` 就销毁会让 `future` 抛 `broken_promise`；
- 二次 `set_value` 抛 `future_error(promise_already_satisfied)`。

### 11.5 有界队列与停止协议

`Recorder::submit_*` 在 `mutex` 下检查 `queue_byte_limit`，入队后 `notify_one`；`stop_async` 设置 `stop_requested`，`notify_all`，写线程排空队列后 `finalize`。`StopResult` 通过 promise 发布给所有等待者。

**练习**：

1. 画出 `Created → Running → Stopping → Stopped` 状态机，标注每个转移由谁触发；
2. 证明 `condition.wait` 的 predicate 不会丢失唤醒；
3. 解释为什么 `finalize()` 必须 `noexcept`，以及 `promise.set_value` 可能抛什么。

**小结**：并发的正确性来自“锁序 + 条件变量谓词 + 所有权生命周期”，不是来自某个新语法。

---

## 第 12 章 C++17 语言特性

### 本章目标

- 掌握项目实际使用的 C++17 语法：CTAD、嵌套命名空间、结构化绑定、`inline constexpr`；
- 知道 `if` 初始化语句、`if constexpr`、折叠表达式、标准属性；
- 能区分 C++11/14/17/20。

### 阅读材料

- `src/recorder.cpp`（第 158、488 行）
- `src/plugin_runtime.cpp`（第 50、220 行）
- `src/text.hpp`（第 5 行）
- `plugins/ros2_jazzy/ros2_adapter.hpp`（第 18 行）
- `plugins/ros2_jazzy/ros2_metadata.cpp`（第 237 行）
- `include/robot_mcap/plugin_runtime.hpp`（第 205-206 行）

### 12.1 CTAD

```cpp
std::lock_guard lock(mutex);   // C++17
std::unique_lock lock(mutex);  // C++17
```

旧写法：

```cpp
std::lock_guard<std::mutex> lock(mutex);  // C++11
```

坑：必须命名；`std::lock_guard(mutex);` 会被解析成声明；C++14 编译不过。

### 12.2 嵌套命名空间

```cpp
namespace robot_mcap::detail {   // C++17
```

旧写法：

```cpp
namespace robot_mcap { namespace detail {
```

只配一个右花括号；不是 inline namespace。

### 12.3 结构化绑定

```cpp
// ros2_metadata.cpp:237
for (const auto& [channel_id, channel] : ordered_channels) {
```

坑：`auto` 会拷贝；C++17 lambda 不能捕获绑定名（C++20 才行）。

### 12.4 `inline constexpr` 变量

```cpp
// plugin_runtime.hpp:205-206
inline constexpr std::uint32_t kPluginModuleAbiVersion = 1;
inline constexpr const char* kPluginModuleEntryPoint = "robot_mcap_plugin_module_v1";
```

C++14 的命名空间作用域 `constexpr` 是内部链接，每个 TU 一份；C++17 的 `inline` 变量允许头文件定义并合并。跨 DLL 仍可能各一份，按值使用即可。

### 12.5 项目没用到的 C++17 语法

- **`if`/`switch` 初始化语句**：可以把 `plugin_runtime.cpp:86` 改成

  ```cpp
  if (const auto entry = entries_.find(service);
      entry != entries_.end() && entry->second.token == token) {
  ```

  注意 `if (auto status = status_.lock())` 不是 C++17，它是 C++98 的条件声明。
- **`if constexpr` / 折叠表达式**：项目模板简单，没有场景。
- **`[[nodiscard]]` / `[[maybe_unused]]` / `[[fallthrough]]`**：C++17；`[[noreturn]]` 是 C++11。项目目前没在公共 API 用 `[[nodiscard]]`，这是可改造点。
- **`std::variant` / `std::any`**：项目用 `shared_ptr<void>` 做类型擦除，不用 `any`。

**练习**：

1. 把 `plugin_runtime.cpp:86` 改成 `if` 初始化语句，编译并确认作用域变化；
2. 给 `Recorder::start()` 加 `[[nodiscard]]`，用 `-Wall -Werror` 编译一个故意丢弃结果的片段；
3. 列一张表：项目里每个特性分别是 C++11/14/17 哪一版。

**小结**：C++17 语法是“减少样板、表达意图”，不是“越新越好”。先确认标准版本，再决定用不用。

---

## 第 13 章 综合练习与代码审查

### 本章目标

- 把前面章节的知识串起来，完成一个小改动；
- 用审查清单检查自己的代码。

### 13.1 综合练习：给会话产物加一个字段

目标：在 `manifest.json` 的 `files[]` 里增加一个 `sha256` 字段（已有 `FileFacts::sha256` 是 `optional<string>`）。

步骤：

1. 读 `src/session_artifacts.cpp` 的 `manifest_json()`，找到 `files[]` 的输出位置；
2. 用 `optional` 的三态决定输出 `null` 还是字符串；
3. 用 `std::filesystem` 计算文件大小/哈希（已有 `sha256_hex`）；
4. 补一个测试：文件存在/不存在两种情况；
5. 用 `generic_string()` 输出路径，确保 Windows 上也用 `/`。

### 13.2 综合练习：修路径校验

目标：修 `ros2_metadata.cpp:178-185` 的 manifest 路径拼接。

要求：

- 拒绝绝对路径、盘符、空段、`.`、`..`、反斜杠；
- 校验失败返回 `Status`，不生成 `metadata.yaml`；
- 加一个 `../outside.mcap` 的测试；
- 说明为什么 `lexically_normal()` 不能替代校验。

### 13.3 代码审查清单

| 维度 | 问题 |
|---|---|
| 所有权 | 这个对象谁拥有？`unique_ptr`/`shared_ptr`/栈？ |
| 生命周期 | 引用/`string_view`/`byte*` 活到使用点了吗？ |
| 错误 | 失败用异常、`Status` 还是 `error_code`？ |
| 异常安全 | move 之前会不会抛？部分构造怎么回滚？ |
| 线程 | 哪些线程访问？锁序是什么？条件变量谓词？ |
| 标准 | 这个特性是 C++17 还是 C++11/14/20？ |
| 平台 | Windows/POSIX 分支都覆盖了吗？ |
| 测试 | 正常、边界、失败路径都有吗？ |

### 13.4 下一步建议

- 给 `Status` 返回函数全面加 `[[nodiscard]]`；
- 用 `std::to_chars` 替换 timing 热路径的 `std::to_string`；
- 评估 `std::scoped_lock` 是否能替代 `register_channel` 的手写锁序；
- 给 `ros2_metadata` 的路径读取加模糊测试；
- 把 CMake 切成严格 C++17 并在 CI 中固定。

**小结**：读懂项目只是第一步；能用项目的约定改一个功能，才算真正掌握。

---

## 附录 A 标准版本速查

| 特性 | 标准 | 项目是否使用 |
|---|---|---|
| `enum class`、`noexcept`、`unique_ptr`、`shared_ptr`、`std::function`、`future`/`promise`、`thread`/`mutex`/`condition_variable`、移动语义、lambda | C++11 | 大量使用 |
| `std::make_unique`、`std::exchange`、泛型 lambda、init-capture、`std::chrono_literals`、聚合 + NSDMI | C++14 | 使用 |
| `std::filesystem`、`std::optional`、`std::string_view`、`std::byte`、CTAD、嵌套命名空间、`inline constexpr`、结构化绑定 | C++17 | 使用 |
| `if` 初始化语句、`if constexpr`、折叠表达式、`[[nodiscard]]`、`[[maybe_unused]]`、`[[fallthrough]]` | C++17 | **未使用**（可改造） |
| `std::variant`、`std::any`、`std::shared_mutex`、`std::scoped_lock`、`std::from_chars`/`to_chars`、`std::clamp`/`apply`/`invoke` | C++17 | **刻意未使用** |
| `std::expected`、`std::move_only_function`、`and_then`/`transform`（optional） | C++23 | 未使用，别误标成 C++17 |

---

## 附录 B 文件地图

| 文件 | 学什么 |
|---|---|
| `include/robot_mcap/types.hpp` | 值类型、`enum class`、`optional`、`byte`、`path` |
| `include/robot_mcap/io.hpp` | 抽象接口、`unique_ptr` 工厂、`string_view`、错误契约 |
| `include/robot_mcap/recorder.hpp` | 公共 API、PImpl、`shared_future`、移动语义 |
| `include/robot_mcap/plugin_runtime.hpp` | 模板、类型擦除、插件 ABI、`inline constexpr` |
| `include/robot_mcap/recording_flow.hpp` | 接口组合、`optional<StopResult>`、生命周期 |
| `src/io.cpp` | `filesystem`、异常/`error_code`、平台分支 |
| `src/mcap_file.cpp` | MCAP 适配、`byte` 缓冲、`optional` 校验 |
| `src/recorder.cpp` | 并发、队列、停止协议、RAII |
| `src/session_artifacts.cpp` | 临时文件 + `rename`、`generic_string`、JSON |
| `src/plugin_runtime.cpp` | 服务注册、依赖排序、动态加载、回滚 |
| `plugins/ros2_jazzy/ros2_metadata.cpp` | 结构化绑定、`optional`、路径安全 |
| `tests/recorder_lifecycle_test.cpp` | 行为测试、future、gated sink |
| `tests/plugin_runtime_test.cpp` | 服务/依赖/回滚、`weak_ptr` |

---

## 附录 C 术语表

- **RAII**：构造函数获取资源，析构函数释放资源；作用域即生命周期。
- **值语义**：对象可拷贝/移动，行为像值；与之相对的是引用/指针语义。
- **PImpl**：Pointer to implementation，用不完整类型隐藏实现。
- **类型擦除**：用统一接口保存不同类型；本项目用 `shared_ptr<void>` + 字符串键。
- **ABI**：二进制接口，包括布局、调用约定、异常/RTTI；C++ 没有稳定 ABI。
- **CTAD**：Class Template Argument Deduction，类模板实参推导（C++17）。
- **NSDMI**：Non-Static Data Member Initializer，默认成员初始化器（C++11）。
- **TOCTOU**：Time-of-check to time-of-use，检查后使用之间状态可能变化。
- **ODR**：One Definition Rule，同一实体在整个程序中只能有一个定义。
- **moved-from**：被移动后的对象，仍然有效但值未指定。

---

**怎么用这份教程**：建议按章顺序读，每章先读指定文件，再回答问题，最后做练习。第 2-5 章是语言地基，第 6-9 章是标准库工具，第 10-11 章是项目核心，第 12-13 章把 C++17 和工程实践合起来。

> 说明：行号按当前工作树核对，修改代码后会漂移。第 6 章提到的 F-06 路径拼接问题来自仓库审查记录，修复状态以当前代码为准。
