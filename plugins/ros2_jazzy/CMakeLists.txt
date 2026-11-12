find_package(rclcpp REQUIRED)
find_package(rosbag2_storage REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)

add_library(robot_mcap_ros2_adapter STATIC
  ros2_adapter.cpp
  ros2_schema.cpp
  ros2_metadata.cpp
)
target_include_directories(robot_mcap_ros2_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(robot_mcap_ros2_adapter PUBLIC
  robot_mcap
  robot_mcap_upstream
  rclcpp::rclcpp
  rosbag2_storage::rosbag2_storage
  ament_index_cpp::ament_index_cpp
)
target_compile_features(robot_mcap_ros2_adapter PUBLIC cxx_std_17)
set_target_properties(robot_mcap_ros2_adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(robot_mcap_ros2_source SHARED ros2_source.cpp)
target_link_libraries(robot_mcap_ros2_source PRIVATE
  robot_mcap_ros2_adapter
  robot_mcap
  rclcpp::rclcpp
  ament_index_cpp::ament_index_cpp
)
set_target_properties(robot_mcap_ros2_source PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
  add_executable(ros2_record_replay_test
    ${PROJECT_SOURCE_DIR}/tests/ros2_jazzy/record_replay_test.cpp)
  target_link_libraries(ros2_record_replay_test PRIVATE
    robot_mcap_ros2_adapter
    robot_mcap
    robot_mcap_upstream
    rclcpp::rclcpp
    std_msgs::std_msgs
    geometry_msgs::geometry_msgs
    Threads::Threads
  )
  target_include_directories(ros2_record_replay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_dependencies(ros2_record_replay_test robot_mcap_ros2_source)
  add_test(NAME ros2_record_replay
    COMMAND bash -lc "source /opt/ros/jazzy/setup.bash && \"$<TARGET_FILE:ros2_record_replay_test>\" \"$<TARGET_FILE:robot_mcap_ros2_source>\" \"${CMAKE_CURRENT_BINARY_DIR}/ros2_record_replay\"")
  set_tests_properties(ros2_record_replay PROPERTIES TIMEOUT 240)
endif()
