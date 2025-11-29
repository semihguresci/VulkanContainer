import os
import subprocess
from pathlib import Path


def clone_vcpkg(destination: Path) -> None:
    """Clone the vcpkg repo if it does not already exist."""
    if destination.exists():
        print(f"vcpkg already present at '{destination}', skipping clone.")
        return

    try:
        subprocess.run([
            "git",
            "clone",
            "https://github.com/microsoft/vcpkg.git",
            str(destination),
        ], check=True)
        print("vcpkg cloned successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to clone vcpkg: {e}")
        raise SystemExit(1)


def bootstrap_vcpkg(vcpkg_dir: Path) -> None:
    """Bootstrap vcpkg if the executable has not been generated yet."""
    vcpkg_exe = vcpkg_dir / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    if vcpkg_exe.exists():
        print(f"Found existing vcpkg binary at '{vcpkg_exe}', skipping bootstrap.")
        return

    bootstrap_script = "bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh"
    bootstrap_path = vcpkg_dir / bootstrap_script
    if not bootstrap_path.exists():
        print(f"Error: Bootstrap script '{bootstrap_script}' not found in '{vcpkg_dir}'.")
        raise SystemExit(1)

    absolute_bootstrap_path = bootstrap_path.resolve()
    print(f"Bootstrapping vcpkg using absolute path: {absolute_bootstrap_path}...")
    try:
        subprocess.run([str(absolute_bootstrap_path)], check=True)
        print("vcpkg bootstrapped successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to bootstrap vcpkg: {e}")
        raise SystemExit(1)


def main() -> None:
    vcpkg_dir = Path("vcpkg")
    clone_vcpkg(vcpkg_dir)
    bootstrap_vcpkg(vcpkg_dir)
    print("vcpkg setup completed successfully.")


if __name__ == "__main__":
    main()
