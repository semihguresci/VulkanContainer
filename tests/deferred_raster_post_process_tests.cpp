#include "Container/renderer/DeferredRasterPostProcess.h"

#include "Container/utility/GuiManager.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::buildDeferredPostProcessFrameState;
using container::renderer::buildDeferredPostProcessPushConstants;
using container::renderer::DeferredPostProcessFrameInputs;
using container::renderer::DeferredPostProcessPushConstantInputs;
using container::renderer::resolvePostProcessExposure;

TEST(DeferredRasterPostProcessTests, MapsExposureCameraBloomAndOitState) {
  container::gpu::ExposureSettings exposure{};
  exposure.mode = container::gpu::kExposureModeAuto;
  exposure.targetLuminance = 0.22f;
  exposure.minExposure = 0.05f;
  exposure.maxExposure = 6.0f;
  exposure.adaptationRate = 2.5f;

  const auto pc =
      buildDeferredPostProcessPushConstants({.outputMode = 12u,
                                             .bloomEnabled = true,
                                             .bloomIntensity = 0.75f,
                                             .exposureSettings = exposure,
                                             .resolvedExposure = 1.4f,
                                             .cameraNear = 0.25f,
                                             .cameraFar = 500.0f,
                                             .oitEnabled = true});

  EXPECT_EQ(pc.outputMode, 12u);
  EXPECT_EQ(pc.bloomEnabled, 1u);
  EXPECT_FLOAT_EQ(pc.bloomIntensity, 0.75f);
  EXPECT_FLOAT_EQ(pc.exposure, 1.4f);
  EXPECT_FLOAT_EQ(pc.cameraNear, 0.25f);
  EXPECT_FLOAT_EQ(pc.cameraFar, 500.0f);
  EXPECT_EQ(pc.oitEnabled, 1u);
  EXPECT_EQ(pc.exposureMode, container::gpu::kExposureModeAuto);
  EXPECT_FLOAT_EQ(pc.targetLuminance, 0.22f);
  EXPECT_FLOAT_EQ(pc.minExposure, 0.05f);
  EXPECT_FLOAT_EQ(pc.maxExposure, 6.0f);
  EXPECT_FLOAT_EQ(pc.adaptationRate, 2.5f);
}

TEST(DeferredRasterPostProcessTests, KeepsInactiveTileCullFallbackCompact) {
  const auto pc =
      buildDeferredPostProcessPushConstants({.tileCullActive = false,
                                             .tileCountX = 42u,
                                             .totalLights = 13u,
                                             .depthSliceCount = 9u});

  EXPECT_EQ(pc.tileCountX, 1u);
  EXPECT_EQ(pc.totalLights, 0u);
  EXPECT_EQ(pc.depthSliceCount, 1u);
}

TEST(DeferredRasterPostProcessTests, ManualExposureBypassesFallbackClamp) {
  container::gpu::ExposureSettings exposure{};
  exposure.mode = container::gpu::kExposureModeManual;
  exposure.manualExposure = 0.25f;
  exposure.minExposure = 1.0f;
  exposure.maxExposure = 2.0f;

  EXPECT_FLOAT_EQ(resolvePostProcessExposure(exposure), 0.25f);
}

TEST(DeferredRasterPostProcessTests, AutoExposureFallbackClampsToRange) {
  container::gpu::ExposureSettings exposure{};
  exposure.mode = container::gpu::kExposureModeAuto;
  exposure.minExposure = 1.0f;
  exposure.maxExposure = 2.0f;

  exposure.manualExposure = 0.25f;
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(exposure), 1.0f);

  exposure.manualExposure = 1.5f;
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(exposure), 1.5f);

  exposure.manualExposure = 3.0f;
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(exposure), 2.0f);
}

TEST(DeferredRasterPostProcessTests, MapsActiveTileCullMetadata) {
  const auto pc =
      buildDeferredPostProcessPushConstants({.tileCullActive = true,
                                             .tileCountX = 42u,
                                             .totalLights = 13u,
                                             .depthSliceCount = 9u});

  EXPECT_EQ(pc.tileCountX, 42u);
  EXPECT_EQ(pc.totalLights, 13u);
  EXPECT_EQ(pc.depthSliceCount, 9u);
}

TEST(DeferredRasterPostProcessTests, IncludesShadowSplitsOnlyWhenRequested) {
  container::gpu::ShadowData shadowData{};
  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
    shadowData.cascades[i].splitDepth = 10.0f + static_cast<float>(i);
  }

  const auto omitted = buildDeferredPostProcessPushConstants(
      {.includeShadowCascadeSplits = false, .shadowData = &shadowData});
  const auto included = buildDeferredPostProcessPushConstants(
      {.includeShadowCascadeSplits = true, .shadowData = &shadowData});

  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
    EXPECT_FLOAT_EQ(omitted.cascadeSplits[i], 0.0f);
    EXPECT_FLOAT_EQ(included.cascadeSplits[i], 10.0f + static_cast<float>(i));
  }
}

TEST(DeferredRasterPostProcessTests, FrameStateAppliesDisplayModePolicy) {
  DeferredPostProcessFrameInputs inputs{};
  inputs.displayMode = container::ui::GBufferViewMode::Overview;
  inputs.bloomPassActive = true;
  inputs.bloomReady = true;
  inputs.bloomEnabled = true;
  inputs.bloomIntensity = 0.5f;
  inputs.tileCullPassActive = true;
  inputs.tiledLightingReady = true;
  inputs.framebufferWidth = 65u;
  inputs.pointLightCount = 7u;

  const auto overviewState = buildDeferredPostProcessFrameState(inputs);
  EXPECT_FALSE(overviewState.bloomActive);
  EXPECT_FALSE(overviewState.tileCullActive);
  EXPECT_EQ(overviewState.pushConstants.bloomEnabled, 0u);
  EXPECT_EQ(overviewState.pushConstants.tileCountX, 1u);
  EXPECT_EQ(overviewState.pushConstants.totalLights, 0u);

  inputs.displayMode = container::ui::GBufferViewMode::Lit;
  const auto litState = buildDeferredPostProcessFrameState(inputs);
  EXPECT_TRUE(litState.bloomActive);
  EXPECT_TRUE(litState.tileCullActive);
  EXPECT_EQ(litState.pushConstants.bloomEnabled, 1u);
  EXPECT_EQ(litState.pushConstants.tileCountX, 5u);
  EXPECT_EQ(litState.pushConstants.totalLights, 7u);
  EXPECT_EQ(litState.pushConstants.depthSliceCount,
            container::gpu::kClusterDepthSlices);
}

TEST(DeferredRasterPostProcessTests, FrameStateUsesShadowDebugModesForSplits) {
  container::gpu::ShadowData shadowData{};
  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
    shadowData.cascades[i].splitDepth = 20.0f + static_cast<float>(i);
  }

  DeferredPostProcessFrameInputs inputs{};
  inputs.displayMode = container::ui::GBufferViewMode::Lit;
  inputs.shadowData = &shadowData;
  const auto litState = buildDeferredPostProcessFrameState(inputs);

  inputs.displayMode = container::ui::GBufferViewMode::ShadowTexelDensity;
  const auto shadowDebugState = buildDeferredPostProcessFrameState(inputs);

  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
    EXPECT_FLOAT_EQ(litState.pushConstants.cascadeSplits[i], 0.0f);
    EXPECT_FLOAT_EQ(shadowDebugState.pushConstants.cascadeSplits[i],
                    20.0f + static_cast<float>(i));
  }
}

} // namespace
