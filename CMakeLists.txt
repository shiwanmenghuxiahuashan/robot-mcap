cmake_minimum_required(VERSION 3.22)
project(robot_mcap LANGUAGES CXX)

include(CTest)
include(cmake/Dependencies.cmake)

find_package(Threads REQUIRED)

# 源码按限界上下文分组，上下文内部再按 DDD 层分组：domain 不依赖其他层，
# application 负责用例编排，infrastructure 提供 I/O 和平台机制。shared_kernel
# 只依赖标准库。私有头通过 PRIVATE src 根目录包含。
add_library(robot_mcap STATIC
  # shared_kernel
  src/shared_kernel/sha256.cpp
  src/shared_kernel/text.cpp

  # infrastructure
  src/infrastructure/filesystem/io.cpp
  src/infrastructure/mcap/mcap_file.cpp
  src/infrastructure/mcap/mcap_vendor.cpp

  # session_format
  src/session_format/domain/documents.cpp
  src/session_format/infrastructure/session_artifacts.cpp

  # plugin_runtime
  src/plugin_runtime/domain/dependency_resolution.cpp
  src/plugin_runtime/domain/descriptor_rules.cpp
  src/plugin_runtime/application/plugin_runtime.cpp
  src/plugin_runtime/application/scope.cpp
  src/plugin_runtime/application/context.cpp
  src/plugin_runtime/infrastructure/service_registry.cpp
  src/plugin_runtime/infrastructure/native_module.cpp

  # recording
  src/recording/domain/message_rules.cpp
  src/recording/application/recorder.cpp
  src/recording/application/recorder_writer.cpp
  src/recording/application/recording_flow.cpp
  src/recording/application/prechecks.cpp
  src/recording/adapters/recording_flow_plugin.cpp
)
target_compile_features(robot_mcap PUBLIC cxx_std_17)
target_include_directories(robot_mcap PUBLIC include PRIVATE src)
set_target_properties(robot_mcap PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(robot_mcap PRIVATE
  robot_mcap_upstream Threads::Threads ${CMAKE_DL_LIBS})

if(BUILD_TESTING)
  add_executable(mcap_roundtrip_test tests/mcap_roundtrip_test.cpp)
  target_link_libraries(mcap_roundtrip_test PRIVATE robot_mcap robot_mcap_upstream)
  add_test(NAME mcap_roundtrip COMMAND mcap_roundtrip_test "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(mcap_roundtrip PROPERTIES TIMEOUT 60)

  add_executable(recorder_lifecycle_test tests/recorder_lifecycle_test.cpp)
  target_link_libraries(recorder_lifecycle_test
    PRIVATE robot_mcap robot_mcap_upstream Threads::Threads)
  add_test(NAME recorder_lifecycle
    COMMAND recorder_lifecycle_test "${CMAKE_CURRENT_BINARY_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/session-v1")
  set_tests_properties(recorder_lifecycle PROPERTIES TIMEOUT 120)

  add_library(robot_mcap_test_plugin SHARED tests/plugin_test_module.cpp)
  target_link_libraries(robot_mcap_test_plugin PRIVATE robot_mcap)
  target_include_directories(robot_mcap_test_plugin PRIVATE tests)

  add_library(robot_mcap_test_bad_plugin SHARED tests/plugin_test_bad_module.cpp)
  target_link_libraries(robot_mcap_test_bad_plugin PRIVATE robot_mcap)
  target_include_directories(robot_mcap_test_bad_plugin PRIVATE tests)

  add_executable(plugin_runtime_test tests/plugin_runtime_test.cpp)
  target_link_libraries(plugin_runtime_test
    PRIVATE robot_mcap robot_mcap_upstream Threads::Threads)
  target_include_directories(plugin_runtime_test PRIVATE tests)
  add_dependencies(plugin_runtime_test robot_mcap_test_plugin robot_mcap_test_bad_plugin)
  add_test(NAME plugin_runtime
    COMMAND plugin_runtime_test "${CMAKE_CURRENT_BINARY_DIR}"
            "$<TARGET_FILE:robot_mcap_test_plugin>"
            "$<TARGET_FILE:robot_mcap_test_bad_plugin>")
  set_tests_properties(plugin_runtime PROPERTIES TIMEOUT 120)
endif()

option(ROBOT_MCAP_WITH_ROS2 "Build the ROS 2 Jazzy adapter and tests" OFF)
if(ROBOT_MCAP_WITH_ROS2)
  add_subdirectory(plugins/ros2_jazzy)
endif()
