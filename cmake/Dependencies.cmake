include(FetchContent)

# Foxglove MCAP C++ v2.2.0. The core milestone writes uncompressed chunks.
FetchContent_Declare(foxglove_mcap
  GIT_REPOSITORY https://github.com/foxglove/mcap.git
  GIT_TAG 9b4d3e36c7f7742541df33e0cff35c95de6af2a1
  GIT_CONFIG filter.lfs.process= filter.lfs.smudge= filter.lfs.required=false
  SOURCE_SUBDIR cpp/mcap/include
)
FetchContent_MakeAvailable(foxglove_mcap)

add_library(robot_mcap_upstream INTERFACE)
target_include_directories(robot_mcap_upstream SYSTEM INTERFACE
  "${foxglove_mcap_SOURCE_DIR}/cpp/mcap/include"
)
target_compile_definitions(robot_mcap_upstream INTERFACE
  MCAP_COMPRESSION_NO_LZ4 MCAP_COMPRESSION_NO_ZSTD
)
