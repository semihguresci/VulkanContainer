#pragma once

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <functional>
#include <memory>
#include <string_view>

namespace utility::window {

class Window {
public:
    using ResizeCallback = std::function<void(int, int)>;

    explicit Window(GLFWwindow* window);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] GLFWwindow* handle() const { return window_; }
    [[nodiscard]] bool shouldClose() const;
    void pollEvents() const;
    void waitForEvents() const;

    void setFramebufferResizeCallback(ResizeCallback callback);
    void setUserPointer(void* userPointer);
    void getFramebufferSize(int& width, int& height) const;

    [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;

private:
    static void FramebufferResizeThunk(GLFWwindow* window, int width, int height);

    GLFWwindow* window_{};
    ResizeCallback resizeCallback_{};
};

class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;
    WindowManager(WindowManager&&) = delete;
    WindowManager& operator=(WindowManager&&) = delete;

    [[nodiscard]] std::unique_ptr<Window> createWindow(uint32_t width, uint32_t height,
                                                       std::string_view title) const;

private:
    bool initialized_ = false;
};

}  // namespace utility::window

