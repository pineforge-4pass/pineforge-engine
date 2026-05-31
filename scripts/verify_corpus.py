#!/usr/bin/env python3
"""Verify a strategy in the corpus against TradingView's exported trades.

Reads `tv_trades.csv` and `engine_trades.csv` from a strategy folder under
`corpus/`, aligns trades by entry time + direction with a 1-hour window
(matches the parent project's parity sweep), and reports the largest
deviations in entry price, exit price, and per-trade P&L.

This mirrors the canonical corpus summary used by the runtime docs:
it applies common-window edge trimming, honours per-strategy metadata,
and reports the five parity labels used in the corpus README.

`--all` covers the three reference-tier categories — `basic/`, `community/`,
`validation/` — i.e. 167 strategies after the equity-mirror anomaly probe
moved to `parity-anomalies/`. The `parity-anomalies/` category is a home
for probes that *deliberately* surface TV-side non-determinism (engine is
correct, divergence is documented in `pineforge-utils/parity-anomalies/`);
it is excluded from `--all` by default so it doesn't mask as a regression.
Use `--include-anomalies` to fold it into the same sweep, or run it
explicitly with `--category parity-anomalies`.

Usage:
  scripts/verify_corpus.py corpus/basic/greedy           # one strategy
  scripts/verify_corpus.py --all                         # 167 reference strategies
  scripts/verify_corpus.py --all --include-anomalies     # + parity-anomalies/
  scripts/verify_corpus.py --category validation         # one category
  scripts/verify_corpus.py --category parity-anomalies   # anomaly probes only
  scripts/verify_corpus.py corpus/... --show-diffs 5     # show first 5 diffs
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timezone, timedelta
from pathlib import Path

# Strict-profile thresholds — must match
# pineforge-utils/validator/validate.py::DEFAULT_PARITY_STRICT exactly.
# Two profiles available, mirroring canonical:
#  - STRICT: tight on all four dims (indicator-style strategies whose exits
#    land on deterministic OHLC levels).
#  - PRODUCTION: relaxes exit (5x) and per-trade pnl (~100x) to absorb TV's
#    broker-emulator sub-bar fill drift. Auto-applied when strategy.pine uses
#    any trail_* parameter on strategy.exit.
STRICT_COUNT_DELTA = 0.01      # 1.0%
STRICT_ENTRY_DELTA = 0.0001    # 0.01%
STRICT_EXIT_DELTA  = 0.0001    # 0.01%
STRICT_PNL_DELTA   = 0.01      # 1.0%

PRODUCTION_COUNT_DELTA = 0.01    # 1.0%   — same as strict
PRODUCTION_ENTRY_DELTA = 0.0001  # 0.01%  — entries must stay tight
PRODUCTION_EXIT_DELTA  = 0.0005  # 0.05%  — exits absorb sub-bar broker drift
PRODUCTION_PNL_DELTA   = 1.0     # 100%   — gate only catastrophic divergence

STRONG_COUNT_DELTA = 0.05
STRONG_ENTRY_DELTA = 0.001
STRONG_EXIT_DELTA  = 0.005
STRONG_PNL_DELTA   = 1.0

# Pine-source comment-strippers + trail_* matcher for profile auto-detect.
# Matches canonical pineforge-utils/validator/validate.py::detect_parity_profile.
_LINE_COMMENT_PATTERN = re.compile(r"//.*?$", re.MULTILINE)
_BLOCK_COMMENT_PATTERN = re.compile(r"/\*.*?\*/", re.DOTALL)
_TRAIL_PATTERN = re.compile(r"\btrail_(points|offset|price)\s*=", re.IGNORECASE)

# Per-strategy near-zero PnL filter: trades whose |tv_pnl| < $0.01 are
# excluded from pnl p90 so scratch trades don't blow up the per-trade ratio.
# Mirrors canonical validate.py line ~1136.
PNL_NEAR_ZERO_USD = 0.01


def detect_parity_profile(pine_source: str) -> str:
    """Return 'production' if pine source uses any trail_* exit parameter, else 'strict'.

    Backport of pineforge-utils/validator/validate.py::detect_parity_profile.
    """
    cleaned = _BLOCK_COMMENT_PATTERN.sub("", pine_source)
    cleaned = _LINE_COMMENT_PATTERN.sub("", cleaned)
    return "production" if _TRAIL_PATTERN.search(cleaned) else "strict"


def parity_for_profile(profile: str) -> dict:
    """Return a fresh threshold dict for the given profile name."""
    if profile == "production":
        return {
            "count": PRODUCTION_COUNT_DELTA,
            "entry": PRODUCTION_ENTRY_DELTA,
            "exit":  PRODUCTION_EXIT_DELTA,
            "pnl":   PRODUCTION_PNL_DELTA,
        }
    return {
        "count": STRICT_COUNT_DELTA,
        "entry": STRICT_ENTRY_DELTA,
        "exit":  STRICT_EXIT_DELTA,
        "pnl":   STRICT_PNL_DELTA,
    }


def resolve_profile(strategy_dir: Path, meta: dict) -> str:
    """Resolve the parity profile for a strategy: inputs.json wins, then auto-detect."""
    forced = str(meta.get("parity_profile", "")).lower()
    if forced in {"strict", "production"}:
        return forced
    pine_path = strategy_dir / "strategy.pine"
    if pine_path.is_file():
        try:
            return detect_parity_profile(pine_path.read_text(encoding="utf-8"))
        except OSError:
            return "strict"
    return "strict"


def interior_time_bounds(
    trim_bars: int,
    warmup_bars: int,
    ohlcv_first_ms: int | None,
    ohlcv_last_ms: int | None,
    bar_ms: int,
) -> tuple[int, int] | None:
    """Return [lo_ms, hi_ms] window for 'interior' entries, or None if trivial.

    Backport of pineforge-utils/validator/validate.py::interior_time_bounds.
    ``trim_bars`` symmetrically excludes edge bars from BOTH ends; ``warmup_bars``
    is an extra asymmetric lead pad. If the OHLCV span is unknown (no source
    csv next to the probe), returns None — we then skip interior trimming and
    rely on the headline stats.
    """
    if trim_bars <= 0 and warmup_bars <= 0:
        return None
    if ohlcv_first_ms is None or ohlcv_last_ms is None or bar_ms <= 0:
        return None
    lead_pad = (trim_bars + max(0, warmup_bars)) * bar_ms
    tail_pad = trim_bars * bar_ms
    lo = ohlcv_first_ms + lead_pad
    hi = ohlcv_last_ms - tail_pad
    if lo >= hi:
        return None
    return lo, hi


def is_interior(entry_ms: int, bounds: tuple[int, int] | None) -> bool:
    if bounds is None:
        return True
    lo, hi = bounds
    return lo <= entry_ms <= hi


def _ohlcv_span_ms(strategy_dir: Path) -> tuple[int | None, int | None, int]:
    """Best-effort: read first/last timestamps + bar interval from an OHLCV csv.

    Looks for ``ohlcv.csv`` (or ``data.csv``) next to strategy.pine. Honors a
    ``bar_ms`` override in inputs.json. Returns (first_ms, last_ms, bar_ms);
    any of these may be None/0 if unavailable, in which case interior trimming
    is skipped.
    """
    candidates = [strategy_dir / n for n in ("ohlcv.csv", "data.csv", "candles.csv")]
    csv_path = next((p for p in candidates if p.is_file()), None)
    if csv_path is None:
        return None, None, 0
    try:
        with csv_path.open(encoding="utf-8-sig") as f:
            reader = csv.reader(f)
            header = next(reader, None)
            if not header:
                return None, None, 0
            # Find a timestamp-like column.
            ts_idx = None
            for i, name in enumerate(header):
                if name.strip().lower() in {"timestamp", "time", "ts", "open_time"}:
                    ts_idx = i
                    break
            if ts_idx is None:
                return None, None, 0
            first = None
            second = None
            last = None
            for row in reader:
                if not row:
                    continue
                try:
                    v = int(float(row[ts_idx]))
                except (ValueError, IndexError):
                    continue
                if first is None:
                    first = v
                elif second is None:
                    second = v
                last = v
            bar_ms = (second - first) if (first is not None and second is not None) else 0
            # Normalize seconds to ms if it looks like seconds.
            if first is not None and first < 10**12:
                first *= 1000
                if last is not None:
                    last *= 1000
                if bar_ms:
                    bar_ms *= 1000
            return first, last, bar_ms
    except OSError:
        return None, None, 0

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
            # TradingView renamed export columns over time: "Trade #" ->
            # "Trade number", "Net P&L *" -> "Net PnL *", and the price column
            # is quote-currency-suffixed ("Price USDT" / "Price USD" / bare
            # "Price"). Accept all spellings so both legacy and current
            # exports parse.
            n = int(row.get("Trade #") or row["Trade number"])
            r = by_num.setdefault(n, {})
            kind = row["Type"]
            time_field = row["Date and time"]
            price_field = next(
                (c for c in row if c == "Price" or c.startswith("Price ")),
                None,
            )
            price = float(row[price_field])
            qty = float(
                row.get("Position size (qty)")
                or row.get("Size (qty)")
                or row.get("Qty")
                or 0.0
            )
            pnl = float(
                row.get("Net P&L USD") or row.get("Net PnL USD")
                or row.get("Net P&L") or row.get("Net PnL") or 0.0
            )
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
    if strategy_dir.parent.name in {"basic", "community", "validation", "parity-anomalies"}:
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

    # Resolve parity profile (strict vs production) per probe.
    profile = resolve_profile(strategy_dir, meta)
    thresh = parity_for_profile(profile)

    # Optional interior trim: drop edge / warmup bars from the headline stats.
    trim_bars = int(meta.get("trim_bars", 0) or 0)
    warmup_bars = int(meta.get("warmup_bars", 0) or 0)
    first_ms, last_ms, bar_ms = _ohlcv_span_ms(strategy_dir)
    bounds = interior_time_bounds(trim_bars, warmup_bars, first_ms, last_ms, bar_ms)

    def _split_interior(pairs: list[tuple[TradePair, TradePair]]) -> list[tuple[TradePair, TradePair]]:
        if bounds is None:
            return pairs
        return [(t, e) for (t, e) in pairs if is_interior(t.entry_time * 1000, bounds)]

    interior_matched = _split_interior(matched)
    # Use interior-only matched pairs for headline gating when bounds are set.
    gating_matched = interior_matched if (bounds is not None and interior_matched) else matched
    # Interior-aware count totals: trim the tv/eng pools too when bounds given.
    if bounds is not None:
        tv_gate = [t for t in tv_cmp if is_interior(t.entry_time * 1000, bounds)]
        eng_gate = [e for e in eng_cmp if is_interior(e.entry_time * 1000, bounds)]
    else:
        tv_gate, eng_gate = tv_cmp, eng_cmp

    count_delta = relative_max(len(tv_gate), len(eng_gate))
    entry_deltas = [relative_max(t.entry_price, e.entry_price) for t, e in gating_matched]
    exit_deltas  = [relative_max(t.exit_price,  e.exit_price)  for t, e in gating_matched]
    # PnL p90 uses *relative-to-tv_pnl*, with near-zero trades excluded so
    # scratch trades don't blow up the ratio. Mirrors canonical line ~1136.
    pnl_deltas: list[float] = []
    for t, e in gating_matched:
        if abs(t.pnl) < PNL_NEAR_ZERO_USD:
            continue
        pnl_deltas.append(abs(t.pnl - e.pnl) / abs(t.pnl))

    entry_p90 = percentile(entry_deltas, 0.90)
    exit_p90  = percentile(exit_deltas,  0.90)
    pnl_p90   = percentile(pnl_deltas,   0.90) if pnl_deltas else 0.0

    count_ok = count_delta < thresh["count"]
    entry_ok = entry_p90  < thresh["entry"]
    exit_ok  = exit_p90   < thresh["exit"]
    pnl_ok   = pnl_p90    < thresh["pnl"]
    all_ok   = count_ok and entry_ok and exit_ok and pnl_ok
    if all_ok:
        label = "excellent"
    elif (
        len(gating_matched) / max(len(tv_gate), 1) >= 0.99
        and count_delta < STRONG_COUNT_DELTA
        and entry_p90 < STRONG_ENTRY_DELTA
        and exit_p90 < STRONG_EXIT_DELTA
        and pnl_p90 < STRONG_PNL_DELTA
    ):
        label = "strong"
    elif len(gating_matched) / max(len(tv_gate), 1) >= 0.90:
        label = "moderate"
    elif gating_matched:
        label = "weak"
    else:
        label = "minimal"

    # Per-probe override: if inputs.json declares an `expected_tier` of
    # "anomaly" or "engine_only", honor it instead of the raw computed tier.
    # Mirrors canonical pineforge-utils/validator/validate.py semantics:
    #   - "anomaly"     = engine produces correct output; TV is non-deterministic
    #                     or wrong on this probe (documented divergence). Reported
    #                     separately so it doesn't mask as a regression.
    #   - "engine_only" = probe is engine-only by design (no faithful TV reference);
    #                     surfaced separately, excluded from headline parity counts.
    # The override only fires when the computed tier is below `excellent` so a
    # future engine improvement that genuinely matches TV is NOT silently masked.
    expected_tier = str(meta.get("expected_tier", "")).strip().lower()
    if expected_tier in {"anomaly", "engine_only"} and label != "excellent":
        label = expected_tier

    # Per-probe override (canonical schema): inputs.json may declare
    # `validation_overrides.expect_tv_match: false` for probes where the
    # engine is correct but TV intentionally diverges (e.g. documented
    # TV-side boundary-bar non-determinism). Mirrors canonical
    # pineforge-utils/validator/validate.py::expect_tv_match=False handling
    # (search ENGINE_ONLY_LABEL there): relabel as `engine_only` so it
    # doesn't mask as a low-tier regression. Same `excellent` guard as
    # above so a future engine fix that genuinely matches TV is not
    # silently masked.
    val_overrides = meta.get("validation_overrides") or {}
    expect_tv_match = bool(val_overrides.get("expect_tv_match", True))
    if not expect_tv_match and label != "excellent":
        label = "engine_only"

    if verbose:
        check = lambda b: "OK" if b else "X"
        match_pct = 100.0 * len(matched) / max(len(tv), 1)
        interior_line = ""
        if bounds is not None:
            interior_line = (
                f"  Interior-only: tv={len(tv_gate)} eng={len(eng_gate)} "
                f"matched={len(gating_matched)} (trim_bars={trim_bars}, warmup_bars={warmup_bars})\n"
            )
        print(
            f"{rel}\n"
            f"  Profile:       {profile}\n"
            f"  TV trades:     {len(tv_cmp)}  (raw {len(tv)})\n"
            f"  Engine trades: {len(eng_cmp)}  (raw {len(eng)})\n"
            f"  Matched:       {len(matched)} ({match_pct:.1f}% of TV)\n"
            f"{interior_line}"
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
    ap.add_argument("--all", action="store_true",
                    help="Verify all reference-tier strategies (basic/, community/, validation/). "
                         "parity-anomalies/ is excluded unless --include-anomalies is given.")
    ap.add_argument("--category", choices=["basic", "community", "validation", "parity-anomalies"],
                    help="Verify all strategies in one category")
    ap.add_argument("--include-anomalies", action="store_true",
                    help="With --all, also include parity-anomalies/ probes. These deliberately "
                         "surface TV-side non-determinism and are documented in "
                         "pineforge-utils/parity-anomalies/. Folding them in degrades the headline "
                         "tier counts, so off by default.")
    ap.add_argument("--show-diffs", type=int, default=0,
                    help="With single-strategy mode, show this many worst-deviation matched trades")
    ap.add_argument("--quiet", action="store_true", help="Print only summary")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    corpus_root = repo_root / "corpus"

    if args.strategy_dir:
        label = verify_one(Path(args.strategy_dir).resolve(),
                           show_diffs=args.show_diffs)
        # `anomaly` and `engine_only` are documented success outcomes
        # (probe is meeting its declared expected_tier).
        return 0 if label in {"excellent", "strong", "anomaly", "engine_only"} else 1

    if args.all or args.category:
        if args.category:
            cats = [args.category]
        else:
            cats = ["basic", "community", "validation"]
            if args.include_anomalies:
                cats.append("parity-anomalies")
        n_total = 0
        counts = {k: 0 for k in ("excellent", "strong", "moderate", "weak",
                                  "minimal", "anomaly", "engine_only", "missing")}
        n_fail: list[str] = []
        for cat in cats:
            cat_root = corpus_root / cat
            if not cat_root.is_dir():
                continue
            for strat in sorted(cat_root.iterdir()):
                if not strat.is_dir():
                    continue
                # Skip the symbol-specified/ container — its children
                # require non-default OHLCV + per-symbol syminfo (pending
                # pineforge-data). Excluded from corpus headline; engine
                # correctness for those surfaces is validated via ctest.
                if (strat / "strategy.pine").is_file() is False and \
                   strat.name == "symbol-specified":
                    continue
                label = verify_one(strat, verbose=not args.quiet)
                if not args.quiet:
                    print()
                n_total += 1
                counts[label] = counts.get(label, 0) + 1
                # `anomaly` and `engine_only` are documented success outcomes
                # (declared via inputs.json::expected_tier) — not failures.
                if label not in {"excellent", "strong", "anomaly", "engine_only"}:
                    n_fail.append(f"{cat}/{strat.name}")
        print()
        print(
            "Verified "
            f"{n_total} strategies — "
            f"excellent={counts['excellent']}, strong={counts['strong']}, "
            f"moderate={counts['moderate']}, weak={counts['weak']}, "
            f"minimal={counts['minimal']}, anomaly={counts['anomaly']}, "
            f"engine_only={counts['engine_only']}, missing={counts['missing']}"
        )
        # Emit a one-liner about parity-anomalies/ when --all skipped them, so
        # the user knows there's more to inspect on demand.
        anomaly_dir = corpus_root / "parity-anomalies"
        if args.all and not args.include_anomalies and anomaly_dir.is_dir():
            n_anomaly = sum(1 for d in anomaly_dir.iterdir() if d.is_dir())
            if n_anomaly:
                print(
                    f"(skipped {n_anomaly} probe(s) in parity-anomalies/ — these surface "
                    "TV-side non-determinism, see corpus/parity-anomalies/README.md; "
                    "rerun with --include-anomalies to fold them in)"
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
