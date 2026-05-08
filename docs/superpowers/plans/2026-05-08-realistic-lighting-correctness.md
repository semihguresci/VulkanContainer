# Realistic Lighting Correctness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make shadow truth stable and physically plausible before editor-light controls or image-polish work.

**Architecture:** Use the existing deferred renderer seams. `ShadowPassDrawPlanner` owns caster route decisions for directional and local shadow passes, `ShadowManager` owns directional cascade and local-light shadow data, shared Slang includes own shadow sampling, and existing validation fixtures own image-level probes.

**Tech Stack:** C++20, Vulkan, Slang shaders, GLM, GoogleTest, CMake/CTest, JSON visual-regression fixtures.

---

## File Structure

- Modify `tests/renderer/shadow/shadow_pass_draw_planner_tests.cpp`: add route tests proving all shadow casters use no-cull routing regardless of winding or surface class.
- Modify `src/renderer/shadow/ShadowPassDrawPlanner.cpp`: keep one shared caster pipeline policy and feed every directional/local caster route through it.
- Modify `tests/validation/rendering_convention_tests.cpp`: add CPU and source-contract tests for reverse-Z shadow comparison, positive shadow viewport mapping, receiver bias, and local-shadow contact behavior.
- Modify `src/renderer/shadow/ShadowPassRecorder.cpp`: keep positive-height shadow viewport, zero-offset scissor, and dynamic reverse-Z raster bias.
- Modify `src/renderer/pipeline/GraphicsPipelineBuilder.cpp`: keep shadow depth pipeline front-face and culling conventions aligned with glTF and route-specific pipelines.
- Modify `shaders/shadow_common.slang`: make directional shadow sampling use reverse-Z compare depth, bounded receiver bias, and normal-offset receiver bias only.
- Modify `shaders/local_shadow_common.slang`: make local shadow-map sampling match directional shadow rules and avoid screen-derivative receiver-plane bias inside divergent light loops.
- Modify `shaders/screen_space_light_shadow_common.slang`: keep screen-space point-light contact visibility explicitly separate from true local shadow maps.
- Modify `tests/fixtures/rendering/realistic_visual_regression.fixtures.json`: add or promote deterministic shadow correctness fixtures only after numeric probes are present.
- Modify `tests/validation/realistic_rendering_validation_tests.cpp`: validate fixture metadata, probe coverage, and active/planned status rules.
- Optional after numeric approval: add golden PNGs under `tests/visual-regression/golden/windows-nvidia/`.

Keep unrelated staged and unstaged work out of every commit. Use path-limited `git add`.

---

### Task 1: Lock Shadow Caster Routing With Tests

**Files:**
- Modify: `tests/renderer/shadow/shadow_pass_draw_planner_tests.cpp`
- Modify: `src/renderer/shadow/ShadowPassDrawPlanner.cpp`

- [ ] **Step 1: Write the failing route test**

Append this test to `tests/renderer/shadow/shadow_pass_draw_planner_tests.cpp`:

```cpp
TEST(ShadowPassDrawPlannerTests,
     EveryVisibleCasterRouteUsesNoCullSoNormalsAndWindingDoNotSuppressShadows) {
  const auto sceneSingleSided = drawCommands(21u);
  const auto sceneWindingFlipped = drawCommands(22u);
  const auto sceneDoubleSided = drawCommands(23u);
  const auto bimSingleSided = drawCommands(24u);
  const auto bimWindingFlipped = drawCommands(25u);
  const auto bimDoubleSided = drawCommands(26u);

  const auto plan = buildShadowPassDrawPlan(
      {.sceneGeometryReady = true,
       .bimGeometryReady = true,
       .sceneGpuCullActive = true,
       .bimGpuFilteredMeshActive = true,
       .sceneDraws = {.singleSided = &sceneSingleSided,
                      .windingFlipped = &sceneWindingFlipped,
                      .doubleSided = &sceneDoubleSided},
       .bimDraws = {.singleSided = &bimSingleSided,
                    .windingFlipped = &bimWindingFlipped,
                    .doubleSided = &bimDoubleSided}});

  ASSERT_TRUE(plan.sceneGpuRoute.active);
  EXPECT_EQ(plan.sceneGpuRoute.pipeline, ShadowPassPipeline::NoCull);

  ASSERT_EQ(plan.sceneCpuRouteCount, 2u);
  for (uint32_t routeIndex = 0u; routeIndex < plan.sceneCpuRouteCount;
       ++routeIndex) {
    EXPECT_EQ(plan.sceneCpuRoutes[routeIndex].pipeline,
              ShadowPassPipeline::NoCull);
  }

  ASSERT_EQ(plan.bimGpuRouteCount, 3u);
  for (uint32_t routeIndex = 0u; routeIndex < plan.bimGpuRouteCount;
       ++routeIndex) {
    EXPECT_EQ(plan.bimGpuRoutes[routeIndex].pipeline,
              ShadowPassPipeline::NoCull);
  }

  ASSERT_EQ(plan.bimCpuRouteCount, 3u);
  for (uint32_t routeIndex = 0u; routeIndex < plan.bimCpuRouteCount;
       ++routeIndex) {
    EXPECT_EQ(plan.bimCpuRoutes[routeIndex].pipeline,
              ShadowPassPipeline::NoCull);
  }
}
```

