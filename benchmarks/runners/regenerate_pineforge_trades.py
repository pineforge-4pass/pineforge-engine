#!/usr/bin/env python3
"""Regenerate `pineforge_trades.csv` for every bench strategy by loading
the pre-built `strategy.dylib` / `strategy.so` (built by cmake) and running
it through `scripts/run_strategy.py` against the pinned OHLCV.

Pre-requisites:
    cmake -B build -DPINEFORGE_BUILD_BENCH_STRATEGIES=ON
    cmake --build build --target pineforge bench_strategies -j

The strategy shared libraries must be co-located with their `generated.cpp`
(i.e. at `assets/strategies/<NN-slug>/strategy.dylib`).  They are produced
by the cmake bench_strategies aggregate target — no call to pineforge-codegen
is made here.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH = REPO_ROOT / "benchmarks"
RUN_STRATEGY = REPO_ROOT / "scripts" / "run_strategy.py"

sys.path.insert(0, str(BENCH))
from paths import DATA, STRATEGIES  # noqa: E402

DEFAULT_OHLCV = DATA / "ETHUSDT_15.csv"


def regen_one(bench_dir: Path, ohlcv: Path) -> tuple[bool, str]:
    # Prefer .dylib (macOS) then .so (Linux).
    dylib = bench_dir / "strategy.dylib"
    if not dylib.exists():
        dylib = bench_dir / "strategy.so"
    if not dylib.exists():
        return (
            False,
            "no strategy.dylib — run: "
            "cmake -B build -DPINEFORGE_BUILD_BENCH_STRATEGIES=ON && "
            "cmake --build build --target bench_strategies -j",
        )

    dst = bench_dir / "pineforge_trades.csv"
    cmd = [
        sys.executable,
        str(RUN_STRATEGY),
        str(bench_dir),
        "--ohlcv",
        str(ohlcv.resolve()),
        "--output",
        str(dst.resolve()),
    ]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return False, "run_strategy.py timed out (300 s)"

    if res.returncode != 0:
        msg = (res.stderr.strip().splitlines() or [f"rc={res.returncode}"])[-1][:200]
        return False, f"run_strategy.py failed: {msg}"
    if not dst.exists():
        return False, "run_strategy.py did not emit pineforge_trades.csv"
    return True, "ok"


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--ohlcv", type=Path, default=DEFAULT_OHLCV)
    ap.add_argument("--only", help="Substring filter on bench slug")
    args = ap.parse_args()

    if not args.ohlcv.exists():
        print(f"ERROR: OHLCV not found at {args.ohlcv}", file=sys.stderr)
        return 1

    started = time.time()
    n_ok = n_fail = 0
    failed: list[tuple[str, str]] = []

    for d in sorted(STRATEGIES.iterdir()):
        if not d.is_dir() or d.name.startswith("_"):
            continue
        if args.only and args.only not in d.name:
            continue
        ok, msg = regen_one(d, args.ohlcv)
        tag = "OK" if ok else "FAIL"
        print(f"  [{d.name:42s}] {tag:4s}  {msg}")
        if ok:
            n_ok += 1
        else:
            n_fail += 1
            failed.append((d.name, msg))

    elapsed = time.time() - started
    print(f"\nregenerated {n_ok}, failed {n_fail}  in {elapsed:.1f}s")
    if failed:
        print("\nfailed:")
        for name, err in failed:
            print(f"  {name}: {err}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
