#!/usr/bin/env python3
"""Scan corpus/validation/* probes for those exercising features shipped
2025-05 -> 2026-05 and not yet covered by the existing 50 benchmarks.

Output: ranked candidate list with feature-tag matrix. Does not modify
benchmark assets — pair with bootstrap_strategies.py to scaffold.

Usage:
    python benchmarks/runners/promote_corpus_probes.py --top 25
    python benchmarks/runners/promote_corpus_probes.py --emit-plan promote.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CORPUS = REPO_ROOT / "corpus" / "validation"
BENCH_RUNNERS = Path(__file__).resolve().parent
sys.path.insert(0, str(BENCH_RUNNERS))
from bootstrap_strategies import DEFAULT_PLAN  # noqa: E402

# Feature -> regex tested against (probe_name, README.md, strategy.pine, inputs.json).
FEATURE_PATTERNS: dict[str, re.Pattern[str]] = {
    "tz_hour_dayofweek":  re.compile(r"\b(hour\s*\(\s*time|dayofweek|set_chart_timezone)\b"),
    "magnifier_waypoints": re.compile(r"\b(magnifier|bar_magnifier)\b", re.I),
    "oca_cancel_sweep":    re.compile(r"\boca(_name)?\b", re.I),
    "fifo_pnl_perleg":     re.compile(r"\b(FIFO|per[_-]leg|avg_entry|pyramiding)\b", re.I),
    "strategy_exit_qty":   re.compile(r"strategy\.exit\b.*\b(qty|qty_percent|oca_name)\b", re.S),
    "ltf_security":        re.compile(r"request\.security.*?,\s*[\"']\d+[smh][\"']", re.S),
    "deferred_flip":       re.compile(r"\b(flip|deferred[-_]flip|opposite)\b", re.I),
    "rma_pine_formula":    re.compile(r"\bta\.rma\b"),
}

# Already-covered features (existing 50 strategies). Any candidate exercising
# a NEW feature beats one only exercising covered features.
COVERED = {
    "01-sma-cross", "02-inside-bar", "03-supertrend", "04-macd-histogram",
    # ... (full set populated from STRATEGIES at runtime)
}


def collect_probe_corpus_paths() -> set[str]:
    """Probes already promoted live as DEFAULT_PLAN[i][0] (corpus relpath)."""
    return {row[0] for row in DEFAULT_PLAN}


def scan_probe(probe_dir: Path) -> dict:
    name = probe_dir.name
    pine = (probe_dir / "strategy.pine").read_text(errors="ignore") if (probe_dir / "strategy.pine").exists() else ""
    readme = (probe_dir / "README.md").read_text(errors="ignore") if (probe_dir / "README.md").exists() else ""
    inputs = (probe_dir / "inputs.json").read_text(errors="ignore") if (probe_dir / "inputs.json").exists() else ""
    haystack = "\n".join((name, pine, readme, inputs))
    has_tv = (probe_dir / "tv_trades.csv").exists()
    features = sorted(f for f, p in FEATURE_PATTERNS.items() if p.search(haystack))
    return {"name": name, "has_tv": has_tv, "features": features, "feature_count": len(features)}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=25, help="emit top-N candidates")
    ap.add_argument("--emit-plan", type=Path, help="write JSON plan compatible with bootstrap_strategies.py")
    args = ap.parse_args()

    promoted = collect_probe_corpus_paths()
    candidates: list[dict] = []
    for d in sorted(CORPUS.iterdir()):
        if not d.is_dir():
            continue
        rel = f"validation/{d.name}"
        if rel in promoted:
            continue
        info = scan_probe(d)
        if not info["has_tv"]:
            continue
        if info["feature_count"] == 0:
            continue
        info["corpus_rel"] = rel
        candidates.append(info)

    candidates.sort(key=lambda c: (-c["feature_count"], c["name"]))
    top = candidates[: args.top]

    print(f"Total scanned: {sum(1 for _ in CORPUS.iterdir() if _.is_dir())}")
    print(f"With tv_trades + new feature(s): {len(candidates)}")
    print(f"Selected top {len(top)}:\n")
    print(f"{'#':>3} {'feat':>4} {'name':<60} features")
    for i, c in enumerate(top, start=51):
        feats = ",".join(c["features"])
        print(f"{i:>3} {c['feature_count']:>4} {c['name']:<60} {feats}")

    if args.emit_plan:
        next_idx = max(row[1] for row in DEFAULT_PLAN) + 1
        plan_rows = []
        for offset, c in enumerate(top):
            slug = c["name"]
            plan_rows.append([c["corpus_rel"], next_idx + offset, slug])
        args.emit_plan.write_text(json.dumps(plan_rows, indent=2))
        print(f"\nWrote {len(plan_rows)} rows to {args.emit_plan}")


if __name__ == "__main__":
    main()