- [ ] **Step 2: Run the test and verify the failure**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R shadow_pass_draw_planner_tests --output-on-failure"
```

Expected on the broken baseline: `EveryVisibleCasterRouteUsesNoCullSoNormalsAndWindingDoNotSuppressShadows` fails because at least one route uses `Primary` or `FrontCull`. If it already passes, keep the test as regression coverage and continue.

- [ ] **Step 3: Implement the shared caster policy**

In `src/renderer/shadow/ShadowPassDrawPlanner.cpp`, ensure the anonymous namespace has this policy:

```cpp
// Shadow-map casters are visibility geometry. Winding and normal direction must
// not decide whether a surface blocks light.
constexpr ShadowPassPipeline kShadowCasterPipeline =
    ShadowPassPipeline::NoCull;
```

Ensure every call to `appendCpuRoute`, `appendBimGpuRoute`, and the scene GPU route uses `kShadowCasterPipeline`:

```cpp
plan.sceneGpuRoute = {.active = inputs_.sceneGpuCullActive,
                      .pipeline = kShadowCasterPipeline};
appendCpuRoute(plan, true, kShadowCasterPipeline,
               inputs_.sceneDraws.singleSided);
appendCpuRoute(plan, true, kShadowCasterPipeline,
               inputs_.sceneDraws.windingFlipped);
appendCpuRoute(plan, true, kShadowCasterPipeline,
               inputs_.sceneDraws.doubleSided);
appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueSingleSided,
                  kShadowCasterPipeline);
appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueWindingFlipped,
                  kShadowCasterPipeline);
appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueDoubleSided,
                  kShadowCasterPipeline);
```

- [ ] **Step 4: Re-run the route tests**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R shadow_pass_draw_planner_tests --output-on-failure"
```

Expected: all `shadow_pass_draw_planner_tests` pass.

- [ ] **Step 5: Commit route coverage**

Run:

```powershell
git add -- tests/renderer/shadow/shadow_pass_draw_planner_tests.cpp src/renderer/shadow/ShadowPassDrawPlanner.cpp
git commit -m "fix: route shadow casters through no-cull policy"
```

---

### Task 2: Lock Reverse-Z Shadow Compare And Bias Contracts

**Files:**
- Modify: `tests/validation/rendering_convention_tests.cpp`
- Modify: `include/Container/utility/SceneData.h`
- Modify: `shaders/shadow_common.slang`
- Modify: `shaders/local_shadow_common.slang`

- [ ] **Step 1: Add CPU helpers for shadow compare contracts**

In `tests/validation/rendering_convention_tests.cpp`, add these helpers near the existing depth and attenuation helpers:

```cpp
float reverseZShadowCompareVisibility(float compareDepth, float storedDepth) {
  return compareDepth >= storedDepth ? 1.0f : 0.0f;
}

float receiverNormalBiasTexels(float normalDotSurfaceToLight,
                               float minBiasTexels,
                               float maxBiasTexels) {
  const float safeMin = std::max(minBiasTexels, 0.0f);
  const float safeMax = std::max(maxBiasTexels, safeMin);
  return std::lerp(safeMin, safeMax,
                   std::clamp(1.0f - normalDotSurfaceToLight, 0.0f, 1.0f));
}
```

- [ ] **Step 2: Add failing convention tests**

Append these tests near the other shadow convention tests in `tests/validation/rendering_convention_tests.cpp`:

