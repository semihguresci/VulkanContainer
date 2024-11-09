#include <gtest/gtest.h>
#include <vulkan/vulkan.h>
#include <iostream>

VkInstance CreateVulkanInstance() {
    VkInstance instance;

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan Unit Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

   VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    createInfo.enabledLayerCount = 1;
    createInfo.ppEnabledLayerNames = &validationLayer;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance");
    }

    return instance;
}

TEST(VulkanTest, CreateInstance) {
    VkInstance instance = VK_NULL_HANDLE;

    try {
        instance = CreateVulkanInstance();
        ASSERT_NE(instance, VK_NULL_HANDLE) << "Failed to create Vulkan instance";
    } catch (const std::runtime_error& e) {
        FAIL() << e.what();
    }

    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }
}

TEST(VulkanTest, EnumeratePhysicalDevices) {
    VkInstance instance = VK_NULL_HANDLE;

    try {
        instance = CreateVulkanInstance();

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        ASSERT_GT(deviceCount, 0) << "No physical devices found";

        std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());
        ASSERT_GT(deviceCount, 0) << "Failed to retrieve physical devices";
    } catch (const std::runtime_error& e) {
        FAIL() << e.what();
    }

    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
