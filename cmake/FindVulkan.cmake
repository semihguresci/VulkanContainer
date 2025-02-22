if(WIN32)
    if (DEFINED ENV{VULKAN_SDK})
        set(Vulkan_SDK_PATH "$ENV{VULKAN_SDK}")
        message(STATUS "Found Vulkan SDK at: ${Vulkan_SDK_PATH}")
    else()
        message(FATAL_ERROR "Vulkan SDK not found. Please set the VULKAN_SDK environment variable.")
    endif()

    # Add include directories and library directories for Vulkan
    set(Vulkan_INCLUDE_DIR "${Vulkan_SDK_PATH}/Include")
    set(Vulkan_LIBRARY_DIR "${Vulkan_SDK_PATH}/Lib")
    find_library(Vulkan_LIBRARY NAMES vulkan-1 HINTS "${Vulkan_LIBRARY_DIR}")
  
    set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")
    set(Vulkan_LIBRARIES "${Vulkan_LIBRARY}")
    set(Vulkan_FOUND TRUE)

if (Vulkan_FOUND)
    include_directories(${Vulkan_INCLUDE_DIRS})
    set(Vulkan::Vulkan ${Vulkan_LIBRARIES})
endif()

elseif(UNIX)
   find_package(Vulkan REQUIRED)
    if(Vulkan_FOUND)
        message(STATUS "Vulkan found!")
        message(STATUS "Vulkan include directory: ${Vulkan_INCLUDE_DIRS}")
        message(STATUS "Vulkan libraries: ${Vulkan_LIBRARIES}")
    else()
        message(FATAL_ERROR "Vulkan not found!")
    endif()
endif()



