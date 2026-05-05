#!/usr/bin/env python3
"""Bootstrap a set of benchmark strategy folders from the corpus.

For each requested strategy this script:
    1. Creates `benchmarks/strategies/<NN-slug>/`.
    2. Copies `strategy.pine`, `tv_trades.csv`, `engine_trades.csv`
       (renamed to `pineforge_trades.csv`) from the corpus.
    3. Stops short of generating `strategy_pyne.py` — that is
       produced separately by `cloud_compile.py`, which is the canonical
       source of truth for the `.pine -> @pyne` translation.

`strategies/_indicators/` and any pre-existing `0*-*/` folders are
preserved.

Usage:
    # add the planned 40 strategies (all non-MTF basic + community + validation)
    python benchmarks/runners/bootstrap_strategies.py

    # custom curated list:
    python benchmarks/runners/bootstrap_strategies.py --plan custom.json
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CORPUS = REPO_ROOT / "corpus"
BENCH_DIR = REPO_ROOT / "benchmarks" / "strategies"

# Default plan: extend the existing 10 hand-ported strategies (which we
# keep as 01-10) with another 40, bringing the suite to ~50 total.
#
# Format per row: (corpus_relative_path, dest_index, dest_slug)
#
# Hand-ported 01..10 stay where they are. We pick:
#   11-16: remaining basic/* (greedy, keltner, parabolic-asr,
#                             pivot-ext, stochastic-slow, volty-expan)
#   17-19: remaining non-MTF community/* (BOS_curv, kanuck,
#                                          scalping-wunder-bots)
#   20-50: 31 validation/* probes spanning indicator, exit semantics,
#          and UDT-method coverage. All MTF probes (mtf-probe-* and
#          ies-probe-* / vcp-probe-* whose parents use request.security)
#          are excluded.
DEFAULT_PLAN: list[tuple[str, int, str]] = [
    # remaining basic/
    ("basic/greedy",            11, "greedy"),
    ("basic/keltner",           12, "keltner"),
    ("basic/parabolic-asr",     13, "parabolic-asr"),
    ("basic/pivot-ext",         14, "pivot-ext"),
    ("basic/stochastic-slow",   15, "stochastic-slow"),
    ("basic/volty-expan",       16, "volty-expan"),

    # remaining non-MTF community/
    ("community/BOS_curv",                17, "bos-curv"),
    ("community/kanuck",                  18, "kanuck"),
    ("community/scalping-wunder-bots",    19, "scalping-wunder-bots"),

    # validation/ — indicator probes (numbered 02..23, skip 01 and 04 already used)
    ("validation/02-bb-squeeze",                  20, "bb-squeeze"),
    ("validation/03-dmi-adx-trend",               21, "dmi-adx-trend"),
    ("validation/05-hma-cross",                   22, "hma-cross"),
    ("validation/06-cci-momentum",                23, "cci-momentum"),
    ("validation/07-tsi-signal",                  24, "tsi-signal"),
    ("validation/08-linreg-channel",              25, "linreg-channel"),
    ("validation/09-aroon-oscillator",            26, "aroon-oscillator"),
    ("validation/10-donchian-breakout",           27, "donchian-breakout"),
    ("validation/11-elder-ray",                   28, "elder-ray"),
    ("validation/12-chandelier-exit",             29, "chandelier-exit"),
    ("validation/13-atr-trailing-stop",           30, "atr-trailing-stop"),
    ("validation/14-vwma-divergence",             31, "vwma-divergence"),
    ("validation/15-momentum-roc",                32, "momentum-roc"),
    ("validation/16-mean-reversion-bb",           33, "mean-reversion-bb"),
    ("validation/17-dual-ma-switch",              34, "dual-ma-switch"),
    ("validation/18-ema-ribbon-loop",             35, "ema-ribbon-loop"),
    ("validation/19-pivot-array-breakout",        36, "pivot-array-breakout"),
    ("validation/20-range-filter-while",          37, "range-filter-while"),
    ("validation/21-adaptive-ma-func",            38, "adaptive-ma-func"),
    ("validation/22-candle-pattern",              39, "candle-pattern"),
    ("validation/23-dual-thrust",                 40, "dual-thrust"),
    ("validation/26-volume-breakout",             41, "volume-breakout"),
    ("validation/27-ma-stack-array",              42, "ma-stack-array"),
    ("validation/28-swing-pivot-atr",             43, "swing-pivot-atr"),
    ("validation/29-median-cross",                44, "median-cross"),
    ("validation/30-multi-indicator-score",       45, "multi-indicator-score"),
    ("validation/31-rsi-bands",                   46, "rsi-bands"),
    ("validation/32-supertrend-adx-filter",       47, "supertrend-adx-filter"),
    ("validation/40-bracket-exit-tp-sl",          48, "bracket-exit-tp-sl"),
    ("validation/41-partial-exit-qty-percent",    49, "partial-exit-qty-percent"),
    ("validation/42-close-immediate-vs-next-bar", 50, "close-immediate-vs-next-bar"),
]


def bootstrap_one(corpus_rel: str, idx: int, slug: str) -> tuple[bool, str]:
    src = CORPUS / corpus_rel
    if not (src / "strategy.pine").exists():
        return False, f"missing source: {src}/strategy.pine"

    dst = BENCH_DIR / f"{idx:02d}-{slug}"
    if dst.exists() and (dst / "strategy.pine").exists():
        return True, "already bootstrapped"

    dst.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "strategy.pine", dst / "strategy.pine")
    if (src / "tv_trades.csv").exists():
        shutil.copy2(src / "tv_trades.csv", dst / "tv_trades.csv")
    if (src / "engine_trades.csv").exists():
        shutil.copy2(src / "engine_trades.csv", dst / "pineforge_trades.csv")
    return True, "bootstrapped"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plan", type=Path,
                    help="JSON file with [{corpus_path, idx, slug}, ...] (overrides default)")
    args = ap.parse_args()

    plan = DEFAULT_PLAN
    if args.plan:
        raw = json.loads(args.plan.read_text(encoding="utf-8"))
        plan = [(r["corpus_path"], r["idx"], r["slug"]) for r in raw]

    n_ok = n_skip = n_fail = 0
    for corpus_rel, idx, slug in plan:
        ok, msg = bootstrap_one(corpus_rel, idx, slug)
        if ok:
            if msg == "already bootstrapped":
                n_skip += 1
                tag = "SKIP"
            else:
                n_ok += 1
                tag = "OK"
        else:
            n_fail += 1
            tag = "FAIL"
        print(f"  {idx:02d}-{slug:38s}  {tag:4s}  {msg}")

    print()
    print(f"bootstrapped {n_ok}, skipped {n_skip}, failed {n_fail}")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
