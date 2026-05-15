#!/usr/bin/env python3
"""Three-way trade-list comparator: TV ↔ PineForge ↔ PyneCore.

For each strategy folder under `benchmarks/` strategy fixtures (`data/` +
`strategies/` inline, or `assets/data` + `assets/strategies` submodule), reads
`tv_trades.csv`, `pineforge_trades.csv`, and `pynecore_trades.csv`,
aligns trades by direction + entry-time within the mutually matched window,
and reports:

    - Trade count (TV / PineForge / PyneCore) inside the common window
    - Entry-price p90 delta vs TV
    - Exit-price  p90 delta vs TV
    - PnL        p90 delta vs TV
    - 7-label match degree (excellent / strong / moderate / weak / minimal /
      anomaly / engine_only)

The window algorithm mirrors the canonical PineForge parity sweep
(`scripts/verify_corpus.py::trim_to_common_match_window`):

    1. align(tv_full, eng_full)       — initial greedy match
    2. trim_to_common_match_window()  — clip to [min_matched_entry − 1h,
                                          max_matched_entry + 1h]
    3. align(tv_trimmed, eng_trimmed) — final gating match

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
import json
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

# Per-strategy near-zero PnL filter: trades whose |tv_pnl| < $0.01 are
# excluded from pnl p90 so scratch trades don't blow up the per-trade ratio.
# Mirrors canonical verify_corpus.py / validate.py.
PNL_NEAR_ZERO_USD = 0.01

# Parity thresholds copied from the canonical
# `validate_detailed_report.py::DEFAULT_PARITY_{STRICT,PRODUCTION}`.
STRICT_COUNT = 0.01      # 1.0%
STRICT_ENTRY = 0.0001    # 0.01% — tick-level entry parity
STRICT_EXIT  = 0.0001    # 0.01% — tick-level exit parity
STRICT_PNL   = 0.01      # 1.0%

PRODUCTION_EXIT = 0.0005 # 0.05% — exits absorb sub-bar broker drift
PRODUCTION_PNL  = 1.0    # 100%  — trail_* exits intrinsically wide

# Strong-tier breakpoints (canonical verify_corpus.py:58-61)
STRONG_COUNT_DELTA = 0.05
STRONG_ENTRY_DELTA = 0.001
STRONG_EXIT_DELTA  = 0.005
STRONG_PNL_DELTA   = 1.0

# Detect strategies that use TradingView's trailing stops; these get
# the production threshold profile per validate_detailed_report.py.
_TRAIL_PATTERN = re.compile(r"\btrail_(points|offset|price)\s*=", re.IGNORECASE)
_LINE_COMMENT = re.compile(r"//.*?$", re.MULTILINE)
_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)

# canonical: scripts/verify_corpus.py:213-220
# TradingView "Date and time" columns are bare wall-clock strings.
# Default is UTC+8 (Asia/Taipei), overridden per-strategy via inputs.json::tv_trades_csv_tz.
TV_CSV_TZ_OFFSET_HOURS = 8     # default, was TV_TZ_OFFSET_HOURS in earlier compare.py
ENGINE_CSV_TZ_OFFSET_HOURS = 0
TV_TZ_BY_NAME = {
    "utc_plus_8": 8,
    "asia_taipei": 8,
    "utc": 0,
}

# Label → degree mapping (degree used for legacy rendering / summary tallying).
# Covers all 7 labels returned by the canonical classifier + overrides.
LABEL_TO_DEGREE = {
    "excellent":   5,
    "strong":      4,
    "moderate":    3,
    "weak":        2,
    "minimal":     1,
    "anomaly":     4,
    "engine_only": 4,
}

# Mirrors `validate_detailed_report.py::MATCH_DEGREE_LABELS`.
DEGREE_LABEL = {5: "excellent", 4: "strong", 3: "moderate", 2: "weak", 1: "minimal"}


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


def load_strategy_metadata(strategy_dir: Path) -> dict:
    """Load inputs.json for a strategy, returning {} if absent. Mirrors verify_corpus.py."""
    inputs_path = strategy_dir / "inputs.json"
    if not inputs_path.exists():
        return {}
    with inputs_path.open(encoding="utf-8") as f:
        return json.load(f)


def tv_timezone_offset(meta: dict) -> int:
    """Resolve TV CSV timezone offset from inputs.json or default. Mirrors verify_corpus.py."""
    tz_name = str(meta.get("tv_trades_csv_tz", "")).lower()
    return TV_TZ_BY_NAME.get(tz_name, TV_CSV_TZ_OFFSET_HOURS)


def detect_parity_profile_from_source(pine_source: str) -> str:
    """Return 'production' if pine source uses trail_* exit params, else 'strict'.

    Mirrors verify_corpus.py::detect_parity_profile (operates on source string).
    """
    cleaned = _BLOCK_COMMENT.sub("", pine_source)
    cleaned = _LINE_COMMENT.sub("", cleaned)
    return "production" if _TRAIL_PATTERN.search(cleaned) else "strict"


def detect_parity_profile(pine_path: Path) -> str:
    """Return 'production' if the strategy uses trail_* exit params, else 'strict'.

    Thin wrapper around detect_parity_profile_from_source; kept for callers
    that pass a Path. Prefer resolve_profile() for new code.
    """
    if not pine_path.exists():
        return "strict"
    try:
        return detect_parity_profile_from_source(
            pine_path.read_text(encoding="utf-8", errors="replace")
        )
    except OSError:
        return "strict"


def resolve_profile(strategy_dir: Path, meta: dict) -> str:
    """Resolve the parity profile: inputs.json wins, then auto-detect. Mirrors verify_corpus.py."""
    forced = str(meta.get("parity_profile", "")).lower()
    if forced in {"strict", "production"}:
        return forced
    pine_path = strategy_dir / "strategy.pine"
    if pine_path.is_file():
        try:
            return detect_parity_profile_from_source(
                pine_path.read_text(encoding="utf-8")
            )
        except OSError:
            return "strict"
    return "strict"


def parity_for_profile(profile: str) -> dict:
    """Return a fresh threshold dict for the given profile name. Mirrors verify_corpus.py."""
    if profile == "production":
        return {
            "count": 0.01,
            "entry": 0.0001,
            "exit":  PRODUCTION_EXIT,
            "pnl":   PRODUCTION_PNL,
        }
    return {
        "count": STRICT_COUNT,
        "entry": STRICT_ENTRY,
        "exit":  STRICT_EXIT,
        "pnl":   STRICT_PNL,
    }


def interior_time_bounds(
    trim_bars: int,
    warmup_bars: int,
    ohlcv_first_ms: int | None,
    ohlcv_last_ms: int | None,
    bar_ms: int,
) -> tuple[int, int] | None:
    """Return [lo_ms, hi_ms] window for 'interior' entries, or None if trivial.

    Mirrors verify_corpus.py::interior_time_bounds verbatim.
    ``trim_bars`` symmetrically excludes edge bars from BOTH ends; ``warmup_bars``
    is an extra asymmetric lead pad. If the OHLCV span is unknown, returns None.
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
    """Return True if entry_ms is within the interior bounds (or bounds is None)."""
    if bounds is None:
        return True
    lo, hi = bounds
    return lo <= entry_ms <= hi


