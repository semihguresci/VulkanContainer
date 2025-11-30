#pragma once

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <optional>
#include <vector>

namespace utility {

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class SwapChainManager {
public:
    SwapChainManager(GLFWwindow* window, VkPhysicalDevice physicalDevice,
                     VkDevice device, VkSurfaceKHR surface);
    ~SwapChainManager();

    SwapChainManager(const SwapChainManager&) = delete;
    SwapChainManager& operator=(const SwapChainManager&) = delete;
    SwapChainManager(SwapChainManager&&) = delete;
    SwapChainManager& operator=(SwapChainManager&&) = delete;

    void initialize();
    void recreate(VkRenderPass renderPass);
    void cleanup();
    void createFramebuffers(VkRenderPass renderPass);
    [[nodiscard]] VkResult present(VkQueue presentQueue, uint32_t imageIndex,
                                  VkSemaphore waitSemaphore) const;

    [[nodiscard]] VkSwapchainKHR swapChain() const { return swapChain_; }
    [[nodiscard]] const std::vector<VkImageView>& imageViews() const {
        return swapChainImageViews_;
    }
    [[nodiscard]] size_t imageCount() const { return swapChainImages_.size(); }
    [[nodiscard]] const std::vector<VkFramebuffer>& framebuffers() const {
        return swapChainFramebuffers_;
    }
    [[nodiscard]] VkFormat imageFormat() const { return swapChainImageFormat_; }
    [[nodiscard]] VkExtent2D extent() const { return swapChainExtent_; }

    static SwapChainSupportDetails QuerySwapChainSupport(
        VkPhysicalDevice device, VkSurfaceKHR surface);
    static QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device,
                                                VkSurfaceKHR surface);

private:
    void createSwapChain();
    void createImageViews();
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(
        const VkSurfaceCapabilitiesKHR& capabilities);

    GLFWwindow* window_;
    VkPhysicalDevice physicalDevice_;
    VkDevice device_;
    VkSurfaceKHR surface_;

    VkSwapchainKHR swapChain_{VK_NULL_HANDLE};
    std::vector<VkImage> swapChainImages_;
    VkFormat swapChainImageFormat_{};
    VkExtent2D swapChainExtent_{};
    std::vector<VkImageView> swapChainImageViews_;
    std::vector<VkFramebuffer> swapChainFramebuffers_;
};

}  // namespace utility

