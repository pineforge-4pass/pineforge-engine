#!/usr/bin/env python3
"""SECONDARY PineForge throughput timer: time each bench strategy INSIDE the
pineforge-release container (run_json.py --bench → raw samples_ns) and emit
{slug: {m_per_s, items_per_second, median_ns, n}} — a subproc-shape JSON that
plot_quartile.py loads with `--format subproc`.

The from-source GBench harness (pineforge_bench.cpp, throughput/no_magnifier)
stays AUTHORITATIVE; this no-toolchain path is for ecosystem/CI parity. Timing
happens inside the container (around run_backtest_full only); the host NEVER
stopwatches `docker run`. Mirrors the GBench `throughput/no_magnifier`
benchmark: magnifier OFF, so items_processed = base OHLCV bars and the timed
region is the base-bar loop.

  items_per_second = items_processed / (median_ns / 1e9)
  M/s              = items_per_second / 1e6

GBench/docker biases: container CPU throttling + FFI make the docker median a
slight under-read of native throughput. This path is advisory, not published.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

# scripts/pf_release_run.py (repo root /scripts) — this file is benchmarks/throughput/.
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
        # magnifier OFF → base-bar throughput, matching throughput/no_magnifier.
        kw = dict(bar_magnifier=False, bench=True,
                  warmup=args.warmup, repeats=args.repeats)
        if args.image:
            kw["image"] = args.image
        try:
            rep = rel.run_release(gen, args.ohlcv, **kw)
        except rel.ReleaseRunError as e:
            print(f"skip {d.name}: {e}", file=sys.stderr)
            continue
        tp = (rep.get("diagnostics", {}) or {}).get("throughput", {}) or {}
        samples = tp.get("samples_ns") or []
        items = tp.get("items_processed")
        if not samples or not items:
            print(f"skip {d.name}: no throughput samples", file=sys.stderr)
            continue
        median_ns = statistics.median(samples)
        if median_ns <= 0:
            print(f"skip {d.name}: non-positive median_ns", file=sys.stderr)
            continue
        items_per_second = items * 1e9 / median_ns
        m_per_s = items_per_second / 1e6
        out[d.name] = {
            "m_per_s": m_per_s,
            "items_per_second": items_per_second,
            "median_ns": median_ns,
            "items_processed": int(items),
            "n": len(samples),
        }
        print(f"{d.name}: {m_per_s:.3f} M/s (items={int(items)} median={median_ns/1e6:.3f}ms n={len(samples)})",
              file=sys.stderr)

    json.dump(out, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
