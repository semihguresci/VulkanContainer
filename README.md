# VulkanSceneRenderer

VulkanSceneRenderer is a C++23 Vulkan renderer for real-time scene rendering,
glTF, BIM, and USD content, physically based materials, shadows,
GPU culling, and debug visualization.

The CMake project and build targets use `VulkanSceneRenderer`. The public
include root remains `include/Container`, and the source namespace remains
`container::`, to avoid a broad source-level API rename.

## Quick Start

Windows release:

```powershell
cmake --preset windows-release
cmake --build out/build/windows-release --target VulkanSceneRenderer --config Release
```

Run tests:

```powershell
ctest --test-dir out/build/windows-release --output-on-failure
```

Run with a BIM sidecar model:

```powershell
$bim = "models\buildingSMART-IFC5-development\examples\Hello Wall\hello-wall.ifcx"
.\out\build\windows-release\VulkanSceneRenderer.exe --bim-model $bim
```

The sidecar path also accepts USD, USDA, USDC, and USDZ mesh files through the
TinyUSDZ-backed importer:

```powershell
$usd = "models\my_ascii_mesh.usda"
.\out\build\windows-release\VulkanSceneRenderer.exe --bim-model $usd
```

Download the OpenUSD sample models with CMake:

```powershell
cmake --build out/build/windows-release --target download_usd_models --config Release
```

## Documentation

- [Project overview](docs/project-overview.md) - features, repository layout,
  and asset organization.
- [Architecture](docs/architecture.md) - runtime ownership, frame flow, and
  subsystem boundaries.
- [Build and test](docs/build-and-test.md) - requirements, presets, build
  commands, helper scripts, and known test status.
- [Development guide](docs/development-guide.md) - renderer conventions,
  shader/C++ layout contracts, and commenting guidance.
- [Renderer telemetry](docs/renderer-telemetry.md) - live frame timing,
  GPU query backends, render graph metrics, and validation commands.
- [Coordinate conventions](docs/coordinate-conventions.md) - source of truth
  for coordinate systems, reverse-Z depth, viewports, culling, and matrix rules.
- [Lighting system plan](docs/lighting-system-improvement-plan.md) - lighting,
  shadows, tiled culling, GTAO, GPU-driven rendering, and bloom rationale.
- [Refactoring plan](docs/refactoring-plan.md) - ownership boundaries,
  dependency cleanup, and render graph direction.
