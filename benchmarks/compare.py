#!/usr/bin/env python3
"""Three-way trade-list comparator: TV ↔ PineForge ↔ PyneCore.

For each strategy folder under `benchmarks/strategies/`, reads
`tv_trades.csv`, `pineforge_trades.csv`, and `pynecore_trades.csv`,
aligns trades by direction + entry-time within a 1-hour gating
window (matches the parent project's parity sweep), and reports:

    - Trade count (TV / PineForge / PyneCore)
    - Match rate vs TV
    - Entry-price p90 delta vs TV
    - Exit-price  p90 delta vs TV
    - PnL        p90 delta vs TV
    - PineForge ↔ PyneCore agreement on every dimension

Output lives in `benchmarks/results/`:
    - trade_comparison.md   per-strategy table
    - summary.md            headline numbers (% strict-pass per engine)

Usage:
    python benchmarks/compare.py
    python benchmarks/compare.py --strategy 01-sma-cross   # one strategy
    python benchmarks/compare.py --quiet                   # only summary
"""
from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH_DIR = REPO_ROOT / "benchmarks"

# Match window for entry-time alignment (matches parent project's gate)
MATCH_WINDOW_S = 3600
ENTRY_PRICE_GATE = 3.00

# Strict-tier thresholds (PineForge's parity profile)
STRICT_COUNT = 0.01
STRICT_ENTRY = 0.0001
STRICT_EXIT = 0.0001
STRICT_PNL = 0.01

# Wall-time at which TV's chart was set when the trades.csv was exported.
# Engine CSVs are emitted in UTC. Override via env var if your re-export
# uses a different chart timezone.
TV_TZ_OFFSET_HOURS = 8


@dataclass
class Trade:
    direction: str
    entry_time: int
    entry_price: float
    exit_time: int
    exit_price: float
    qty: float
    pnl: float


def parse_dt(s: str, tz_offset_hours: int) -> int:
    fmts = ["%Y-%m-%d %H:%M", "%Y-%m-%d %H:%M:%S"]
    last_err: Exception | None = None
    for fmt in fmts:
        try:
            tz = timezone.utc if tz_offset_hours == 0 else None
            dt = datetime.strptime(s, fmt)
            if tz is not None:
                dt = dt.replace(tzinfo=tz)
            else:
                from datetime import timedelta
                dt = dt.replace(tzinfo=timezone(timedelta(hours=tz_offset_hours)))
            return int(dt.timestamp())
        except ValueError as e:
            last_err = e
    raise ValueError(f"unparseable time {s!r}: {last_err}")


