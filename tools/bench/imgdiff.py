#!/usr/bin/env python3
"""Compare a candidate render against a golden with a pixel tolerance.

    python3 tools/bench/imgdiff.py golden.png candidate.png [--threshold 16]
            [--max-frac 0.0003] [--diff diff.png]

Renders are not bit-exact same-machine (order-dependent float summation in
heavily-lit views leaves a few dozen sparse pixels differing), so this gates on
the count of pixels whose per-channel delta exceeds --threshold, failing if that
exceeds --max-frac of the image. On failure --diff writes the candidate with
mismatches highlighted (nothing is written on a pass).

Exit: 0 within tolerance, 1 over, 2 on error (e.g. size mismatch).
"""
import argparse
import sys

import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("candidate")
    ap.add_argument("--threshold", type=int, default=16, help="per-channel delta (0-255) counted as a diff")
    ap.add_argument("--max-frac", type=float, default=0.0003, help="max fraction of mismatched pixels before FAIL")
    ap.add_argument("--diff", help="on failure, write a magenta-highlight diff PNG here")
    args = ap.parse_args()

    g = Image.open(args.golden).convert("RGB")
    c = Image.open(args.candidate).convert("RGB")
    if g.size != c.size:
        print(f"error: size mismatch golden {g.size} vs candidate {c.size}", file=sys.stderr)
        sys.exit(2)

    delta = np.abs(np.asarray(g, np.int16) - np.asarray(c, np.int16)).max(axis=2)
    mism = delta > args.threshold
    n, total = int(mism.sum()), int(delta.size)
    ok = n / total <= args.max_frac

    print(f"{args.candidate} vs {args.golden}: {n}/{total} px > {args.threshold} "
          f"({n / total * 100:.4f}%, limit {args.max_frac * 100:.4f}%), "
          f"max delta {int(delta.max())} -> {'OK' if ok else 'DIFF'}")

    if args.diff and not ok:
        out = np.asarray(c).copy()
        out[mism] = [255, 0, 255]
        Image.fromarray(out).save(args.diff)

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
