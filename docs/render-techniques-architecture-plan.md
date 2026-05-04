# Multi-Technique Rendering Architecture Plan

Status: proposed architecture plan
Date: 2026-05-02
Scope: architectural changes needed to support deferred raster, forward raster,
ray tracing, path tracing, Gaussian splatting, and NeRF/radiance-field style
rendering in one renderer without turning the current frame recorder into a
larger monolith.

## Objective

The renderer should support multiple rendering algorithms as selectable
techniques:

- Deferred raster rendering, which is the current production path.
- Forward or Forward+ raster rendering.
- Hardware ray tracing.
- Progressive path tracing.
- Gaussian splatting.
- NeRF/radiance-field rendering.

The main architectural goal is to make the current deferred pipeline one
technique among several, not the central abstraction that every new algorithm
must fit into.

## Current State

The renderer already has useful foundations:

- Manager-based ownership under `RendererFrontend`.
- A render graph with pass/resource dependency metadata.
- Shared scene, material, light, camera, environment, culling, telemetry, and
  GUI systems.
- Slang shaders compiled to SPIR-V.
- GPU-driven raster draws and indirect culling.
- Deferred opaque rendering plus forward transparent/OIT rendering.

The main constraints are structural:

- `FrameRecorder` owns the concrete pass registration and command recording for
  one frame shape.
- `FrameRecordParams` is a large fixed contract for the current deferred path.
- `PipelineTypes` stores one fixed set of graphics pipeline handles.
- `FrameResources` assumes G-buffer, scene color, OIT, and framebuffer resources.
- `RenderPassId` and `RenderResourceId` are fixed enums for the current frame
  graph.
- Device setup does not yet negotiate Vulkan ray-tracing extensions/features.

These are good implementation details for the current renderer, but poor
extension points for path tracing, ray tracing, splatting, and radiance fields.

## Target Shape

Introduce a render-technique layer:

```cpp
enum class RenderTechniqueId {
  DeferredRaster,
  ForwardRaster,
  RayTracing,
  PathTracing,
  GaussianSplatting,
  RadianceField,
};

struct RenderTechniqueRequirements {
  bool requiresRaster{false};
  bool requiresCompute{false};
  bool requiresRayTracingPipeline{false};
  bool requiresAccelerationStructures{false};
  bool requiresRayQuery{false};
  bool progressive{false};
};

class RenderTechnique {
 public:
  virtual ~RenderTechnique() = default;

  [[nodiscard]] virtual RenderTechniqueId id() const = 0;
  [[nodiscard]] virtual RenderTechniqueRequirements requirements() const = 0;

  virtual void createResources(RenderSystemContext& context) = 0;
  virtual void destroyResources(RenderSystemContext& context) = 0;
  virtual void resize(RenderSystemContext& context, VkExtent2D extent) = 0;
  virtual void registerPasses(RenderGraphBuilder& graph) = 0;
  virtual void update(RenderSystemContext& context, const FrameInput& input) = 0;
  virtual void recordUi(container::ui::GuiManager& gui) = 0;
};
```

`RendererFrontend` should own a `RenderTechniqueRegistry`, choose the active
technique, and keep shared subsystems outside any single technique:

- Vulkan device, swapchain, command buffers, frame sync.
- Scene, ECS world, materials, textures, object buffers.
- Camera and camera buffers.
- Lighting, shadows, environment, exposure.
- GUI, telemetry, screenshots, resize handling.

Each technique owns its algorithm-specific resources, pipelines, graph passes,
and settings.

## Core Architectural Changes

### 1. Technique Registry

Add:

```text
include/Container/renderer/RenderTechnique.h
include/Container/renderer/RenderTechniqueRegistry.h
src/renderer/RenderTechniqueRegistry.cpp
```

Responsibilities:

- Register available techniques at startup.
- Report capability requirements.
- Select the best fallback when a requested technique is unsupported.
- Expose user-facing labels for GUI and CLI/config.
- Preserve active technique settings across resize when possible.

Initial registry entries:

- `DeferredRasterTechnique`, backed by the current frame path.
- `ForwardRasterTechnique`, initially minimal.

Later entries:

- `RayTracingTechnique`
- `PathTracingTechnique`
- `GaussianSplattingTechnique`
- `RadianceFieldTechnique`

