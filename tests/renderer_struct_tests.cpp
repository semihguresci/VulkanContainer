// renderer_struct_tests.cpp
//
// CPU-only unit tests for the renderer value structs introduced during the
// refactoring sessions.  No GPU, no window, and no Vulkan device are required.
//
// Structs under test
// ------------------
//   DebugRenderState         (DebugRenderState.h)
//   SceneState               (SceneState.h)
//   FrameRecordParams        (FrameRecorder.h)
//     ::PushConstantState
//     ::RenderPassHandles
//   RenderGraph              (RenderGraph.h)

#include <gtest/gtest.h>

#include "Container/renderer/DebugRenderState.h"
#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/RenderGraph.h"
#include "Container/renderer/SceneState.h"

#include <limits>
#include <string>
#include <vector>

namespace {

// ============================================================================
// DebugRenderState
// ============================================================================

TEST(DebugRenderState, DefaultsAreAllFalse) {
    container::renderer::DebugRenderState s{};
    EXPECT_FALSE(s.directionalOnly);
    EXPECT_FALSE(s.visualizePointLightStencil);
    EXPECT_FALSE(s.directionalOnlyKeyDown);
    EXPECT_FALSE(s.visualizePointLightStencilKeyDown);
}

TEST(DebugRenderState, ToggleDirectionalOnly) {
    container::renderer::DebugRenderState s{};
    s.directionalOnly = !s.directionalOnly;
    EXPECT_TRUE(s.directionalOnly);
    s.directionalOnly = !s.directionalOnly;
    EXPECT_FALSE(s.directionalOnly);
}

TEST(DebugRenderState, KeyDownTrackerIsIndependentOfToggle) {
    container::renderer::DebugRenderState s{};
    s.directionalOnlyKeyDown = true;
    EXPECT_FALSE(s.directionalOnly)
        << "Key-down tracker must not affect the toggle state";
}

// ============================================================================
// SceneState
// ============================================================================

TEST(SceneState, DefaultNodeIndicesAreInvalid) {
    container::renderer::SceneState ss{};
    constexpr uint32_t kInvalid = container::scene::SceneGraph::kInvalidNode;
    EXPECT_EQ(ss.rootNode,         kInvalid);
    EXPECT_EQ(ss.cubeNode,         kInvalid);
    EXPECT_EQ(ss.selectedMeshNode, kInvalid);
}

TEST(SceneState, DefaultDiagCubeIndexIsDisabled) {
    container::renderer::SceneState ss{};
    EXPECT_EQ(ss.diagCubeObjectIndex, std::numeric_limits<uint32_t>::max());
}

TEST(SceneState, DefaultIndexTypeIsUint32) {
    container::renderer::SceneState ss{};
    EXPECT_EQ(ss.indexType, VK_INDEX_TYPE_UINT32);
}

TEST(SceneState, DefaultBufferSlicesAreNull) {
    container::renderer::SceneState ss{};
    EXPECT_EQ(ss.vertexSlice.buffer, VK_NULL_HANDLE);
    EXPECT_EQ(ss.indexSlice.buffer,  VK_NULL_HANDLE);
}

// ============================================================================
// FrameRecordParams::RenderPassHandles
// ============================================================================

TEST(FrameRecordParams_RenderPassHandles, DefaultsAreNullHandle) {
    container::renderer::FrameRecordParams::RenderPassHandles rp{};
    EXPECT_EQ(rp.depthPrepass,  VK_NULL_HANDLE);
    EXPECT_EQ(rp.gBuffer,       VK_NULL_HANDLE);
    EXPECT_EQ(rp.lighting,      VK_NULL_HANDLE);
    EXPECT_EQ(rp.postProcess,   VK_NULL_HANDLE);
}

TEST(FrameRecordParams_RenderPassHandles, AggregateInitCoversAllFields) {
    // Use sentinel values so that any missing field in the aggregate init
    // would remain as VK_NULL_HANDLE and fail the assertions below.
    const VkRenderPass kA = reinterpret_cast<VkRenderPass>(0x1);
    const VkRenderPass kB = reinterpret_cast<VkRenderPass>(0x2);
    const VkRenderPass kC = reinterpret_cast<VkRenderPass>(0x3);
    const VkRenderPass kD = reinterpret_cast<VkRenderPass>(0x4);

    container::renderer::FrameRecordParams::RenderPassHandles rp{kA, kB, kC, kD};
    EXPECT_EQ(rp.depthPrepass, kA);
    EXPECT_EQ(rp.gBuffer,      kB);
    EXPECT_EQ(rp.lighting,     kC);
    EXPECT_EQ(rp.postProcess,  kD);
}

// ============================================================================
// FrameRecordParams::PushConstantState
// ============================================================================

TEST(FrameRecordParams_PushConstantState, DefaultsAreNullptr) {
    container::renderer::FrameRecordParams::PushConstantState pc{};
    EXPECT_EQ(pc.bindless,        nullptr);
    EXPECT_EQ(pc.light,           nullptr);
    EXPECT_EQ(pc.wireframe,       nullptr);
    EXPECT_EQ(pc.normalValidation, nullptr);
    EXPECT_EQ(pc.surfaceNormal,   nullptr);
}

TEST(FrameRecordParams_PushConstantState, PointersRoundtrip) {
    container::gpu::BindlessPushConstants              bc{};
    container::renderer::LightPushConstants         lc{};
    container::renderer::WireframePushConstants               wc{};
    container::renderer::NormalValidationPushConstants nc{};
    container::renderer::SurfaceNormalPushConstants    sc{};

    container::renderer::FrameRecordParams::PushConstantState ps{&bc, &lc, &wc, &nc, &sc};

    EXPECT_EQ(ps.bindless,         &bc);
    EXPECT_EQ(ps.light,            &lc);
    EXPECT_EQ(ps.wireframe,        &wc);
    EXPECT_EQ(ps.normalValidation, &nc);
    EXPECT_EQ(ps.surfaceNormal,    &sc);
}

// ============================================================================
// FrameRecordParams — top-level defaults
// ============================================================================

TEST(FrameRecordParams, DefaultFramePointerIsNull) {
    container::renderer::FrameRecordParams p{};
    EXPECT_EQ(p.frame, nullptr);
}

TEST(FrameRecordParams, DefaultDescriptorSetsAreNullHandle) {
    container::renderer::FrameRecordParams p{};
    EXPECT_EQ(p.sceneDescriptorSet, VK_NULL_HANDLE);
    EXPECT_EQ(p.lightDescriptorSet, VK_NULL_HANDLE);
}

TEST(FrameRecordParams, DefaultDebugFlagsAreFalse) {
    container::renderer::FrameRecordParams p{};
    EXPECT_FALSE(p.debugDirectionalOnly);
    EXPECT_FALSE(p.debugVisualizePointLightStencil);
    EXPECT_FALSE(p.wireframeRasterModeSupported);
    EXPECT_FALSE(p.wireframeWideLinesSupported);
}

TEST(FrameRecordParams, DefaultDiagCubeIndexIsDisabled) {
    container::renderer::FrameRecordParams p{};
    EXPECT_EQ(p.diagCubeObjectIndex, std::numeric_limits<uint32_t>::max());
}

// ============================================================================
// RenderGraph
// ============================================================================

using container::renderer::RenderGraph;
using container::renderer::RenderPassNode;
using container::renderer::FrameRecordParams;

TEST(RenderGraph, DefaultConstructionIsEmpty) {
    RenderGraph g;
    EXPECT_EQ(g.passCount(), 0u);
    EXPECT_EQ(g.enabledPassCount(), 0u);
}

TEST(RenderGraph, AddPassIncreasesCount) {
    RenderGraph g;
    g.addPass("A", [](VkCommandBuffer, const FrameRecordParams&) {});
    EXPECT_EQ(g.passCount(), 1u);
    g.addPass("B", [](VkCommandBuffer, const FrameRecordParams&) {});
    EXPECT_EQ(g.passCount(), 2u);
}

TEST(RenderGraph, AddPassReturnsEnabledByDefault) {
    RenderGraph g;
    auto& node = g.addPass("X", [](VkCommandBuffer, const FrameRecordParams&) {});
    EXPECT_TRUE(node.enabled);
    EXPECT_EQ(node.name, "X");
}

TEST(RenderGraph, ExecuteCallsEnabledPassesInOrder) {
    std::vector<std::string> order;
    RenderGraph g;
    g.addPass("First",  [&](VkCommandBuffer, const FrameRecordParams&) { order.push_back("First"); });
    g.addPass("Second", [&](VkCommandBuffer, const FrameRecordParams&) { order.push_back("Second"); });
    g.addPass("Third",  [&](VkCommandBuffer, const FrameRecordParams&) { order.push_back("Third"); });

    FrameRecordParams params{};
    g.execute(nullptr, params);

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "First");
    EXPECT_EQ(order[1], "Second");
    EXPECT_EQ(order[2], "Third");
}

