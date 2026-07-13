#!/usr/bin/env python3
"""Characterize run-to-run benchmark noise from N stats JSONs of the SAME build.

Feed it several runs of one binary/config. It reports, per timing metric, the
spread of the per-run medians and writes a noise file that compare.py uses to set
per-metric tolerance = max(k * sigma, relative floor).

    python3 tools/bench/noise_floor.py run1.json run2.json ... [--out noise.json] [-k 3]

"""
import argparse
import json
import statistics
import sys


# Config fields that must match for runs to be comparable at all.
GUARD_FIELDS = [
    ("scene", lambda d: d.get("scene")),
    ("device", lambda d: d.get("device")),
    ("driver", lambda d: d.get("driver")),
    ("resolution", lambda d: (d["resolution"]["width"], d["resolution"]["height"])),
    ("backend", lambda d: d["culling"]["backend"]),
    ("hiz", lambda d: d["culling"]["hiz_occlusion"]),
    ("draw_indirect_count", lambda d: d["culling"].get("draw_indirect_count")),
    ("msaa", lambda d: d.get("msaa_samples")),
    ("hdr", lambda d: d.get("hdr")),
]


def load(paths):
    runs = []
    for p in paths:
        with open(p) as f:
            runs.append((p, json.load(f)))
    return runs


def check_uniform(runs):
    """All runs must share config; warn if the deterministic checksum drifts."""
    base = runs[0][1]
    for name, get in GUARD_FIELDS:
        vals = {get(d) for _, d in runs}
        if len(vals) > 1:
            sys.exit(f"error: runs differ in {name}: {vals}, not the same config")
    checksums = {d.get("counter_checksum") for _, d in runs}
    if len(checksums) > 1:
        print(f"WARNING: counter_checksum differs across runs ({checksums}). "
              "The workload was not identical.")
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+", help="stats JSONs from the same build/config")
    ap.add_argument("--out", help="write per-metric sigma here for compare.py")
    ap.add_argument("-k", type=float, default=3.0, help="sigma multiplier for the suggested tolerance")
    args = ap.parse_args()

    if len(args.runs) < 2:
        sys.exit("error: need at least 2 runs to estimate noise")

    runs = load(args.runs)
    base = check_uniform(runs)
    metrics = list(base["timings_ms"].keys())

    noise = {}
    print(f"noise floor over {len(runs)} runs: {base['scene']} @ "
          f"{base['resolution']['width']}x{base['resolution']['height']} "
          f"{base['culling']['backend']}\n")
    print(f"{'metric':<24}{'min':>9}{'median':>9}{'max':>9}{'sigma':>9}{'cv%':>8}{'tol(±)':>9}")
    for m in metrics:
        medians = [d["timings_ms"][m]["median"] for _, d in runs]
        lo, hi = min(medians), max(medians)
        mid = statistics.median(medians)
        sigma = statistics.pstdev(medians) if len(medians) > 1 else 0.0
        cv = (sigma / mid * 100.0) if mid > 1e-6 else 0.0
        tol = args.k * sigma
        noise[m] = {"sigma": sigma, "median": mid, "min": lo, "max": hi, "cv_pct": cv}
        if hi > 0.01:  # skip inactive passes (all-zero)
            print(f"{m:<24}{lo:>9.3f}{mid:>9.3f}{hi:>9.3f}{sigma:>9.3f}{cv:>8.1f}{tol:>9.3f}")

    if args.out:
        with open(args.out, "w") as f:
            json.dump({"k": args.k, "runs": len(runs), "metrics": noise}, f, indent=1)
        print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