### 2. Render System Context

Introduce a narrow context passed into techniques instead of letting them depend
directly on all of `RendererFrontend`:

```cpp
struct RenderSystemContext {
  VulkanContextResult& ctx;
  container::gpu::PipelineManager& pipelineManager;
  container::gpu::AllocationManager& allocationManager;
  container::gpu::SwapChainManager& swapChainManager;
  CommandBufferManager& commandBufferManager;
  SceneGpuData& sceneGpuData;
  FrameResourceRegistry& frameResources;
  RendererTelemetry* telemetry{nullptr};
};
```

This prevents every new algorithm from adding more fields to
`RendererFrontend::OwnedSubsystems` and `FrameRecordParams`.

### 3. Frame Resource Registry

Replace the hardcoded deferred attachment bundle with a registry model.

Current `FrameResources` can remain for the deferred path temporarily, but the
target should be:

```cpp
enum class FrameImageUsage {
  ColorAttachment,
  DepthStencil,
  Sampled,
  Storage,
  TransferSrc,
  TransferDst,
};

struct FrameImageDesc {
  std::string name;
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkExtent2D extent{};
  VkImageUsageFlags usage{0};
  VkImageAspectFlags aspect{0};
  uint32_t mipLevels{1};
  uint32_t arrayLayers{1};
};
```

Deferred raster requests G-buffer attachments. Path tracing requests HDR output
and accumulation images. Gaussian splatting requests transient sort/bin buffers
and possibly storage images. NeRF requests output and occupancy/volume resources.

### 4. Pipeline Ownership

Stop extending one `GraphicsPipelines` struct for every algorithm.

Target pattern:

```cpp
class TechniquePipelineSet {
 public:
  virtual void create(RenderSystemContext& context) = 0;
  virtual void destroy(VkDevice device) = 0;
};
```

Per-technique examples:

- `DeferredRasterPipelines`
- `ForwardRasterPipelines`
- `RayTracingPipelines`
- `PathTracingPipelines`
- `SplattingPipelines`
- `RadianceFieldPipelines`

Keep shared helper builders, but avoid one global pipeline-output struct.

### 5. Render Graph Builder

Keep `RenderGraph` as the executor, but add a builder layer that supports
technique-local pass/resource names.

Current fixed enums are useful for tests and debug labels, but new algorithms
need dynamic extension. A transitional approach is:

- Keep existing `RenderPassId` for current raster passes.
- Add `TechniquePassId { technique, localId, debugName }`.
- Allow `RenderGraphBuilder` to register passes with stable string/debug IDs.
- Preserve dependency validation and telemetry naming.

Target examples:

```text
DeferredRaster:
  FrustumCull -> DepthPrepass -> GBuffer -> Lighting -> Bloom -> PostProcess

ForwardRaster:
  FrustumCull -> DepthPrepass -> ForwardLighting -> Bloom -> PostProcess

RayTracing:
  BuildOrUpdateTLAS -> RayTracePrimary -> Denoise -> PostProcess

PathTracing:
  BuildOrUpdateTLAS -> PathTraceSample -> Accumulate -> Denoise -> PostProcess

GaussianSplatting:
  SplatCull -> SplatSortOrBin -> SplatRaster -> Composite -> PostProcess

RadianceField:
  RayMarchField -> CompositeMeshes -> PostProcess
```

### 6. Shared Scene GPU Data

Add a renderer-independent facade over scene buffers:

```cpp
class SceneGpuData {
 public:
  VkBuffer vertexBuffer() const;
  VkBuffer indexBuffer() const;
  VkBuffer objectBuffer() const;
  VkBuffer materialBuffer() const;
  VkDescriptorSet sceneDescriptorSet(uint32_t imageIndex) const;

  uint64_t geometryRevision() const;
  uint64_t materialRevision() const;
  uint64_t transformRevision() const;
};
```

Raster techniques use it for draw commands. Ray/path tracing use it to build
bottom-level and top-level acceleration structures. Splatting and radiance-field
techniques can add separate providers while still compositing with mesh depth or
scene color.

### 7. Device Capability Negotiation

Extend Vulkan device creation to record capabilities:

```cpp
struct RendererDeviceCapabilities {
  bool rayTracingPipeline{false};
  bool accelerationStructure{false};
  bool rayQuery{false};
  bool descriptorIndexing{false};
  bool bufferDeviceAddress{false};
  bool timelineSemaphore{false};
};
```

