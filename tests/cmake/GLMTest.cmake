add_executable(GLMTest tests/glm_tests.cpp)
add_custom_test(NAME GLMTest COMMAND ${CMAKE_BINARY_DIR}/tests/GLMTest)