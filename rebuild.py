import os
import shutil
import subprocess
import argparse

# Define paths
root_dir = os.path.dirname(os.path.abspath(__file__))
build_dir = os.path.join(root_dir, "build")

# Parse command-line arguments
parser = argparse.ArgumentParser(description="Build and run tests for the project.")
parser.add_argument("--run-tests", action="store_true", help="Run tests after building the project.")
parser.add_argument("--external-dir", type=str, default=os.path.join(root_dir, "external"), help="Path to store third-party dependencies.")

args = parser.parse_args()

# Remove the existing build directory
print("Removing the build directory...")
shutil.rmtree(build_dir, ignore_errors=True)

# Recreate the build directory
print("Creating a fresh build directory...")
os.makedirs(build_dir, exist_ok=True)

external_dir = args.external_dir.replace("\\", "/")

# Configure the project using CMake
print("Configuring the project with CMake...")
subprocess.run(["cmake", "..", f"-DEXTERNAL_DIR={external_dir}"], cwd=build_dir, check=True)

# Build the project with specified options
print("Building the project...")
subprocess.run(
    ["cmake", "-S", root_dir, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release", "-DENABLE_TESTS=1"],
    check=True
)

# Build the main executable first
subprocess.run(["cmake", "--build", build_dir, "--target", "VulkanProject"], check=True)

# Build and run the tests if requested
if args.run_tests:
    # Build the tests
    subprocess.run(["cmake", "--build", build_dir, "--target", "build_tests"], check=True)

    # Run the tests
    subprocess.run(["cmake", "--build", build_dir, "--target", "run_tests"], check=True)

print("Rebuild complete.")
