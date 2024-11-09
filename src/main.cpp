// src/main.cpp
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <iostream>

int main() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW!" << std::endl;
        return -1;
    }

    std::cout << "GLFW initialized successfully." << std::endl;

    // Vulkan setup and any other operations can go here

    glfwTerminate();
    return 0;
}
