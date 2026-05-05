#!/usr/bin/env python3
"""Three-way trade-list comparator: TV ↔ PineForge ↔ PyneCore.

For each strategy folder under `benchmarks/` strategy fixtures (`data/` +
`strategies/` inline, or `assets/data` + `assets/strategies` submodule), reads
`tv_trades.csv`, `pineforge_trades.csv`, and `pynecore_trades.csv`,
clips all three to a common entry-time window, aligns trades by
direction + entry-time within that window, and reports:

    - Trade count (TV / PineForge / PyneCore) inside the common window
    - Entry-price p90 delta vs TV
    - Exit-price  p90 delta vs TV
    - PnL        p90 delta vs TV
    - 5-tier match degree (excellent / strong / moderate / weak / minimal)

The common window algorithm mirrors the canonical PineForge parity
sweep (`validate_detailed_report.py::common_entry_window_ms`):

    [lo, hi] = ohlcv_span ∩ tv_entry_span ∩ engine_entry_span

This is critical: TV's chart export typically covers ~3 weeks BEFORE
our OHLCV CSV starts (so we can't reproduce those trades — no bars),
and our 36k-bar OHLCV extends ~4 weeks AFTER TV's export ends (so the
engine fires entries TV's export doesn't include). Comparing without
clipping inflates the "count delta" by the union of those two
overhangs even when the engine is bit-perfect inside the window where
both have data.

Strict-tier thresholds match the parent project:
    count_rel_diff   < 1.0%
    entry_price_p90  < 0.01%
    exit_price_p90   < 0.01%
    pnl_p90          < 1.0%       (strict profile, no trail)
    pnl_p90          < 100%       (production profile, trail_* exits)

Output lives in `benchmarks/results/`:
    - trade_comparison.md   per-strategy table
    - summary.md            headline numbers (% match-degree per engine)

Usage:
    python benchmarks/compare.py
    python benchmarks/compare.py --strategy 01-sma-cross   # one strategy
    python benchmarks/compare.py --quiet                   # only summary
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timezone, timedelta
from pathlib import Path

_SYS_BENCH = Path(__file__).resolve().parent
if str(_SYS_BENCH) not in sys.path:
    sys.path.insert(0, str(_SYS_BENCH))
from paths import BENCH, DATA, REPO_ROOT, STRATEGIES  # noqa: E402

BENCH_DIR = BENCH

# OHLCV resolution order (first existing wins):
#   1. DATA/ETHUSDT_15.csv — snapshot (paths: benchmarks/assets/data or benchmarks/data)
#   2. benchmarks/_workdir/data/ETHUSDT_15.csv — working copy from run_all.sh
#   3. corpus/data/ohlcv_ETH-USDT-USDT_15m.csv — fallback
_CANDIDATE_OHLCV = [
    DATA / "ETHUSDT_15.csv",
    BENCH_DIR / "_workdir" / "data" / "ETHUSDT_15.csv",
    REPO_ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_15m.csv",
]
OHLCV_PATH = next((p for p in _CANDIDATE_OHLCV if p.exists()), _CANDIDATE_OHLCV[-1])

# Match window for entry-time alignment (matches parent project's gate)
MATCH_WINDOW_S = 3600
ENTRY_PRICE_GATE = 3.00

# Parity thresholds copied from the canonical
# `validate_detailed_report.py::DEFAULT_PARITY_{STRICT,PRODUCTION}`.
STRICT_COUNT = 0.01      # 1.0%
STRICT_ENTRY = 0.0001    # 0.01% — tick-level entry parity
STRICT_EXIT  = 0.0001    # 0.01% — tick-level exit parity
STRICT_PNL   = 0.01      # 1.0%

PRODUCTION_EXIT = 0.0005 # 0.05% — exits absorb sub-bar broker drift
PRODUCTION_PNL  = 1.0    # 100%  — trail_* exits intrinsically wide

# Detect strategies that use TradingView's trailing stops; these get
# the production threshold profile per validate_detailed_report.py.
_TRAIL_PATTERN = re.compile(r"\btrail_(points|offset|price)\s*=", re.IGNORECASE)
_LINE_COMMENT = re.compile(r"//.*?$", re.MULTILINE)
_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)

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
            dt = datetime.strptime(s, fmt)
            if tz_offset_hours == 0:
                dt = dt.replace(tzinfo=timezone.utc)
            else:
                dt = dt.replace(tzinfo=timezone(timedelta(hours=tz_offset_hours)))
            return int(dt.timestamp())
        except ValueError as e:
            last_err = e
    raise ValueError(f"unparseable time {s!r}: {last_err}")


def detect_parity_profile(pine_path: Path) -> str:
    """Return 'production' if the strategy uses trail_* exit params, else 'strict'.

    Mirrors `validate_detailed_report.py::detect_parity_profile`.
    """
    if not pine_path.exists():
        return "strict"
    src = pine_path.read_text(encoding="utf-8", errors="replace")
    src = _BLOCK_COMMENT.sub("", src)
    src = _LINE_COMMENT.sub("", src)
    return "production" if _TRAIL_PATTERN.search(src) else "strict"


def ohlcv_span_seconds() -> tuple[int, int]:
    """Return (first_ts_s, last_ts_s) of the corpus OHLCV feed."""
    with OHLCV_PATH.open() as f:
        reader = csv.DictReader(f)
        rows = [int(r["timestamp"]) for r in reader]
    return rows[0] // 1000, rows[-1] // 1000


def common_window(tv: list["Trade"], eng: list["Trade"],
                  ohlcv_lo: int, ohlcv_hi: int) -> tuple[int, int] | None:
    """Intersection of OHLCV span and both trade-list entry spans.

    Mirrors `validate_detailed_report.py::common_entry_window_ms`.
    """
    lo, hi = ohlcv_lo, ohlcv_hi
    if tv:
        lo = max(lo, min(t.entry_time for t in tv))
        hi = min(hi, max(t.entry_time for t in tv))
    if eng:
        lo = max(lo, min(t.entry_time for t in eng))
        hi = min(hi, max(t.entry_time for t in eng))
    return (lo, hi) if lo <= hi else None


def filter_to_window(trades: list["Trade"], lo: int, hi: int) -> list["Trade"]:
    return [t for t in trades if lo <= t.entry_time <= hi]


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
    n_engine_total: int        # full count, before window clip
    n_engine_in_window: int    # after window clip
    n_tv_in_window: int
    n_matched: int
    count_delta: float
    entry_p90: float
    exit_p90: float
    pnl_p90: float
    profile: str               # "strict" or "production"
    degree: int                # 1..5; 5=excellent, 1=minimal
    label: str                 # 'excellent' / 'strong' / 'moderate' / 'weak' / 'minimal'


# Mirrors `validate_detailed_report.py::MATCH_DEGREE_LABELS`.
DEGREE_LABEL = {5: "excellent", 4: "strong", 3: "moderate", 2: "weak", 1: "minimal"}


def classify_match_degree(*, count_d: float, entry_p90: float,
                          exit_p90: float, pnl_p90: float,
                          match_pct: float, profile: str) -> tuple[int, str]:
    """5-tier classifier matching the canonical sweep's heuristic.

    - excellent: every dimension passes the strict (or production) threshold,
      and ≥95% of TV trades have a matched engine trade.
    - strong: count + entry strict, exit/PnL within 5x strict (or absorbed
      by production envelope), match_pct ≥ 90%.
    - moderate: count + entry within 5x strict, match_pct ≥ 75%.
    - weak: match_pct ≥ 50%.
    - minimal: match_pct < 50%.
    """
    max_exit = STRICT_EXIT if profile == "strict" else PRODUCTION_EXIT
    max_pnl  = STRICT_PNL  if profile == "strict" else PRODUCTION_PNL

    excellent = (count_d  < STRICT_COUNT
                 and entry_p90 < STRICT_ENTRY
                 and exit_p90  < max_exit
                 and pnl_p90   < max_pnl
                 and match_pct >= 0.95)
    if excellent:
        return 5, DEGREE_LABEL[5]
    strong = (count_d  < STRICT_COUNT * 5
              and entry_p90 < STRICT_ENTRY * 5
              and exit_p90  < max_exit * 5
              and pnl_p90   < max_pnl * 1.5
              and match_pct >= 0.90)
    if strong:
        return 4, DEGREE_LABEL[4]
    moderate = (count_d  < STRICT_COUNT * 10
                and entry_p90 < STRICT_ENTRY * 10
                and match_pct >= 0.75)
    if moderate:
        return 3, DEGREE_LABEL[3]
    if match_pct >= 0.50:
        return 2, DEGREE_LABEL[2]
    return 1, DEGREE_LABEL[1]


def compute_diff(
    name: str,
    eng_full: list[Trade],
    tv_full: list[Trade],
    *,
    ohlcv_lo: int,
    ohlcv_hi: int,
    profile: str,
) -> EngineDiff:
    """Window-clipped TV-vs-engine diff.

    Both lists are restricted to the intersection of OHLCV bar span,
    TV entry span, and engine entry span before any other comparison.
    """
    win = common_window(tv_full, eng_full, ohlcv_lo, ohlcv_hi)
    if win is None:
        return EngineDiff(name, len(eng_full), 0, 0, 0,
                          1.0, 1.0, 1.0, 1.0, profile, 1, DEGREE_LABEL[1])
    lo, hi = win
    tv = filter_to_window(tv_full, lo, hi)
    eng = filter_to_window(eng_full, lo, hi)

    matched = align(tv, eng)
    n_tv = len(tv)
    n_eng = len(eng)
    if n_tv == 0 and n_eng == 0:
        return EngineDiff(name, len(eng_full), 0, 0, 0,
                          0.0, 0.0, 0.0, 0.0, profile, 5, DEGREE_LABEL[5])

    count_delta = abs(n_tv - n_eng) / max(n_tv, n_eng, 1)
    if matched:
        entry_p90 = percentile([relmax(t.entry_price, e.entry_price) for t, e in matched], 0.9)
        exit_p90  = percentile([relmax(t.exit_price,  e.exit_price)  for t, e in matched], 0.9)
        pnl_p90   = percentile(
            [abs(t.pnl - e.pnl) / abs(t.pnl)
             for t, e in matched if abs(t.pnl) > 0.01],
            0.9,
        )
    else:
        entry_p90 = exit_p90 = pnl_p90 = 1.0

    match_pct = len(matched) / max(n_tv, 1)
    degree, label = classify_match_degree(
        count_d=count_delta, entry_p90=entry_p90,
        exit_p90=exit_p90, pnl_p90=pnl_p90,
        match_pct=match_pct, profile=profile,
    )
    return EngineDiff(name, len(eng_full), n_eng, n_tv, len(matched),
                      count_delta, entry_p90, exit_p90, pnl_p90,
                      profile, degree, label)


def fmt_pct(x: float) -> str:
    return f"{x*100:.4f}%"


_DEGREE_EMOJI = {5: "🟢", 4: "🟢", 3: "🟡", 2: "🟠", 1: "🔴"}


def render_strategy_block(name: str, profile: str, tv_full: int,
                          diffs: list[EngineDiff]) -> str:
    lines = [f"### {name}  *(profile: {profile})*", ""]
    lines.append(f"- TV trades (raw): **{tv_full}**")
    if diffs:
        win_tv = diffs[0].n_tv_in_window
        lines.append(f"- TV trades inside common window: **{win_tv}**")
    for d in diffs:
        emoji = _DEGREE_EMOJI[d.degree]
        match_pct = 100 * d.n_matched / max(d.n_tv_in_window, 1)
        lines.append(
            f"- **{d.name}** {emoji} **{d.label}**  "
            f"(engine trades: {d.n_engine_total}, in-window: {d.n_engine_in_window}, "
            f"matched {d.n_matched} = {match_pct:.1f}% of TV-in-window)\n"
            f"    - count delta:  `{fmt_pct(d.count_delta)}`\n"
            f"    - entry p90:    `{fmt_pct(d.entry_p90)}`\n"
            f"    - exit  p90:    `{fmt_pct(d.exit_p90)}`\n"
            f"    - PnL   p90:    `{fmt_pct(d.pnl_p90)}`"
        )
    lines.append("")
    return "\n".join(lines)


def render_pf_pc_agreement(strategy: str, pf: list[Trade], pc: list[Trade],
                           ohlcv_lo: int, ohlcv_hi: int) -> str:
    """Engine-vs-engine agreement, clipped to the same common window."""
    win = common_window(pf, pc, ohlcv_lo, ohlcv_hi)
    if win is None:
        return f"### {strategy} — PineForge ↔ PyneCore\n\nno common window\n"
    lo, hi = win
    pf_w = filter_to_window(pf, lo, hi)
    pc_w = filter_to_window(pc, lo, hi)
    matched = align(pf_w, pc_w)
    if not matched:
        return f"### {strategy} — PineForge ↔ PyneCore\n\nno shared trades in window\n"
    count_delta = abs(len(pf_w) - len(pc_w)) / max(len(pf_w), len(pc_w), 1)
    entry_p90 = percentile([relmax(a.entry_price, b.entry_price) for a, b in matched], 0.9)
    exit_p90 = percentile([relmax(a.exit_price, b.exit_price) for a, b in matched], 0.9)
    pnl_p90 = percentile(
        [abs(a.pnl - b.pnl) / abs(a.pnl) for a, b in matched if abs(a.pnl) > 0.01],
        0.9,
    )
    return (
        f"### {strategy} — PineForge ↔ PyneCore agreement (in common window)\n\n"
        f"- shared trades: {len(matched)} / max({len(pf_w)}, {len(pc_w)})\n"
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

    strategies_root = STRATEGIES
    if args.strategy:
        strategy_dirs = [strategies_root / args.strategy]
    else:
        strategy_dirs = sorted(d for d in strategies_root.iterdir() if d.is_dir())

    ohlcv_lo, ohlcv_hi = ohlcv_span_seconds()

    sections: list[str] = [
        "# Trade comparison\n",
        "Each strategy is run through PineForge and PyneCore against the\n"
        "same 36k-bar OHLCV feed. PineTS is excluded from this report —\n"
        "their strategy backtester is a roadmap item (per [their\n"
        "README](https://github.com/LuxAlgo/PineTS#roadmap)). Both columns\n"
        "are diffed against the same `tv_trades.csv` ground truth.\n",
        "**Window-clipped comparison.** TV's chart export typically covers\n"
        "~3 weeks of history *before* this repo's OHLCV begins, and our\n"
        "OHLCV extends ~4 weeks *after* TV's export ends. To make the\n"
        "count fair, we clip both lists to\n"
        "`[OHLCV span] ∩ [TV entry span] ∩ [engine entry span]` before\n"
        "comparing — the same algorithm the canonical PineForge parity\n"
        "sweep (`validate_detailed_report.py::common_entry_window_ms`)\n"
        "uses.\n",
        "**5-tier match degree** mirrors the canonical sweep:\n"
        "🟢 *excellent* (count + all p90 strict, ≥95% match) → "
        "🟢 *strong* (within 5x strict, ≥90% match) → "
        "🟡 *moderate* → 🟠 *weak* → 🔴 *minimal*. "
        "Strategies that use TradingView's `trail_*` exits get the "
        "production threshold profile (exit p90 <0.05%, PnL p90 <100%) "
        "matching the canonical sweep.\n",
    ]
    summary_rows: list[tuple[str, str, int, EngineDiff, EngineDiff]] = []

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

        profile = detect_parity_profile(strat_dir / "strategy.pine")
        tv = parse_trades(tv_path, TV_TZ_OFFSET_HOURS)
        pf = parse_trades(pf_path, 0)
        pc = parse_trades(pc_path, 0)

        diffs = [
            compute_diff("PineForge", pf, tv, ohlcv_lo=ohlcv_lo, ohlcv_hi=ohlcv_hi, profile=profile),
            compute_diff("PyneCore",  pc, tv, ohlcv_lo=ohlcv_lo, ohlcv_hi=ohlcv_hi, profile=profile),
        ]
        summary_rows.append((strat_dir.name, profile, len(tv), diffs[0], diffs[1]))

        if not args.quiet:
            print(f"=== {strat_dir.name}  ({profile}) ===")
            print(f"  raw counts:    TV={len(tv)}  PineForge={len(pf)}  PyneCore={len(pc)}")
            for d in diffs:
                print(f"  {d.name:9s} {_DEGREE_EMOJI[d.degree]} {d.label:9s}  "
                      f"in-window TV={d.n_tv_in_window} engine={d.n_engine_in_window} "
                      f"matched={d.n_matched}  "
                      f"count={fmt_pct(d.count_delta):>9s}  "
                      f"entry={fmt_pct(d.entry_p90):>9s}  "
                      f"exit={fmt_pct(d.exit_p90):>9s}  "
                      f"pnl={fmt_pct(d.pnl_p90):>9s}")
            print()

        sections.append(render_strategy_block(strat_dir.name, profile, len(tv), diffs))
        sections.append(render_pf_pc_agreement(strat_dir.name, pf, pc, ohlcv_lo, ohlcv_hi))

    # Tally the 5-tier breakdown per engine.
    summary_lines = [
        "# Summary\n",
        "Match degree per the canonical PineForge parity sweep "
        "(window-clipped; trail_* strategies use production thresholds).\n",
        "| Strategy | Profile | TV (raw / win) | PineForge | PyneCore |",
        "|---|---|---|---|---|",
    ]
    pf_tally: dict[str, int] = {l: 0 for l in DEGREE_LABEL.values()}
    pc_tally: dict[str, int] = {l: 0 for l in DEGREE_LABEL.values()}
    for name, profile, tv_raw, pf_d, pc_d in summary_rows:
        pf_tally[pf_d.label] += 1
        pc_tally[pc_d.label] += 1
        summary_lines.append(
            f"| {name} | {profile} | {tv_raw} / {pf_d.n_tv_in_window} | "
            f"{_DEGREE_EMOJI[pf_d.degree]} {pf_d.label} ({pf_d.n_engine_in_window}) | "
            f"{_DEGREE_EMOJI[pc_d.degree]} {pc_d.label} ({pc_d.n_engine_in_window}) |"
        )
    summary_lines.append("")
    n = len(summary_rows)
    for label in ("excellent", "strong", "moderate", "weak", "minimal"):
        summary_lines.append(
            f"- **{label}**: PineForge {pf_tally[label]}/{n}, PyneCore {pc_tally[label]}/{n}"
        )

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
