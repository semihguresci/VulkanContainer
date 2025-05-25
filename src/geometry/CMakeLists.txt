# Geometry component
add_library(VulkanContainer_geometry
    Mesh.cpp
    # Other geometry source files...
)

target_compile_features(VulkanContainer_geometry PUBLIC cxx_std_23)


target_include_directories(VulkanContainer_geometry PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include/
  ${Vulkan_INCLUDE_DIRS}

)

target_link_libraries(VulkanContainer_geometry PUBLIC
    VulkanContainer_utility
    VulkanDependencies
)