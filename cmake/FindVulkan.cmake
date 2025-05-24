# Cross-platform FindVulkan.cmake
# Supports system Vulkan SDK on Windows and Linux

# Allow user override or detect environment
set(VULKAN_SDK_PATH "$ENV{VULKAN_SDK}" CACHE PATH "Path to Vulkan SDK")

if(WIN32)
    if(NOT VULKAN_SDK_PATH)
        message(FATAL_ERROR "VULKAN_SDK environment variable not set. Please install the Vulkan SDK.")
    endif()

    set(Vulkan_INCLUDE_DIR "${VULKAN_SDK_PATH}/Include")
    set(Vulkan_LIBRARY "${VULKAN_SDK_PATH}/Lib/vulkan-1.lib")
    set(Vulkan_LAYER_DIR "${VULKAN_SDK_PATH}/etc/vulkan/explicit_layer.d")

elseif(UNIX)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(VULKAN REQUIRED vulkan)
        set(Vulkan_INCLUDE_DIR "${VULKAN_INCLUDE_DIRS}")
        set(Vulkan_LIBRARY "${VULKAN_LIBRARIES}")
    endif()

    if(NOT Vulkan_INCLUDE_DIR)
        find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h PATH_SUFFIXES include)
    endif()

    if(NOT Vulkan_LIBRARY)
        find_library(Vulkan_LIBRARY NAMES vulkan PATH_SUFFIXES lib)
    endif()

    if(VULKAN_SDK_PATH)
        list(APPEND Vulkan_INCLUDE_DIR "${VULKAN_SDK_PATH}/include")
        list(APPEND Vulkan_LIBRARY "${VULKAN_SDK_PATH}/lib/libvulkan.so")
    endif()

    set(Vulkan_LAYER_DIR "/etc/vulkan/explicit_layer.d")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Vulkan DEFAULT_MSG Vulkan_LIBRARY Vulkan_INCLUDE_DIR)

if(Vulkan_FOUND)
    set(Vulkan_LIBRARIES ${Vulkan_LIBRARY})
    set(Vulkan_INCLUDE_DIRS ${Vulkan_INCLUDE_DIR})
    include_directories(${Vulkan_INCLUDE_DIR})
    message(STATUS "Vulkan found: ${Vulkan_LIBRARY}")
    message(STATUS "Vulkan include dir: ${Vulkan_INCLUDE_DIR}")

    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${Vulkan_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
        )
    endif()
else()
    message(FATAL_ERROR "Vulkan not found")
endif()