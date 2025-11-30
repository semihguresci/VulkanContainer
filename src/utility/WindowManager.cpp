#include <Container/utility/WindowManager.h>


#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace utility::window {

namespace {
constexpr int kDefaultClientApi = GLFW_NO_API;
}

// Window implementation
Window::Window(GLFWwindow* window) : window_(window) {
  if (!window_) {
    throw std::runtime_error("Invalid GLFW window handle.");
  }
}

Window::~Window() {
  if (window_) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
}

Window::Window(Window&& other) noexcept
    : window_(std::exchange(other.window_, nullptr)) {}

Window& Window::operator=(Window&& other) noexcept {
  if (this != &other) {
    if (window_) {
      glfwDestroyWindow(window_);
    }
    window_ = std::exchange(other.window_, nullptr);
  }
  return *this;
}

bool Window::shouldClose() const { return glfwWindowShouldClose(window_) != 0; }

void Window::setShouldClose(bool value) {
  glfwSetWindowShouldClose(window_, value ? GLFW_TRUE : GLFW_FALSE);
}

void Window::pollEvents() const { glfwPollEvents(); }

void Window::waitForEvents() const { glfwWaitEvents(); }

void Window::setTitle(const std::string& title) {
  glfwSetWindowTitle(window_, title.c_str());
}

void Window::setSize(int width, int height) {
  glfwSetWindowSize(window_, width, height);
}

void Window::getWindowSize(int& width, int& height) const {
  glfwGetWindowSize(window_, &width, &height);
}

void Window::getFramebufferSize(int& width, int& height) const {
  glfwGetFramebufferSize(window_, &width, &height);
}

void Window::setPosition(int x, int y) { glfwSetWindowPos(window_, x, y); }

void Window::getPosition(int& x, int& y) const {
  glfwGetWindowPos(window_, &x, &y);
}

void Window::maximize() { glfwMaximizeWindow(window_); }

void Window::restore() { glfwRestoreWindow(window_); }

void Window::iconify() { glfwIconifyWindow(window_); }

void Window::show() { glfwShowWindow(window_); }

void Window::hide() { glfwHideWindow(window_); }

void Window::focus() { glfwFocusWindow(window_); }

void Window::requestAttention() { glfwRequestWindowAttention(window_); }

void Window::setFramebufferResizeCallback(ResizeCallback callback) {
  resizeCallback_ = std::move(callback);
  if (resizeCallback_) {
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, FramebufferResizeThunk);
  } else {
    glfwSetFramebufferSizeCallback(window_, nullptr);
  }
}

void Window::setUserPointer(void* userPointer) {
  glfwSetWindowUserPointer(window_, userPointer);
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create window surface!");
  }
  return surface;
}

void Window::FramebufferResizeThunk(GLFWwindow* window, int width, int height) {
  auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self && self->resizeCallback_) {
    self->resizeCallback_(width, height);
  }
}

// WindowManager implementation
WindowManager::WindowManager() {
  if (glfwInit() != GLFW_TRUE) {
    throw std::runtime_error("Failed to initialize GLFW!");
  }

  // Set error callback for better debugging
  glfwSetErrorCallback([](int error, const char* description) {
    throw std::runtime_error("GLFW Error " + std::to_string(error) + ": " +
                             description);
  });

  initialized_ = true;
  glfwWindowHint(GLFW_CLIENT_API, kDefaultClientApi);
}

WindowManager::~WindowManager() {
  if (initialized_) {
    glfwTerminate();
  }
}

std::unique_ptr<Window> WindowManager::createWindow(
    uint32_t width, uint32_t height, std::string_view title) const {
  if (!initialized_) {
    throw std::runtime_error("GLFW not initialized before creating a window!");
  }

  GLFWwindow* window =
      glfwCreateWindow(static_cast<int>(width), static_cast<int>(height),
                       std::string(title).c_str(), nullptr, nullptr);
  if (!window) {
    throw std::runtime_error("Failed to create GLFW window!");
  }

  return std::make_unique<Window>(window);
}

std::unique_ptr<Window> WindowManager::createWindow(
    const WindowConfig& config) const {
  if (!initialized_) {
    throw std::runtime_error("GLFW not initialized before creating a window!");
  }

  // Set window hints based on configuration
  glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_DECORATED, config.decorated ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_FOCUSED, config.focused ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_MAXIMIZED, config.maximized ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_CENTER_CURSOR,
                 config.centerCursor ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER,
                 config.transparentFramebuffer ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_FLOATING, config.floating ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_FOCUS_ON_SHOW,
                 config.focusOnShow ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_SAMPLES, config.samples);
  glfwWindowHint(GLFW_REFRESH_RATE, config.refreshRate);

  GLFWwindow* window = glfwCreateWindow(static_cast<int>(config.width),
                                        static_cast<int>(config.height),
                                        config.title.c_str(), nullptr, nullptr);

  if (!window) {
    throw std::runtime_error("Failed to create GLFW window!");
  }

  return std::make_unique<Window>(window);
}

void WindowManager::pollEvents() const { glfwPollEvents(); }

void WindowManager::waitEvents() const { glfwWaitEvents(); }

void WindowManager::waitEventsTimeout(double timeout) const {
  glfwWaitEventsTimeout(timeout);
}

void WindowManager::postEmptyEvent() const { glfwPostEmptyEvent(); }

double WindowManager::getTime() const { return glfwGetTime(); }

void WindowManager::setTime(double time) { glfwSetTime(time); }

std::vector<WindowManager::MonitorInfo> WindowManager::getMonitors() const {
  std::vector<MonitorInfo> monitors;
  int count;
  GLFWmonitor** glfwMonitors = glfwGetMonitors(&count);

  for (int i = 0; i < count; ++i) {
    const GLFWvidmode* mode = glfwGetVideoMode(glfwMonitors[i]);
    monitors.push_back({glfwGetMonitorName(glfwMonitors[i]), mode->width,
                        mode->height, mode->refreshRate});
  }
  return monitors;
}

WindowManager::MonitorInfo WindowManager::getPrimaryMonitor() const {
  GLFWmonitor* primary = glfwGetPrimaryMonitor();
  if (!primary) {
    throw std::runtime_error("No primary monitor found!");
  }

  const GLFWvidmode* mode = glfwGetVideoMode(primary);
  return {glfwGetMonitorName(primary), mode->width, mode->height,
          mode->refreshRate};
}

bool WindowManager::isVulkanSupported() const {
  return glfwVulkanSupported() == GLFW_TRUE;
}

std::vector<const char*> WindowManager::getRequiredInstanceExtensions() const {
  uint32_t count;
  const char** extensions = glfwGetRequiredInstanceExtensions(&count);

  if (!extensions) {
    return {};
  }

  return std::vector<const char*>(extensions, extensions + count);
}

}  // namespace utility::window