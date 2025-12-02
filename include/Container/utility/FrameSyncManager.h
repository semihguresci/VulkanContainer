#pragma once

#include <vector>

#include <vulkan/vulkan.hpp>

namespace utility {

// Manages per-frame synchronization objects for swap chain image acquisition
// and rendering completion.
class FrameSyncManager {
public:
    FrameSyncManager(vk::Device device, size_t framesInFlight);
    ~FrameSyncManager();

    FrameSyncManager(const FrameSyncManager&) = delete;
    FrameSyncManager& operator=(const FrameSyncManager&) = delete;

    void initialize(size_t swapChainImageCount);
    void cleanup();

    [[nodiscard]] vk::Semaphore imageAvailable(size_t frameIndex) const;
    [[nodiscard]] vk::Semaphore renderFinishedForImage(size_t imageIndex) const;
    [[nodiscard]] vk::Fence fence(size_t frameIndex) const;

    void waitForFrame(size_t frameIndex) const;
    void resetFence(size_t frameIndex) const;

    void recreateRenderFinishedSemaphores(size_t swapChainImageCount);

    [[nodiscard]] size_t framesInFlight() const { return framesInFlight_; }

private:
    void destroyRenderFinishedSemaphores();

    vk::Device device_{};
    size_t framesInFlight_{};
    size_t swapChainImageCount_{0};

    std::vector<vk::UniqueSemaphore> imageAvailableSemaphores_;
    std::vector<vk::UniqueSemaphore> renderFinishedSemaphores_;
    std::vector<vk::UniqueFence> inFlightFences_;
};

}  // namespace utility

