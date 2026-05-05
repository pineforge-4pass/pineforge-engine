#!/usr/bin/env python3
"""Three-way per-bar indicator comparator: PineForge ↔ PyneCore ↔ PineTS.

Reads (via ``paths.ASSETS`` — ``benchmarks/assets/…`` submodule or legacy
``benchmarks/strategies/…``):
    …/_indicators/canonical_pineforge.csv
    …/_indicators/canonical_pyne.csv
    …/_indicators/canonical_pinets.csv

For each (engine_pair, indicator_column) combination, computes the
per-bar absolute and relative deltas, and reports:
  - max-abs delta
  - p50, p90, p99, max relative delta
  - count of bars where any engine returned NA but the others didn't

Output: benchmarks/results/indicator_comparison.md

Treats early bars (before each indicator's warmup window) where engines
disagree on NA-ness as a known semantic divergence — counted but not
counted as a defect.
"""
from __future__ import annotations

import csv
import math
import os
from collections import defaultdict
import sys
from pathlib import Path

_SYS_BENCH = Path(__file__).resolve().parent
if str(_SYS_BENCH) not in sys.path:
    sys.path.insert(0, str(_SYS_BENCH))
from paths import ASSETS, BENCH, REPO_ROOT  # noqa: E402

INDIR = ASSETS / "strategies" / "_indicators"
OUT = BENCH / "results" / "indicator_comparison.md"


def _md_relpath(from_dir: Path, target: Path) -> str:
    return Path(os.path.relpath(target.resolve(), from_dir.resolve())).as_posix()

INDICATOR_COLS = [
    "ema21", "sma21", "rsi14", "atr14",
    "macd_line", "macd_signal", "macd_hist",
    "bb_basis", "bb_upper", "bb_lower",
]


def load_csv(path: Path) -> list[dict[str, float | None]]:
    """Return per-bar dict; values are float or None for NA/empty."""
    rows: list[dict[str, float | None]] = []
    with path.open(encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            out: dict[str, float | None] = {}
            for col in INDICATOR_COLS:
                v = row.get(col, "")
                if v is None or v == "":
                    out[col] = None
                else:
                    try:
                        f_val = float(v)
                        out[col] = None if math.isnan(f_val) else f_val
                    except ValueError:
                        out[col] = None
            rows.append(out)
    return rows


def percentile(xs: list[float], p: float) -> float:
    if not xs:
        return 0.0
    s = sorted(xs)
    k = (len(s) - 1) * p
    f, c = int(k), min(int(k) + 1, len(s) - 1)
    return s[f] if f == c else s[f] * (c - k) + s[c] * (k - f)


def relmax(a: float, b: float) -> float:
    return abs(a - b) / max(abs(a), abs(b), 1e-9)


def diff_two(a: list[dict], b: list[dict]) -> dict[str, dict]:
    """For each indicator column, return delta stats between two engine outputs."""
    n = min(len(a), len(b))
    stats: dict[str, dict] = {}
    for col in INDICATOR_COLS:
        rels: list[float] = []
        abs_deltas: list[float] = []
        n_both = n_a_only = n_b_only = n_neither = 0
        for i in range(n):
            va, vb = a[i][col], b[i][col]
            if va is None and vb is None:
                n_neither += 1
            elif va is None:
                n_b_only += 1
            elif vb is None:
                n_a_only += 1
            else:
                n_both += 1
                rels.append(relmax(va, vb))
                abs_deltas.append(abs(va - vb))
        stats[col] = {
            "n_bars": n,
            "n_both": n_both,
            "n_a_only": n_a_only,
            "n_b_only": n_b_only,
            "n_neither": n_neither,
            "max_abs": max(abs_deltas) if abs_deltas else 0.0,
            "p50_rel": percentile(rels, 0.5),
            "p90_rel": percentile(rels, 0.9),
            "p99_rel": percentile(rels, 0.99),
            "max_rel": max(rels) if rels else 0.0,
        }
    return stats


def fmt_e(x: float, sig: int = 3) -> str:
    if x == 0:
        return "0"
    return f"{x:.{sig}e}"


def render_pair_table(name_a: str, name_b: str, stats: dict[str, dict]) -> str:
    lines = [f"### {name_a} ↔ {name_b}\n"]
    lines.append(
        "| Indicator | Both-NA | A-only | B-only | Both-num | "
        "max-abs | p50-rel | p90-rel | p99-rel | max-rel |"
    )
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for col in INDICATOR_COLS:
        s = stats[col]
        lines.append(
            f"| {col} | {s['n_neither']:>3d} | {s['n_a_only']:>3d} | "
            f"{s['n_b_only']:>3d} | {s['n_both']:>5d} | "
            f"{fmt_e(s['max_abs'])} | {fmt_e(s['p50_rel'])} | "
            f"{fmt_e(s['p90_rel'])} | {fmt_e(s['p99_rel'])} | "
            f"{fmt_e(s['max_rel'])} |"
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    pf_path = INDIR / "canonical_pineforge.csv"
    pc_path = INDIR / "canonical_pyne.csv"
    pt_path = INDIR / "canonical_pinets.csv"

    missing = [p.name for p in (pf_path, pc_path, pt_path) if not p.exists()]
    if missing:
        print(f"missing input files: {missing}")
        print("expected:")
        for p in (pf_path, pc_path, pt_path):
            print(f"  {p}")
        return 1

    print(f"loading {pf_path.name}…")
    pf = load_csv(pf_path)
    print(f"loading {pc_path.name}…")
    pc = load_csv(pc_path)
    print(f"loading {pt_path.name}…")
    pt = load_csv(pt_path)
    print(f"  PineForge: {len(pf)} bars")
    print(f"  PyneCore:  {len(pc)} bars")
    print(f"  PineTS:    {len(pt)} bars")

    s_pf_pc = diff_two(pf, pc)
    s_pf_pt = diff_two(pf, pt)
    s_pc_pt = diff_two(pc, pt)

    pine_src = INDIR / "canonical.pine"
    pine_href = _md_relpath(OUT.parent, pine_src)
    sections = [
        "# Indicator comparison\n",
        "All three engines compute the canonical indicator script "
        f"([`canonical.pine`]({pine_href})) "
        "on the same 36,361-bar OHLCV feed. This table reports per-bar "
        "absolute and relative deltas across every pair of engines.\n",
        "**NA columns** count bars where one engine reported a number "
        "but the other was still in its warmup window (or vice versa). "
        "Engines disagree on warmup behaviour for some indicators "
        "(e.g. PineForge's EMA emits the first close at bar 0 while "
        "PyneCore/PineTS wait for length-1 bars of history). This is a "
        "documented semantic divergence, not a numerical defect.\n",
        "**Both-num columns** are the bars where both engines emitted a "
        "value. The relative-delta percentiles are computed only over "
        "those bars.\n",
        render_pair_table("PineForge", "PyneCore", s_pf_pc),
        render_pair_table("PineForge", "PineTS",   s_pf_pt),
        render_pair_table("PyneCore",  "PineTS",   s_pc_pt),
    ]

    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text("\n".join(sections), encoding="utf-8")
    print(f"\nwrote {OUT.relative_to(REPO_ROOT)}")

    print("\nsummary (max-rel across all 10 indicators):")
    for label, stats in (("PF↔PC", s_pf_pc), ("PF↔PT", s_pf_pt), ("PC↔PT", s_pc_pt)):
        worst = max(s["max_rel"] for s in stats.values())
        print(f"  {label}: {fmt_e(worst)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
