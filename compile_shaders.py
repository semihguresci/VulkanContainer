import os
import subprocess

# Define base directories
base_dir = os.getcwd()
shader_dir = os.path.join(base_dir, "shaders")
test_shader_dir = os.path.join(base_dir, "tests", "shaders")
output_dir = os.path.join(base_dir, "shaders")
test_output_dir = os.path.join(base_dir, "tests", "shaders")

# Ensure the output directories exist
os.makedirs(output_dir, exist_ok=True)
os.makedirs(test_output_dir, exist_ok=True)

# Function to compile shaders
def compile_shaders(input_dir, output_dir):
    print(f"Compiling shaders from directory: {input_dir}")
    
    # Walk through input directory and find shader files
    for root, _, files in os.walk(input_dir):
        for file in files:
            if file.endswith((".glsl", ".vert", ".frag", ".geom", ".tesc", ".tese", ".comp", ".mesh")):
                shader_path = os.path.join(root, file)
                print(f"Found shader: {shader_path}")

                # Determine output subdirectory and filename
                relative_path = os.path.relpath(root, input_dir)
                output_subdir = os.path.join(output_dir, relative_path)
                os.makedirs(output_subdir, exist_ok=True)

                output_file = os.path.join(output_subdir, f"{file}.spv")

                # Compile shader to SPIR-V
                print(f"Compiling {shader_path} to {output_file}")
                try:
                    subprocess.run(["glslc", shader_path, "-o", output_file], check=True)
                    print(f"Successfully compiled {shader_path} to {output_file}")
                except subprocess.CalledProcessError:
                    print(f"Failed to compile {shader_path}")

# Compile shaders in main and test directories
compile_shaders(shader_dir, output_dir)
compile_shaders(test_shader_dir, test_output_dir)
