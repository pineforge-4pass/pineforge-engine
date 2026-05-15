#!/usr/bin/env python3
"""Regenerate `pineforge_trades.csv` for every bench strategy by
codegen+building each `assets/strategies/<NN>/strategy.pine` and
running it through `scripts/run_strategy.py` against the pinned OHLCV.

This script no longer depends on the corpus-side bootstrap_strategies
DEFAULT_PLAN (which became stale after corpus consolidation in
commit ef6ce58). The bench is now the source of truth for its own
50+N strategy.pine files.

Pre-requisites:
    - cmake -B build -DPINEFORGE_BUILD_TESTS=ON
      cmake --build build --target pineforge -j
    - sibling pineforge-codegen repo checked out next to engine
      (or PINEFORGE_CODEGEN_PATH env var set)
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

sys.path.insert(0, str(BENCH / "runners"))
from compile_bench_strategy import compile_one  # noqa: E402

DEFAULT_OHLCV = DATA / "ETHUSDT_15.csv"


def regen_one(bench_dir: Path, ohlcv: Path) -> tuple[bool, str]:
    try:
        dylib = compile_one(bench_dir)
    except Exception as e:
        return False, f"compile failed: {str(e).splitlines()[0][:200]}"

    dst = bench_dir / "pineforge_trades.csv"
    cmd = [
        sys.executable, str(RUN_STRATEGY),
        str(dylib.parent),
        "--ohlcv", str(ohlcv.resolve()),
        "--output", str(dst.resolve()),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if res.returncode != 0:
        msg = (res.stderr.strip().splitlines() or [f"rc={res.returncode}"])[-1][:200]
        return False, f"run_strategy.py failed: {msg}"
    if not dst.exists():
        return False, "run_strategy.py did not emit pineforge_trades.csv"
    return True, "ok"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
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
