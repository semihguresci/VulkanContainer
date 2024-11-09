if (NOT Vulkan_FOUND)
    if (DEFINED ENV{VULKAN_SDK})
        set(Vulkan_SDK_PATH "$ENV{VULKAN_SDK}")
        message(STATUS "Found Vulkan SDK at: ${Vulkan_SDK_PATH}")
    else()
        message(FATAL_ERROR "Vulkan SDK not found. Please set the VULKAN_SDK environment variable.")
    endif()

    # Add include directories and library directories for Vulkan
    set(Vulkan_INCLUDE_DIR "${Vulkan_SDK_PATH}/Include")
    set(Vulkan_LIBRARY_DIR "${Vulkan_SDK_PATH}/Lib")

    # Locate the Vulkan library
    if (WIN32)
        find_library(Vulkan_LIBRARY NAMES vulkan-1 HINTS "${Vulkan_LIBRARY_DIR}")
    elseif (UNIX)
        find_library(Vulkan_LIBRARY NAMES vulkan HINTS "${Vulkan_LIBRARY_DIR}")
    endif()

    if (NOT Vulkan_LIBRARY)
        message(FATAL_ERROR "Could not find Vulkan library in: ${Vulkan_LIBRARY_DIR}")
    endif()

    set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")
    set(Vulkan_LIBRARIES "${Vulkan_LIBRARY}")

    set(Vulkan_FOUND TRUE)
endif()

if (Vulkan_FOUND)
    include_directories(${Vulkan_INCLUDE_DIRS})
    set(Vulkan::Vulkan ${Vulkan_LIBRARIES})
endif()
