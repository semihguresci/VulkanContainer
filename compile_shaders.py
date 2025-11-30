import os
import re
import subprocess
import sys
from typing import Dict, List, Tuple


SLANG_TARGETS: List[Tuple[str, str, str]] = [
    ("vsMain", "vertex", "vert"),
    ("psMain", "fragment", "frag"),
    ("csMain", "compute", "comp"),
    ("gsMain", "geometry", "geom"),
    ("hsMain", "hull", "tesc"),
    ("dsMain", "domain", "tese"),
    ("asMain", "amplification", "task"),
    ("msMain", "mesh", "mesh"),
]

PROFILES: Dict[str, str] = {
    "vertex": "vs_6_0",
    "fragment": "ps_6_0",
    "compute": "cs_6_0",
    "geometry": "gs_6_0",
    "hull": "hs_6_0",
    "domain": "ds_6_0",
    "amplification": "as_6_5",
    "mesh": "ms_6_5",
}


def compile_slang_shader(shader_path: str, output_dir: str) -> None:
    """
    Compile a Slang shader with predefined entry points to SPIR-V.

    The script checks for common entry-point names covering vertex,
    fragment, compute, geometry, hull (tessellation control), domain
    (tessellation evaluation), amplification/task, and mesh stages, and
    compiles any that are present in a single slangc invocation per file.
    """
    print(f"Compiling Slang shader: {shader_path}")

    os.makedirs(output_dir, exist_ok=True)
    shader_name, _ = os.path.splitext(os.path.basename(shader_path))

    with open(shader_path, "r", encoding="utf-8") as file:
        source = file.read()

    stages_to_compile: List[Tuple[str, str, str]] = []

    for entry, stage, suffix in SLANG_TARGETS:
        if re.search(rf"\b{re.escape(entry)}\s*\(", source):
            stages_to_compile.append((entry, stage, suffix))
        else:
            print(f"Skipping missing entry '{entry}' in {shader_path}")

    if not stages_to_compile:
        print(f"No known entry points found in {shader_path}. Nothing to compile.")
        return

    command: List[str] = ["slangc", shader_path]

    for entry, stage, suffix in stages_to_compile:
        profile = PROFILES[stage]
        output_file = os.path.normpath(
            os.path.join(output_dir, f"{shader_name}.{suffix}.spv")
        )

        # Use a unique target name per entry point to avoid conflicting
        # profiles and output paths when compiling multiple stages in one call.
        target_name = f"spirv-{suffix}"

        command.extend(
            [
                "-target",
                target_name,
                "-entry",
                entry,
                "-stage",
                stage,
                "-profile",
                profile,
                "-o",
                output_file,
            ]
        )

    print(f"Running: {' '.join(command)}")
    try:
        subprocess.run(command, check=True)
        for entry, _, suffix in stages_to_compile:
            output_file = os.path.join(output_dir, f"{shader_name}.{suffix}.spv")
            print(f"Successfully compiled {entry} to {output_file}")
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"Failed to compile one or more entries {[entry for entry, _, _ in stages_to_compile]} from {shader_path}"
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
