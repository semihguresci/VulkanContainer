import os
import subprocess
import argparse

root_dir = os.path.dirname(os.path.abspath(__file__))

parser = argparse.ArgumentParser(description="Build and run tests for the project.")
parser.add_argument("--run-tests", action="store_true", help="Run tests after building the project.")
parser.add_argument("--preset", type=str, default="windows-release",
                    help="CMake configure preset to use (windows-release/linux-release/etc.)")

args = parser.parse_args()

preset = args.preset

# Configure using preset
print(f"Configuring with preset '{preset}'...")
subprocess.run(["cmake", "--preset", preset], cwd=root_dir, check=True)

# Figure out build dir from preset convention
build_dir = os.path.join(root_dir, "out", "build", preset)

print("Building the project...")
subprocess.run(["cmake", "--build", build_dir, "--config", "Release"], check=True)

# Build main executable (correct target name)
subprocess.run(["cmake", "--build", build_dir, "--target", "VulkanContainer", "--config", "Release"], check=True)

if args.run_tests:
    subprocess.run(["cmake", "--build", build_dir, "--target", "build_tests", "--config", "Release"], check=True)
    subprocess.run(["cmake", "--build", build_dir, "--target", "run_tests", "--config", "Release"], check=True)

print("Rebuild complete.")
