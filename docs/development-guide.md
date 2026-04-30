# Development Guide

Before changing rendering, shader, coordinate, culling, or projection logic,
read [coordinate-conventions.md](coordinate-conventions.md). It is the source
of truth for right-handed coordinates, reverse-Z depth, viewport orientation,
front-face selection, UV/NDC conversion, and matrix upload rules.

For architecture and feature context, read:

- [architecture.md](architecture.md) for current ownership and frame flow.
- [refactoring-plan.md](refactoring-plan.md) for dependency cleanup, historical
  refactoring phases, and render graph direction.
- [lighting-system-improvement-plan.md](lighting-system-improvement-plan.md)
  for lighting, shadow, tiled culling, GTAO, GPU-driven rendering, and bloom
  rationale.

## Renderer Conventions

- Scene passes use a negative-height viewport and clockwise front faces.
- Shadow passes use a positive-height viewport and shadow-specific UV mapping.
- Depth is reverse-Z: near maps to `1.0`, far maps to `0.0`, and depth clears
  to `0.0`.
- Projection matrices must not hide a Vulkan Y flip.
- Fullscreen passes should derive UV from `SV_Position` or clearly document any
  NDC/UV conversion.

## Shader and C++ Layout Contracts

`shaders/lighting_structs.slang` and `include/Container/utility/SceneData.h`
together define GPU layout contracts. Keep field order, alignment, and array
sizes synchronized.

Shader structs duplicated across passes, such as scene `ObjectBuffer` data,
must remain compatible with the descriptor data written by C++. Prefer shared
shader includes for common logic and document any deliberate duplication.

## Commenting Guidance

Add comments where code depends on renderer conventions, Vulkan layout state, or
shader/C++ data layout coupling. Good comment targets include:

- viewport orientation and front-face assumptions
- reverse-Z depth reconstruction or comparison
- descriptor and push-constant layout coupling
- GPU-driven draw-list fallbacks
- barriers and image layout transitions
- pass dependencies in `FrameRecorder`

Avoid comments that only restate simple local code.
