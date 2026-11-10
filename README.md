# robot-mcap

Independent C++17 MCAP file writer. See the [session contract](docs/contracts/session-v1.md) for the planned recorder artifacts.

## Build and test

```sh
cmake -S . -B build/core -DBUILD_TESTING=ON
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

CMake fetches the pinned Foxglove MCAP revision on the first build. Set
`FETCHCONTENT_SOURCE_DIR_FOXGLOVE_MCAP` to an existing checkout of that
revision for an offline build. The core currently writes indexed,
uncompressed MCAP files without ROS or compression library dependencies.
