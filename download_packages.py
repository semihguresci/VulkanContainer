import subprocess
import os
import sys

def bootstrap_vcpkg(vcpkg_path):
    """Run vcpkg bootstrap to initialize the installation"""
    bootstrap_path = os.path.join(os.path.dirname(vcpkg_path), "bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh")
    
    if not os.path.exists(bootstrap_path):
        print(f"Error: bootstrap script not found at '{bootstrap_path}'")
        sys.exit(1)

    print("Bootstrapping vcpkg...")
    try:
        subprocess.run([bootstrap_path], check=True)
        print("vcpkg bootstrap completed successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to bootstrap vcpkg: {e}")
        sys.exit(1)

def install_dependencies(vcpkg_path, vcpkg_json_path, triplet=None):
    """Install dependencies with --recurse and triplet support"""
    if not os.path.exists(vcpkg_path):
        print(f"Error: vcpkg executable not found at '{vcpkg_path}'")
        sys.exit(1)

    if not os.path.exists(vcpkg_json_path):
        print(f"Error: vcpkg.json file not found at '{vcpkg_json_path}'")
        sys.exit(1)

    command = [vcpkg_path, "install", "--recurse"]

    if triplet:
        command.extend(["--triplet", triplet])

    print("Installing vcpkg dependencies...")
    try:
        subprocess.run(command, check=True)
        print("Dependencies installed successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to install dependencies: {e}")
        sys.exit(1)

def main():
    current_dir = os.path.abspath(os.path.dirname(__file__))
    vcpkg_dir = os.path.join(current_dir, "vcpkg")
    vcpkg_path = os.path.join(vcpkg_dir, "vcpkg.exe" if os.name == "nt" else "vcpkg")
    vcpkg_json_path = os.path.join(current_dir, "vcpkg.json")

    # Default triplet (change as needed)
    triplet = "x64-windows" if os.name == "nt" else "x64-linux"

    # First bootstrap vcpkg
    bootstrap_vcpkg(vcpkg_path)
    
    # Then install dependencies
    install_dependencies(vcpkg_path, vcpkg_json_path, triplet)

if __name__ == "__main__":
    main()