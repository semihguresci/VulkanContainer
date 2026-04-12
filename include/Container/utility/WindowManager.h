#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Container/common/CommonGLFW.h"
#include "Container/common/CommonVulkan.h"

// Forward declarations
struct GLFWwindow;
struct GLFWmonitor;

namespace container::window {

class Window {
 public:
  using ResizeCallback = std::function<void(int width, int height)>;

  explicit Window(GLFWwindow* window);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  Window(Window&& other) noexcept;
  Window& operator=(Window&& other) noexcept;

  // Window state queries
  [[nodiscard]] bool shouldClose() const;
  void setShouldClose(bool value);
  void pollEvents() const;
  void waitForEvents() const;

  // Window properties
  void setTitle(const std::string& title);
  void setSize(int width, int height);
  void getWindowSize(int& width, int& height) const;
  void getFramebufferSize(int& width, int& height) const;
  void setPosition(int x, int y);
  void getPosition(int& x, int& y) const;

  // Window operations
  void maximize();
  void restore();
  void iconify();
  void show();
  void hide();
  void focus();
  void requestAttention();

  // Callback setters
  void setFramebufferResizeCallback(ResizeCallback callback);

  // Vulkan-specific (C Vulkan)
  void setUserPointer(void* userPointer);
  VkSurfaceKHR createSurface(VkInstance instance) const;

  // Raw window handle access
  [[nodiscard]] GLFWwindow* getNativeWindow() const { return window_; }

 private:
  GLFWwindow* window_ = nullptr;
  ResizeCallback resizeCallback_;

  static void FramebufferResizeThunk(GLFWwindow* window, int width, int height);
};

class WindowManager {
 public:
  struct WindowConfig {
    uint32_t width = 800;
    uint32_t height = 600;
    std::string title = "Application";
    bool resizable = true;
    bool visible = true;
    bool decorated = true;
    bool focused = true;
    bool maximized = false;
    bool centerCursor = true;
    bool transparentFramebuffer = false;
    bool floating = false;
    bool focusOnShow = true;
    int samples = 0;
    int refreshRate = 0;
  };

  struct MonitorInfo {
    std::string name;
    int width;
    int height;
    int refreshRate;
  };

  WindowManager();
  ~WindowManager();

  WindowManager(const WindowManager&) = delete;
  WindowManager& operator=(const WindowManager&) = delete;

  std::unique_ptr<Window> createWindow(uint32_t width, uint32_t height,
                                       std::string_view title) const;
  std::unique_ptr<Window> createWindow(const WindowConfig& config) const;

  void pollEvents() const;
  void waitEvents() const;
  void waitEventsTimeout(double timeout) const;
  void postEmptyEvent() const;

  [[nodiscard]] double getTime() const;
  void setTime(double time);

  [[nodiscard]] std::vector<MonitorInfo> getMonitors() const;
  [[nodiscard]] MonitorInfo getPrimaryMonitor() const;

  // Vulkan support
  [[nodiscard]] bool isVulkanSupported() const;
  [[nodiscard]] std::vector<const char*> getRequiredInstanceExtensions() const;

  [[nodiscard]] bool isInitialized() const { return initialized_; }

 private:
  bool initialized_ = false;
};

}  // namespace container::window