```cpp
TEST(RenderingConventionTests,
     ReverseZShadowCompareKeepsClearDepthLitAndCloserBlockersShadowed) {
  constexpr float kClearDepth = 0.0f;
  constexpr float kReceiverDepth = 0.55f;
  constexpr float kCloserBlockerDepth = 0.80f;
  constexpr float kFartherDepth = 0.30f;

  EXPECT_FLOAT_EQ(reverseZShadowCompareVisibility(kReceiverDepth, kClearDepth),
                  1.0f);
  EXPECT_FLOAT_EQ(
      reverseZShadowCompareVisibility(kReceiverDepth, kCloserBlockerDepth),
      0.0f);
  EXPECT_FLOAT_EQ(reverseZShadowCompareVisibility(kReceiverDepth, kFartherDepth),
                  1.0f);
}

TEST(RenderingConventionTests,
     ReceiverNormalBiasChangesSamplePositionButNotCasterParticipation) {
  constexpr float kTexelSizeWorld = 0.015625f;
  constexpr float kFacingBiasTexels =
      receiverNormalBiasTexels(1.0f, 1.5f, 3.5f);
  constexpr float kGrazingBiasTexels =
      receiverNormalBiasTexels(0.0f, 1.5f, 3.5f);

  EXPECT_NEAR(kFacingBiasTexels * kTexelSizeWorld, 0.0234375f, 1e-7f);
  EXPECT_NEAR(kGrazingBiasTexels * kTexelSizeWorld, 0.0546875f, 1e-7f);
  EXPECT_FLOAT_EQ(reverseZShadowCompareVisibility(0.55f, 0.80f), 0.0f)
      << "Normals may offset receiver sampling but must not remove blocker depth.";
}
```

- [ ] **Step 3: Add shader-source contract tests**

Append this test to `tests/validation/rendering_convention_tests.cpp`:

```cpp
TEST(RenderingConventionTests,
     ShadowShadersUseNormalsOnlyForReceiverBiasAndShading) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");
  const std::string localShadow =
      readRepoTextFile("shaders/local_shadow_common.slang");
  const std::string shadowDrawPlanner =
      readRepoTextFile("src/renderer/shadow/ShadowPassDrawPlanner.cpp");

  EXPECT_TRUE(contains(shadowCommon, "OffsetShadowSamplePosition"));
  EXPECT_TRUE(contains(shadowCommon, "worldPosition + normal * normalBias"));
  EXPECT_TRUE(contains(shadowCommon, "shadowNDC.z + bias"));
  EXPECT_TRUE(contains(localShadow, "OffsetLocalShadowSamplePosition"));
  EXPECT_TRUE(contains(localShadow, "worldPosition +"));
  EXPECT_TRUE(contains(localShadow, "shadowNdc.z + bias"));
  EXPECT_TRUE(contains(shadowDrawPlanner, "ShadowPassPipeline::NoCull"));

  EXPECT_FALSE(contains(shadowCommon, "if (NdotL <= 0.0"));
  EXPECT_FALSE(contains(localShadow, "if (NdotL <= 0.0"));
}
```

- [ ] **Step 4: Run convention tests and verify the failure**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R rendering_convention_tests --output-on-failure"
```

Expected on the broken baseline: at least one new shadow convention test fails because compare direction, bias sign, or shader source contracts are wrong. If they pass, keep the tests and continue.

- [ ] **Step 5: Implement directional shadow sampling contract**

In `shaders/shadow_common.slang`, keep the directional sample path in this shape:

```slang
float3 samplePos = OffsetShadowSamplePosition(
    worldPosition, normal, lightDir, cascadeIndex, shadowData);
float shadow = SampleCascadeShadow(
    samplePos, normal, lightDir, cascadeIndex,
    shadowData, shadowAtlas, shadowSampler);
```

Inside `SampleCascadeShadow`, use a positive receiver bias with reverse-Z comparison:

```slang
float bias = ComputeSlopeScaledBias(normal, lightDir, shadowData) +
             ComputeReceiverPlaneDepthBias(shadowNDC, shadowData);
float compareDepth = shadowNDC.z + bias;

return SampleWeightedPcf(
    shadowUV, cascadeIndex, compareDepth, shadowData, shadowAtlas,
    shadowSampler);
```

Keep out-of-atlas samples lit:

```slang
if (!all(isfinite(shadowUV)) ||
    any(shadowUV < 0.0) || any(shadowUV > 1.0) ||
    shadowNDC.z < 0.0 || shadowNDC.z > 1.0)
{
    return 1.0;
}
```

- [ ] **Step 6: Implement local shadow sampling contract**

In `shaders/local_shadow_common.slang`, keep divergent local-light loops free of receiver-plane derivatives:

```slang
float LocalShadowReceiverPlaneBias(float3 shadowNdc,
                                   LocalShadowBuffer localShadow)
{
    return 0.0;
}
```

Use the same reverse-Z compare direction:

```slang
float bias = LocalShadowSlopeBias(normal, surfaceToLight, localShadow) +
    LocalShadowReceiverPlaneBias(shadowNdc, localShadow);