def parse_trades(path: Path, tz_offset_hours: int) -> list[Trade]:
    """Read TV-mirror CSV (two rows per trade, exit-then-entry, reverse-chrono)."""
    by_num: dict[int, dict] = {}
    with path.open(encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            n = int(row["Trade #"])
            r = by_num.setdefault(n, {})
            kind = row["Type"]
            time_field = row["Date and time"]
            price_field = "Price USDT" if "Price USDT" in row else "Price"
            price = float(row[price_field])
            qty = float(
                row.get("Position size (qty)")
                or row.get("Size (qty)")
                or row.get("Qty")
                or 0.0
            )
            pnl = float(row.get("Net P&L USD") or row.get("Net PnL") or 0.0)
            direction = "long" if "long" in kind.lower() else "short"
            r["direction"] = direction
            r["qty"] = qty
            r["pnl"] = pnl
            if kind.startswith("Entry"):
                r["entry_time"] = parse_dt(time_field, tz_offset_hours)
                r["entry_price"] = price
            else:
                r["exit_time"] = parse_dt(time_field, tz_offset_hours)
                r["exit_price"] = price

    pairs: list[Trade] = []
    for n in sorted(by_num):
        r = by_num[n]
        if "entry_price" not in r or "exit_price" not in r:
            continue
        pairs.append(Trade(
            direction=r["direction"],
            entry_time=r["entry_time"],
            entry_price=r["entry_price"],
            exit_time=r["exit_time"],
            exit_price=r["exit_price"],
            qty=r["qty"],
            pnl=r["pnl"],
        ))
    pairs.sort(key=lambda t: t.entry_time)
    return pairs


def align(a: list[Trade], b: list[Trade]) -> list[tuple[Trade, Trade]]:
    """Greedy entry-time alignment within MATCH_WINDOW_S; same-direction only."""
    matched: list[tuple[Trade, Trade]] = []
    used: set[int] = set()
    j_start = 0
    for at in a:
        while j_start < len(b) and b[j_start].entry_time < at.entry_time - MATCH_WINDOW_S:
            j_start += 1
        best = -1
        best_dt = MATCH_WINDOW_S + 1
        for j in range(j_start, len(b)):
            if j in used:
                continue
            bt = b[j]
            if bt.entry_time > at.entry_time + MATCH_WINDOW_S:
                break
            if bt.direction != at.direction:
                continue
            if abs(bt.entry_price - at.entry_price) > ENTRY_PRICE_GATE:
                continue
            dt = abs(bt.entry_time - at.entry_time)
            if dt < best_dt:
                best_dt = dt
                best = j
        if best >= 0:
            matched.append((at, b[best]))
            used.add(best)
    return matched


def relmax(a: float, b: float) -> float:
    return abs(a - b) / max(abs(a), abs(b), 1e-9)


def percentile(xs: list[float], p: float) -> float:
    if not xs:
        return 0.0
    s = sorted(xs)
    k = (len(s) - 1) * p
    f, c = int(k), min(int(k) + 1, len(s) - 1)
    return s[f] if f == c else s[f] * (c - k) + s[c] * (k - f)


@dataclass
class EngineDiff:
    name: str
    n_trades: int
    n_matched: int
    count_delta: float
    entry_p90: float
    exit_p90: float
    pnl_p90: float
    strict_pass: bool


def compute_diff(name: str, eng: list[Trade], tv: list[Trade]) -> EngineDiff:
    matched = align(tv, eng)
    if not matched:
        return EngineDiff(name, len(eng), 0, 1.0, 1.0, 1.0, 1.0, False)
    count_delta = relmax(len(tv), len(eng))
    entry_p90 = percentile([relmax(t.entry_price, e.entry_price) for t, e in matched], 0.9)
    exit_p90 = percentile([relmax(t.exit_price, e.exit_price) for t, e in matched], 0.9)
    pnl_p90 = percentile([relmax(t.pnl, e.pnl) for t, e in matched], 0.9)
    strict = (count_delta < STRICT_COUNT
              and entry_p90 < STRICT_ENTRY
              and exit_p90 < STRICT_EXIT
              and pnl_p90 < STRICT_PNL)
    return EngineDiff(name, len(eng), len(matched), count_delta,
                      entry_p90, exit_p90, pnl_p90, strict)


def fmt_pct(x: float) -> str:
    return f"{x*100:.4f}%"


def render_strategy_block(name: str, tv_n: int, diffs: list[EngineDiff]) -> str:
    lines = [f"### {name}", ""]
    lines.append(f"- TV trades: **{tv_n}**")
    for d in diffs:
        match_pct = 100 * d.n_matched / max(tv_n, 1)
        verdict = "✅ STRICT PASS" if d.strict_pass else "⚠ drift"
        lines.append(
            f"- **{d.name}** trades: {d.n_trades} (matched {d.n_matched}, "
            f"{match_pct:.1f}% of TV) — {verdict}\n"
            f"    - count delta:   `{fmt_pct(d.count_delta)}`\n"
            f"    - entry p90:     `{fmt_pct(d.entry_p90)}`\n"
            f"    - exit  p90:     `{fmt_pct(d.exit_p90)}`\n"
            f"    - PnL   p90:     `{fmt_pct(d.pnl_p90)}`"
        )
    lines.append("")
    return "\n".join(lines)


def render_pf_pc_agreement(strategy: str,
                           pf: list[Trade], pc: list[Trade]) -> str:
    """Engine-vs-engine agreement table (PineForge ↔ PyneCore)."""
    matched = align(pf, pc)
    if not matched:
        return f"### {strategy} — PineForge ↔ PyneCore\n\n**no shared trades**\n"
    count_delta = relmax(len(pf), len(pc))
    entry_p90 = percentile([relmax(a.entry_price, b.entry_price) for a, b in matched], 0.9)
    exit_p90 = percentile([relmax(a.exit_price, b.exit_price) for a, b in matched], 0.9)
    pnl_p90 = percentile([relmax(a.pnl, b.pnl) for a, b in matched], 0.9)
    return (
        f"### {strategy} — PineForge ↔ PyneCore agreement\n\n"
        f"- shared trades: {len(matched)} / max({len(pf)}, {len(pc)})\n"
        f"- count delta: `{fmt_pct(count_delta)}`\n"
        f"- entry p90:   `{fmt_pct(entry_p90)}`\n"
        f"- exit  p90:   `{fmt_pct(exit_p90)}`\n"
        f"- PnL   p90:   `{fmt_pct(pnl_p90)}`\n"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--strategy", help="Limit to one strategy folder (e.g. 01-sma-cross)")
    ap.add_argument("--quiet", action="store_true", help="Only print summary")
    ap.add_argument("--no-write", action="store_true",
                    help="Don't write Markdown reports — print to stdout instead")
    args = ap.parse_args()

    strategies_root = BENCH_DIR / "strategies"
    if args.strategy:
        strategy_dirs = [strategies_root / args.strategy]
    else:
        strategy_dirs = sorted(d for d in strategies_root.iterdir() if d.is_dir())

    sections: list[str] = ["# Trade comparison\n"]
    sections.append(
        "Each strategy is run through PineForge and PyneCore against the\n"
        "same 36k-bar OHLCV feed. PineTS is excluded from this report —\n"
        "their strategy backtester is a roadmap item (per [their\n"
        "README](https://github.com/LuxAlgo/PineTS#roadmap)). Both columns\n"
        "are diffed against the same `tv_trades.csv` ground truth.\n"
    )
    summary_rows: list[tuple[str, int, EngineDiff, EngineDiff]] = []

    for strat_dir in strategy_dirs:
        if not strat_dir.is_dir():
            continue
        tv_path = strat_dir / "tv_trades.csv"
        pf_path = strat_dir / "pineforge_trades.csv"
        pc_path = strat_dir / "pynecore_trades.csv"
        if not (tv_path.exists() and pf_path.exists() and pc_path.exists()):
            print(f"SKIP {strat_dir.name}: missing input(s) "
                  f"(tv={tv_path.exists()}, pf={pf_path.exists()}, pc={pc_path.exists()})",
                  file=sys.stderr)
            continue

        tv = parse_trades(tv_path, TV_TZ_OFFSET_HOURS)
        pf = parse_trades(pf_path, 0)
        pc = parse_trades(pc_path, 0)

        diffs = [
            compute_diff("PineForge", pf, tv),
            compute_diff("PyneCore", pc, tv),
        ]
        summary_rows.append((strat_dir.name, len(tv), diffs[0], diffs[1]))

        if not args.quiet:
            print(f"=== {strat_dir.name} ===")
            print(f"  TV: {len(tv)}   PineForge: {len(pf)}   PyneCore: {len(pc)}")
            for d in diffs:
                v = "OK" if d.strict_pass else "drift"
                print(f"  {d.name:9s}: matched={d.n_matched:5d}  "
                      f"count={fmt_pct(d.count_delta):>10s}  "
                      f"entry={fmt_pct(d.entry_p90):>10s}  "
                      f"exit={fmt_pct(d.exit_p90):>10s}  "
                      f"pnl={fmt_pct(d.pnl_p90):>10s}  -> {v}")
            print()

        sections.append(render_strategy_block(strat_dir.name, len(tv), diffs))
        sections.append(render_pf_pc_agreement(strat_dir.name, pf, pc))

    summary_lines = ["# Summary\n", "Strict-tier pass = count <1.0%, entry/exit p90 <0.01%, PnL p90 <1.0%.\n"]
    summary_lines.append("| Strategy | TV trades | PineForge | PyneCore |")
    summary_lines.append("|---|---:|---|---|")
    pf_pass = pc_pass = 0
    for name, tv_n, pf_d, pc_d in summary_rows:
        pf_v = "✅" if pf_d.strict_pass else "drift"
        pc_v = "✅" if pc_d.strict_pass else "drift"
        if pf_d.strict_pass:
            pf_pass += 1
        if pc_d.strict_pass:
            pc_pass += 1
        summary_lines.append(
            f"| {name} | {tv_n} | {pf_d.n_trades} {pf_v} | {pc_d.n_trades} {pc_v} |"
        )
    summary_lines.append("")
    summary_lines.append(f"**PineForge** strict-pass: {pf_pass}/{len(summary_rows)}")
    summary_lines.append(f"**PyneCore** strict-pass:  {pc_pass}/{len(summary_rows)}")

    if args.no_write:
        print("\n".join(sections))
        print()
        print("\n".join(summary_lines))
    else:
        results_dir = BENCH_DIR / "results"
        results_dir.mkdir(exist_ok=True)
        (results_dir / "trade_comparison.md").write_text("\n".join(sections), encoding="utf-8")
        (results_dir / "summary.md").write_text("\n".join(summary_lines), encoding="utf-8")
        print(f"wrote {results_dir.relative_to(REPO_ROOT)}/trade_comparison.md")
        print(f"wrote {results_dir.relative_to(REPO_ROOT)}/summary.md")
        print()
        print("\n".join(summary_lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
