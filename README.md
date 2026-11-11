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

## Tests

The CTest suite covers:

- `mcap_roundtrip`: writer/reader round trip, schemas, raw payloads and invalid inputs.
- `recorder_lifecycle`: queue-full and stop boundaries with a gated sink; copy and
  ownership buffer lifetime; write, finish, timing, manifest and sync failures;
  two-instance isolation; concurrent registration and same-session start; timing
  sidecar bytes against `tests/fixtures/session-v1/01-finalized`; and manifest
  statistics cross-checked with the upstream reader.

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