float shadow = SampleLocalShadowLayerPcf(
    layerIndex, shadowUv, shadowNdc.z + bias, filterRadiusTexels,
    localShadow, localShadowAtlas, localShadowSampler);
```

- [ ] **Step 7: Keep CPU bias defaults aligned with reverse-Z**

In `include/Container/utility/SceneData.h`, keep `ShadowSettings` defaults in this shape:

```cpp
struct ShadowSettings {
  float normalBiasMinTexels{1.5f};
  float normalBiasMaxTexels{3.5f};
  float slopeBiasScale{0.0012f};
  float receiverPlaneBiasScale{1.5f};
  float filterRadiusTexels{1.0f};
  float cascadeBlendFraction{0.1f};
  float constantDepthBias{0.00035f};
  float maxDepthBias{0.006f};
  float rasterConstantBias{-4.0f};
  float rasterSlopeBias{-1.5f};
  bool localContactVisibility{true};
};
```

- [ ] **Step 8: Re-run convention tests**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R rendering_convention_tests --output-on-failure"
```

Expected: all `rendering_convention_tests` pass.

- [ ] **Step 9: Commit shadow compare contracts**

Run:

```powershell
git add -- tests/validation/rendering_convention_tests.cpp include/Container/utility/SceneData.h shaders/shadow_common.slang shaders/local_shadow_common.slang
git commit -m "fix: align shadow sampling with reverse-z bias"
```

---

### Task 3: Lock Shadow Viewport, Winding, And Pipeline State

**Files:**
- Modify: `tests/validation/rendering_convention_tests.cpp`
- Modify: `src/renderer/shadow/ShadowPassRecorder.cpp`
- Modify: `src/renderer/pipeline/GraphicsPipelineBuilder.cpp`

- [ ] **Step 1: Add a source-contract test for shadow raster state**

Append this test to `tests/validation/rendering_convention_tests.cpp`:

```cpp
TEST(RenderingConventionTests,
     ShadowDepthPassUsesPositiveViewportAndDynamicReverseZRasterBias) {
  const std::string shadowRecorder =
      readRepoTextFile("src/renderer/shadow/ShadowPassRecorder.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/pipeline/GraphicsPipelineBuilder.cpp");

  EXPECT_TRUE(contains(shadowRecorder, "viewport.height = static_cast<float>"));
  EXPECT_FALSE(contains(shadowRecorder, "viewport.height = -static_cast<float>"));
  EXPECT_TRUE(contains(shadowRecorder, "vkCmdSetDepthBias"));
  EXPECT_TRUE(contains(shadowRecorder, "inputs.rasterConstantBias"));
  EXPECT_TRUE(contains(shadowRecorder, "inputs.rasterSlopeBias"));

  EXPECT_TRUE(contains(pipelineBuilder,
                       "shadowRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "shadowRaster.cullMode = VK_CULL_MODE_BACK_BIT"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "shadowNoCullRaster.cullMode = VK_CULL_MODE_NONE"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "local_shadow_depth_no_cull_pipeline"));
}
```

- [ ] **Step 2: Run convention tests and verify the failure**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R rendering_convention_tests --output-on-failure"
```

Expected on the broken baseline: the new test fails if the shadow pass inherited negative scene viewport orientation or fixed positive raster bias.

- [ ] **Step 3: Implement positive shadow viewport recording**

In `src/renderer/shadow/ShadowPassRecorder.cpp`, keep `recordShadowPassCommands` viewport setup in this shape:

```cpp
VkViewport viewport{};
VkExtent2D extent = inputs.extent;
if (extent.width == 0u || extent.height == 0u) {
  extent = {container::gpu::kShadowMapResolution,
            container::gpu::kShadowMapResolution};
}
viewport.width = static_cast<float>(extent.width);
viewport.height = static_cast<float>(extent.height);
viewport.minDepth = 0.0f;
viewport.maxDepth = 1.0f;
vkCmdSetViewport(cmd, 0, 1, &viewport);

VkRect2D scissor{};
scissor.extent = extent;
vkCmdSetScissor(cmd, 0, 1, &scissor);

vkCmdSetDepthBias(cmd, inputs.rasterConstantBias, 0.0f,
                  inputs.rasterSlopeBias);
