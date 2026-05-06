#include "Container/renderer/platform/VulkanContext.h"
#include "Container/utility/DebugMessengerExt.h"

namespace container::renderer {

VulkanContext::VulkanContext(VulkanContextResult result,
                             bool enableValidationLayers)
    : result_(std::move(result))
    , enableValidationLayers_(enableValidationLayers) {}

VulkanContext::~VulkanContext() {
  // Device must already be destroyed before this runs (caller's responsibility).
  if (enableValidationLayers_ &&
      result_.debugMessenger != VK_NULL_HANDLE) {
    container::gpu::DestroyDebugUtilsMessengerEXT(
        result_.instance, result_.debugMessenger, nullptr);
    result_.debugMessenger = VK_NULL_HANDLE;
  }

  if (result_.surface != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(result_.instance, result_.surface, nullptr);
    result_.surface = VK_NULL_HANDLE;
  }

  result_.instanceWrapper.reset();
}

}  // namespace container::renderer
