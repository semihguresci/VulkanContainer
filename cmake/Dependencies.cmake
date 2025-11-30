# cmake/Dependencies.cmake

# Set up external directory
if(NOT DEFINED EXTERNAL_DIR)
    set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external" CACHE PATH "Directory for external dependencies")
endif()

include(cmake/DependenciesSettings.cmake)

# Find Vulkan with custom module
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(Vulkan REQUIRED)

if(Vulkan_FOUND)
    message(STATUS "✅ Vulkan found - Version: ${Vulkan_VERSION}")
else()
    message(FATAL_ERROR "❌ Vulkan not found - Please install Vulkan SDK and set VULKAN_SDK")
endif()

# Create consolidated dependencies target
add_library(VulkanDependencies INTERFACE)

# Define all required packages
set(REQUIRED_PACKAGES
    volk
    VulkanMemoryAllocator
    MaterialX
    glm
    fmt
    glfw3
    GTest
    imgui
    assimp
    cxxopts
    EnTT
    slang
    PNG
    libzip
    nlohmann_json
    spdlog
    yaml-cpp
    Eigen3
    VulkanUtilityLibraries
    libjpeg-turbo
)

# Find all packages
foreach(pkg IN LISTS REQUIRED_PACKAGES)
    find_package(${pkg} CONFIG REQUIRED)
endforeach()

# Handle TinyGLTF separately
find_package(tinygltf CONFIG QUIET)
if(NOT tinygltf_FOUND)
    find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")
    if(TINYGLTF_INCLUDE_DIRS)
        add_library(tinygltf INTERFACE)
        target_include_directories(tinygltf INTERFACE ${TINYGLTF_INCLUDE_DIRS})
        add_library(tinygltf::tinygltf ALIAS tinygltf)
        message(STATUS "✅ TinyGLTF found (manual)")
    else()
        message(WARNING "⚠️ TinyGLTF not found - some features may be disabled")
    endif()
endif()

find_path(STB_INCLUDE_DIRS "stb_image.h" PATH_SUFFIXES stb)
if (STB_INCLUDE_DIRS)
    add_library(stb INTERFACE)
    target_include_directories(stb INTERFACE ${STB_INCLUDE_DIRS})
    add_library(stb::stb ALIAS stb)
    message(STATUS "✅ STB found (manual)")
else()
  message(WARNING "⚠️ STB not found - some features may be disabled")
endif()




# Include directories
target_include_directories(VulkanDependencies INTERFACE
    ${Vulkan_INCLUDE_DIRS}
    ${Stb_INCLUDE_DIR}
    $<IF:$<TARGET_EXISTS:tinygltf::tinygltf>,${TINYGLTF_INCLUDE_DIRS},"">
    $<IF:$<TARGET_EXISTS:stb::stb>,${STB_INCLUDE_DIRS},"">
)

# Vulkan-related libraries
set(VULKAN_LIBS
    Vulkan::Vulkan
    volk::volk
    volk::volk_headers
    GPUOpen::VulkanMemoryAllocator
    Vulkan::SafeStruct
    Vulkan::LayerSettings
    Vulkan::UtilityHeaders
    Vulkan::CompilerConfiguration
)

 set(SHADER_LIBS
      slang::gfx 
      slang::slang 
      slang::slang-llvm 
      slang::slang-glslang
    )

# Graphics/rendering libraries
set(GRAPHICS_LIBS
    glfw
    glm::glm
    imgui::imgui
)

set(MATERIALX_LIBS
    MaterialXCore
    MaterialXFormat
    MaterialXGenShader
    MaterialXRender
)

# Utility libraries
set(UTILITY_LIBS
    fmt::fmt
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
    cxxopts::cxxopts
    EnTT::EnTT
    Eigen3::Eigen
)

# Media libraries
set(MEDIA_LIBS
    assimp::assimp
    PNG::PNG
    libzip::zip
    $<IF:$<TARGET_EXISTS:tinygltf::tinygltf>,tinygltf::tinygltf,"">
    $<IF:$<TARGET_EXISTS:stb::stb>,stb::stb,"">
    $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
)
    
# Combine all libraries
target_link_libraries(VulkanDependencies INTERFACE
    ${VULKAN_LIBS}
    ${SHADER_LIBS}
    ${GRAPHICS_LIBS}
    ${UTILITY_LIBS}
    ${MEDIA_LIBS}
    ${MATERIALX_LIBS}
)

# Optional: Check presence of validation layer JSON
if(ENABLE_VULKAN_VALIDATION_LAYERS)
    if(EXISTS "${Vulkan_LAYER_DIR}/VkLayer_khronos_validation.json")
        message(STATUS "✅ Vulkan validation layers available at: ${Vulkan_LAYER_DIR}")
        set(ENV{VK_LAYER_PATH} "${Vulkan_LAYER_DIR}")
    else()
        message(WARNING "⚠️ Vulkan validation layers requested but not found in: ${Vulkan_LAYER_DIR}")
    endif()
endif()

# Print summary
message(STATUS "--------------------------------------------------")
message(STATUS "Dependency Configuration Summary:")
message(STATUS "Vulkan: ${Vulkan_VERSION}")
message(STATUS "slang :(${slang_VERSION})")
message(STATUS "spdlog: ${spdlog_VERSION}")
message(STATUS "--------------------------------------------------")