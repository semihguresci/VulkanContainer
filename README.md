# VulkanContainer

The VulkanContainer project covers Vulkan features on both Windows and Linux platforms.

## Dependency management with vcpkg
- Dependencies are declared in `vcpkg.json` and resolved in manifest mode.
- Run `setup_vcpkg.py` once to clone and bootstrap vcpkg; reruns now skip work when the repository and binary already exist.
- Use `download_packages.py` to install dependencies. It respects `VCPKG_DEFAULT_TRIPLET` so you can align installs with your build triplet (defaults to `x64-linux` on Linux and `x64-windows` on Windows).
- CMake presets set `VCPKG_DEFAULT_BINARY_CACHE` to `.vcpkg-cache` and enable `binarycaching`, so repeated builds reuse compiled ports. Override the cache location with the environment variable if desired.

CMakeLists are also maintained to build the project.
