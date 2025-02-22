import subprocess
import os
import sys

def download_vcpkg_dependencies(vcpkg_path, vcpkg_json_path, triplet=None):
    if not os.path.exists(vcpkg_path):
        print(f"Error: vcpkg executable not found at '{vcpkg_path}'.")
        sys.exit(1)

    if not os.path.exists(vcpkg_json_path):
        print(f"Error: vcpkg.json file not found at '{vcpkg_json_path}'.")
        sys.exit(1)

    command = [vcpkg_path, "integrate install"]

    if triplet:
        command.extend(["--triplet", triplet])

    print(f"Downloading vcpkg dependencies...")

    try:
        subprocess.run(command, check=True)
        print("vcpkg dependencies downloaded successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to download vcpkg dependencies: {e}")
        sys.exit(1)

def main():
    current_dir = os.path.abspath(os.path.dirname(__file__))
    vcpkg_dir = os.path.join(current_dir, "vcpkg")
    vcpkg_path = os.path.join(vcpkg_dir, "vcpkg.exe" if os.name == "nt" else "vcpkg")
    vcpkg_json_path = os.path.join(current_dir, "vcpkg.json")

    triplet = None

    download_vcpkg_dependencies(vcpkg_path, vcpkg_json_path, triplet)

if __name__ == "__main__":
    main()