#include "Container/renderer/GraphicsPipelineBuilder.h"
#include "Container/renderer/DebugOverlayRenderer.h"
#include "Container/renderer/LightingManager.h"
#include "Container/geometry/Model.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <array>
#include <stdexcept>

namespace container::renderer {

using container::gpu::BindlessPushConstants;
using container::gpu::PostProcessPushConstants;
using container::gpu::ShadowPushConstants;
using container::gpu::TiledLightingPushConstants;

GraphicsPipelineBuilder::GraphicsPipelineBuilder(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::PipelineManager&            pipelineManager,
    bool                                           wireframeRasterModeSupported)
    : device_(std::move(device))
    , pipelineManager_(pipelineManager)
    , wireframeRasterModeSupported_(wireframeRasterModeSupported) {
}

PipelineBuildResult GraphicsPipelineBuilder::build(
    const std::filesystem::path&     shaderDir,
    const PipelineDescriptorLayouts& descriptorLayouts,
    const PipelineRenderPasses&      renderPasses) const {

  // ---- shader loading helpers -----------------------------------------------
  const auto loadModule = [&](const char* path) {
    std::filesystem::path shaderPath(path);
    if (shaderPath.is_relative()) {
      shaderPath = shaderDir / shaderPath;
    }
    const auto fileData = container::util::readFile(shaderPath);
    return container::gpu::createShaderModule(device_->device(), fileData);
  };

  const auto makeStage = [](VkShaderModule module, VkShaderStageFlagBits stage) {
    VkPipelineShaderStageCreateInfo info{};
    info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage  = stage;
    info.module = module;
    switch (stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:   info.pName = "vertMain"; break;
      case VK_SHADER_STAGE_FRAGMENT_BIT: info.pName = "fragMain"; break;
      case VK_SHADER_STAGE_GEOMETRY_BIT: info.pName = "geomMain"; break;
      default:                           info.pName = "main";     break;
    }
    return info;
  };

  // ---- load shader modules --------------------------------------------------
  VkShaderModule depthPrepassVert = loadModule("spv_shaders/depth_prepass.vert.spv");
  VkShaderModule depthPrepassFrag = loadModule("spv_shaders/depth_prepass.frag.spv");
  VkShaderModule gBufferVert      = loadModule("spv_shaders/gbuffer.vert.spv");
  VkShaderModule gBufferFrag      = loadModule("spv_shaders/gbuffer.frag.spv");
  VkShaderModule dirVert          = loadModule("spv_shaders/deferred_directional.vert.spv");
  VkShaderModule dirFrag          = loadModule("spv_shaders/deferred_directional.frag.spv");
  VkShaderModule stencilVert      = loadModule("spv_shaders/light_stencil.vert.spv");
  VkShaderModule stencilFrag      = loadModule("spv_shaders/light_stencil.frag.spv");
  VkShaderModule pointVert        = loadModule("spv_shaders/point_light.vert.spv");
  VkShaderModule pointFrag        = loadModule("spv_shaders/point_light.frag.spv");
  VkShaderModule pointDbgVert     = loadModule("spv_shaders/point_light_stencil_debug.vert.spv");
  VkShaderModule pointDbgFrag     = loadModule("spv_shaders/point_light_stencil_debug.frag.spv");
  VkShaderModule transVert        = loadModule("spv_shaders/forward_transparent.vert.spv");
  VkShaderModule transFrag        = loadModule("spv_shaders/forward_transparent.frag.spv");
  VkShaderModule postVert         = loadModule("spv_shaders/post_process.vert.spv");
  VkShaderModule postFrag         = loadModule("spv_shaders/post_process.frag.spv");
  VkShaderModule dbgVert          = loadModule("spv_shaders/geometry_debug.vert.spv");
  VkShaderModule dbgFrag          = loadModule("spv_shaders/geometry_debug.frag.spv");
  VkShaderModule nvVert           = loadModule("spv_shaders/normal_validation.vert.spv");
  VkShaderModule nvGeom           = loadModule("spv_shaders/normal_validation.geom.spv");
  VkShaderModule nvFrag           = loadModule("spv_shaders/normal_validation.frag.spv");
  VkShaderModule wfVert           = loadModule("spv_shaders/wireframe_debug.vert.spv");
  VkShaderModule wfFrag           = loadModule("spv_shaders/wireframe_debug.frag.spv");
  VkShaderModule wfFbVert         = loadModule("spv_shaders/wireframe_fallback.vert.spv");
  VkShaderModule wfFbGeom         = loadModule("spv_shaders/wireframe_fallback.geom.spv");
  VkShaderModule wfFbFrag         = loadModule("spv_shaders/wireframe_fallback.frag.spv");
  VkShaderModule snVert           = loadModule("spv_shaders/surface_normals.vert.spv");
  VkShaderModule snGeom           = loadModule("spv_shaders/surface_normals.geom.spv");
  VkShaderModule snFrag           = loadModule("spv_shaders/surface_normals.frag.spv");
  VkShaderModule onVert           = loadModule("spv_shaders/object_normals.vert.spv");
  VkShaderModule onFrag           = loadModule("spv_shaders/object_normals.frag.spv");
  VkShaderModule lgVert           = loadModule("spv_shaders/light_gizmo.vert.spv");
  VkShaderModule lgFrag           = loadModule("spv_shaders/light_gizmo.frag.spv");
  VkShaderModule sdVert           = loadModule("spv_shaders/shadow_depth.vert.spv");
  VkShaderModule sdFrag           = loadModule("spv_shaders/shadow_depth.frag.spv");
  VkShaderModule tlVert           = loadModule("spv_shaders/tiled_lighting.vert.spv");
  VkShaderModule tlFrag           = loadModule("spv_shaders/tiled_lighting.frag.spv");

  // ---- shader stage arrays --------------------------------------------------
  std::array<VkPipelineShaderStageCreateInfo, 2> depthPrepassStages = {
      makeStage(depthPrepassVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(depthPrepassFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> gBufferStages = {
      makeStage(gBufferVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(gBufferFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> dirStages = {
      makeStage(dirVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(dirFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> stencilStages = {
      makeStage(stencilVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(stencilFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> pointStages = {
      makeStage(pointVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(pointFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> pointDbgStages = {
      makeStage(pointDbgVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(pointDbgFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> transStages = {
      makeStage(transVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(transFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> postStages = {
      makeStage(postVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(postFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> dbgStages = {
      makeStage(dbgVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(dbgFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 3> nvStages = {
      makeStage(nvVert,  VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(nvGeom,  VK_SHADER_STAGE_GEOMETRY_BIT),
      makeStage(nvFrag,  VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> wfStages = {
      makeStage(wfVert,  VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(wfFrag,  VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 3> wfFbStages = {
      makeStage(wfFbVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(wfFbGeom, VK_SHADER_STAGE_GEOMETRY_BIT),
      makeStage(wfFbFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 3> snStages = {
      makeStage(snVert,  VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(snGeom,  VK_SHADER_STAGE_GEOMETRY_BIT),
      makeStage(snFrag,  VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> onStages = {
      makeStage(onVert,  VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(onFrag,  VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> lgStages = {
      makeStage(lgVert,  VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(lgFrag,  VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> sdStages = {
      makeStage(sdVert,  VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(sdFrag,  VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> tlStages = {
      makeStage(tlVert,  VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(tlFrag,  VK_SHADER_STAGE_FRAGMENT_BIT)};

  // ---- vertex input states --------------------------------------------------
  const auto bindingDesc   = container::geometry::Vertex::bindingDescription();
  const auto attribDescs   = container::geometry::Vertex::attributeDescriptions();

  VkPipelineVertexInputStateCreateInfo fullVertexInput{};
  fullVertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  fullVertexInput.vertexBindingDescriptionCount   = 1;
  fullVertexInput.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attribDescs.size());
  fullVertexInput.pVertexBindingDescriptions   = &bindingDesc;
  fullVertexInput.pVertexAttributeDescriptions = attribDescs.data();

  std::array<VkVertexInputAttributeDescription, 1> posOnlyAttribs   = {attribDescs[0]};
  std::array<VkVertexInputAttributeDescription, 2> posTexAttribs    = {attribDescs[0], attribDescs[2]};
  std::array<VkVertexInputAttributeDescription, 3> posTexNormAttribs= {attribDescs[0], attribDescs[2], attribDescs[3]};

  VkPipelineVertexInputStateCreateInfo posOnlyInput = fullVertexInput;
  posOnlyInput.vertexAttributeDescriptionCount = 1;
  posOnlyInput.pVertexAttributeDescriptions    = posOnlyAttribs.data();

  VkPipelineVertexInputStateCreateInfo posTexInput = fullVertexInput;
  posTexInput.vertexAttributeDescriptionCount = 2;
  posTexInput.pVertexAttributeDescriptions    = posTexAttribs.data();

  VkPipelineVertexInputStateCreateInfo posTexNormInput = fullVertexInput;
  posTexNormInput.vertexAttributeDescriptionCount = 3;
  posTexNormInput.pVertexAttributeDescriptions    = posTexNormAttribs.data();

  VkPipelineVertexInputStateCreateInfo emptyVertexInput{};
  emptyVertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  // ---- input assemblies -----------------------------------------------------
  VkPipelineInputAssemblyStateCreateInfo triAssembly{};
  triAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  triAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineInputAssemblyStateCreateInfo pointAssembly = triAssembly;
  pointAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

  VkPipelineInputAssemblyStateCreateInfo lineAssembly = triAssembly;
  lineAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

  // ---- viewport state (dynamic) --------------------------------------------
  VkPipelineViewportStateCreateInfo vpState{};
  vpState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vpState.viewportCount = 1;
  vpState.scissorCount  = 1;

  // ---- rasterizers ----------------------------------------------------------
  VkPipelineRasterizationStateCreateInfo sceneRaster{};
  sceneRaster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  sceneRaster.polygonMode = VK_POLYGON_MODE_FILL;
  sceneRaster.lineWidth   = 1.0f;
  // Match Blender's material preview behavior for imported scenes. Some glTF
  // assets include thin wall-mounted reliefs whose effective winding does not
  // survive the full projection/viewport path consistently; culling them makes
  // whole details, such as Sponza's lion reliefs, disappear.
  sceneRaster.cullMode    = VK_CULL_MODE_NONE;
  // glTF authors geometry with counter-clockwise front faces in clip/NDC
  // space. Scene passes use a negative-height viewport, which flips Y during
  // viewport transform, so those faces are clockwise in framebuffer space.
  sceneRaster.frontFace   = VK_FRONT_FACE_CLOCKWISE;

  VkPipelineRasterizationStateCreateInfo noCullRaster = sceneRaster;
  noCullRaster.cullMode = VK_CULL_MODE_NONE;

  VkPipelineRasterizationStateCreateInfo fullscreenRaster = sceneRaster;
  fullscreenRaster.cullMode = VK_CULL_MODE_NONE;

  VkPipelineRasterizationStateCreateInfo normalLineRaster = sceneRaster;
  normalLineRaster.cullMode = VK_CULL_MODE_NONE;

  VkPipelineRasterizationStateCreateInfo wfRaster = sceneRaster;
  wfRaster.polygonMode = VK_POLYGON_MODE_LINE;
  wfRaster.cullMode    = VK_CULL_MODE_NONE;
  wfRaster.lineWidth   = 1.0f;

  VkPipelineRasterizationStateCreateInfo wfFbRaster = sceneRaster;
  wfFbRaster.cullMode = VK_CULL_MODE_NONE;

  // ---- multisample ----------------------------------------------------------
  VkPipelineMultisampleStateCreateInfo msaa{};
  msaa.sType               = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // ---- color blend attachments ----------------------------------------------
  VkPipelineColorBlendAttachmentState opaqueAttach{};
  opaqueAttach.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendAttachmentState additiveAttach = opaqueAttach;
  additiveAttach.blendEnable        = VK_TRUE;
  additiveAttach.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  additiveAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  additiveAttach.colorBlendOp        = VK_BLEND_OP_ADD;
  additiveAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  additiveAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  additiveAttach.alphaBlendOp        = VK_BLEND_OP_ADD;

  VkPipelineColorBlendAttachmentState overlayAttach = opaqueAttach;
  overlayAttach.blendEnable        = VK_TRUE;
  overlayAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  overlayAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  overlayAttach.colorBlendOp        = VK_BLEND_OP_ADD;
  overlayAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  overlayAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  overlayAttach.alphaBlendOp        = VK_BLEND_OP_ADD;

  VkPipelineColorBlendAttachmentState noColorAttach = opaqueAttach;
  noColorAttach.colorWriteMask = 0;

  std::array<VkPipelineColorBlendAttachmentState, 4> gBufAttachs =
      {opaqueAttach, opaqueAttach, opaqueAttach, opaqueAttach};
  std::array<VkPipelineColorBlendAttachmentState, 1> opaqueArr    = {opaqueAttach};
  std::array<VkPipelineColorBlendAttachmentState, 1> additiveArr  = {additiveAttach};
  std::array<VkPipelineColorBlendAttachmentState, 1> overlayArr   = {overlayAttach};
  std::array<VkPipelineColorBlendAttachmentState, 1> noColorArr   = {noColorAttach};

  // blend state helpers
  auto makeBlend = [](const VkPipelineColorBlendAttachmentState* arr,
                      uint32_t count) {
    VkPipelineColorBlendStateCreateInfo b{};
    b.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    b.attachmentCount = count;
    b.pAttachments    = arr;
    return b;
  };

  VkPipelineColorBlendStateCreateInfo noBlend      = makeBlend(nullptr, 0);
  VkPipelineColorBlendStateCreateInfo gBufBlend    = makeBlend(gBufAttachs.data(), 4);
  VkPipelineColorBlendStateCreateInfo opaqueBlend  = makeBlend(opaqueArr.data(), 1);
  VkPipelineColorBlendStateCreateInfo addBlend     = makeBlend(additiveArr.data(), 1);
  VkPipelineColorBlendStateCreateInfo overlayBlend = makeBlend(overlayArr.data(), 1);
  VkPipelineColorBlendStateCreateInfo noColorBlend = makeBlend(noColorArr.data(), 1);

  // ---- dynamic states -------------------------------------------------------
  std::array<VkDynamicState, 2> dynStates     = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  std::array<VkDynamicState, 3> wfDynStates   = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                   VK_DYNAMIC_STATE_LINE_WIDTH};
  VkPipelineDynamicStateCreateInfo dynState{};
  dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
  dynState.pDynamicStates    = dynStates.data();

  VkPipelineDynamicStateCreateInfo wfDynState{};
  wfDynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  wfDynState.dynamicStateCount = static_cast<uint32_t>(wfDynStates.size());
  wfDynState.pDynamicStates    = wfDynStates.data();

  // ---- depth/stencil states -------------------------------------------------
  VkPipelineDepthStencilStateCreateInfo depthPrepassDS{};
  depthPrepassDS.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthPrepassDS.depthTestEnable  = VK_TRUE;
  depthPrepassDS.depthWriteEnable = VK_TRUE;
  depthPrepassDS.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

  VkPipelineDepthStencilStateCreateInfo gBufDS     = depthPrepassDS;
  gBufDS.depthWriteEnable = VK_FALSE;
  // Replaying geometry after the depth prepass with an exact compare can
  // drop fragments on detailed meshes due to tiny rasterization/precision
  // differences between passes. Reverse-Z keeps nearer fragments at larger
  // depth values, so GREATER_OR_EQUAL preserves the prepass rejection while
  // allowing numerically equivalent fragments to shade.
  gBufDS.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

  VkPipelineDepthStencilStateCreateInfo noDS{};
  noDS.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

  VkPipelineDepthStencilStateCreateInfo normalLineDS = depthPrepassDS;
  normalLineDS.depthWriteEnable = VK_FALSE;

  VkPipelineDepthStencilStateCreateInfo wfDepthDS    = normalLineDS;
  VkPipelineDepthStencilStateCreateInfo wfNoDepthDS  = noDS;

  VkPipelineDepthStencilStateCreateInfo stencilDS = depthPrepassDS;
  stencilDS.depthWriteEnable  = VK_FALSE;
  stencilDS.stencilTestEnable = VK_TRUE;
  stencilDS.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                     VK_STENCIL_OP_DECREMENT_AND_WRAP,
                     VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};
  stencilDS.back  = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                     VK_STENCIL_OP_INCREMENT_AND_WRAP,
                     VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};

  VkPipelineDepthStencilStateCreateInfo pointLightDS = noDS;
  pointLightDS.stencilTestEnable = VK_TRUE;
  pointLightDS.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                        VK_STENCIL_OP_KEEP,
                        VK_COMPARE_OP_NOT_EQUAL, 0xff, 0x00, 0};
  pointLightDS.back  = pointLightDS.front;

  VkPipelineDepthStencilStateCreateInfo transparentDS = depthPrepassDS;
  transparentDS.depthWriteEnable = VK_FALSE;

  // ---- push constant ranges -------------------------------------------------
  VkPushConstantRange scenePCR{};
  scenePCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  scenePCR.size       = sizeof(BindlessPushConstants);

  VkPushConstantRange lightPCR{};
  lightPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  lightPCR.size       = sizeof(LightPushConstants);

  VkPushConstantRange postPCR{};
  postPCR.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  postPCR.size       = sizeof(PostProcessPushConstants);

  VkPushConstantRange wfPCR{};
  wfPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  wfPCR.size       = sizeof(WireframePushConstants);

  VkPushConstantRange snPCR{};
  snPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT;
  snPCR.size       = sizeof(SurfaceNormalPushConstants);

  VkPushConstantRange nvPCR{};
  nvPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT;
  nvPCR.size       = sizeof(NormalValidationPushConstants);

  VkPushConstantRange shadowPCR{};
  shadowPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  shadowPCR.size       = sizeof(ShadowPushConstants);

  VkPushConstantRange tiledLightPCR{};
  tiledLightPCR.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  tiledLightPCR.size       = sizeof(TiledLightingPushConstants);

  // ---- pipeline layouts -----------------------------------------------------
  PipelineLayouts layouts;
  layouts.scene = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene}, {snPCR});
  layouts.transparent = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene, descriptorLayouts.light, descriptorLayouts.oit},
      {scenePCR});
  layouts.lighting = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.lighting, descriptorLayouts.light}, {lightPCR});
  layouts.tiledLighting = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.lighting, descriptorLayouts.tiled}, {tiledLightPCR});
  layouts.shadow = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene, descriptorLayouts.shadow}, {shadowPCR});
  layouts.postProcess = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.postProcess, descriptorLayouts.oit}, {postPCR});
  layouts.wireframe = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene}, {wfPCR});
  layouts.normalValidation = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene}, {nvPCR});
  layouts.surfaceNormal = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene}, {snPCR});

  // ---- base pipeline create info --------------------------------------------
  // scene geometry base (depth prepass / gbuffer)
  VkGraphicsPipelineCreateInfo scenePCI{};
  scenePCI.sType                 = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  scenePCI.pVertexInputState     = &posTexInput;
  scenePCI.pInputAssemblyState   = &triAssembly;
  scenePCI.pViewportState        = &vpState;
  scenePCI.pRasterizationState   = &sceneRaster;
  scenePCI.pMultisampleState     = &msaa;
  scenePCI.pDepthStencilState    = &depthPrepassDS;
  scenePCI.pColorBlendState      = &noBlend;
  scenePCI.pDynamicState         = &dynState;
  scenePCI.layout                = layouts.scene;
  scenePCI.renderPass            = renderPasses.depthPrepass;

  // fullscreen quad base (lighting passes)
  VkGraphicsPipelineCreateInfo fsPCI{};
  fsPCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  fsPCI.pVertexInputState   = &emptyVertexInput;
  fsPCI.pInputAssemblyState = &triAssembly;
  fsPCI.pViewportState      = &vpState;
  fsPCI.pRasterizationState = &fullscreenRaster;
  fsPCI.pMultisampleState   = &msaa;
  fsPCI.pDepthStencilState  = &noDS;
  fsPCI.pColorBlendState    = &opaqueBlend;
  fsPCI.pDynamicState       = &dynState;
  fsPCI.layout              = layouts.lighting;
  fsPCI.renderPass          = renderPasses.lighting;

  // scene mesh base (transparent / debug variants)
  VkGraphicsPipelineCreateInfo meshPCI{};
  meshPCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  meshPCI.pVertexInputState   = &fullVertexInput;
  meshPCI.pInputAssemblyState = &triAssembly;
  meshPCI.pViewportState      = &vpState;
  meshPCI.pRasterizationState = &sceneRaster;
  meshPCI.pMultisampleState   = &msaa;
  meshPCI.pDepthStencilState  = &transparentDS;
  meshPCI.pColorBlendState    = &noColorBlend;
  meshPCI.pDynamicState       = &dynState;
  meshPCI.layout              = layouts.transparent;
  meshPCI.renderPass          = renderPasses.lighting;

  // ---- create pipelines -----------------------------------------------------
  GraphicsPipelines pipelines;

  // Depth prepass
  scenePCI.stageCount = static_cast<uint32_t>(depthPrepassStages.size());
  scenePCI.pStages    = depthPrepassStages.data();
  pipelines.depthPrepass = pipelineManager_.createGraphicsPipeline(
      scenePCI, "depth_prepass_pipeline");

  VkGraphicsPipelineCreateInfo depthNoCullPCI = scenePCI;
  depthNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.depthPrepassNoCull = pipelineManager_.createGraphicsPipeline(
      depthNoCullPCI, "depth_prepass_no_cull_pipeline");

  // GBuffer
  VkGraphicsPipelineCreateInfo gBufPCI = scenePCI;
  gBufPCI.stageCount          = static_cast<uint32_t>(gBufferStages.size());
  gBufPCI.pStages             = gBufferStages.data();
  // The deferred G-buffer shader consumes normal and tangent attributes in
  // addition to position/UV, so it must use the full vertex layout.
  gBufPCI.pVertexInputState   = &fullVertexInput;
  gBufPCI.pDepthStencilState  = &gBufDS;
  gBufPCI.pColorBlendState    = &gBufBlend;
  gBufPCI.renderPass          = renderPasses.gBuffer;
  pipelines.gBuffer = pipelineManager_.createGraphicsPipeline(
      gBufPCI, "gbuffer_pipeline");

  VkGraphicsPipelineCreateInfo gBufNoCullPCI = gBufPCI;
  gBufNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.gBufferNoCull = pipelineManager_.createGraphicsPipeline(
      gBufNoCullPCI, "gbuffer_no_cull_pipeline");

  // Shadow depth
  VkPipelineRasterizationStateCreateInfo shadowRaster = sceneRaster;
  // Shadow cascades render with a positive-height viewport, so they preserve
  // glTF's native CCW winding in framebuffer space.
  shadowRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Keep shadow caster winding policy aligned with the scene passes. Imported
  // assets can contain reliefs and thin surfaces with mixed effective winding;
  // culling them in the shadow pass makes those details stop casting.
  shadowRaster.cullMode = VK_CULL_MODE_NONE;
  shadowRaster.depthBiasEnable         = VK_TRUE;
  // Negative bias for reverse-Z: pushes stored depth towards far (0.0),
  // making the shadow surface appear slightly farther from the light
  // to avoid self-shadowing.
  shadowRaster.depthBiasConstantFactor = -4.0f;
  shadowRaster.depthBiasSlopeFactor    = -1.5f;

  VkGraphicsPipelineCreateInfo sdPCI = scenePCI;
  sdPCI.stageCount          = static_cast<uint32_t>(sdStages.size());
  sdPCI.pStages             = sdStages.data();
  sdPCI.pVertexInputState   = &posTexInput;
  sdPCI.pInputAssemblyState = &triAssembly;
  sdPCI.pRasterizationState = &shadowRaster;
  sdPCI.pDepthStencilState  = &depthPrepassDS;
  sdPCI.pColorBlendState    = &noBlend;
  sdPCI.layout              = layouts.shadow;
  sdPCI.renderPass          = renderPasses.shadow;
  pipelines.shadowDepth = pipelineManager_.createGraphicsPipeline(
      sdPCI, "shadow_depth_pipeline");

  VkPipelineRasterizationStateCreateInfo shadowNoCullRaster = shadowRaster;
  shadowNoCullRaster.cullMode = VK_CULL_MODE_NONE;

  VkGraphicsPipelineCreateInfo sdNoCullPCI = sdPCI;
  sdNoCullPCI.pRasterizationState = &shadowNoCullRaster;
  pipelines.shadowDepthNoCull = pipelineManager_.createGraphicsPipeline(
      sdNoCullPCI, "shadow_depth_no_cull_pipeline");

  // Directional light
  fsPCI.stageCount = static_cast<uint32_t>(dirStages.size());
  fsPCI.pStages    = dirStages.data();
  pipelines.directionalLight = pipelineManager_.createGraphicsPipeline(
      fsPCI, "directional_light_pipeline");

  // Stencil volume
  VkGraphicsPipelineCreateInfo stencilPCI = fsPCI;
  stencilPCI.stageCount         = static_cast<uint32_t>(stencilStages.size());
  stencilPCI.pStages            = stencilStages.data();
  stencilPCI.pDepthStencilState = &stencilDS;
  stencilPCI.pColorBlendState   = &noColorBlend;
  pipelines.stencilVolume = pipelineManager_.createGraphicsPipeline(
      stencilPCI, "stencil_volume_pipeline");

  // Point light
  VkGraphicsPipelineCreateInfo pointPCI = fsPCI;
  pointPCI.stageCount         = static_cast<uint32_t>(pointStages.size());
  pointPCI.pStages            = pointStages.data();
  pointPCI.pDepthStencilState = &pointLightDS;
  pointPCI.pColorBlendState   = &addBlend;
  pipelines.pointLight = pipelineManager_.createGraphicsPipeline(
      pointPCI, "point_light_pipeline");

  // Point light stencil debug
  VkGraphicsPipelineCreateInfo pointDbgPCI = pointPCI;
  pointDbgPCI.stageCount       = static_cast<uint32_t>(pointDbgStages.size());
  pointDbgPCI.pStages          = pointDbgStages.data();
  pointDbgPCI.pColorBlendState = &opaqueBlend;
  pipelines.pointLightStencilDebug = pipelineManager_.createGraphicsPipeline(
      pointDbgPCI, "point_light_stencil_debug_pipeline");

  // Transparent (OIT)
  pipelines.transparent = pipelineManager_.createGraphicsPipeline(
      meshPCI, "transparent_pipeline");

  VkGraphicsPipelineCreateInfo transparentNoCullPCI = meshPCI;
  transparentNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.transparentNoCull = pipelineManager_.createGraphicsPipeline(
      transparentNoCullPCI, "transparent_no_cull_pipeline");

  // Post process
  VkGraphicsPipelineCreateInfo postPCI = fsPCI;
  postPCI.stageCount  = static_cast<uint32_t>(postStages.size());
  postPCI.pStages     = postStages.data();
  postPCI.layout      = layouts.postProcess;
  postPCI.renderPass  = renderPasses.postProcess;
  pipelines.postProcess = pipelineManager_.createGraphicsPipeline(
      postPCI, "post_process_pipeline");

  // Geometry debug
  VkGraphicsPipelineCreateInfo dbgPCI = meshPCI;
  dbgPCI.stageCount           = static_cast<uint32_t>(dbgStages.size());
  dbgPCI.pStages              = dbgStages.data();
  dbgPCI.pVertexInputState    = &posOnlyInput;
  dbgPCI.pInputAssemblyState  = &pointAssembly;
  dbgPCI.pColorBlendState     = &overlayBlend;
  dbgPCI.pDepthStencilState   = &noDS;
  dbgPCI.layout               = layouts.scene;
  pipelines.geometryDebug = pipelineManager_.createGraphicsPipeline(
      dbgPCI, "geometry_debug_pipeline");

  // Normal validation
  VkGraphicsPipelineCreateInfo nvPCI = meshPCI;
  nvPCI.stageCount           = static_cast<uint32_t>(nvStages.size());
  nvPCI.pStages              = nvStages.data();
  nvPCI.pVertexInputState    = &fullVertexInput;
  nvPCI.pInputAssemblyState  = &triAssembly;
  nvPCI.pColorBlendState     = &overlayBlend;
  nvPCI.pDepthStencilState   = &normalLineDS;
  nvPCI.layout               = layouts.normalValidation;
  nvPCI.renderPass           = renderPasses.lighting;
  pipelines.normalValidation = pipelineManager_.createGraphicsPipeline(
      nvPCI, "normal_validation_pipeline");

  // Wireframe depth + wireframe no-depth
  VkGraphicsPipelineCreateInfo wfPCI = meshPCI;
  wfPCI.pInputAssemblyState = &triAssembly;
  wfPCI.pColorBlendState    = &overlayBlend;
  wfPCI.layout              = layouts.wireframe;
  wfPCI.renderPass          = renderPasses.lighting;

  if (wireframeRasterModeSupported_) {
    wfPCI.stageCount         = static_cast<uint32_t>(wfStages.size());
    wfPCI.pStages            = wfStages.data();
    wfPCI.pRasterizationState = &wfRaster;
    wfPCI.pDynamicState      = &wfDynState;
  } else {
    wfPCI.stageCount         = static_cast<uint32_t>(wfFbStages.size());
    wfPCI.pStages            = wfFbStages.data();
    wfPCI.pRasterizationState = &wfFbRaster;
    wfPCI.pDynamicState      = &dynState;
  }
  wfPCI.pDepthStencilState = &wfDepthDS;
  pipelines.wireframeDepth = pipelineManager_.createGraphicsPipeline(
      wfPCI, "wireframe_depth_pipeline");

  wfPCI.pDepthStencilState = &wfNoDepthDS;
  pipelines.wireframeNoDepth = pipelineManager_.createGraphicsPipeline(
      wfPCI, "wireframe_no_depth_pipeline");

  // Surface normal lines
  VkGraphicsPipelineCreateInfo snPCI = meshPCI;
  snPCI.stageCount           = static_cast<uint32_t>(snStages.size());
  snPCI.pStages              = snStages.data();
  snPCI.pVertexInputState    = &fullVertexInput;
  snPCI.pInputAssemblyState  = &triAssembly;
  snPCI.pRasterizationState  = &normalLineRaster;
  snPCI.pColorBlendState     = &overlayBlend;
  snPCI.pDepthStencilState   = &normalLineDS;
  snPCI.pDynamicState        = &wfDynState;
  snPCI.layout               = layouts.surfaceNormal;
  snPCI.renderPass           = renderPasses.lighting;
  pipelines.surfaceNormalLine = pipelineManager_.createGraphicsPipeline(
      snPCI, "surface_normal_line_pipeline");

  // Object normals debug
  VkGraphicsPipelineCreateInfo onPCI = meshPCI;
  onPCI.stageCount           = static_cast<uint32_t>(onStages.size());
  onPCI.pStages              = onStages.data();
  onPCI.pVertexInputState    = &posTexNormInput;
  onPCI.pColorBlendState     = &overlayBlend;
  onPCI.pDepthStencilState   = &normalLineDS;
  onPCI.layout               = layouts.scene;
  pipelines.objectNormalDebug = pipelineManager_.createGraphicsPipeline(
      onPCI, "object_normal_debug_pipeline");

  VkGraphicsPipelineCreateInfo onNoCullPCI = onPCI;
  onNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.objectNormalDebugNoCull = pipelineManager_.createGraphicsPipeline(
      onNoCullPCI, "object_normal_debug_no_cull_pipeline");

  // Light gizmo
  VkGraphicsPipelineCreateInfo lgPCI = fsPCI;
  lgPCI.stageCount          = static_cast<uint32_t>(lgStages.size());
  lgPCI.pStages             = lgStages.data();
  lgPCI.pVertexInputState   = &emptyVertexInput;
  lgPCI.pInputAssemblyState = &lineAssembly;
  lgPCI.pColorBlendState    = &overlayBlend;
  lgPCI.pDepthStencilState  = &noDS;
  lgPCI.layout              = layouts.lighting;
  lgPCI.renderPass          = renderPasses.lighting;
  pipelines.lightGizmo = pipelineManager_.createGraphicsPipeline(
      lgPCI, "light_gizmo_pipeline");

  // Tiled point light (fullscreen, additive blend, no stencil)
  VkGraphicsPipelineCreateInfo tlPCI = fsPCI;
  tlPCI.stageCount          = static_cast<uint32_t>(tlStages.size());
  tlPCI.pStages             = tlStages.data();
  tlPCI.pVertexInputState   = &emptyVertexInput;
  tlPCI.pColorBlendState    = &addBlend;
  tlPCI.pDepthStencilState  = &noDS;
  tlPCI.layout              = layouts.tiledLighting;
  tlPCI.renderPass          = renderPasses.lighting;
  pipelines.tiledPointLight = pipelineManager_.createGraphicsPipeline(
      tlPCI, "tiled_point_light_pipeline");

  // ---- destroy shader modules -----------------------------------------------
  const std::array<VkShaderModule, 39> modules = {
      depthPrepassVert, depthPrepassFrag,
      gBufferVert,      gBufferFrag,
      dirVert,          dirFrag,
      stencilVert,      stencilFrag,
      pointVert,        pointFrag,
      pointDbgVert,     pointDbgFrag,
      transVert,        transFrag,
      postVert,         postFrag,
      dbgVert,          dbgFrag,
      nvVert,           nvGeom,  nvFrag,
      wfVert,           wfFrag,
      wfFbVert,         wfFbGeom, wfFbFrag,
      snVert,           snGeom,  snFrag,
      onVert,           onFrag,
      lgVert,           lgFrag,
      sdVert,           sdFrag,
      tlVert,           tlFrag};
  for (VkShaderModule m : modules) {
    vkDestroyShaderModule(device_->device(), m, nullptr);
  }

  return {layouts, pipelines};
}

}  // namespace container::renderer
