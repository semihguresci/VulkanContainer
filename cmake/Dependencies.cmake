# cmake/Dependencies.cmake

add_library(VulkanDependencies INTERFACE)

find_package(Vulkan REQUIRED)

# Set up external directory
if(NOT DEFINED EXTERNAL_DIR)
    set(EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external" CACHE PATH "Directory for external dependencies")
endif()

include(cmake/DependenciesSettings.cmake)

# Find Vulkan with custom module
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(Vulkan_FOUND)
    message(STATUS "✅ Vulkan found - Version: ${Vulkan_VERSION}")
else()
    message(FATAL_ERROR "❌ Vulkan not found - Please install Vulkan SDK and set VULKAN_SDK")
endif()

find_package(VulkanMemoryAllocator CONFIG REQUIRED)

message(STATUS "---- VMA targets check ----")
if(TARGET GPUOpen::VulkanMemoryAllocator)
  message(STATUS "Found target: GPUOpen::VulkanMemoryAllocator")
endif()
if(TARGET VulkanMemoryAllocator::VulkanMemoryAllocator)
  message(STATUS "Found target: VulkanMemoryAllocator::VulkanMemoryAllocator")
endif()
if(TARGET VulkanMemoryAllocator)
  message(STATUS "Found target: VulkanMemoryAllocator")
endif()
message(STATUS "---------------------------")

set(VMA_TARGET "")
if(TARGET GPUOpen::VulkanMemoryAllocator)
  set(VMA_TARGET GPUOpen::VulkanMemoryAllocator)
elseif(TARGET VulkanMemoryAllocator::VulkanMemoryAllocator)
  set(VMA_TARGET VulkanMemoryAllocator::VulkanMemoryAllocator)
elseif(TARGET VulkanMemoryAllocator)
  set(VMA_TARGET VulkanMemoryAllocator)
else()
  message(FATAL_ERROR "VMA found but no known imported target name is available.")
endif()
get_target_property(_vma_includes ${VMA_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "VMA include dirs: ${_vma_includes}")
target_include_directories(VulkanDependencies INTERFACE
    ${Vulkan_INCLUDE_DIRS}
)


option(ENABLE_VULKAN_VALIDATION_LAYERS "Enable Vulkan validation layers" ON)

if(ENABLE_VULKAN_VALIDATION_LAYERS)
  # Try to locate the Khronos validation layer manifest
  set(_vulkan_sdk $ENV{VULKAN_SDK})

  set(_candidate_layer_dirs "")
  if(_vulkan_sdk)
    if(WIN32)
      list(APPEND _candidate_layer_dirs
        "${_vulkan_sdk}/Bin"
        "${_vulkan_sdk}/bin"
      )
    else()
      list(APPEND _candidate_layer_dirs
        "${_vulkan_sdk}/share/vulkan/explicit_layer.d"
        "${_vulkan_sdk}/etc/vulkan/explicit_layer.d"
        "${_vulkan_sdk}/lib/vulkan/layers"
      )
    endif()
  endif()

  # Also allow user override
  if(DEFINED Vulkan_LAYER_DIR)
    list(APPEND _candidate_layer_dirs "${Vulkan_LAYER_DIR}")
  endif()

  # Search for the JSON
  find_file(VK_KHRONOS_VALIDATION_JSON
    NAMES VkLayer_khronos_validation.json
    HINTS ${_candidate_layer_dirs}
    NO_DEFAULT_PATH
  )

  if(VK_KHRONOS_VALIDATION_JSON)
    get_filename_component(VK_LAYER_PATH_DIR "${VK_KHRONOS_VALIDATION_JSON}" DIRECTORY)
    message(STATUS "✅ Found validation layer manifest: ${VK_KHRONOS_VALIDATION_JSON}")
    message(STATUS "   Suggested VK_LAYER_PATH: ${VK_LAYER_PATH_DIR}")

    set(ENV{VK_LAYER_PATH} "${VK_LAYER_PATH_DIR}:$ENV{VK_LAYER_PATH}")
  else()
    message(WARNING
      "⚠️ ENABLE_VULKAN_VALIDATION_LAYERS=ON but VkLayer_khronos_validation.json was not found.\n"
      "   Install Vulkan SDK (LunarG) or your distro's vulkan-validation-layers package.\n"
      "   If installed, set VK_LAYER_PATH to the directory containing VkLayer_khronos_validation.json."
    )
  endif()
endif()


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
    Boost
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
    $<IF:$<TARGET_EXISTS:tinygltf::tinygltf>,${TINYGLTF_INCLUDE_DIRS},"">
    $<IF:$<TARGET_EXISTS:stb::stb>,${STB_INCLUDE_DIRS},"">
)

# Vulkan-related libraries
set(VULKAN_LIBS
    Vulkan::Vulkan
    volk::volk
    volk::volk_headers
    Vulkan::SafeStruct
    Vulkan::LayerSettings
    Vulkan::UtilityHeaders
    Vulkan::CompilerConfiguration
    $<IF:$<TARGET_EXISTS:Vulkan::Headers>,Vulkan::Headers,>
)

list(APPEND VULKAN_LIBS ${VMA_TARGET})

 set(SHADER_LIBS
      slang::slang 
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
    Boost::boost
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

# Print summary
message(STATUS "--------------------------------------------------")
message(STATUS "Dependency Configuration Summary:")
message(STATUS "Vulkan: ${Vulkan_VERSION}")
message(STATUS "slang :(${slang_VERSION})")
message(STATUS "spdlog: ${spdlog_VERSION}")
message(STATUS "--------------------------------------------------")