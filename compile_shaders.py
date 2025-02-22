import os
import subprocess

def compile_shaders(input_dir, output_dir):
    """Compiles shaders from input_dir to output_dir."""
    print(f"Compiling shaders from directory: {input_dir} to {output_dir}")

    os.makedirs(output_dir, exist_ok=True) 

    for root, _, files in os.walk(input_dir):
        for file in files:
            if file.endswith((".glsl", ".vert", ".frag", ".geom", ".tesc", ".tese", ".comp", ".mesh")):
                shader_path = os.path.join(root, file)
                print(f"Found shader: {shader_path}")

                relative_path = os.path.relpath(root, input_dir)
                output_subdir = os.path.join(output_dir, relative_path)
                os.makedirs(output_subdir, exist_ok=True)

                output_file = os.path.join(output_subdir, f"{file}.spv")

                print(f"Compiling {shader_path} to {output_file}")
                try:
                    subprocess.run(["glslc", shader_path, "-o", output_file], check=True)
                    print(f"Successfully compiled {shader_path} to {output_file}")
                except subprocess.CalledProcessError:
                    print(f"Failed to compile {shader_path}")


