#include <Container/utility/FrameSyncManager.h>

#include <stdexcept>

namespace utility {

FrameSyncManager::FrameSyncManager(VkDevice device, size_t framesInFlight)
    : device_(device), framesInFlight_(framesInFlight) {}

FrameSyncManager::~FrameSyncManager() { cleanup(); }

void FrameSyncManager::initialize() {
    imageAvailableSemaphores_.resize(framesInFlight_);
    renderFinishedSemaphores_.resize(framesInFlight_);
    inFlightFences_.resize(framesInFlight_);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < framesInFlight_; i++) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                              &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                              &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) !=
                VK_SUCCESS) {
            throw std::runtime_error(
                "failed to create synchronization objects for a frame!");
        }
    }
}

void FrameSyncManager::cleanup() {
    for (size_t i = 0; i < imageAvailableSemaphores_.size(); i++) {
        vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
    }
    for (size_t i = 0; i < renderFinishedSemaphores_.size(); i++) {
        vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
    }
    for (size_t i = 0; i < inFlightFences_.size(); i++) {
        vkDestroyFence(device_, inFlightFences_[i], nullptr);
    }

    imageAvailableSemaphores_.clear();
    renderFinishedSemaphores_.clear();
    inFlightFences_.clear();
}

VkSemaphore FrameSyncManager::imageAvailable(size_t frameIndex) const {
    return imageAvailableSemaphores_.at(frameIndex);
}

VkSemaphore FrameSyncManager::renderFinished(size_t frameIndex) const {
    return renderFinishedSemaphores_.at(frameIndex);
}

VkFence FrameSyncManager::fence(size_t frameIndex) const {
    return inFlightFences_.at(frameIndex);
}

void FrameSyncManager::waitForFrame(size_t frameIndex) const {
    vkWaitForFences(device_, 1, &inFlightFences_.at(frameIndex), VK_TRUE,
                   UINT64_MAX);
}

void FrameSyncManager::resetFence(size_t frameIndex) const {
    vkResetFences(device_, 1, &inFlightFences_.at(frameIndex));
}

}  // namespace utility

