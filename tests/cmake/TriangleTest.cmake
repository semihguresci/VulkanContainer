add_executable(TrianngleTest tests/triangle_test.cpp)
add_custom_test(NAME TrianngleTest COMMAND ${CMAKE_BINARY_DIR}/tests/TriangleTest ${CMAKE_BINARY_DIR}/tests/shaders)