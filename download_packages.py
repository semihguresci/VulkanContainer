import os
import subprocess
import sys
from pathlib import Path


def bootstrap_vcpkg(vcpkg_dir: Path) -> None:
    """Run vcpkg bootstrap if the executable is missing."""
    vcpkg_path = vcpkg_dir / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    if vcpkg_path.exists():
        print(f"Found existing vcpkg binary at '{vcpkg_path}', skipping bootstrap.")
        return

    bootstrap_path = vcpkg_dir / ("bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh")
    if not bootstrap_path.exists():
        print(f"Error: bootstrap script not found at '{bootstrap_path}'")
        raise SystemExit(1)

    print("Bootstrapping vcpkg...")
    try:
        subprocess.run([str(bootstrap_path)], check=True)
        print("vcpkg bootstrap completed successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to bootstrap vcpkg: {e}")
        raise SystemExit(1)


def install_dependencies(vcpkg_executable: Path, vcpkg_json_path: Path, triplet: str | None = None) -> None:
    """Install dependencies with --recurse and optional triplet."""
    if not vcpkg_executable.exists():
        print(f"Error: vcpkg executable not found at '{vcpkg_executable}'")
        raise SystemExit(1)

    if not vcpkg_json_path.exists():
        print(f"Error: vcpkg.json file not found at '{vcpkg_json_path}'")
        raise SystemExit(1)

    command = [str(vcpkg_executable), "install", "--recurse"]

    if triplet:
        command.extend(["--triplet", triplet])

    print("Installing vcpkg dependencies...")
    try:
        subprocess.run(command, check=True)
        print("Dependencies installed successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to install dependencies: {e}")
        raise SystemExit(1)


def main() -> None:
    current_dir = Path(__file__).resolve().parent
    vcpkg_dir = current_dir / "vcpkg"
    vcpkg_executable = vcpkg_dir / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    vcpkg_json_path = current_dir / "vcpkg.json"

    triplet = os.environ.get(
        "VCPKG_DEFAULT_TRIPLET",
        "x64-windows" if os.name == "nt" else "x64-linux",
    )

    bootstrap_vcpkg(vcpkg_dir)
    install_dependencies(vcpkg_executable, vcpkg_json_path, triplet)


if __name__ == "__main__":
    main()