Ray tracing and path tracing should require:

- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_tracing_pipeline`
- `VK_KHR_deferred_host_operations`
- `VK_KHR_buffer_device_address`
- `VK_KHR_spirv_1_4`
- `VK_KHR_shader_float_controls`

Ray query based hybrid paths can require:

- `VK_KHR_ray_query`
- `VK_KHR_acceleration_structure`

Unsupported techniques should be hidden or shown as unavailable in GUI.

## Technique-Specific Plans

### Deferred Raster

Purpose: preserve current output and make it the baseline technique.

Work:

- Move current `FrameRecorder::buildGraph()` pass registration into
  `DeferredRasterTechnique`.
- Move depth, G-buffer, lighting, OIT, bloom, exposure, and post-process record
  helpers into deferred-specific files.
- Keep current frame flow and tests intact during migration.

Acceptance:

- Visual output and render graph ordering match current behavior.
- Existing render graph tests still pass.
- `RendererFrontend` delegates graph construction to the active technique.

### Forward Raster / Forward+

Purpose: support direct material evaluation without G-buffer constraints and
provide a simpler real-time path for transparency-heavy scenes.

Work:

- Add forward color/depth attachments.
- Reuse tiled/clustered light buffers where possible.
- Add `ForwardRasterTechnique` and `ForwardRasterPipelines`.
- Support opaque, masked, transparent, and debug overlays.
- Reuse shadow, environment, and material descriptors.

Acceptance:

- Meshes render without G-buffer attachments.
- Forward path can share lighting/material code with transparent/OIT shaders.
- GUI can switch between deferred and forward without restarting.

### Ray Tracing

Purpose: hardware ray tracing for primary/secondary rays, reflections, shadows,
or debug/reference views.

Work:

- Add `AccelerationStructureManager`.
- Build BLAS from mesh vertex/index buffers.
- Build/update TLAS from object transforms.
- Add ray tracing pipeline builder: raygen, miss, closest-hit, any-hit.
- Add shader binding table allocation.
- Add storage image output and post-process composite.
- Add material/texture descriptor access from hit shaders.

Acceptance:

- Ray tracing mode renders primary visibility against existing mesh geometry.
- TLAS updates when transforms change.
- Geometry/material revisions trigger BLAS rebuilds only when needed.
- Unsupported hardware reports a clean unavailable state.

### Path Tracing

Purpose: progressive physically based rendering with accumulation.

Work:

- Build on `RayTracingTechnique`.
- Add accumulation image, sample count buffer, RNG seed/state.
- Reset accumulation on camera, scene, material, lighting, or resolution changes.
- Add progressive controls: max samples, bounces, Russian roulette, denoiser on/off.
- Add optional denoiser integration later.

Acceptance:

- Static camera accumulates progressively.
- Moving camera resets history.
- Output path shares post-process/tone mapping with other techniques.
- Telemetry reports sample index and GPU pass time.

### Gaussian Splatting

Purpose: render point/splat clouds as a first-class scene representation.

Data model:

```cpp
struct GaussianSplat {
  glm::vec3 position;
  float opacity;
  glm::vec4 rotation;
  glm::vec3 scale;
  uint32_t shOffset;
};
```

Work:

- Add `SplatAsset` and `SplatCloudGpuData`.
- Add loader support for chosen splat formats.
- Add compute culling and depth sorting or tile binning.
- Add splat raster/composite pass.
- Add controls for splat scale, opacity, SH degree, sorting mode.

Acceptance:

- Splat cloud renders independently and can composite with mesh scene color.
- Sort/bin buffers resize safely.
- Camera movement updates ordering without rebuilding static splat data.

### NeRF / Radiance Field

Purpose: support neural/radiance-field style volumetric rendering.

Work:

- Define a `RadianceFieldAsset` abstraction separate from mesh assets.
- Start with a pragmatic non-neural representation: density/color grids,
  sparse voxel grids, or baked feature grids.
- Add ray-march compute pass that writes HDR scene color and depth/alpha.
- Add occupancy grid or empty-space skipping.
- Add composition with mesh depth.

Acceptance:

- Radiance field can render to the same HDR output chain as other techniques.
- Mesh depth can occlude or be composited with field output.
- The data interface does not assume triangle geometry.

## Implementation Phases

### Phase 0: Planning And Guardrails

- Add this document.
- Add a short architecture note to `docs/architecture.md` once implementation
  starts.
- Do not add ray/path tracing directly into current `FrameRecorder`.

### Phase 1: Technique Skeleton

- Add `RenderTechniqueId`, `RenderTechnique`, `RenderTechniqueRegistry`.
- Add app/GUI setting for active technique.
- Register only `DeferredRasterTechnique` initially.
- Keep behavior identical.

### Phase 2: Move Deferred Path Behind Technique

- Move current graph registration out of `FrameRecorder`.
- Keep `FrameRecorder` as graph execution and shared utility only.
- Move deferred-specific record helpers into `DeferredRasterTechnique`.
- Preserve current tests.

### Phase 3: Resource And Pipeline Registries

- Add frame image/buffer registry.
- Split pipeline state by technique.
- Keep compatibility adapters for current deferred structs until migration is
  complete.

### Phase 4: Forward Technique

- Implement forward raster with shared scene/material/light data.
- Add GUI switch between deferred and forward.
- Add render graph tests for forward pass ordering.

### Phase 5: Ray Tracing Foundation

- Add capability negotiation and extension setup.
- Add `AccelerationStructureManager`.
- Add BLAS/TLAS build/update path.
- Add minimal ray tracing output pass.

### Phase 6: Path Tracing

- Add accumulation/history resources.
- Add progressive path tracing shaders.
- Add reset logic and sample telemetry.

### Phase 7: Splatting

- Add splat asset and GPU data path.
- Add cull/sort/bin/composite passes.
- Add first visual test scene.

### Phase 8: Radiance Fields

- Add radiance-field asset abstraction.
- Add ray-march compute path.
- Add mesh/field compositing.

### Phase 9: Validation

- Add CPU tests for technique registry, capability fallback, graph registration,
  and accumulation reset rules.
- Add optional GPU visual fixtures per technique.
- Add telemetry fields for active technique, pass count, and progressive sample
  state.

## File Ownership Guide

Expected areas:

| Area | Files |
|---|---|
| Technique API | `include/Container/renderer/RenderTechnique*.h`, `src/renderer/RenderTechnique*.cpp` |
| Existing deferred migration | `FrameRecorder.*`, new `DeferredRasterTechnique.*` |
| Forward raster | new `ForwardRasterTechnique.*`, forward shaders |
| Ray/path tracing | new `AccelerationStructureManager.*`, ray tracing pipeline builder, ray/path shaders |
| Splatting | new splat asset loader/GPU data manager, splat shaders |
| Radiance fields | new radiance-field asset/GPU data manager, ray-march shaders |
| Shared resources | `FrameResources.*`, `FrameResourceManager.*`, new resource registry |
| Device capabilities | `AppConfig.h`, `VulkanContextInitializer.*`, `VulkanDevice.*` |
| UI | `GuiManager.*`, debug/telemetry presenters |
| Tests | render graph tests, technique registry tests, visual regression fixtures |

## Risks And Mitigations

| Risk | Mitigation |
|---|---|
| `FrameRecorder` grows with every technique | Move pass registration and algorithm-specific recording into technique classes first. |
| One global pipeline struct becomes unmaintainable | Use technique-owned pipeline sets. |
| Ray tracing availability varies by GPU | Add explicit capability negotiation and clean fallback UI. |
| Resource lifetimes become unclear | Use a frame resource registry plus technique-owned persistent resources. |
| Path tracing history becomes stale | Track camera, scene, material, light, and resize revisions for accumulation reset. |
| Splatting and NeRF do not fit mesh scene data | Treat them as separate scene providers that composite through shared frame outputs. |

## Near-Term Acceptance Criteria

The first successful architectural milestone is not a new algorithm. It is:

- Current deferred rendering runs through `DeferredRasterTechnique`.
- `RendererFrontend` can select an active technique through a registry.
- Render graph registration is delegated to the technique.
- Current tests and screenshots remain stable.
- Adding `ForwardRasterTechnique` does not require adding fields to the deferred
  pipeline structs.

Only after this milestone should ray tracing, path tracing, splatting, or
radiance fields be implemented.
