#!/usr/bin/env python3
"""Verify a strategy in the corpus against TradingView's exported trades.

Reads `tv_trades.csv` and `engine_trades.csv` from a strategy folder under
`corpus/`, aligns trades by entry time + direction with a 1-hour window
(matches the parent project's parity sweep), and reports the largest
deviations in entry price, exit price, and per-trade P&L.

This mirrors the canonical corpus summary used by the runtime docs:
it applies common-window edge trimming, honours per-strategy metadata,
and reports the five parity labels used in the corpus README.

Usage:
  scripts/verify_corpus.py corpus/basic/greedy           # one strategy
  scripts/verify_corpus.py --all                         # entire corpus
  scripts/verify_corpus.py --category validation         # one category
  scripts/verify_corpus.py corpus/... --show-diffs 5     # show first 5 diffs
"""
from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from datetime import datetime, timezone, timedelta
from pathlib import Path

# Strict-profile thresholds — must match
# pineforge-utils/validator/validate.py::DEFAULT_PARITY_STRICT exactly.
# This verifier and the external validator both apply the same gates so a
# pass here implies a pass there (and vice versa). No production override.
STRICT_COUNT_DELTA = 0.01      # 1.0%
STRICT_ENTRY_DELTA = 0.0001    # 0.01%
STRICT_EXIT_DELTA  = 0.0001    # 0.01%
STRICT_PNL_DELTA   = 0.01      # 1.0%

STRONG_COUNT_DELTA = 0.05
STRONG_ENTRY_DELTA = 0.001
STRONG_EXIT_DELTA  = 0.005
STRONG_PNL_DELTA   = 1.0

# Match window for time-based alignment (matches parent project's gate).
MATCH_WINDOW_SECONDS = 3600    # 1 hour
ENTRY_PRICE_GATE     = 3.00    # $3 — defends against same-bar duplicates

# TradingView "Date and time" columns are bare wall-clock strings with no
# timezone marker. They reflect the chart's display timezone at export.
# The corpus in this repo was exported with the chart set to Asia/Taipei
# (UTC+8). Engine CSVs are emitted in UTC. Override at parse time if your
# own re-exports use a different chart timezone.
TV_CSV_TZ_OFFSET_HOURS = 8     # Asia/Taipei
ENGINE_CSV_TZ_OFFSET_HOURS = 0 # UTC

TV_TZ_BY_NAME = {
    "utc_plus_8": 8,
    "asia_taipei": 8,
    "utc": 0,
}


@dataclass
class TradePair:
    direction: str        # 'long' / 'short'
    entry_time: int       # unix seconds
    entry_price: float
    exit_time: int        # unix seconds
    exit_price: float
    qty: float
    pnl: float
    trade_num: int = 0


def parse_dt(s: str, tz_offset_hours: int) -> int:
    """Parse 'YYYY-MM-DD HH:MM' (wall time in tz_offset_hours) as unix seconds (UTC)."""
    tz = timezone(timedelta(hours=tz_offset_hours))
    return int(datetime.strptime(s, "%Y-%m-%d %H:%M").replace(tzinfo=tz).timestamp())


def parse_trades(csv_path: Path, *, tz_offset_hours: int) -> list[TradePair]:
    by_num: dict[int, dict] = {}
    # TradingView exports include a UTF-8 BOM; utf-8-sig strips it.
    with csv_path.open(encoding="utf-8-sig") as f:
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

    pairs: list[TradePair] = []
    for n in sorted(by_num):
        r = by_num[n]
        if "entry_price" not in r or "exit_price" not in r:
            continue
        pairs.append(TradePair(
            direction=r["direction"],
            entry_time=r["entry_time"],
            entry_price=r["entry_price"],
            exit_time=r["exit_time"],
            exit_price=r["exit_price"],
            qty=r["qty"],
            pnl=r["pnl"],
            trade_num=n,
        ))
    pairs.sort(key=lambda t: t.entry_time)
    return pairs


def load_strategy_metadata(strategy_dir: Path) -> dict:
    inputs_path = strategy_dir / "inputs.json"
    if not inputs_path.exists():
        return {}
    import json
    with inputs_path.open(encoding="utf-8") as f:
        return json.load(f)


def tv_timezone_offset(meta: dict) -> int:
    tz_name = str(meta.get("tv_trades_csv_tz", "")).lower()
    return TV_TZ_BY_NAME.get(tz_name, TV_CSV_TZ_OFFSET_HOURS)


