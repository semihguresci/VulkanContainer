include(FetchContent)

FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.3.296 
    SOURCE_DIR ${EXTERNAL_DIR}/vulkanheaders-src
    BINARY_DIR ${EXTERNAL_DIR}/vulkanheaders-build
)
FetchContent_MakeAvailable(VulkanHeaders)
