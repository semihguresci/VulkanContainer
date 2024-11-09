import os
import shutil
import subprocess

# Define paths
root_dir = os.path.dirname(os.path.abspath(__file__))
build_dir = os.path.join(root_dir, "build")

# Remove the existing build directory
print("Removing the build directory...")
shutil.rmtree(build_dir, ignore_errors=True)

# Recreate the build directory
print("Creating a fresh build directory...")
os.makedirs(build_dir, exist_ok=True)

# Configure the project using CMake
print("Configuring the project with CMake...")
subprocess.run(["cmake", ".."], cwd=build_dir, check=True)

# Build the project with specified options
print("Building the project...")
subprocess.run(
    ["cmake", "-S", root_dir, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release", "-DENABLE_TESTS=1"],
    check=True
)

# Build the main executable first
subprocess.run(["cmake", "--build", build_dir, "--target", "VulkanProject"], check=True)

# Build the tests
subprocess.run(["cmake", "--build", build_dir, "--target", "build_tests"], check=True)

# Run the tests
subprocess.run(["cmake", "--build", build_dir, "--target", "run_tests"], check=True)

print("Rebuild complete.")
