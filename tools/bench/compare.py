#!/usr/bin/env python3
"""Compare benchmark candidate run(s) against baseline run(s); gate regressions.

    python3 tools/bench/compare.py --baseline base.json [base2.json ...] \
                                   --candidate cand.json [cand2.json ...] \
                                   [--noise noise.json] [--rel 2.0] [--abs 0.05]

Pass MULTIPLE runs per side to defend against thermal throttling: the tool
compares the BEST median per metric on each side, i.e. the run closest to the
un-throttled ceiling. Interleave the runs A B A B ... so both sides share the
thermal envelope.

Gates, in order:
  * config guard  - GUARD_FIELDS (scene, driver, resolution, backend, ...) must
                    match, else exit 2
  * counter gate  - counter_checksum must match, else the workload diverged (a
                    culling/LOD/visibility change): investigate, then rebaseline
  * timing gate   - per metric, regression if best-candidate is slower than
                    best-baseline by more than tolerance = max(k*sigma, rel%, abs)

Exit: 0 clean, 1 regression or workload divergence, 2 incomparable configs.
"""
import argparse
import json
import sys


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
    ("warmup_frames", lambda d: d.get("warmup_frames")),
    ("measured_frames", lambda d: d.get("measured_frames")),
]


def load(paths):
    out = []
    for p in paths:
        with open(p) as f:
            out.append(json.load(f))
    return out


def guard(base_runs, cand_runs):
    """Refuse to compare runs from different configs"""
    b, c = base_runs[0], cand_runs[0]
    for name, get in GUARD_FIELDS:
        if get(b) != get(c):
            print(f"error: cannot compare across {name}: baseline={get(b)} candidate={get(c)}",
                  file=sys.stderr)
            sys.exit(2)


# Timings are durations (lower is better) except these, where a larger median is
# better. Keep in sync with FrameStats.
HIGHER_IS_BETTER = {"gpu_overlap"}


def best_median(runs, metric):
    """Median of the run closest to the un-throttled ceiling"""
    medians = [r["timings_ms"][metric]["median"] for r in runs]
    return max(medians) if metric in HIGHER_IS_BETTER else min(medians)


def counter_ranges(run):
    return {k: (v["min"], v["max"]) for k, v in run["counters"].items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", nargs="+", required=True)
    ap.add_argument("--candidate", nargs="+", required=True)
    ap.add_argument("--noise", help="noise.json from noise_floor.py for per-metric sigma")
    ap.add_argument("--rel", type=float, default=2.0, help="relative tolerance %% when no sigma")
    ap.add_argument("--abs", type=float, default=0.05, help="absolute tolerance floor (ms)")
    args = ap.parse_args()

    base = load(args.baseline)
    cand = load(args.candidate)
    guard(base, cand)

    noise = {}
    if args.noise:
        with open(args.noise) as f:
            nf = json.load(f)
            noise = nf.get("metrics", {})

    b0, c0 = base[0], cand[0]
    print(f"{b0['scene']} @ {b0['resolution']['width']}x{b0['resolution']['height']} "
          f"{b0['culling']['backend']} | baseline {len(base)} run(s) vs candidate {len(cand)} run(s)\n")

    status = 0

    # Counter gate: deterministic per-frame sequence; divergence = workload changed.
    bc, cc = b0.get("counter_checksum"), c0.get("counter_checksum")
    if bc != cc:
        status = 1
        print(f"WORKLOAD DIVERGED: counter_checksum {bc} -> {cc}")
        bram, cram = counter_ranges(b0), counter_ranges(c0)
        for k in bram:
            if bram[k] != cram.get(k):
                print(f"    {k}: {bram[k]} -> {cram.get(k)}")
        print("  A code change altered visibility/LOD. Investigate; rebaseline if intended.\n")
    else:
        print(f"counters match ({bc})\n")

    # Timing gate.
    rows = []
    for m in b0["timings_ms"]:
        bm, cm = best_median(base, m), best_median(cand, m)
        if max(bm, cm) < 0.01:
            continue  # inactive pass
        delta = cm - bm
        worse = -delta if m in HIGHER_IS_BETTER else delta  # positive = regression, either direction
        pct = (delta / bm * 100.0) if bm > 1e-6 else 0.0
        sigma = noise.get(m, {}).get("sigma")
        if sigma is not None:
            tol = max(nf["k"] * sigma, args.abs)
        else:
            tol = max(bm * args.rel / 100.0, args.abs)
        verdict = "REGRESSION" if worse > tol else ("improved" if worse < -tol else "")
        rows.append((worse, m, bm, cm, delta, pct, tol, verdict))
        if verdict == "REGRESSION":
            status = 1

    rows.sort(key=lambda r: -r[0])  # biggest regression first
    print(f"{'metric':<24}{'base':>8}{'cand':>8}{'delta':>8}{'pct':>8}{'tol':>7}  verdict")
    for worse, m, bm, cm, delta, pct, tol, verdict in rows:
        if verdict or abs(pct) >= 1.0:
            print(f"{m:<24}{bm:>8.3f}{cm:>8.3f}{delta:>+8.3f}{pct:>+7.1f}%{tol:>7.3f}  {verdict}")

    print("\n" + ("REGRESSIONS FOUND" if status == 1 and bc == cc else
                  "WORKLOAD DIVERGED, rebaseline needed" if bc != cc else "clean"))
    sys.exit(status)


if __name__ == "__main__":
    main()
