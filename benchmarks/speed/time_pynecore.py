#!/usr/bin/env python3
"""Time `pyne run` for each benchmark strategy via subprocess wall-time.

Output: JSON {strategy: {median_ms, p95_ms, n}} to stdout.
Includes interpreter startup + framework import time, which is the
realistic per-strategy cost for a Python-runtime engine.

Strategies 51-75 lack strategy_pyne.py (PyneSys quota exhaustion); those
are skipped gracefully when the subprocess returns non-zero.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH = REPO_ROOT / "benchmarks"
sys.path.insert(0, str(BENCH))
from paths import STRATEGIES  # noqa: E402

DEFAULT_N = 20


def time_one(strat_dir: Path, n: int) -> dict | None:
    samples_ms: list[float] = []
    for _ in range(n):
        t0 = time.perf_counter()
        result = subprocess.run(
            [
                "uv", "run", "python", "runners/run_pynecore.py",
                str(strat_dir),
                "--no-write",
            ],
            cwd=str(BENCH),
            capture_output=True,
        )
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        if result.returncode != 0:
            # Missing strategy_pyne.py or other fatal error — skip this strategy.
            return None
        samples_ms.append(elapsed_ms)
    arr = np.array(samples_ms)
    return {
        "median_ms": float(np.median(arr)),
        "p95_ms": float(np.percentile(arr, 95)),
        "n": n,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--n", type=int, default=DEFAULT_N,
                    help=f"Repetitions per strategy (default: {DEFAULT_N})")
    ap.add_argument("--only", default=None,
                    help="Filter: only time strategies whose name contains this string")
    ap.add_argument("--workers", type=int, default=8,
                    help="Number of concurrent workers (default: 8)")
    args = ap.parse_args()

    strat_dirs = []
    for d in sorted(STRATEGIES.iterdir()):
        if not d.is_dir() or d.name.startswith("_") or d.name.startswith("."):
            continue
        if args.only and args.only not in d.name:
            continue
        strat_dirs.append(d)

    out: dict[str, dict] = {}

    def process_strat(d: Path) -> tuple[str, dict | None]:
        res = time_one(d, args.n)
        return d.name, res

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {executor.submit(process_strat, d): d for d in strat_dirs}
        for fut in as_completed(futures):
            name, result = fut.result()
            if result is not None:
                out[name] = result
                print(
                    f"{name}: median={result['median_ms']:.1f}ms  p95={result['p95_ms']:.1f}ms",
                    file=sys.stderr,
                )
            else:
                print(f"{name}: SKIP (no strategy_pyne.py or subprocess error)", file=sys.stderr)

    # Sort final output alphabetically by strategy name
    sorted_out = {k: out[k] for k in sorted(out.keys())}
    json.dump(sorted_out, sys.stdout, indent=2)
    print(file=sys.stdout)  # trailing newline


if __name__ == "__main__":
    main()
