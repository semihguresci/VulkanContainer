#include <gtest/gtest.h>
#include <GLFW/glfw3.h>

TEST(GLFWTest, WindowCreation) {
    ASSERT_EQ(glfwInit(), GLFW_TRUE) << "Failed to initialize GLFW";

    GLFWwindow* window = glfwCreateWindow(800, 600, "Test Window", nullptr, nullptr);
    ASSERT_NE(window, nullptr) << "Failed to create GLFW window";

    glfwDestroyWindow(window);

    glfwTerminate();
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
