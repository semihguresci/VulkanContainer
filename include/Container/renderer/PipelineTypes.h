#pragma once

#include "Container/common/CommonVulkan.h"

namespace container::renderer {

// All Vulkan pipeline layouts created by GraphicsPipelineBuilder.
struct PipelineLayouts {
  VkPipelineLayout scene{VK_NULL_HANDLE};
  VkPipelineLayout transparent{VK_NULL_HANDLE};
  VkPipelineLayout lighting{VK_NULL_HANDLE};
  VkPipelineLayout tiledLighting{VK_NULL_HANDLE};
  VkPipelineLayout shadow{VK_NULL_HANDLE};
  VkPipelineLayout postProcess{VK_NULL_HANDLE};
  VkPipelineLayout wireframe{VK_NULL_HANDLE};
  VkPipelineLayout normalValidation{VK_NULL_HANDLE};
  VkPipelineLayout surfaceNormal{VK_NULL_HANDLE};
};

// All Vulkan pipelines created by GraphicsPipelineBuilder.
struct GraphicsPipelines {
  // Depth/G-buffer/shadow pipelines are split by culling mode so double-sided
  // materials can disable culling, while mirrored transforms can invert the
  // cull side after glTF triangle winding has been repaired at load time.
  VkPipeline depthPrepass{VK_NULL_HANDLE};
  VkPipeline depthPrepassFrontCull{VK_NULL_HANDLE};
  VkPipeline depthPrepassNoCull{VK_NULL_HANDLE};
  VkPipeline gBuffer{VK_NULL_HANDLE};
  VkPipeline gBufferFrontCull{VK_NULL_HANDLE};
  VkPipeline gBufferNoCull{VK_NULL_HANDLE};
  VkPipeline shadowDepth{VK_NULL_HANDLE};
  VkPipeline shadowDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline shadowDepthNoCull{VK_NULL_HANDLE};
  VkPipeline directionalLight{VK_NULL_HANDLE};
  VkPipeline stencilVolume{VK_NULL_HANDLE};
  VkPipeline pointLight{VK_NULL_HANDLE};
  VkPipeline pointLightStencilDebug{VK_NULL_HANDLE};
  VkPipeline tiledPointLight{VK_NULL_HANDLE};
  VkPipeline transparent{VK_NULL_HANDLE};
  VkPipeline transparentFrontCull{VK_NULL_HANDLE};
  VkPipeline transparentNoCull{VK_NULL_HANDLE};
  VkPipeline postProcess{VK_NULL_HANDLE};
  VkPipeline geometryDebug{VK_NULL_HANDLE};
  VkPipeline normalValidation{VK_NULL_HANDLE};
  VkPipeline normalValidationFrontCull{VK_NULL_HANDLE};
  VkPipeline normalValidationNoCull{VK_NULL_HANDLE};
  VkPipeline wireframeDepth{VK_NULL_HANDLE};
  VkPipeline wireframeDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline wireframeNoDepth{VK_NULL_HANDLE};
  VkPipeline wireframeNoDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline surfaceNormalLine{VK_NULL_HANDLE};
  VkPipeline objectNormalDebug{VK_NULL_HANDLE};
  VkPipeline objectNormalDebugFrontCull{VK_NULL_HANDLE};
  VkPipeline objectNormalDebugNoCull{VK_NULL_HANDLE};
  VkPipeline lightGizmo{VK_NULL_HANDLE};
};

// Input descriptor-set layouts required to build the pipeline layouts.
struct PipelineDescriptorLayouts {
  VkDescriptorSetLayout scene{VK_NULL_HANDLE};
  VkDescriptorSetLayout lighting{VK_NULL_HANDLE};
  VkDescriptorSetLayout light{VK_NULL_HANDLE};
  VkDescriptorSetLayout tiled{VK_NULL_HANDLE};
  VkDescriptorSetLayout shadow{VK_NULL_HANDLE};
  VkDescriptorSetLayout postProcess{VK_NULL_HANDLE};
  VkDescriptorSetLayout oit{VK_NULL_HANDLE};
};

// Input render passes required to create the pipelines.
struct PipelineRenderPasses {
  VkRenderPass depthPrepass{VK_NULL_HANDLE};
  VkRenderPass gBuffer{VK_NULL_HANDLE};
  VkRenderPass shadow{VK_NULL_HANDLE};
  VkRenderPass lighting{VK_NULL_HANDLE};
  VkRenderPass postProcess{VK_NULL_HANDLE};
};

// All outputs returned by a single build() call.
struct PipelineBuildResult {
  PipelineLayouts   layouts;
  GraphicsPipelines pipelines;
};

}  // namespace container::renderer
