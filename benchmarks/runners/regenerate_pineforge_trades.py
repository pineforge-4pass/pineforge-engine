#!/usr/bin/env python3
"""Re-generate `pineforge_trades.csv` for every benchmark strategy by
running the existing corpus-built `strategy.so` against the extended
OHLCV file at `_workdir/data/ETHUSDT_15.csv`.

Each `benchmarks/strategies/<NN-slug>/` folder maps to one
`corpus/<category>/<corpus_name>/` folder via the same plan
`bootstrap_strategies.py` uses. This script:

    1. Resolves the corpus folder for each benchmark folder
    2. Runs `scripts/run_strategy.py <corpus_folder> --ohlcv <extended>`
    3. Copies the resulting `engine_trades.csv` to
       `benchmarks/strategies/<NN>/pineforge_trades.csv`

Pre-requisites:
    - `cmake -B build -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON`
      `cmake --build build --target corpus_strategies`
      (puts strategy.dylib/.so into every corpus/<>/<>/ folder)
    - Extended OHLCV at `_workdir/data/ETHUSDT_15.csv`
      (produced by `fetch_extended_ohlcv.py`)
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH_DIR = REPO_ROOT / "benchmarks"
CORPUS_ROOT = REPO_ROOT / "corpus"
RUN_STRATEGY = REPO_ROOT / "scripts" / "run_strategy.py"

# Prefer the LFS-tracked snapshot, fall back to the live working copy.
_OHLCV_CANDIDATES = [
    BENCH_DIR / "data" / "ETHUSDT_15.csv",
    BENCH_DIR / "_workdir" / "data" / "ETHUSDT_15.csv",
]
DEFAULT_OHLCV = next((p for p in _OHLCV_CANDIDATES if p.exists()), _OHLCV_CANDIDATES[-1])

# Reuse the canonical mapping from bootstrap_strategies.py.
sys.path.insert(0, str(BENCH_DIR / "runners"))
from bootstrap_strategies import DEFAULT_PLAN  # noqa: E402


def regen_one(corpus_rel: str, idx: int, slug: str, ohlcv: Path) -> tuple[bool, str]:
    corpus_dir = CORPUS_ROOT / corpus_rel
    bench_dir = BENCH_DIR / "strategies" / f"{idx:02d}-{slug}"

    if not bench_dir.exists():
        return False, f"benchmark folder missing: {bench_dir.relative_to(REPO_ROOT)}"
    if not corpus_dir.exists():
        return False, f"corpus folder missing: {corpus_dir.relative_to(REPO_ROOT)}"
    so_files = list(corpus_dir.glob("strategy.*"))
    so_files = [p for p in so_files if p.suffix in (".so", ".dylib", ".dll")]
    if not so_files:
        return False, (f"no strategy.so/.dylib/.dll in {corpus_dir.relative_to(REPO_ROOT)} — "
                       "build with `cmake --build build --target corpus_strategies`")

    cmd = [
        sys.executable, str(RUN_STRATEGY),
        str(corpus_dir), "--ohlcv", str(ohlcv.resolve()),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if res.returncode != 0:
        msg = (res.stderr.strip().splitlines() or [f"rc={res.returncode}"])[-1][:200]
        return False, f"run_strategy.py failed: {msg}"

    src = corpus_dir / "engine_trades.csv"
    if not src.exists():
        return False, "run_strategy.py did not emit engine_trades.csv"
    dst = bench_dir / "pineforge_trades.csv"
    shutil.copy2(src, dst)
    return True, "ok"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ohlcv", type=Path, default=DEFAULT_OHLCV,
                    help=f"OHLCV CSV (default: {DEFAULT_OHLCV.relative_to(REPO_ROOT)})")
    ap.add_argument("--only", help="Substring filter on benchmark slug")
    args = ap.parse_args()

    if not args.ohlcv.exists():
        print(f"ERROR: OHLCV not found at {args.ohlcv}", file=sys.stderr)
        print("Run scripts/fetch_extended_ohlcv.py first.", file=sys.stderr)
        return 1

    started = time.time()
    n_ok = n_fail = 0
    failed: list[tuple[str, str]] = []

    for corpus_rel, idx, slug in DEFAULT_PLAN:
        if args.only and args.only not in slug:
            continue
        ok, msg = regen_one(corpus_rel, idx, slug, args.ohlcv)
        tag = "OK" if ok else "FAIL"
        print(f"  [{idx:02d}-{slug:38s}] {tag:4s}  {msg}")
        if ok:
            n_ok += 1
        else:
            n_fail += 1
            failed.append((f"{idx:02d}-{slug}", msg))

    elapsed = time.time() - started
    print()
    print(f"regenerated {n_ok}, failed {n_fail}  in {elapsed:.1f}s")
    if failed:
        print()
        print("failed:")
        for name, err in failed:
            print(f"  {name}: {err}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
