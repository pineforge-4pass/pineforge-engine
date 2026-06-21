#!/usr/bin/env python3
"""SECONDARY PineForge speed timer: time each bench strategy INSIDE the
pineforge-release container (run_json.py --bench → raw samples_ns) and emit
{slug: {median_ms, p95_ms, n}} — the same shape as the competitor timers
(speed/time_pynecore.py etc), so speed/aggregate.py loads it with
`--pineforge-format subproc`.

The from-source GBench harness (pineforge_bench.cpp) stays AUTHORITATIVE; this
no-toolchain path is for ecosystem/CI parity. Timing happens inside the
container (around run_backtest_full only); the host NEVER stopwatches `docker
run` (container/transpile/dlopen cold-start would swamp the hot loop). Mirrors
the GBench `with_magnifier` benchmark: magnifier ON, 4 samples, endpoints.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

# scripts/pf_release_run.py (repo root /scripts) — this file is benchmarks/speed/.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import pf_release_run as rel  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--strategies", type=Path, required=True,
                    help="bench strategies dir (each <slug>/generated.cpp)")
    ap.add_argument("--ohlcv", type=Path, required=True, help="bench OHLCV CSV")
    ap.add_argument("--image", default=None, help="pineforge-release image")
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--repeats", type=int, default=20)
    ap.add_argument("--only", default="", help="substring filter on slug")
    args = ap.parse_args()

    out: dict[str, dict] = {}
    for d in sorted(p for p in args.strategies.iterdir() if p.is_dir()):
        gen = d / "generated.cpp"
        if not gen.exists():
            continue
        if args.only and args.only not in d.name:
            continue
        kw = dict(bar_magnifier=True, magnifier_samples=4, magnifier_dist="endpoints",
                  bench=True, warmup=args.warmup, repeats=args.repeats)
        if args.image:
            kw["image"] = args.image
        try:
            rep = rel.run_release(gen, args.ohlcv, **kw)
        except rel.ReleaseRunError as e:
            print(f"skip {d.name}: {e}", file=sys.stderr)
            continue
        samples = (rep.get("diagnostics", {}).get("timing", {}) or {}).get("samples_ns") or []
        if not samples:
            print(f"skip {d.name}: no timing samples", file=sys.stderr)
            continue
        ms = sorted(s / 1e6 for s in samples)
        median_ms = statistics.median(ms)
        p95_ms = ms[min(len(ms) - 1, int(0.95 * len(ms)))]
        out[d.name] = {"median_ms": median_ms, "p95_ms": p95_ms, "n": len(ms)}
        print(f"{d.name}: median={median_ms:.3f}ms p95={p95_ms:.3f}ms n={len(ms)}",
              file=sys.stderr)

    json.dump(out, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
