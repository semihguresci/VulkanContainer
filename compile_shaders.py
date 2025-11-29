import os
import subprocess
import sys
from typing import Dict, List, Tuple


SLANG_TARGETS: List[Tuple[str, str, str]] = [
    ("vsMain", "vertex", "vert"),
    ("psMain", "fragment", "frag"),
]

PROFILES: Dict[str, str] = {
    "vertex": "vs_6_0",
    "fragment": "ps_6_0",
}


def compile_slang_shader(shader_path: str, output_dir: str) -> None:
    """
    Compile a Slang shader with predefined entry points to SPIR-V.

    Each shader file is expected to expose a vertex entry point named
    `vsMain` and a fragment entry point named `psMain`.
    """
    print(f"Compiling Slang shader: {shader_path}")

    os.makedirs(output_dir, exist_ok=True)
    shader_name, _ = os.path.splitext(os.path.basename(shader_path))

    for entry, stage, suffix in SLANG_TARGETS:
        profile = PROFILES[stage]
        output_file = os.path.join(output_dir, f"{shader_name}.{suffix}.spv")
        command = [
            "slangc",
            shader_path,
            "-target",
            "spirv",
            "-stage",
            stage,
            "-profile",
            profile,
            "-entry",
            entry,
            "-o",
            output_file,
        ]

        print(f"Running: {' '.join(command)}")
        try:
            subprocess.run(command, check=True)
            print(f"Successfully compiled {entry} to {output_file}")
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                f"Failed to compile entry '{entry}' from {shader_path}"
            ) from exc


def compile_shaders(input_dir: str, output_dir: str) -> None:
    """Compiles all Slang shaders from input_dir to output_dir."""
    print(f"Compiling shaders from directory: {input_dir} to {output_dir}")

    for root, _, files in os.walk(input_dir):
        for file in files:
            if file.endswith(".slang"):
                shader_path = os.path.join(root, file)
                relative_path = os.path.relpath(root, input_dir)
                output_subdir = os.path.join(output_dir, relative_path)
                compile_slang_shader(shader_path, output_subdir)


def main():
    if len(sys.argv) != 3:
        print("Usage: python compile_shaders.py <input_dir> <output_dir>")
        sys.exit(1)

    input_dir = sys.argv[1]
    output_dir = sys.argv[2]

    if not os.path.isdir(input_dir):
        print(f"Error: Input directory '{input_dir}' does not exist.")
        sys.exit(1)

    compile_shaders(input_dir, output_dir)


if __name__ == "__main__":
    main()
