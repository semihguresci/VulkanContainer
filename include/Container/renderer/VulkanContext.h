#pragma once

#include "Container/app/AppConfig.h"
#include "Container/renderer/VulkanContextInitializer.h"

namespace container::renderer {

// RAII owner of a VulkanContextResult.
// Destroys the debug messenger, surface, and instance wrapper in the
// correct dependency order on destruction.
class VulkanContext {
 public:
  VulkanContext(VulkanContextResult result, bool enableValidationLayers);
  ~VulkanContext();

  VulkanContext(const VulkanContext&)            = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;
  VulkanContext(VulkanContext&&)                 = delete;
  VulkanContext& operator=(VulkanContext&&)      = delete;

  // Access the underlying context data (borrowed reference).
  VulkanContextResult&       result()       { return result_; }
  const VulkanContextResult& result() const { return result_; }

 private:
  VulkanContextResult result_;
  bool                enableValidationLayers_;
};

}  // namespace container::renderer
