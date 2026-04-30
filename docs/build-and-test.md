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
download used by the default Sponza scene.

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
  and `render_graph_tests` pass in the current Windows release build.
- Window/Vulkan tests require a working Vulkan runtime and display environment.

The helper script configures, builds, and optionally runs CTest:

```powershell
python rebuild.py --preset windows-release --run-tests
```
