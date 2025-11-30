#pragma once

#include <vector>

#include <vulkan/vulkan.h>

namespace utility {

// Manages per-frame synchronization objects for swap chain image acquisition
// and rendering completion.
class FrameSyncManager {
public:
    FrameSyncManager(VkDevice device, size_t framesInFlight);
    ~FrameSyncManager();

    FrameSyncManager(const FrameSyncManager&) = delete;
    FrameSyncManager& operator=(const FrameSyncManager&) = delete;

    void initialize(size_t swapChainImageCount);
    void cleanup();

    [[nodiscard]] VkSemaphore imageAvailable(size_t frameIndex) const;
    [[nodiscard]] VkSemaphore renderFinishedForImage(size_t imageIndex) const;
    [[nodiscard]] VkFence fence(size_t frameIndex) const;

    void waitForFrame(size_t frameIndex) const;
    void resetFence(size_t frameIndex) const;

    void recreateRenderFinishedSemaphores(size_t swapChainImageCount);

    [[nodiscard]] size_t framesInFlight() const { return framesInFlight_; }

private:
    void destroyRenderFinishedSemaphores();

    VkDevice device_{VK_NULL_HANDLE};
    size_t framesInFlight_{};
    size_t swapChainImageCount_{0};

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
};

}  // namespace utility

