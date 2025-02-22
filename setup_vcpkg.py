import os
import subprocess

def clone_vcpkg():
    try:
        subprocess.run(["git", "clone", "https://github.com/microsoft/vcpkg.git"], check=True)
        print("vcpkg cloned successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to clone vcpkg: {e}")
        exit(1)

def bootstrap_vcpkg():
    vcpkg_dir = os.path.join("vcpkg")
    bootstrap_script = "bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh"
    bootstrap_path = os.path.join(vcpkg_dir, bootstrap_script)
    if not os.path.exists(bootstrap_path):
        print(f"Error: Bootstrap script '{bootstrap_script}' not found in '{vcpkg_dir}'.")
        exit(1)
    absolute_bootstrap_path = os.path.abspath(bootstrap_path)
    print(f"Bootstrapping vcpkg using absolute path: {absolute_bootstrap_path}...") 
    try:
        subprocess.run([absolute_bootstrap_path], check=True)
        print("vcpkg bootstrapped successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Failed to bootstrap vcpkg: {e}")
        exit(1)

def main():
    clone_vcpkg()
    bootstrap_vcpkg()
    print("vcpkg setup completed successfully.")

if __name__ == "__main__":
    main()