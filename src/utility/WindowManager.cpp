#include <Container/utility/WindowManager.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace utility::window {

namespace {
constexpr int kDefaultClientApi = GLFW_NO_API;
}

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

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_) != 0;
}

void Window::pollEvents() const {
    glfwPollEvents();
}

void Window::waitForEvents() const {
    glfwWaitEvents();
}

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

void Window::getFramebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(window_, &width, &height);
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("failed to create window surface!");
    }

    return surface;
}

void Window::FramebufferResizeThunk(GLFWwindow* window, int width, int height) {
    auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->resizeCallback_) {
        self->resizeCallback_(width, height);
    }
}

WindowManager::WindowManager() {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("failed to initialize GLFW!");
    }

    initialized_ = true;
    glfwWindowHint(GLFW_CLIENT_API, kDefaultClientApi);
}

WindowManager::~WindowManager() {
    if (initialized_) {
        glfwTerminate();
    }
}

std::unique_ptr<Window> WindowManager::createWindow(uint32_t width, uint32_t height,
                                                    std::string_view title) const {
    if (!initialized_) {
        throw std::runtime_error("GLFW not initialized before creating a window!");
    }

    GLFWwindow* window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height),
                                          std::string(title).c_str(), nullptr, nullptr);
    if (!window) {
        throw std::runtime_error("failed to create GLFW window!");
    }

    return std::make_unique<Window>(window);
}

}  // namespace utility::window