TEST(RenderGraph, ExecuteSkipsDisabledPasses) {
    std::vector<std::string> order;
    RenderGraph g;
    g.addPass("A", [&](VkCommandBuffer, const FrameRecordParams&) { order.push_back("A"); });
    g.addPass("B", [&](VkCommandBuffer, const FrameRecordParams&) { order.push_back("B"); });
    g.addPass("C", [&](VkCommandBuffer, const FrameRecordParams&) { order.push_back("C"); });

    g.findPass("B")->enabled = false;

    FrameRecordParams params{};
    g.execute(nullptr, params);

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], "A");
    EXPECT_EQ(order[1], "C");
}

TEST(RenderGraph, ExecuteOnEmptyGraphIsNoOp) {
    RenderGraph g;
    FrameRecordParams params{};
    g.execute(nullptr, params);  // Should not crash.
    EXPECT_EQ(g.passCount(), 0u);
}

TEST(RenderGraph, FindPassByName) {
    RenderGraph g;
    g.addPass("Alpha", [](VkCommandBuffer, const FrameRecordParams&) {});
    g.addPass("Beta",  [](VkCommandBuffer, const FrameRecordParams&) {});

    auto* node = g.findPass("Beta");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->name, "Beta");
}

TEST(RenderGraph, FindPassReturnsNullForMissing) {
    RenderGraph g;
    g.addPass("Only", [](VkCommandBuffer, const FrameRecordParams&) {});
    EXPECT_EQ(g.findPass("Missing"), nullptr);
}

