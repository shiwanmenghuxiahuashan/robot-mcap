cmake_minimum_required(VERSION 3.22)
project(robot_mcap LANGUAGES CXX)

include(CTest)
include(cmake/Dependencies.cmake)

add_library(robot_mcap STATIC src/mcap_file.cpp src/mcap_vendor.cpp)
target_compile_features(robot_mcap PUBLIC cxx_std_17)
target_include_directories(robot_mcap PUBLIC include)
target_link_libraries(robot_mcap PRIVATE robot_mcap_upstream)

if(BUILD_TESTING)
  add_executable(mcap_roundtrip_test tests/mcap_roundtrip_test.cpp)
  target_link_libraries(mcap_roundtrip_test PRIVATE robot_mcap robot_mcap_upstream)
  add_test(NAME mcap_roundtrip COMMAND mcap_roundtrip_test "${CMAKE_CURRENT_BINARY_DIR}")
endif()
