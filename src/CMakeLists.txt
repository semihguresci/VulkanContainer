add_library(VulkanContainer_Core 
    ContainerCore.cpp
)

# Add subdirectories
add_subdirectory(geometry)
add_subdirectory(utility)

target_link_libraries(VulkanContainer_Core PUBLIC 
    VulkanDependencies    
    VulkanContainer_geometry  
    VulkanContainer_utility)


target_include_directories(VulkanContainer_Core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${Vulkan_INCLUDE_DIRS}
)
