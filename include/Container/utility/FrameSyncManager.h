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

    void initialize();
    void cleanup();

    [[nodiscard]] VkSemaphore imageAvailable(size_t frameIndex) const;
    [[nodiscard]] VkSemaphore renderFinished(size_t frameIndex) const;
    [[nodiscard]] VkFence fence(size_t frameIndex) const;

    void waitForFrame(size_t frameIndex) const;
    void resetFence(size_t frameIndex) const;

    [[nodiscard]] size_t framesInFlight() const { return framesInFlight_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    size_t framesInFlight_{};

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
};

}  // namespace utility