```

- [ ] **Step 4: Implement shadow pipeline state**

In `src/renderer/pipeline/GraphicsPipelineBuilder.cpp`, keep the shadow raster pipeline setup in this shape:

```cpp
VkPipelineRasterizationStateCreateInfo shadowRaster = sceneRaster;
shadowRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
shadowRaster.cullMode = VK_CULL_MODE_BACK_BIT;
shadowRaster.depthBiasEnable = VK_TRUE;
shadowRaster.depthBiasConstantFactor = 0.0f;
shadowRaster.depthBiasClamp = 0.0f;
shadowRaster.depthBiasSlopeFactor = 0.0f;

VkPipelineRasterizationStateCreateInfo shadowFrontCullRaster = shadowRaster;
shadowFrontCullRaster.cullMode = VK_CULL_MODE_FRONT_BIT;

VkPipelineRasterizationStateCreateInfo shadowNoCullRaster = shadowRaster;
shadowNoCullRaster.cullMode = VK_CULL_MODE_NONE;
```

Use `shadowNoCullRaster` for `shadow_depth_no_cull_pipeline` and `local_shadow_depth_no_cull_pipeline`.

- [ ] **Step 5: Re-run convention tests**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R rendering_convention_tests --output-on-failure"
```

Expected: all `rendering_convention_tests` pass.

- [ ] **Step 6: Commit viewport and pipeline contracts**

Run:

```powershell
git add -- tests/validation/rendering_convention_tests.cpp src/renderer/shadow/ShadowPassRecorder.cpp src/renderer/pipeline/GraphicsPipelineBuilder.cpp
git commit -m "fix: stabilize shadow viewport and raster state"
```

---

### Task 4: Separate True Local Shadow Maps From Screen-Space Contact Visibility

**Files:**
- Modify: `tests/validation/rendering_convention_tests.cpp`
- Modify: `shaders/screen_space_light_shadow_common.slang`
- Modify: `shaders/point_light.slang`
- Modify: `shaders/tiled_lighting.slang`
- Modify: `src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp`
- Modify: `include/Container/utility/SceneData.h`

- [ ] **Step 1: Add source-contract test for local shadow and contact visibility**

Append this test to `tests/validation/rendering_convention_tests.cpp`:

```cpp
TEST(RenderingConventionTests,
     ScreenSpaceContactVisibilityCannotReplaceLocalShadowMaps) {
  const std::string screenSpace =
      readRepoTextFile("shaders/screen_space_light_shadow_common.slang");
  const std::string localShadow =
      readRepoTextFile("shaders/local_shadow_common.slang");
  const std::string pointLight =
      readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string lightingRecorder = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp");

  EXPECT_TRUE(contains(screenSpace, "not a local shadow map"));
  EXPECT_TRUE(contains(localShadow, "Texture2DArray<float> localShadowAtlas"));
  EXPECT_TRUE(contains(localShadow, "SampleCmpLevelZero"));
  EXPECT_TRUE(contains(pointLight, "bool useLocalShadowMap"));
  EXPECT_TRUE(contains(pointLight, "LocalPointLightShadowVisibility("));
  EXPECT_TRUE(contains(pointLight, "ScreenSpacePointLightContactVisibility("));
  EXPECT_TRUE(contains(tiledLighting, "bool useLocalShadowMap"));
  EXPECT_TRUE(contains(tiledLighting, "LocalPointLightShadowVisibility("));
  EXPECT_TRUE(contains(tiledLighting, "ScreenSpacePointLightContactVisibility("));
  EXPECT_TRUE(contains(lightingRecorder, "shadowSettings.localContactVisibility"));
}
```

- [ ] **Step 2: Run convention tests and verify the failure**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R rendering_convention_tests --output-on-failure"
```

Expected on the broken baseline: the test fails if point/spot lighting uses screen-space contact as the only shadow path or if the contact term is not gated by UI settings.

- [ ] **Step 3: Keep contact visibility explicitly optional**

In `include/Container/utility/SceneData.h`, keep the setting:

```cpp
bool localContactVisibility{true};
```

In `src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp`, keep the push constant setup equivalent to:

```cpp
const uint32_t contactVisibilityEnabled =
    p.shadows.shadowSettings.localContactVisibility ? 1u : 0u;
```

Pass this to point and tiled lighting push constants separately from `localShadowEnabled`.

- [ ] **Step 4: Keep point and tiled lighting using both terms**

In `shaders/point_light.slang` and `shaders/tiled_lighting.slang`, keep the logic in this shape:

```slang
bool useLocalShadowMap =
    LocalShadowMapsEnabled(pc.localShadowEnabled, uLocalShadow) &&
    PointLightHasLocalShadowLayers(light, uLocalShadow);
