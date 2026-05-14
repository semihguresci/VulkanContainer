# MSAA

The renderer supports multisample anti-aliasing for the deferred raster scene
path. MSAA is implemented as private multisampled render targets for the scene
depth and G-buffer passes, with resolves back into the existing single-sample
images consumed by lighting, compute, readback, and post-processing.

## User Configuration

MSAA can be selected at startup:

```powershell
.\out\build\windows-release\VulkanSceneRenderer.exe --msaa 4
```

The requested value is clamped to the nearest supported sample count that is
available for both framebuffer color and depth attachments. Unsupported values
such as `3` round down to the nearest Vulkan sample flag before device clamping.

At runtime, the scene controls panel exposes an `MSAA` combo box. It lists only
the sample counts supported by the current physical device. Changing the value
queues a renderer resource rebuild at the start of the next frame after the
device is idle.

## Ownership

MSAA sample discovery and conversion helpers live in:

- `include/Container/renderer/core/RendererMsaa.h`
- `src/renderer/core/RendererMsaa.cpp`

`RendererFrontend` owns the active sample count, supported GUI options, and
runtime recreation path. `FrameResourceManager` owns the actual multisampled
attachments. `RenderPassManager` owns render-pass attachment descriptions and
resolve wiring. `GraphicsPipelineBuilder` applies the sample count to scene
depth and G-buffer pipelines.

## Resource Model

The graph-visible frame resources remain single-sample. These images keep their
existing descriptor, compute, and post-process contracts:

- scene depth/stencil
- albedo
- normal
- material
- emissive
- specular
- pick ID

When MSAA is enabled, `FrameResourceManager` additionally creates private
multisampled attachments:

- `depthStencilMsaa`
- `albedoMsaa`
- `normalMsaa`
- `materialMsaa`
- `emissiveMsaa`
- `specularMsaa`
- `pickIdMsaa`

Depth prepass framebuffers contain the MSAA depth attachment followed by the
single-sample depth resolve attachment. G-buffer framebuffers contain six MSAA
color attachments, MSAA depth, five single-sample color resolve attachments,
and single-sample depth. The graph-visible attachments are still the resolved
single-sample outputs.

## Render Passes

MSAA affects these passes:

- Depth prepass
- BIM depth prepass
- Scene G-buffer
- BIM G-buffer

The depth and G-buffer render passes use `vkCreateRenderPass2` when MSAA is
enabled so `VkSubpassDescriptionDepthStencilResolve` can be chained legally.
Single-sample render passes still use the existing `vkCreateRenderPass` path.

Color resolves are wired with `pResolveAttachments` for the float/UNORM
G-buffer targets. The integer pick ID target is not color-resolved, because
Vulkan does not allow normal multisample color resolves for integer formats.
Picking is handled by a later single-sample transparent-pick pass that clears
and repopulates the pick ID target from opaque and transparent draw plans.

Depth resolve mode is selected from device support. The preferred mode is
`VK_RESOLVE_MODE_MAX_BIT`, then `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` as fallback.
This preserves reverse-Z depth semantics better than choosing the nearest-depth
sample arbitrarily.

## Pipeline Sample Counts

Only pipelines that render into the multisampled scene attachments use the
active scene sample count:

- scene depth prepass
- BIM depth prepass
- scene G-buffer
- BIM G-buffer

Fullscreen, compute-fed, transparent, shadow, debug, GUI, and post-process
pipelines remain single-sample because they render into single-sample targets
or do not use raster multisampling.

## Render Graph Boundary

The current render graph does not model attachment sample counts or resolve
edges directly. That is intentional for the current implementation.

MSAA is treated as an implementation detail inside frame-resource creation,
framebuffer creation, render-pass creation, and pipeline creation. The graph
continues to see the same single-sample resource contracts it saw before MSAA:
G-buffer outputs and depth are available after the same passes, in the same
layouts, with the same consumers.

This avoids forcing every consumer of a graph image to understand whether the
producer used MSAA internally. It also avoids exposing transient implementation
attachments as public graph resources.

## Extending The Design

Add render graph support for MSAA only when a new feature needs graph-level
reasoning about sample counts or explicit resolve operations. Examples include:

- transient attachment allocation owned by the graph
- technique-specific sample counts in the same frame
- programmable/custom resolve passes
- graph validation that verifies sample compatibility between producers and
  consumers

If that happens, add sample metadata to graph resource declarations and model
resolve operations as first-class pass edges. Until then, keep multisampled
attachments private and publish only the resolved single-sample images.

## Testing

Focused coverage lives in
`tests/renderer/core/resource_pipeline_registry_tests.cpp`:

- frame resource registry preserves image sample-count metadata
- sample-count conversion works for Vulkan sample flags and user values
- requested MSAA values clamp against color/depth device support
- supported sample counts are listed in ascending GUI order

Useful verification commands:

```powershell
cmake --build out/build/windows-release --target resource_pipeline_registry_tests --config Release
.\out\build\windows-release\tests\resource_pipeline_registry_tests.exe
```

For compile coverage of the app entry point and all renderer targets:

```powershell
cmake --build out/build/windows-release --config Release
```