def _ohlcv_span_ms(strategy_dir: Path) -> tuple[int | None, int | None, int]:
    """Best-effort: read first/last timestamps + bar interval from a per-strategy OHLCV csv.

    Looks for ohlcv.csv / data.csv / candles.csv next to strategy.pine.
    For existing 50 bench strategies (no per-strategy OHLCV), returns (None, None, 0)
    and interior trimming is skipped (no-op). Future corpus-promoted bench strategies
    (Phase 5) will have these files and will be honoured.
    Mirrors verify_corpus.py::_ohlcv_span_ms verbatim.
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


def ohlcv_span_seconds() -> tuple[int, int]:
    """Return (first_ts_s, last_ts_s) of the corpus OHLCV feed.

    NOTE: The canonical sweep (verify_corpus.py) uses align-then-trim
    (trim_to_common_match_window) rather than OHLCV-span intersection for the
    comparison window. This function is kept for potential sanity-check use
    but is no longer called by compute_diff().
    """
    with OHLCV_PATH.open() as f:
        reader = csv.DictReader(f)
        rows = [int(r["timestamp"]) for r in reader]
    return rows[0] // 1000, rows[-1] // 1000


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


def trim_to_common_match_window(
    tv: list[Trade],
    eng: list[Trade],
    matched: list[tuple[Trade, Trade]],
) -> tuple[list[Trade], list[Trade]]:
    """Drop leading/trailing edge trades outside the mutually matched window.

    The canonical parity sweep compares the common in-window trade region.
    This avoids failing a strategy because one engine has an extra bootstrap or
    terminal trade just outside the overlapping validation window.
    Mirrors verify_corpus.py::trim_to_common_match_window verbatim.
    """
    if not matched:
        return tv, eng
    lo = min(min(t.entry_time, e.entry_time) for t, e in matched) - MATCH_WINDOW_S
    hi = max(max(t.entry_time, e.entry_time) for t, e in matched) + MATCH_WINDOW_S
    tv_trim = [t for t in tv if lo <= t.entry_time <= hi]
    eng_trim = [e for e in eng if lo <= e.entry_time <= hi]
    return tv_trim, eng_trim


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
    n_engine_in_window: int    # after window clip (interior gate if bounds set)
    n_tv_in_window: int        # after window clip (interior gate if bounds set)
    n_matched: int
    count_delta: float
    entry_p90: float
    exit_p90: float
    pnl_p90: float
    profile: str               # "strict" or "production"
    degree: int                # 1..5; 5=excellent, 1=minimal (derived from label)
    label: str                 # 'excellent'/'strong'/'moderate'/'weak'/'minimal'/'anomaly'/'engine_only'


def classify_match_degree_canonical(
    *,
    count_d: float,
    entry_p90: float,
    exit_p90: float,
    pnl_p90: float,
    gating_matched_n: int,
    tv_gate_n: int,
    profile: str,
) -> str:
    """Canonical tier classifier. Mirrors verify_corpus.py:434-454 exactly.

    - excellent: all four thresholds pass.
    - strong: 99%+ match + STRONG_* breakpoints pass.
    - moderate: 90%+ match.
    - weak: any matched trades.
    - minimal: zero matched trades.
    """
    thresh = parity_for_profile(profile)
    all_ok = (
        count_d   < thresh["count"]
        and entry_p90 < thresh["entry"]
        and exit_p90  < thresh["exit"]
        and pnl_p90   < thresh["pnl"]
    )
    if all_ok:
        return "excellent"
    match_pct = gating_matched_n / max(tv_gate_n, 1)
    strong = (
        match_pct  >= 0.99
        and count_d   < STRONG_COUNT_DELTA
        and entry_p90 < STRONG_ENTRY_DELTA
        and exit_p90  < STRONG_EXIT_DELTA
        and pnl_p90   < STRONG_PNL_DELTA
    )
    if strong:
        return "strong"
    if match_pct >= 0.90:
        return "moderate"
    if gating_matched_n > 0:
        return "weak"
    return "minimal"


def compute_diff(
    name: str,
    eng_full: list[Trade],
    tv_full: list[Trade],
    *,
    profile: str,
    strategy_dir: Path,
    meta: dict,
) -> EngineDiff:
    """Canonical align-then-trim TV-vs-engine diff.

    Mirrors verify_corpus.py::verify_one() logic:
      1. align(tv, eng) — initial greedy pass
      2. trim_to_common_match_window() — clip to matched-window ± MATCH_WINDOW_S
      3. align(tv_trim, eng_trim) — final gating match
      4. interior trim (if inputs.json has trim_bars/warmup_bars)
      5. classify_match_degree_canonical()
      6. expected_tier / validation_overrides overrides
    """
    matched = align(tv_full, eng_full)
    tv_cmp, eng_cmp = trim_to_common_match_window(tv_full, eng_full, matched)
    matched = align(tv_cmp, eng_cmp)

    if not matched:
        all_zero = (len(tv_cmp) == 0 and len(eng_cmp) == 0)
        deg = 5 if all_zero else 1
        lbl = DEGREE_LABEL[deg]
        return EngineDiff(name, len(eng_full), len(eng_cmp), len(tv_cmp), 0,
                          0.0, 0.0, 0.0, 0.0, profile, deg, lbl)

    # Interior trim from inputs.json (no-op for existing 50 bench strategies
    # that have no per-strategy OHLCV; becomes active for Phase 5 promotions).
    trim_bars = int(meta.get("trim_bars", 0) or 0)
    warmup_bars = int(meta.get("warmup_bars", 0) or 0)
    first_ms, last_ms, bar_ms = _ohlcv_span_ms(strategy_dir)
    bounds = interior_time_bounds(trim_bars, warmup_bars, first_ms, last_ms, bar_ms)

    if bounds is not None:
        tv_gate = [t for t in tv_cmp if is_interior(t.entry_time * 1000, bounds)]
        eng_gate = [e for e in eng_cmp if is_interior(e.entry_time * 1000, bounds)]
        gating_matched = [
            (t, e) for (t, e) in matched if is_interior(t.entry_time * 1000, bounds)
        ]
        if not gating_matched:
            gating_matched = matched
    else:
        tv_gate, eng_gate, gating_matched = tv_cmp, eng_cmp, matched

    count_delta = abs(len(tv_gate) - len(eng_gate)) / max(len(tv_gate), len(eng_gate), 1)
    entry_p90 = percentile(
        [relmax(t.entry_price, e.entry_price) for t, e in gating_matched], 0.9
    )
    exit_p90 = percentile(
        [relmax(t.exit_price, e.exit_price) for t, e in gating_matched], 0.9
    )
    pnl_p90 = percentile(
        [
            abs(t.pnl - e.pnl) / abs(t.pnl)
            for t, e in gating_matched
            if abs(t.pnl) >= PNL_NEAR_ZERO_USD
        ],
        0.9,
    )

    label = classify_match_degree_canonical(
        count_d=count_delta,
        entry_p90=entry_p90,
        exit_p90=exit_p90,
        pnl_p90=pnl_p90,
        gating_matched_n=len(gating_matched),
        tv_gate_n=len(tv_gate),
        profile=profile,
    )

    # Per-strategy override: inputs.json::expected_tier = "anomaly" / "engine_only"
    # Mirrors verify_corpus.py:466-468. Only fires when computed tier < excellent.
    expected_tier = str(meta.get("expected_tier", "")).strip().lower()
    if expected_tier in {"anomaly", "engine_only"} and label != "excellent":
        label = expected_tier

    # Per-strategy override: inputs.json::validation_overrides.expect_tv_match = false
    # Mirrors verify_corpus.py:479-482. Relabels as engine_only.
    val_overrides = meta.get("validation_overrides") or {}
    if not bool(val_overrides.get("expect_tv_match", True)) and label != "excellent":
        label = "engine_only"

    degree = LABEL_TO_DEGREE.get(label, 1)
    return EngineDiff(
        name, len(eng_full), len(eng_gate), len(tv_gate), len(gating_matched),
        count_delta, entry_p90, exit_p90, pnl_p90, profile, degree, label,
    )


def fmt_pct(x: float) -> str:
    return f"{x*100:.4f}%"


_LABEL_EMOJI = {
    "excellent":   "🟢",
    "strong":      "🟢",
    "moderate":    "🟡",
    "weak":        "🟠",
    "minimal":     "🔴",
    "anomaly":     "🔵",
    "engine_only": "🟣",
}

# Keep legacy dict for any code that accesses _DEGREE_EMOJI by degree int.
_DEGREE_EMOJI = {5: "🟢", 4: "🟢", 3: "🟡", 2: "🟠", 1: "🔴"}


def render_strategy_block(name: str, profile: str, tv_full: int,
                          diffs: list[EngineDiff]) -> str:
    lines = [f"### {name}  *(profile: {profile})*", ""]
    lines.append(f"- TV trades (raw): **{tv_full}**")
    if diffs:
        win_tv = diffs[0].n_tv_in_window
        lines.append(f"- TV trades inside common window: **{win_tv}**")
    for d in diffs:
        emoji = _LABEL_EMOJI.get(d.label, "🔴")
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


def render_pf_pc_agreement(strategy: str, pf: list[Trade], pc: list[Trade]) -> str:
    """Engine-vs-engine agreement, clipped to the same common match window."""
    matched = align(pf, pc)
    pf_w, pc_w = trim_to_common_match_window(pf, pc, matched)
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

    sections: list[str] = [
        "# Trade comparison\n",
        "Each strategy is run through PineForge and PyneCore against the\n"
        "same 36k-bar OHLCV feed. PineTS is excluded from this report —\n"
        "their strategy backtester is a roadmap item (per [their\n"
        "README](https://github.com/LuxAlgo/PineTS#roadmap)). Both columns\n"
        "are diffed against the same `tv_trades.csv` ground truth.\n",
        "**Window algorithm (align-then-trim).** TV's chart export typically covers\n"
        "~3 weeks of history *before* this repo's OHLCV begins, and our\n"
        "OHLCV extends ~4 weeks *after* TV's export ends. To make the\n"
        "count fair, we use the same canonical algorithm as\n"
        "`scripts/verify_corpus.py::trim_to_common_match_window`:\n"
        "1. align(tv, engine) — initial greedy match; 2. trim to\n"
        "[min_matched_entry − 1h, max_matched_entry + 1h]; 3. re-align\n"
        "on the trimmed lists. Per-strategy `inputs.json` overrides\n"
        "(`trim_bars`, `warmup_bars`, `expected_tier`,\n"
        "`validation_overrides.expect_tv_match`) are honoured.\n",
        "**7-label match degree** mirrors the canonical sweep:\n"
        "🟢 *excellent* (all four p90 thresholds pass) → "
        "🟢 *strong* (99%+ match + STRONG_* breakpoints) → "
        "🟡 *moderate* (90%+ match) → 🟠 *weak* (any matched) → 🔴 *minimal* (0 matched). "
        "🔵 *anomaly* and 🟣 *engine_only* are declared via `inputs.json::expected_tier` "
        "or `validation_overrides.expect_tv_match: false` — they indicate documented "
        "TV-side non-determinism or engine-only probes, not regressions. "
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

        meta = load_strategy_metadata(strat_dir)
        profile = resolve_profile(strat_dir, meta)
        tv = parse_trades(tv_path, tv_timezone_offset(meta))
        pf = parse_trades(pf_path, ENGINE_CSV_TZ_OFFSET_HOURS)
        pc = parse_trades(pc_path, ENGINE_CSV_TZ_OFFSET_HOURS)

        diffs = [
            compute_diff("PineForge", pf, tv, profile=profile,
                         strategy_dir=strat_dir, meta=meta),
            compute_diff("PyneCore",  pc, tv, profile=profile,
                         strategy_dir=strat_dir, meta=meta),
        ]
        summary_rows.append((strat_dir.name, profile, len(tv), diffs[0], diffs[1]))

        if not args.quiet:
            print(f"=== {strat_dir.name}  ({profile}) ===")
            print(f"  raw counts:    TV={len(tv)}  PineForge={len(pf)}  PyneCore={len(pc)}")
            for d in diffs:
                emoji = _LABEL_EMOJI.get(d.label, "🔴")
                print(f"  {d.name:9s} {emoji} {d.label:11s}  "
                      f"in-window TV={d.n_tv_in_window} engine={d.n_engine_in_window} "
                      f"matched={d.n_matched}  "
                      f"count={fmt_pct(d.count_delta):>9s}  "
                      f"entry={fmt_pct(d.entry_p90):>9s}  "
                      f"exit={fmt_pct(d.exit_p90):>9s}  "
                      f"pnl={fmt_pct(d.pnl_p90):>9s}")
            print()

        sections.append(render_strategy_block(strat_dir.name, profile, len(tv), diffs))
        sections.append(render_pf_pc_agreement(strat_dir.name, pf, pc))

    # Tally the label breakdown per engine.
    all_labels = ("excellent", "strong", "moderate", "weak", "minimal",
                  "anomaly", "engine_only")
    summary_lines = [
        "# Summary\n",
        "Match degree per the canonical PineForge parity sweep "
        "(align-then-trim window; trail_* strategies use production thresholds; "
        "inputs.json overrides honoured).\n",
        "| Strategy | Profile | TV (raw / win) | PineForge | PyneCore |",
        "|---|---|---|---|---|",
    ]
    pf_tally: dict[str, int] = {lbl: 0 for lbl in all_labels}
    pc_tally: dict[str, int] = {lbl: 0 for lbl in all_labels}
    for name, profile, tv_raw, pf_d, pc_d in summary_rows:
        pf_tally[pf_d.label] = pf_tally.get(pf_d.label, 0) + 1
        pc_tally[pc_d.label] = pc_tally.get(pc_d.label, 0) + 1
        pf_emoji = _LABEL_EMOJI.get(pf_d.label, "🔴")
        pc_emoji = _LABEL_EMOJI.get(pc_d.label, "🔴")
        summary_lines.append(
            f"| {name} | {profile} | {tv_raw} / {pf_d.n_tv_in_window} | "
            f"{pf_emoji} {pf_d.label} ({pf_d.n_engine_in_window}) | "
            f"{pc_emoji} {pc_d.label} ({pc_d.n_engine_in_window}) |"
        )
    summary_lines.append("")
    n = len(summary_rows)
    for label in all_labels:
        pf_cnt = pf_tally.get(label, 0)
        pc_cnt = pc_tally.get(label, 0)
        if pf_cnt or pc_cnt:
            summary_lines.append(
                f"- **{label}**: PineForge {pf_cnt}/{n}, PyneCore {pc_cnt}/{n}"
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
