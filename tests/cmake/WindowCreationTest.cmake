add_executable(WindowCreationTest tests/window_creation_test.cpp)
add_test(NAME WindowCreationTest COMMAND ${CMAKE_BINARY_DIR}/tests/WindowCreationTest)