def align_by_time(tv: list[TradePair], eng: list[TradePair]) -> list[tuple[TradePair, TradePair]]:
    """Greedy time-window alignment: pair TV[i] with the closest engine trade
    that has the same direction and an entry within MATCH_WINDOW_SECONDS.
    """
    matched: list[tuple[TradePair, TradePair]] = []
    used_eng: set[int] = set()
    j_start = 0
    for tv_t in tv:
        # Advance the engine cursor to the first plausibly-matching candidate.
        while j_start < len(eng) and eng[j_start].entry_time < tv_t.entry_time - MATCH_WINDOW_SECONDS:
            j_start += 1
        best_j = -1
        best_dt = MATCH_WINDOW_SECONDS + 1
        for j in range(j_start, len(eng)):
            if j in used_eng:
                continue
            e = eng[j]
            if e.entry_time > tv_t.entry_time + MATCH_WINDOW_SECONDS:
                break
            if e.direction != tv_t.direction:
                continue
            if abs(e.entry_price - tv_t.entry_price) > ENTRY_PRICE_GATE:
                continue
            dt = abs(e.entry_time - tv_t.entry_time)
            if dt < best_dt:
                best_dt = dt
                best_j = j
        if best_j >= 0:
            matched.append((tv_t, eng[best_j]))
            used_eng.add(best_j)
    return matched


def trim_to_common_match_window(
    tv: list[TradePair],
    eng: list[TradePair],
    matched: list[tuple[TradePair, TradePair]],
) -> tuple[list[TradePair], list[TradePair]]:
    """Drop leading/trailing edge trades outside the mutually matched window.

    The canonical parity sweep compares the common in-window trade region.
    This avoids failing a strategy because one engine has an extra bootstrap or
    terminal trade just outside the overlapping validation window.
    """
    if not matched:
        return tv, eng
    lo = min(min(t.entry_time, e.entry_time) for t, e in matched) - MATCH_WINDOW_SECONDS
    hi = max(max(t.entry_time, e.entry_time) for t, e in matched) + MATCH_WINDOW_SECONDS
    tv_trim = [t for t in tv if lo <= t.entry_time <= hi]
    eng_trim = [e for e in eng if lo <= e.entry_time <= hi]
    return tv_trim, eng_trim


def relative_max(a: float, b: float) -> float:
    denom = max(abs(a), abs(b), 1e-9)
    return abs(a - b) / denom


def percentile(xs: list[float], p: float) -> float:
    if not xs: return 0.0
    s = sorted(xs)
    k = (len(s) - 1) * p
    f, c = int(k), min(int(k) + 1, len(s) - 1)
    if f == c: return s[f]
    return s[f] * (c - k) + s[c] * (k - f)