TEST(RenderGraph, FindPassConst) {
    RenderGraph g;
    g.addPass("Node", [](VkCommandBuffer, const FrameRecordParams&) {});
    const auto& cg = g;
    const auto* node = cg.findPass("Node");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->name, "Node");
}

TEST(RenderGraph, EnabledPassCountReflectsToggles) {
    RenderGraph g;
    g.addPass("A", [](VkCommandBuffer, const FrameRecordParams&) {});
    g.addPass("B", [](VkCommandBuffer, const FrameRecordParams&) {});
    g.addPass("C", [](VkCommandBuffer, const FrameRecordParams&) {});

    EXPECT_EQ(g.enabledPassCount(), 3u);

    g.findPass("B")->enabled = false;
    EXPECT_EQ(g.enabledPassCount(), 2u);

    g.findPass("B")->enabled = true;
    EXPECT_EQ(g.enabledPassCount(), 3u);
}

TEST(RenderGraph, ClearRemovesAllPasses) {
    RenderGraph g;
    g.addPass("A", [](VkCommandBuffer, const FrameRecordParams&) {});
    g.addPass("B", [](VkCommandBuffer, const FrameRecordParams&) {});
    EXPECT_EQ(g.passCount(), 2u);

    g.clear();
    EXPECT_EQ(g.passCount(), 0u);
    EXPECT_EQ(g.enabledPassCount(), 0u);
    EXPECT_EQ(g.findPass("A"), nullptr);
}

TEST(RenderGraph, ToggleViaFindPass) {
    std::vector<std::string> order;
    RenderGraph g;
    g.addPass("Depth",    [&](VkCommandBuffer, const FrameRecordParams&) { order.push_back("Depth"); });
    g.addPass("Lighting", [&](VkCommandBuffer, const FrameRecordParams&) { order.push_back("Lighting"); });

    g.findPass("Depth")->enabled = false;

    FrameRecordParams params{};
    g.execute(nullptr, params);

    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "Lighting");
}

}  // namespace
