#!/usr/bin/env python3
"""Generates IBL resources for every .hdr and .exr in this folder (recursive).

Requires cmgen and ktx on PATH.

Per environment:
  <name>/  — irradiance SH (sh.txt), prefiltered specular KTX, and skybox KTX

Shared (generated once):
  brdf_lut.ktx — BRDF integration LUT
"""

import subprocess
import sys
from pathlib import Path

CUBEMAP_SIZE = 1024
DFG_SIZE = 256

def run(cmd: list[str]) -> None:
    print(f"  > {' '.join(cmd)}")
    subprocess.check_call(cmd)

def generate_brdf_lut(script_dir: Path) -> None:
    brdf_ktx = script_dir / "brdf_lut.ktx"
    brdf_exr = script_dir / "brdf_lut.exr"

    if brdf_ktx.exists():
        return

    if brdf_exr.exists():
        print("Removing stale BRDF LUT EXR...")
        brdf_exr.unlink()

    print("Generating BRDF LUT...")
    run(["cmgen", f"--size={DFG_SIZE}", f"--ibl-dfg={brdf_exr}"])

    print("Converting BRDF LUT to KTX...")
    run(["ktx", "create", "--format", "R16G16_SFLOAT", str(brdf_exr), str(brdf_ktx)])

def generate_ibl(env_file: Path) -> None:
    deploy_dir = env_file.parent / env_file.stem

    if deploy_dir.exists():
        print(f"Skipping {env_file.name} (already exists)")
        return

    print(f"Generating IBL for {env_file.name}...")
    run(["cmgen", f"--size={CUBEMAP_SIZE}", "--format=ktx",
         f"--deploy={deploy_dir}", str(env_file)])

def main() -> None:
    script_dir = Path(__file__).resolve().parent
    env_files = sorted(
        p for p in script_dir.rglob("*")
        if p.suffix.lower() in (".hdr", ".exr")
        and p.stem != "brdf_lut"
    )

    if not env_files:
        print(f"No .hdr or .exr files found in {script_dir}")
        sys.exit(0)

    generate_brdf_lut(script_dir)

    for f in env_files:
        generate_ibl(f)

    print("Done.")

if __name__ == "__main__":
    main()