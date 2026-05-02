# Build and Test

## Requirements

- CMake 3.23 or newer.
- Ninja on `PATH`, or an explicit `CMAKE_MAKE_PROGRAM`.
- A C++23 compiler.
- Vulkan SDK, including `glslangValidator`, `slangc`, and validation layers.
- vcpkg with manifest dependencies from `vcpkg.json`.

Set `VCPKG_ROOT` before configuring. Keep machine-local overrides in an
untracked `CMakeUserPresets.json`.

On Windows, use a Visual Studio Developer Command Prompt so MSVC, Windows SDK,
and Ninja are discoverable.

## Build

Windows release:

```powershell
cmake --preset windows-release
cmake --build out/build/windows-release --target VulkanSceneRenderer --config Release
```

Windows debug:

```powershell
cmake --preset windows-debug
cmake --build out/build/windows-debug --target VulkanSceneRenderer --config Debug
```

If CMake reuses a stale compiler path, refresh the configure cache:

```powershell
cmake --fresh --preset windows-release
```

Linux presets are also provided:

```sh
cmake --preset linux-release
cmake --build out/build/linux-release --target VulkanSceneRenderer
```

The build compiles Slang shaders and copies or generates runtime assets through
the `shaders`, `copy_materials`, `copy_hdr`, and `generate_models` targets.
`ENABLE_SAMPLE_MODEL_DOWNLOAD` controls the pinned glTF Sample Models archive
download used by the default Sponza scene. `ENABLE_BIM_SAMPLE_MODEL_DOWNLOAD`
controls the buildingSMART IFC5-development fetch used by BIM importer tests
and manual IFC/IFCX validation. The BIM fetch checks the repository `main` ref
at build time and refreshes the local archive when that ref changes.
`ENABLE_USD_SAMPLE_MODEL_DOWNLOAD` downloads the OpenUSD Kitchen Set and
PointInstancedMedCity archives into `models/OpenUSD-Sample-Assets` for USD
loader validation and follow-up work.

To download or refresh only the USD samples through CMake:

```powershell
cmake --build out/build/windows-release --target download_usd_models --config Release
```

Run the renderer with a BIM sidecar model:

```powershell
$bim = "models\buildingSMART-IFC5-development\examples\Hello Wall\hello-wall.ifcx"
.\out\build\windows-release\VulkanSceneRenderer.exe --bim-model $bim
```

Use `--bim-import-scale` when a BIM source needs a unit or scene scale override.
The same sidecar route accepts `.usd`, `.usda`, `.usdc`, and `.usdz` mesh files.
`ENABLE_TINYUSDZ_USD_LOADER` controls the TinyUSDZ-backed importer; when it is
disabled, the fallback loader only handles lightweight ASCII USD/USDA meshes and
stored text-root USDZ packages.

## Tests

CPU tests are enabled by default through `ENABLE_TESTS`. Window/Vulkan tests are
opt-in with `ENABLE_WINDOWED_TESTS=ON`.

Build and run the suite:

```powershell
cmake --build out/build/windows-release --config Release
ctest --test-dir out/build/windows-release --output-on-failure
```

Known Windows release status:

- `glm_tests`, `ecs_tests`, `scene_graph_tests`, `rendering_convention_tests`,
  `dotbim_loader_tests`, `ifc_tessellated_loader_tests`, and
  `ifcx_loader_tests`, `usd_loader_tests`, and `render_graph_tests` pass in the
  current Windows release build.
- Window/Vulkan tests require a working Vulkan runtime and display environment.

The helper script configures, builds, and optionally runs CTest:

```powershell
python rebuild.py --preset windows-release --run-tests
```
