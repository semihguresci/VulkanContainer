#pragma once

#include "Container/renderer/core/FrameRecorder.h"

#include <string_view>

namespace container::renderer {

enum class ShadowPipelineId {
  Depth,
  DepthFrontCull,
  DepthNoCull,
};

[[nodiscard]] inline std::string_view shadowPipelineName(
    ShadowPipelineId id) {
  switch (id) {
  case ShadowPipelineId::Depth:
    return "shadow-depth";
  case ShadowPipelineId::DepthFrontCull:
    return "shadow-depth-front-cull";
  case ShadowPipelineId::DepthNoCull:
    return "shadow-depth-no-cull";
  }
  return {};
}

enum class ShadowPipelineLayoutId {
  Shadow,
};

[[nodiscard]] inline std::string_view shadowPipelineLayoutName(
    ShadowPipelineLayoutId id) {
  switch (id) {
  case ShadowPipelineLayoutId::Shadow:
    return "shadow";
  }
  return {};
}

[[nodiscard]] inline VkPipelineLayout shadowPipelineLayout(
    const FrameRecordParams &params, ShadowPipelineLayoutId id) {
  return params.pipelineLayout(RenderTechniqueId::DeferredRaster,
                               shadowPipelineLayoutName(id));
}

[[nodiscard]] inline bool shadowPipelineLayoutReady(
    const FrameRecordParams &params, ShadowPipelineLayoutId id) {
  return shadowPipelineLayout(params, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline VkPipeline shadowPipelineHandle(
    const FrameRecordParams &params, ShadowPipelineId id) {
  return params.pipelineHandle(RenderTechniqueId::DeferredRaster,
                               shadowPipelineName(id));
}

[[nodiscard]] inline bool shadowPipelineReady(
    const FrameRecordParams &params, ShadowPipelineId id) {
  return shadowPipelineHandle(params, id) != VK_NULL_HANDLE;
}

} // namespace container::renderer