def verify_one(strategy_dir: Path, *, verbose: bool = True, show_diffs: int = 0) -> str:
    rel = strategy_dir.name
    if strategy_dir.parent.name in {"basic", "community", "validation"}:
        rel = f"{strategy_dir.parent.name}/{strategy_dir.name}"
    meta = load_strategy_metadata(strategy_dir)
    tv_path = strategy_dir / str(meta.get("tv_trades_csv", "tv_trades.csv"))
    eng_path = strategy_dir / "engine_trades.csv"
    if not tv_path.exists() or not eng_path.exists():
        if verbose:
            print(f"{rel}\n  MISSING (tv: {tv_path.exists()}, engine: {eng_path.exists()})")
        return "missing"

    tv = parse_trades(tv_path, tz_offset_hours=tv_timezone_offset(meta))
    eng = parse_trades(eng_path, tz_offset_hours=ENGINE_CSV_TZ_OFFSET_HOURS)
    matched = align_by_time(tv, eng)
    tv_cmp, eng_cmp = trim_to_common_match_window(tv, eng, matched)
    matched = align_by_time(tv_cmp, eng_cmp)

    if not matched:
        if verbose:
            print(f"{rel}: TV={len(tv)} engine={len(eng)} matched=0  (no aligned trades)")
        return "excellent" if len(tv_cmp) == 0 and len(eng_cmp) == 0 else "minimal"

    count_delta = relative_max(len(tv_cmp), len(eng_cmp))
    entry_deltas = [relative_max(t.entry_price, e.entry_price) for t, e in matched]
    exit_deltas  = [relative_max(t.exit_price,  e.exit_price)  for t, e in matched]
    pnl_deltas   = [relative_max(t.pnl,         e.pnl)         for t, e in matched]

    entry_p90 = percentile(entry_deltas, 0.90)
    exit_p90  = percentile(exit_deltas,  0.90)
    pnl_p90   = percentile(pnl_deltas,   0.90)

    profile = "strict"
    count_ok = count_delta < STRICT_COUNT_DELTA
    entry_ok = entry_p90  < STRICT_ENTRY_DELTA
    exit_ok  = exit_p90   < STRICT_EXIT_DELTA
    pnl_ok   = pnl_p90    < STRICT_PNL_DELTA
    all_ok   = count_ok and entry_ok and exit_ok and pnl_ok
    if all_ok:
        label = "excellent"
    elif (
        len(matched) / max(len(tv_cmp), 1) >= 0.99
        and count_delta < STRONG_COUNT_DELTA
        and entry_p90 < STRONG_ENTRY_DELTA
        and exit_p90 < STRONG_EXIT_DELTA
        and pnl_p90 < STRONG_PNL_DELTA
    ):
        label = "strong"
    elif len(matched) / max(len(tv_cmp), 1) >= 0.90:
        label = "moderate"
    elif matched:
        label = "weak"
    else:
        label = "minimal"

    if verbose:
        check = lambda b: "OK" if b else "X"
        match_pct = 100.0 * len(matched) / max(len(tv), 1)
        print(
            f"{rel}\n"
            f"  Profile:       {profile}\n"
            f"  TV trades:     {len(tv_cmp)}  (raw {len(tv)})\n"
            f"  Engine trades: {len(eng_cmp)}  (raw {len(eng)})\n"
            f"  Matched:       {len(matched)} ({match_pct:.1f}% of TV)\n"
            f"  Count delta:           {count_delta * 100:8.4f}%  ({check(count_ok)})\n"
            f"  Entry-price p90 delta: {entry_p90  * 100:8.4f}%  ({check(entry_ok)})\n"
            f"  Exit-price  p90 delta: {exit_p90   * 100:8.4f}%  ({check(exit_ok)})\n"
            f"  PnL         p90 delta: {pnl_p90    * 100:8.4f}%  ({check(pnl_ok)})\n"
            f"  -> {label}"
        )
        if show_diffs > 0:
            # Show the trades with the worst deltas
            ranked = sorted(zip(matched, entry_deltas, exit_deltas, pnl_deltas),
                            key=lambda x: -max(x[1], x[2], x[3]))
            print(f"\n  worst {show_diffs} matched trades by max-of-(entry, exit, pnl) delta:")
            for (tv_t, e_t), ed, xd, pd in ranked[:show_diffs]:
                print(
                    f"    TV  #{tv_t.trade_num:4d} {tv_t.direction:5s} "
                    f"@{datetime.fromtimestamp(tv_t.entry_time, tz=timezone.utc):%Y-%m-%d %H:%M} "
                    f"entry={tv_t.entry_price:10.4f} exit={tv_t.exit_price:10.4f} pnl={tv_t.pnl:+10.4f}"
                )
                print(
                    f"    eng #{e_t.trade_num:4d} {e_t.direction:5s} "
                    f"@{datetime.fromtimestamp(e_t.entry_time, tz=timezone.utc):%Y-%m-%d %H:%M} "
                    f"entry={e_t.entry_price:10.4f} exit={e_t.exit_price:10.4f} pnl={e_t.pnl:+10.4f}"
                )
                print(
                    f"           deltas: entry={ed*100:.4f}% exit={xd*100:.4f}% pnl={pd*100:.4f}%"
                )
    return label


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("strategy_dir", nargs="?", help="Single strategy folder, e.g. corpus/basic/greedy")
    ap.add_argument("--all", action="store_true", help="Verify every strategy in corpus/")
    ap.add_argument("--category", choices=["basic", "community", "validation"],
                    help="Verify all strategies in one category")
    ap.add_argument("--show-diffs", type=int, default=0,
                    help="With single-strategy mode, show this many worst-deviation matched trades")
    ap.add_argument("--quiet", action="store_true", help="Print only summary")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    corpus_root = repo_root / "corpus"

    if args.strategy_dir:
        label = verify_one(Path(args.strategy_dir).resolve(),
                           show_diffs=args.show_diffs)
        return 0 if label in {"excellent", "strong"} else 1

    if args.all or args.category:
        cats = [args.category] if args.category else ["basic", "community", "validation"]
        n_total = 0
        counts = {k: 0 for k in ("excellent", "strong", "moderate", "weak", "minimal", "missing")}
        n_fail: list[str] = []
        for cat in cats:
            for strat in sorted((corpus_root / cat).iterdir()):
                if not strat.is_dir():
                    continue
                label = verify_one(strat, verbose=not args.quiet)
                if not args.quiet:
                    print()
                n_total += 1
                counts[label] = counts.get(label, 0) + 1
                if label not in {"excellent", "strong"}:
                    n_fail.append(f"{cat}/{strat.name}")
        print()
        print(
            "Verified "
            f"{n_total} strategies — "
            f"excellent={counts['excellent']}, strong={counts['strong']}, "
            f"moderate={counts['moderate']}, weak={counts['weak']}, "
            f"minimal={counts['minimal']}, missing={counts['missing']}"
        )
        if n_fail:
            print()
            print("Below strong tier:")
            for s in n_fail:
                print(f"  {s}")
        return 0 if not n_fail else 1

    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