if (useLocalShadowMap)
{
    visibility *= LocalPointLightShadowVisibility(
        light, worldPosition, normal, surfaceToLight,
        uLocalShadow, gLocalShadowAtlas, gLocalShadowSampler);
}

if (pc.contactVisibilityEnabled != 0u)
{
    visibility *= ScreenSpacePointLightContactVisibility(
        worldPosition, normal, lightPosition,
        gDepthTexture, gNormalTexture, uCamera.viewProj,
        uCamera.inverseViewProj);
}
```

Keep this order: true local shadow first, optional contact visibility second.

- [ ] **Step 5: Re-run convention tests**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R rendering_convention_tests --output-on-failure"
```

Expected: all `rendering_convention_tests` pass.

- [ ] **Step 6: Commit local shadow/contact separation**

Run:

```powershell
git add -- tests/validation/rendering_convention_tests.cpp shaders/screen_space_light_shadow_common.slang shaders/point_light.slang shaders/tiled_lighting.slang src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp include/Container/utility/SceneData.h
git commit -m "fix: separate local shadow maps from contact visibility"
```

---

### Task 5: Add Deterministic Shadow Correctness Fixtures

**Files:**
- Modify: `tests/fixtures/rendering/realistic_visual_regression.fixtures.json`
- Modify: `tests/validation/realistic_rendering_validation_tests.cpp`
- Optional create: `models/validation/open_shadow_wall_blocker.gltf`
- Optional create: `models/validation/closed_shadow_room_blocker.gltf`

- [ ] **Step 1: Add metadata validation for open and closed shadow fixtures**

In `tests/validation/realistic_rendering_validation_tests.cpp`, append:

```cpp
TEST(RealisticRenderingValidation,
     ShadowCorrectnessFixturesCoverOpenAndClosedWorlds) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");

  const Json *openScene =
      findSceneById(fixtures.at("scenes"), "open_shadow_wall_blocker");
  const Json *closedScene =
      findSceneById(fixtures.at("scenes"), "closed_shadow_room_blocker");

  ASSERT_NE(openScene, nullptr);
  ASSERT_NE(closedScene, nullptr);

  for (const Json *scene : {openScene, closedScene}) {
    EXPECT_EQ(scene->at("category").get<std::string>(), "shadow");
    EXPECT_EQ(scene->at("renderMode").get<std::string>(), "final-lit");
    ASSERT_TRUE(scene->contains("probes"));

    bool foundRatioProbe = false;
    bool foundStabilityProbe = false;
    for (const Json &probe : scene->at("probes")) {
      const std::string kind = probe.at("kind").get<std::string>();
      if (kind == "relative_region_luminance") {
        foundRatioProbe = true;
        EXPECT_TRUE(probe.contains("litRegionUv"));
        EXPECT_TRUE(probe.contains("shadowRegionUv"));
        EXPECT_LE(probe.at("maximumShadowToLitRatio").get<double>(), 0.75);
      }
      if (kind == "camera_stability_shadow_ratio") {
        foundStabilityProbe = true;
        EXPECT_LE(probe.at("maximumRatioDrift").get<double>(), 0.10);
      }
    }
    EXPECT_TRUE(foundRatioProbe);
    EXPECT_TRUE(foundStabilityProbe);
  }
}
```

