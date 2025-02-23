if(WIN32)
    if (DEFINED ENV{VULKAN_SDK})
        set(Vulkan_SDK_PATH "$ENV{VULKAN_SDK}")
        message(STATUS "Found Vulkan SDK at: ${Vulkan_SDK_PATH}")
        set(CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}" ${CMAKE_PREFIX_PATH})
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

if(ENABLE_VULKAN_VALIDATION_LAYERS)

set(VulkanValidationLayers_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}") 
set(VulkanValidationLayers_LIBRARY_DIR "${Vulkan_LIBRARY_DIR}") 

find_library(VulkanValidationLayers_LIBRARY
    NAMES vulkan-1
    PATHS "${Vulkan_LIBRARY_DIRS}" "${Vulkan_SDK}/Lib"
    REQUIRED
)
if(VulkanValidationLayers_LIBRARY)
    set(VulkanValidationLayers_FOUND TRUE)
    message(STATUS "Vulkan Validation Layers found.")
    set(VulkanValidationLayers_INCLUDE_DIRS "${VulkanValidationLayers_INCLUDE_DIR}")
    set(VulkanValidationLayers_LIBRARIES "${VulkanValidationLayers_LIBRARY}")
    include_directories(${VulkanValidationLayers_INCLUDE_DIRS})
    set(VulkanValidationLayers::VulkanValidationLayers ${VulkanValidationLayers_LIBRARIES})

else()
    set(VulkanValidationLayers_FOUND FALSE)
    message(WARNING "Vulkan Validation Layers not found.")
endif()

endif()

elseif(UNIX)
   find_package(Vulkan REQUIRED)
   find_package(VulkanValidationLayers REQUIRED)
   if(Vulkan_FOUND)
        message(STATUS "Vulkan found!")
        message(STATUS "Vulkan include directory: ${Vulkan_INCLUDE_DIRS}")
        message(STATUS "Vulkan libraries: ${Vulkan_LIBRARIES}")
   else()
        message(FATAL_ERROR "Vulkan not found!")
    endif()
endif()



