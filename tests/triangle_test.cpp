#include <gtest/gtest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdexcept>

namespace {

class GlfwGuard {
 public:
  GlfwGuard() {
    if (glfwInit() != GLFW_TRUE) {
      throw std::runtime_error("Failed to initialize GLFW");
    }
  }

  ~GlfwGuard() { glfwTerminate(); }

  GlfwGuard(const GlfwGuard&) = delete;
  GlfwGuard& operator=(const GlfwGuard&) = delete;
};

}  // namespace

TEST(TriangleTest, GlfwReportsVulkanSupportAndExtensions) {
  GlfwGuard glfw{};

  ASSERT_EQ(glfwVulkanSupported(), GLFW_TRUE);

  uint32_t extensionCount = 0;
  const char** extensions =
      glfwGetRequiredInstanceExtensions(&extensionCount);

  ASSERT_NE(extensions, nullptr);
  ASSERT_GT(extensionCount, 0u);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