- [ ] **Step 2: Run validation tests and verify the failure**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R realistic_rendering_validation_tests --output-on-failure"
```

Expected: the new test fails because `open_shadow_wall_blocker` and `closed_shadow_room_blocker` are not defined.

- [ ] **Step 3: Add fixture entries**

Add these two scene entries to `tests/fixtures/rendering/realistic_visual_regression.fixtures.json` under `scenes`:

```json
{
  "id": "open_shadow_wall_blocker",
  "category": "shadow",
  "status": "planned",
  "asset": "models/validation/open_shadow_wall_blocker.gltf",
  "renderMode": "final-lit",
  "camera": {
    "position": [0.0, 1.6, 4.5],
    "target": [0.0, 0.9, 0.0],
    "verticalFovDegrees": 42.0
  },
  "lighting": {
    "environment": "hdr/citrus_orchard_road_puresky_4k.exr",
    "environmentIntensity": 0.05,
    "exposure": 0.25,
    "directionalLight": {
      "illuminanceLux": 7000.0,
      "direction": [-0.35, -0.85, -0.30],
      "color": [1.0, 0.96, 0.90]
    },
    "pointLights": []
  },
  "probes": [
    {
      "id": "wall_blocker_casts_open_floor_shadow",
      "kind": "relative_region_luminance",
      "litRegionUv": [0.265, 0.720, 0.090, 0.055],
      "shadowRegionUv": [0.455, 0.730, 0.110, 0.060],
      "maximumShadowToLitRatio": 0.70
    },
    {
      "id": "open_shadow_stays_stable_across_camera_sweep",
      "kind": "camera_stability_shadow_ratio",
      "maximumRatioDrift": 0.10
    }
  ],
  "screenshots": {
    "golden": "tests/visual-regression/golden/{platform}/open_shadow_wall_blocker.png",
    "candidate": "test_results/visual-regression/candidate/{platform}/open_shadow_wall_blocker.png",
    "diff": "test_results/visual-regression/diff/{platform}/open_shadow_wall_blocker.png"
  }
}
```

```json
{
  "id": "closed_shadow_room_blocker",
  "category": "shadow",
  "status": "planned",
  "asset": "models/validation/closed_shadow_room_blocker.gltf",
  "renderMode": "final-lit",
  "camera": {
    "position": [0.0, 1.25, 3.8],
    "target": [0.0, 0.95, 0.0],
    "verticalFovDegrees": 38.0
  },
  "lighting": {
    "environment": "hdr/citrus_orchard_road_puresky_4k.exr",
    "environmentIntensity": 0.0,
    "exposure": 0.25,
    "directionalLight": {
      "illuminanceLux": 5000.0,
      "direction": [-0.20, -0.90, -0.30],
      "color": [1.0, 0.96, 0.90]
    },
    "pointLights": []
  },
  "probes": [
    {
      "id": "closed_room_blocker_contains_floor_shadow",
      "kind": "relative_region_luminance",
      "litRegionUv": [0.300, 0.760, 0.090, 0.055],
      "shadowRegionUv": [0.470, 0.770, 0.110, 0.060],
      "maximumShadowToLitRatio": 0.70
    },
    {
      "id": "closed_shadow_stays_stable_across_camera_sweep",
      "kind": "camera_stability_shadow_ratio",
      "maximumRatioDrift": 0.10
    }
  ],
  "screenshots": {
    "golden": "tests/visual-regression/golden/{platform}/closed_shadow_room_blocker.png",
    "candidate": "test_results/visual-regression/candidate/{platform}/closed_shadow_room_blocker.png",
    "diff": "test_results/visual-regression/diff/{platform}/closed_shadow_room_blocker.png"
  }
}
```

- [ ] **Step 4: Add minimal fixture assets if missing**

If `models/validation/open_shadow_wall_blocker.gltf` or `models/validation/closed_shadow_room_blocker.gltf` does not exist, create compact glTF scenes with:

- one gray floor,
- one vertical blocker panel,
- one receiving wall for the closed room,
- one material with `doubleSided: false`,
- one mirrored node in the closed fixture to exercise winding/cull handling.

Use existing validation assets as format references: `models/validation/cornell_box_local_light.gltf` and active fixture assets already listed in `tests/fixtures/rendering/realistic_visual_regression.fixtures.json`.

- [ ] **Step 5: Re-run validation tests**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R realistic_rendering_validation_tests --output-on-failure"
```

Expected: `realistic_rendering_validation_tests` pass while the new fixtures remain `planned`.

- [ ] **Step 6: Commit fixture metadata**

Run:

```powershell
git add -- tests/fixtures/rendering/realistic_visual_regression.fixtures.json tests/validation/realistic_rendering_validation_tests.cpp models/validation/open_shadow_wall_blocker.gltf models/validation/closed_shadow_room_blocker.gltf
git commit -m "test: add shadow correctness fixtures"
```

---

### Task 6: Promote The Local-Light Cornell Fixture When Numeric Probes Pass

**Files:**
- Modify: `tests/fixtures/rendering/realistic_visual_regression.fixtures.json`
- Modify: `tests/validation/realistic_rendering_validation_tests.cpp`
- Optional add: `tests/visual-regression/golden/windows-nvidia/cornell_box_local_light_occlusion.png`

- [ ] **Step 1: Capture the planned Cornell fixture with numeric probes**

