# robot-mcap

Independent C++17 MCAP recording core. The library provides a single-file indexed
MCAP writer and a session recorder with a bounded asynchronous queue, copy/ownership
submission paths, and a shared stop result. See the [session contract](docs/contracts/session-v1.md)
for the on-disk format.

## Build and test

```sh
cmake -S . -B build/core -DBUILD_TESTING=ON
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

CMake fetches the pinned Foxglove MCAP revision on the first build. Set
`FETCHCONTENT_SOURCE_DIR_FOXGLOVE_MCAP` to an existing checkout of that revision for an
offline build. The core has no ROS or compression-library dependency.

## Recorder

```cpp
#include "robot_mcap/recorder.hpp"

robot_mcap::RecorderOptions options;
options.parent_directory = "sessions";
options.session_id = "demo-001";
options.queue_byte_limit = 64ULL * 1024 * 1024;

robot_mcap::Recorder recorder(options);
if (!recorder.start().ok()) {
  return 1;
}

robot_mcap::ChannelId channel = 0;
if (!recorder.register_channel({"/sensor/raw", "raw", std::nullopt}, channel).ok()) {
  return 1;
}

std::vector<std::byte> payload = /* bytes */;
auto submitted = recorder.submit_copy(
    channel, {timestamp_ns, std::nullopt, payload.data(), payload.size()});
if (!submitted.ok()) {
  // QueueFull, Stopped, WriterFailed or InvalidArgument
}

const auto stop = recorder.stop();  // stop_async() returns the same shared future
```

- `submit_copy` copies before returning, so the caller may reuse its buffer immediately.
- `submit_owned` takes an rvalue `OwnedMessage`; ownership transfers only on success.
- A failed writer or timing sidecar finalizes the session autonomously; `stop_async()`
  observes that result without requiring a stop call.
- `start()` creates `<parent>/<session_id>` exclusively. During recording the MCAP and
  optional timing sidecar carry `.partial`; successful completion renames them and
  publishes `manifest.json` through a temporary file.
- `DurabilityMode::Sync` fsyncs the data, timing sidecar, config and manifest temporary
  file before the manifest rename, then the session and parent directories. A sync
  failure fails the stop even when the MCAP is structurally complete.

A completed session contains `recording-config.json`, `data_0000.mcap`,
`manifest.json`, and `data_0000.timing.ndjson` when per-message timing evidence is
needed. Failed sessions retain `.partial` artifacts and publish a best-effort failed
manifest as described by the contract.

## Plugin runtime

The core can be used directly, or assembled from plugins with a static configuration.
The plugin runtime resolves versioned services, orders plugins by dependency, runs
prechecks, and executes an explicit shutdown barrier.

```cpp
auto recorder = std::make_shared<robot_mcap::Recorder>(recorder_options);
auto sink = std::make_shared<robot_mcap::CoreRecordingSink>(recorder);
auto flow = std::make_shared<robot_mcap::RecordingFlow>(
    robot_mcap::StartContext{parent_directory, required_free_bytes});

robot_mcap::PluginRuntime runtime;
runtime.bind_host_service<robot_mcap::IRecordingSink>(sink, "IRecordingSink");
runtime.add_plugin_factory("recording_flow",
                           robot_mcap::recording_flow_factory(flow));
runtime.add_plugin_factory("ros2_source", ros2_source_factory);
runtime.start({"ros2_source", "recording_flow"});
// ... record ...
runtime.stop();
```

- All plugins `apply()` in dependency order; every provider's declared services are
  verified before a dependent applies. Then all plugins `activate()` in the same order,
  so sources only start producing after the recording flow has passed prechecks and
  started the core.
- Sources require `IMessageSink` and `IRecordingStatus`; only the recording flow
  receives `IRecordingSink` (start/stop control). The flow provides the message-only
  view to sources.
- Shutdown runs all stop actions, then all waits, then all finalizes, then all cleanup
  and service registrations, in reverse initialization order. The recording flow stops
  the core last, after sources have flushed their final messages.
- Prechecks include output-directory writability and a free-space threshold. Plugins
  can add checks through the `IPrecheckRegistry` service; all checks run before the
  core starts. `RecordingFlowResult::post_check` stays `NotRequested` when no
  post-check is configured.
- Native modules expose `robot_mcap_plugin_module_v1` and must be built with the same
  toolchain and headers. `PluginRuntime::load_plugin_module()` registers their factory;
  the module stays loaded until every object it created has been destroyed.
- Service IDs are derived from `service_id<T>()`; provide the interface type
  explicitly, for example `context.provide<IMessageSink>(sink)`.

## Tests

The CTest suite covers:

- `mcap_roundtrip`: writer/reader round trip, schemas, raw payloads and invalid inputs.
- `recorder_lifecycle`: queue-full and stop boundaries with a gated sink; copy and
  ownership buffer lifetime; write, finish, timing, manifest and sync failures;
  two-instance isolation; concurrent registration and same-session start; timing
  sidecar bytes against `tests/fixtures/session-v1/01-finalized`; and manifest
  statistics cross-checked with the upstream reader.
- `plugin_runtime`: missing dependencies, duplicate providers/factories, version
  mismatches, dependency cycles, mid-apply and activate rollback, cleanup phase order,
  in-flight callback joins, throwing stop actions, optional dependencies, precheck
  registration/closure, sink startup failure, source-before-flow ordering, a final
  message submitted during finalize, core-only recording through the flow, and native
  module load/lifetime/two-instance isolation.

Sanitizer builds used during development (run with an absolute test directory):

```sh
cmake -S . -B build/tsan -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/tsan
setarch $(uname -m) -R ./build/tsan/recorder_lifecycle_test "$PWD/build/tsan" \
  "$PWD/tests/fixtures/session-v1"
```

`setarch -R` works around the WSL kernel's memory layout for ThreadSanitizer.
AddressSanitizer/UBSan builds use the same pattern with
`-fsanitize=address,undefined`.

## Platform notes

Linux (Ubuntu 22.04 WSL, GCC 11.4, CMake 3.22) is the validated environment.
Windows sources use `_wfopen_s`, `_commit` and `FlushFileBuffers` for directory
durability, but the Windows build is not validated in this repository. Some Windows
filesystems reject directory flushes; sync mode reports that as a failure rather than
claiming confirmation.
