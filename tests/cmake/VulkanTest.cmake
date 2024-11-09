add_executable(VulkanTest ${CMAKE_SOURCE_DIR}/tests/vulkan_tests.cpp)
add_custom_test(NAME VulkanTest COMMAND ${CMAKE_BINARY_DIR}/tests/VulkanTest)