Run the visual regression test with planned fixtures included:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && tests\validation\visual_regression_gpu_tests.exe --include-planned --scene cornell_box_local_light_occlusion"
```

Expected: a candidate image is written under `test_results/visual-regression/candidate/windows-nvidia/cornell_box_local_light_occlusion.png`, and the probe output reports the relative luminance and penumbra checks.

- [ ] **Step 2: Promote only if the candidate satisfies probes**

If the command reports probe success, copy the candidate into the golden path:

```powershell
Copy-Item -Force `
  'test_results\visual-regression\candidate\windows-nvidia\cornell_box_local_light_occlusion.png' `
  'tests\visual-regression\golden\windows-nvidia\cornell_box_local_light_occlusion.png'
```

If the probe fails, leave the fixture planned and return to Tasks 1-4.

- [ ] **Step 3: Make Cornell active after golden promotion**

In `tests/fixtures/rendering/realistic_visual_regression.fixtures.json`, change:

```json
"id": "cornell_box_local_light_occlusion",
"category": "shadow",
"status": "planned",
```

to:

```json
"id": "cornell_box_local_light_occlusion",
"category": "shadow",
"status": "active",
```

- [ ] **Step 4: Update the validation expectation**

In `tests/validation/realistic_rendering_validation_tests.cpp`, change the Cornell status assertion from:

```cpp
EXPECT_EQ(scene->at("status").get<std::string>(), "planned")
    << "The locally authored Cornell fixture is diagnostic-only until it is "
       "replaced by a traceable external reference-backed scene.";
```

to:

```cpp
EXPECT_EQ(scene->at("status").get<std::string>(), "active")
    << "The Cornell local-light fixture is active after numeric probe and "
       "golden-image promotion.";
```

- [ ] **Step 5: Run fixture validation and active visual capture**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R realistic_rendering_validation_tests --output-on-failure"
```

Then run:

```powershell
cmd /c "tests\validation\visual_regression_gpu_tests.exe --scene cornell_box_local_light_occlusion"
```

Expected: fixture validation passes, active capture runs, and image comparison uses the promoted golden.

- [ ] **Step 6: Commit Cornell promotion**

Run:

```powershell
git add -- tests/fixtures/rendering/realistic_visual_regression.fixtures.json tests/validation/realistic_rendering_validation_tests.cpp tests/visual-regression/golden/windows-nvidia/cornell_box_local_light_occlusion.png
git commit -m "test: promote cornell local shadow fixture"
```

---

### Task 7: Run Focused Verification

**Files:**
- No source edits.

- [ ] **Step 1: Run focused renderer tests**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug && ctest --test-dir out\build\windows-debug -R ""(shadow_pass_draw_planner_tests|shadow_pass_raster_planner_tests|shadow_pass_scope_planner_tests|rendering_convention_tests|realistic_rendering_validation_tests)"" --output-on-failure"
```

Expected: all selected tests pass.

- [ ] **Step 2: Run deferred/shadow regression subset**

Run:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 && ctest --test-dir out\build\windows-debug -R ""(shadow_|deferred_raster_lighting|deferred_point_lighting|deferred_raster_post_process)"" --output-on-failure"
```

Expected: all selected tests pass.

- [ ] **Step 3: Run active visual regression for active shadow scenes**

Run:

```powershell
cmd /c "tests\validation\visual_regression_gpu_tests.exe --category shadow"
```

Expected: every active shadow fixture passes image and numeric probes. Planned scenes may be skipped unless `--include-planned` is supplied.

- [ ] **Step 4: Record verification in the final implementation response**

Include these exact result categories in the completion note:

- focused CTest subset,
- deferred/shadow CTest subset,
- active shadow visual regression,
- any planned fixture captures that were intentionally not promoted.

---

## Self-Review

Spec coverage:

- Shadow correctness: Tasks 1-4.
- Winding and surface routing: Tasks 1 and 3.
- Camera perspective and reverse-Z: Tasks 2 and 3.
- Open and closed environments: Task 5.
- Local point/spot/area shadow behavior: Tasks 2, 4, and 6.
- PBR correctness foundation: Tasks 2, 5, and 7 establish stable shadow input before exposure, IBL, AO, and tone mapping calibration.

Placeholder scan:

- The plan contains no incomplete markers or unspecified implementation steps.
- Optional asset/golden steps are guarded by explicit file paths and probe results.

Type consistency:

- All route tests use existing `ShadowPassDrawPlan`, `ShadowPassPipeline`, and `buildShadowPassDrawPlan`.
- Shader tests use existing `readRepoTextFile` and `contains` helpers.
- Fixture tests use existing JSON fixture helpers and status values `planned` and `active`.
