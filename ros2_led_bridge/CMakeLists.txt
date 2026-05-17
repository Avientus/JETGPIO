cmake_minimum_required(VERSION 3.8)
project(ros2_led_bridge)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/LedCommand.msg"
)

add_executable(led_bridge_node src/led_bridge_node.cpp)
ament_target_dependencies(led_bridge_node rclcpp)
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")
target_link_libraries(led_bridge_node ${cpp_typesupport_target})

install(TARGETS led_bridge_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
