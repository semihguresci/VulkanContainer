#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/pipeline/PipelineTypes.h"
#include "Container/renderer/shadow/ShadowCascadeFramePassRecorder.h"

#include <gtest/gtest.h>

#include <array>

namespace {

using container::gpu::kShadowCascadeCount;
using container::renderer::FrameRecordParams;
using container::renderer::GraphicsPipelines;
using container::renderer::PipelineRegistry;
using container::renderer::ShadowCascadeFramePassRecorder;
using container::renderer::buildGraphicsPipelineHandleRegistry;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

[[nodiscard]] FrameRecordParams recordableShadowParams(
    std::array<VkFramebuffer, kShadowCascadeCount> &framebuffers,
    const PipelineRegistry *pipelineHandles) {
  framebuffers[0] = fakeHandle<VkFramebuffer>(0x4);

  FrameRecordParams params{};
  params.renderPasses.shadow = fakeHandle<VkRenderPass>(0x1);
  params.registries.pipelineHandles = pipelineHandles;
  params.descriptors.shadowDescriptorSet = fakeHandle<VkDescriptorSet>(0x3);
  params.shadows.shadowFramebuffers = framebuffers.data();
  return params;
}

} // namespace

TEST(ShadowCascadeFramePassRecorderTests,
     CanRecordCascadeRequiresShadowPassResources) {
  std::array<VkFramebuffer, kShadowCascadeCount> framebuffers{};
  GraphicsPipelines pipelines;
  pipelines.shadowDepth = fakeHandle<VkPipeline>(0x2);
  const auto pipelineRegistry = buildGraphicsPipelineHandleRegistry(pipelines);
  const FrameRecordParams params =
      recordableShadowParams(framebuffers, pipelineRegistry.get());
  const ShadowCascadeFramePassRecorder recorder;

  EXPECT_TRUE(recorder.canRecordCascade(params, 0u));
  EXPECT_FALSE(recorder.canRecordCascade(params, kShadowCascadeCount));
}

TEST(ShadowCascadeFramePassRecorderTests,
     CanRecordCascadeRejectsMissingFramebuffer) {
  std::array<VkFramebuffer, kShadowCascadeCount> framebuffers{};
  GraphicsPipelines pipelines;
  pipelines.shadowDepth = fakeHandle<VkPipeline>(0x2);
  const auto pipelineRegistry = buildGraphicsPipelineHandleRegistry(pipelines);
  FrameRecordParams params =
      recordableShadowParams(framebuffers, pipelineRegistry.get());
  framebuffers[0] = VK_NULL_HANDLE;
  const ShadowCascadeFramePassRecorder recorder;

  EXPECT_FALSE(recorder.canRecordCascade(params, 0u));
}

TEST(ShadowCascadeFramePassRecorderTests,
     CanRecordCascadeRejectsMissingPipeline) {
  std::array<VkFramebuffer, kShadowCascadeCount> framebuffers{};
  FrameRecordParams params = recordableShadowParams(framebuffers, nullptr);
  const ShadowCascadeFramePassRecorder recorder;

  EXPECT_FALSE(recorder.canRecordCascade(params, 0u));
}
