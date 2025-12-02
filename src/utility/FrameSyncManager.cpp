#include <Container/utility/FrameSyncManager.h>

#include <stdexcept>

namespace utility {

FrameSyncManager::FrameSyncManager(vk::Device device, size_t framesInFlight)
    : device_(device), framesInFlight_(framesInFlight) {}

FrameSyncManager::~FrameSyncManager() { cleanup(); }

void FrameSyncManager::initialize(size_t swapChainImageCount) {
    swapChainImageCount_ = swapChainImageCount;
    imageAvailableSemaphores_.resize(framesInFlight_);
    renderFinishedSemaphores_.resize(swapChainImageCount_);
    inFlightFences_.resize(framesInFlight_);

    const vk::SemaphoreCreateInfo semaphoreInfo{};
    const vk::FenceCreateInfo fenceInfo{vk::FenceCreateFlagBits::eSignaled};

    for (size_t i = 0; i < framesInFlight_; i++) {
        imageAvailableSemaphores_[i] = device_.createSemaphoreUnique(semaphoreInfo);
        inFlightFences_[i] = device_.createFenceUnique(fenceInfo);
    }

    for (size_t i = 0; i < swapChainImageCount_; i++) {
        renderFinishedSemaphores_[i] =
            device_.createSemaphoreUnique(semaphoreInfo);
    }
}

void FrameSyncManager::cleanup() {
    destroyRenderFinishedSemaphores();

    imageAvailableSemaphores_.clear();
    inFlightFences_.clear();

    device_.waitIdle();
}

vk::Semaphore FrameSyncManager::imageAvailable(size_t frameIndex) const {
    return imageAvailableSemaphores_.at(frameIndex).get();
}

vk::Semaphore FrameSyncManager::renderFinishedForImage(
    size_t imageIndex) const {
    return renderFinishedSemaphores_.at(imageIndex).get();
}

vk::Fence FrameSyncManager::fence(size_t frameIndex) const {
    return inFlightFences_.at(frameIndex).get();
}

void FrameSyncManager::waitForFrame(size_t frameIndex) const {
    device_.waitForFences(inFlightFences_.at(frameIndex).get(), VK_TRUE,
                          UINT64_MAX);
}

void FrameSyncManager::resetFence(size_t frameIndex) const {
    device_.resetFences(inFlightFences_.at(frameIndex).get());
}

void FrameSyncManager::recreateRenderFinishedSemaphores(
    size_t swapChainImageCount) {
    destroyRenderFinishedSemaphores();

    swapChainImageCount_ = swapChainImageCount;
    renderFinishedSemaphores_.resize(swapChainImageCount_);

    const vk::SemaphoreCreateInfo semaphoreInfo{};

    for (size_t i = 0; i < swapChainImageCount_; i++) {
        renderFinishedSemaphores_[i] =
            device_.createSemaphoreUnique(semaphoreInfo);
    }
}

void FrameSyncManager::destroyRenderFinishedSemaphores() {
    renderFinishedSemaphores_.clear();
}

}  // namespace utility

