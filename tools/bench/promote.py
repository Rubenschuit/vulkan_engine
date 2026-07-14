#!/usr/bin/env python3
"""Promote a bench.sh capture into a tracked baseline set.

    python3 tools/bench/promote.py <machine-id> --commit <hash> [--from bench] [-k 3]

Reads <from>/<scene>/<pose>_<backend>_<n>.json (bench.sh output), copies the
repeats into bench/baselines/<machine-id>/<scene>/, writes a per-config
noise file (.noise.json) for compare.py, and a manifest.json. Warns on any
config whose counter_checksum is not stable across repeats (expected for meshlet
on MoltenVK — the readback race; compare.py gates those on stable counters).
"""
import argparse
import glob
import json
import os
import re
import shutil
import statistics
import sys
from collections import defaultdict

BASELINE_ROOT = "bench/baselines"
PATHS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "paths")


def runConfig(d):
    """Per-run config tuple; one capture must be uniform in all of these."""
    return (d.get("schema"), d["device"], d["driver"],
            d["resolution"]["width"], d["resolution"]["height"],
            d["msaa_samples"], d["hdr"], d["warmup_frames"], d["measured_frames"],
            d.get("fixed_dt"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("machine", help="machine id, e.g. m1pro-moltenvk")
    ap.add_argument("--commit", required=True, help="git commit the capture was built from")
    ap.add_argument("--from", dest="src", default="bench", help="bench.sh output dir")
    ap.add_argument("-k", type=float, default=3.0, help="sigma multiplier for tolerances")
    args = ap.parse_args()

    known_scenes = {d.lower() for d in os.listdir(PATHS_DIR)
                    if os.path.isdir(os.path.join(PATHS_DIR, d))}
    groups = defaultdict(list)
    skipped_dirs = set()
    for path in glob.glob(os.path.join(args.src, "*", "*_*_*.json")):
        scene = os.path.basename(os.path.dirname(path))
        if scene.lower() not in known_scenes:
            skipped_dirs.add(scene)
            continue
        m = re.match(r"(.+)_(cpu|gpu|meshlet)_(\d+)\.json$", os.path.basename(path))
        if m:
            groups[(scene, m.group(1), m.group(2))].append(path)
    for d in sorted(skipped_dirs):
        print(f"skip {d}/: no tools/bench/paths/{d.lower()}/, not a scene capture")
    if not groups:
        sys.exit(f"no bench.sh runs found under {args.src}/<scene>/")

    dest_root = os.path.join(BASELINE_ROOT, args.machine)
    configs, meta, all_cfgs = [], None, set()
    for (scene, pose, backend), paths in sorted(groups.items()):
        paths.sort()
        # A best-of-N baseline (and a noise estimate) needs >= 2 repeats.
        if len(paths) < 2:
            print(f"skip {scene}/{pose}/{backend}: only {len(paths)} repeat")
            continue
        ds = [json.load(open(p)) for p in paths]
        cfgs = {runConfig(d) for d in ds}
        all_cfgs |= cfgs
        if len(cfgs) > 1:
            sys.exit(f"error: {scene}/{pose}/{backend} repeats differ in device/driver/res/config; "
                     "not a single capture")
        meta = meta or ds[0]
        deterministic = len({d["counter_checksum"] for d in ds}) == 1

        dest_dir = os.path.join(dest_root, scene)
        os.makedirs(dest_dir, exist_ok=True)
        for i, p in enumerate(paths, 1):
            shutil.copy(p, os.path.join(dest_dir, f"{pose}_{backend}_{i}.json"))

        noise = {}
        for name in ds[0]["timings_ms"]:
            meds = [d["timings_ms"][name]["median"] for d in ds]
            noise[name] = {
                "sigma": statistics.pstdev(meds) if len(meds) > 1 else 0.0,
                "median": statistics.median(meds), "min": min(meds), "max": max(meds),
            }
        with open(os.path.join(dest_dir, f"{pose}_{backend}.noise.json"), "w") as f:
            json.dump({"k": args.k, "runs": len(ds), "metrics": noise}, f, indent=1)

        configs.append({
            "scene": scene, "pose": pose, "backend": backend, "repeats": len(ds),
            "deterministic_checksum": deterministic,
            "gpu_median_best_ms": round(min(d["timings_ms"]["gpu_time"]["median"] for d in ds), 3),
        })
        if not deterministic:
            print(f"WARN {scene}/{pose}/{backend}: checksum not stable across repeats "
                  "(expected for meshlet/MoltenVK; gated on stable counters)")

    if not configs:
        sys.exit("error: nothing promotable (each config needs >= 2 repeats)")
    if len(all_cfgs) > 1:
        sys.exit("error: capture mixes configs (device/driver/res/...); promote one config at a time")

    notes = ["Timings: Release, best-of-N, same machine. Debug timings not comparable."]
    if any(not c["deterministic_checksum"] for c in configs):
        notes.append("A meshlet config has a non-deterministic cull_visible_objects readback "
                     "(no DrawIndirectCount); compare.py gates those on stable counters.")

    manifest = {
        "machine": args.machine, "device": meta["device"], "driver": meta["driver"],
        "resolution": meta["resolution"], "msaa_samples": meta["msaa_samples"], "hdr": meta["hdr"],
        "warmup_frames": meta["warmup_frames"], "measured_frames": meta["measured_frames"],
        "commit": args.commit, "captured": meta["timestamp"], "sigma_k": args.k,
        "configs": configs, "notes": notes,
    }
    os.makedirs(dest_root, exist_ok=True)
    with open(os.path.join(dest_root, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"\npromoted {len(configs)} configs -> {dest_root}/ (manifest.json + per-config noise)")


if __name__ == "__main__":
    main()
