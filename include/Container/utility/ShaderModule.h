#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "Container/common/CommonVulkan.h"

namespace utility::vulkan {

/**
 * @brief Create a Vulkan shader module from SPIR-V bytecode.
 *
 * @param device  Logical Vulkan device
 * @param code    SPIR-V bytecode (must be uint32_t aligned)
 * @return VkShaderModule
 *
 * @throws std::runtime_error on failure
 */
[[nodiscard]] inline VkShaderModule createShaderModule(VkDevice device,
                                         const std::vector<char>& code) {
  if (code.empty()) {
    throw std::runtime_error("Shader code is empty");
  }

  if (code.size() % 4 != 0) {
    throw std::runtime_error("Shader code size is not a multiple of 4");
  }

  VkShaderModuleCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = code.size();
  createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

  VkShaderModule shaderModule = VK_NULL_HANDLE;

  VkResult result =
      vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule);

  if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to create shader module");
  }

  return shaderModule;
}

}  // namespace utility::vulkan
