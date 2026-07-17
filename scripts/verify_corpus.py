#!/usr/bin/env python3
"""Verify a strategy in the corpus against TradingView's exported trades.

Reads `tv_trades.csv` and `engine_trades.csv` from a strategy folder under
`corpus/`, aligns trades by entry time + direction with a 1-hour window
(matches the parent project's parity sweep), and reports the largest
deviations in entry price, exit price, and per-trade P&L.

This mirrors the canonical corpus summary used by the runtime docs:
it applies common-window edge trimming, honours per-strategy metadata,
and reports the five parity labels used in the corpus README.

`--all` covers the reference-tier categories (`basic/`, `community/`,
`validation/` — whichever exist in this checkout). The `parity-anomalies/` category is a home
for probes that *deliberately* surface TV-side non-determinism (engine is
correct, divergence is documented in `pineforge-utils/parity-anomalies/`);
it is excluded from `--all` by default so it doesn't mask as a regression.
Use `--include-anomalies` to fold it into the same sweep, or run it
explicitly with `--category parity-anomalies`.

Usage:
  scripts/verify_corpus.py corpus/basic/greedy           # one strategy
  scripts/verify_corpus.py --all                         # all reference strategies
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
from dataclasses import dataclass, field
from datetime import datetime, timezone, timedelta
from pathlib import Path

# THIS FILE IS THE CANONICAL PARITY RUBRIC (single source of truth).
# pineforge-utils/validator/validate.py and any other comparator must track
# THESE thresholds/semantics, not the other way around.
# Two profiles available:
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

STRONG_COUNT_DELTA = 0.06
STRONG_ENTRY_DELTA = 0.001
STRONG_EXIT_DELTA  = 0.005
STRONG_PNL_DELTA   = 1.0

# Coverage gate — matched trades as a fraction of ALL closed TV trades (the
# interior-trimmed full export, NOT the self-selected common match window).
# Every other gated metric is computed inside a window derived from the
# matches themselves, so without this gate a run that reproduces almost
# nothing can grade top-tier on the sliver it happened to reproduce
# (observed in production: 'excellent' at 0.9% real coverage). The
# small-N alternative (<=1 unmatched TV trade) keeps single boundary-trade
# cases fair for short exports.
COVERAGE_EXCELLENT = 0.99
COVERAGE_STRONG    = 0.95
COVERAGE_MODERATE  = 0.75

# The qty-normalized PnL comparison exists to absorb tiny equity-compounding
# sizing drift between two independently compounded simulations. Unbounded,
# it would also absorb arbitrarily large position-sizing bugs (any wrong qty
# with correct prices rescales into a perfect PnL match), so the rescue only
# applies when the sizing drift itself is small.
QTY_NORM_BAND = 0.02  # |tv_qty/eng_qty - 1| <= 2%

# Pine-source comment-strippers + trail_* matcher for profile auto-detect.
# Matches canonical pineforge-utils/validator/validate.py::detect_parity_profile.
_LINE_COMMENT_PATTERN = re.compile(r"//.*?$", re.MULTILINE)
_BLOCK_COMMENT_PATTERN = re.compile(r"/\*.*?\*/", re.DOTALL)
_TRAIL_PATTERN = re.compile(r"\btrail_(points|offset|price)\s*=", re.IGNORECASE)

# Per-strategy near-zero PnL filter: trades whose |tv_pnl| < $0.01 are
# excluded from pnl p90 so scratch trades don't blow up the per-trade ratio.
# Mirrors canonical validate.py line ~1136.
PNL_NEAR_ZERO_USD = 0.01
# TradingView's exported Net PnL column is rounded to cents. For sub-dollar
# trades, the last half-cent of CSV quantization can look like a multi-percent
# relative PnL miss even when entry, exit, qty and commission are all exact.
TV_PNL_ROUNDING_EPSILON_USD = 0.005

# MAE and MFE are REPORT-ONLY (not gated). Both excursions depend on the
# intrabar price path, which TV sources from finer (1m/tick) data than the
# local OHLC feed can resolve — gating on them punishes data resolution,
# not engine correctness. Regression classes a MAE gate would have caught
# (sign flip ~200%, per-unit-vs-total qty ~50%) remain visible in the
# report-only section; treat large report-only MAE/MFE drift as a
# diagnostic lead, not a tier verdict.


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
    # Report-only fields (compared but NOT gated — see the field-coverage
    # disclosure). pnl_pct is in percent. mfe/mae are TOTAL USD excursions
    # ((price diff) * qty, summed over pyramid entries) in TV's export
    # convention: mfe (favorable) >= 0, mae (adverse) <= 0.
    pnl_pct: float = 0.0
    mfe: float = 0.0
    mae: float = 0.0


@dataclass
class VerificationResult:
    """Canonical structured parity analysis for one corpus strategy.

    ``verify_one`` is intentionally only a presentation wrapper around this
    object.  Report generators must consume :func:`analyze_strategy` instead
    of copying the rubric; otherwise a new gate can silently change CLI tiers
    without changing the generated validation report.
    """

    strategy_dir: Path
    rel: str
    label: str
    profile: str = "n/a"
    notes: str = ""
    tv_path: Path | None = None
    eng_path: Path | None = None
    tv_exists: bool = True
    eng_exists: bool = True
    no_aligned_trades: bool = False
    tv_count: int = 0
    eng_count: int = 0
    tv_raw_count: int = 0
    eng_raw_count: int = 0
    matched_count: int = 0
    gating_matched_count: int = 0
    tv_gate_count: int = 0
    eng_gate_count: int = 0
    count_delta: float = 0.0
    count_abs_delta: int = 0
    entry_p90: float = 0.0
    exit_p90: float = 0.0
    pnl_p90: float = 0.0
    coverage: float = 0.0
    unmatched_total: int = 0
    coverage_tv_count: int = 0
    trim_bars: int = 0
    warmup_bars: int = 0
    bounds: tuple[int, int] | None = None
    count_ok: bool = False
    entry_ok: bool = False
    exit_ok: bool = False
    pnl_ok: bool = False
    coverage_ok: bool = False
    unmatched_in_window: int = 0
    entry_p100: float = 0.0
    exit_p100: float = 0.0
    pnl_p100: float = 0.0
    qty_p100: float = 0.0
    pnlpct_p100: float = 0.0
    mfe_p90: float = 0.0
    mae_p90: float = 0.0
    matched: list[tuple[TradePair, TradePair]] = field(default_factory=list, repr=False)
    entry_deltas: list[float] = field(default_factory=list, repr=False)
    exit_deltas: list[float] = field(default_factory=list, repr=False)
    pnl_deltas: list[float] = field(default_factory=list, repr=False)
    qty_deltas: list[float] = field(default_factory=list, repr=False)
    pnlpct_deltas: list[float] = field(default_factory=list, repr=False)

    @property
    def tier(self) -> str:
        return self.label

    def report_row(self) -> dict:
        """Return the canonical report-facing fields without re-analysis."""
        return {
            "slug": self.strategy_dir.name,
            "rel": self.rel,
            "tier": self.label,
            "tv": self.tv_count,
            "eng": self.eng_count,
            "matched": self.matched_count,
            "count_delta": self.count_delta,
            "count_abs_delta": self.count_abs_delta,
            "entry_p90": self.entry_p90,
            "exit_p90": self.exit_p90,
            "pnl_p90": self.pnl_p90,
            "profile": self.profile,
            "notes": self.notes,
        }


def tv_tzinfo(meta: dict):
    """Resolve the TV export timezone to a tzinfo.

    Accepts the legacy fixed-offset aliases (utc / utc_plus_8 / asia_taipei) AND
    any IANA zone name (e.g. 'America/New_York', 'Europe/London') so DST-bearing
    exchanges parse correctly — a fixed integer offset silently mis-aligns trades
    across a DST transition (e.g. US equities flip UTC-4 -> UTC-5 in November).
    """
    name = str(meta.get("tv_trades_csv_tz", "")).strip()
    low = name.lower()
    if low in TV_TZ_BY_NAME:
        return timezone(timedelta(hours=TV_TZ_BY_NAME[low]))
    if "/" in name:  # looks like an IANA zone -> DST-aware
        try:
            from zoneinfo import ZoneInfo
            return ZoneInfo(name)
        except Exception:
            pass
    return timezone(timedelta(hours=TV_CSV_TZ_OFFSET_HOURS))  # default Asia/Taipei


def parse_dt(s: str, tz) -> int:
    """Parse 'YYYY-MM-DD HH:MM' (wall time in tz) as unix seconds (UTC).

    ``tz`` is any datetime.tzinfo — a fixed timezone(...) or a DST-aware
    zoneinfo.ZoneInfo. localizing the naive wall clock then taking .timestamp()
    yields the correct POSIX instant including DST.
    """
    return int(datetime.strptime(s, "%Y-%m-%d %H:%M").replace(tzinfo=tz).timestamp())


def still_open_trade_nums(csv_path: Path) -> set:
    """Trade numbers whose EXIT row carries Signal=='Open' (a position still
    open at TV's export-window end; a mark-to-market snapshot, not a close)."""
    import csv as _csv
    nums = set()
    try:
        with csv_path.open(encoding="utf-8-sig") as f:
            for r in _csv.DictReader(f):
                if (r.get("Type") or "").lower().startswith("exit") \
                        and (r.get("Signal") or "").strip().lower() == "open":
                    n = r.get("Trade #") or r.get("Trade number")
                    if n is not None:
                        nums.add(int(n))
    except Exception:
        pass
    return nums


def parse_trades(csv_path: Path, *, tz) -> list[TradePair]:
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
            # Quote-currency-suffixed columns ("... USD" / "... USDT" / ...),
            # mirroring the price_field prefix match above. A perp export's
            # "Net PnL USDT" / "Favorable excursion USDT" previously matched
            # NONE of the exact "...USD" spellings below and silently fell
            # back to 0.0, making pnl_p90/mfe/mae trivially "0.0% OK"
            # regardless of real drift.
            def _ccy_col(row: dict, *prefixes: str):
                for c in row:
                    # Exclude the "... %" variant (e.g. "Net P&L %") — that's
                    # a separate, currency-less percent field, not a quote-
                    # currency-suffixed amount, and must not be matched here.
                    if c.endswith(" %"):
                        continue
                    if any(c == p or c.startswith(p + " ") for p in prefixes):
                        return row[c]
                return None
            pnl = float(_ccy_col(row, "Net P&L", "Net PnL") or 0.0)
            # Report-only fields (TV vs engine column spellings differ).
            pnl_pct = float(
                row.get("Net P&L %") or row.get("Net PnL %") or 0.0
            )
            mfe = float(_ccy_col(row, "Favorable excursion") or row.get("MFE") or 0.0)
            # TV exports adverse excursion as a NEGATIVE USD value; current
            # engine CSVs use the same name + convention. Legacy engine CSVs
            # ("MAE" column) emitted the positive magnitude — normalize those
            # to TV's sign so old artifacts still compare correctly.
            adverse = _ccy_col(row, "Adverse excursion")
            if adverse:
                mae = float(adverse)
            else:
                mae = -abs(float(row.get("MAE") or 0.0)) or 0.0
            direction = "long" if "long" in kind.lower() else "short"
            r["direction"] = direction
            r["qty"] = qty
            r["pnl"] = pnl
            r["pnl_pct"] = pnl_pct
            r["mfe"] = mfe
            r["mae"] = mae
            if kind.startswith("Entry"):
                r["entry_time"] = parse_dt(time_field, tz)
                r["entry_price"] = price
            else:
                r["exit_time"] = parse_dt(time_field, tz)
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
            pnl_pct=r.get("pnl_pct", 0.0),
            mfe=r.get("mfe", 0.0),
            mae=r.get("mae", 0.0),
        ))
    pairs.sort(key=lambda t: t.entry_time)
    return pairs


def consolidate_fragments(pairs: list[TradePair]) -> list[TradePair]:
    """Reunite the fragment rows that split a single logical fill into one trade.

    TradingView's "List of Trades" (and the engine, mirroring it) splits one
    entry FILL across several ``Trade #`` rows whenever that position is closed
    in lots — either a tiny ``qty_step`` rounding remainder that shares the
    SAME entry time AND price, or FIFO partial-close fragments of a grid bot
    where one entry is drained by several exit orders. Every such fragment is a
    *different exit lot of the same entry*, so the entry side is identical
    across the group: same bar timestamp, same fill price, same direction.

    Left raw, these fragments break the entry-time pairing in
    :func:`align_by_time`: two fragments share one entry instant, so the greedy
    matcher cross-pairs a TV lot with the wrong engine lot and reports spurious
    count + exit-price deltas (the tell-tale ~90% qty-p90 is the fingerprint).
    This helper merges each fill back into one trade and is applied
    SYMMETRICALLY to the TV and engine lists, so a genuinely fragmented
    strategy still pairs 1:1.

    Merge key = ``(entry_time, entry_price, direction)`` compared EXACTLY: two
    rows merge iff they share the same bar, the same fill price (read from the
    identical CSV cell, hence bit-identical within one file) and the same side
    — i.e. they are the same fill event. Two *distinct* trades can never
    collide, because a second independent entry must occur on a different bar
    or at a different fill price (a different grid level), either of which
    changes the key. For an un-fragmented strategy every group has size 1, so
    this is a strict no-op and the reference corpus is left byte-identical.

    The merged trade keeps the shared entry (time + price) and direction, sums
    the per-lot qty / pnl / excursions, and represents the exit by the lots'
    qty-weighted-average price at the final close time — the way TradingView
    aggregates a multi-lot deal. When every fragment shares one exit (pure
    qty_step rounding) that average IS the shared exit price, kept exactly so
    the comparison stays bit-for-bit unchanged.

    >>> mk = lambda n, et, ep, xt, xp, q, p: TradePair("long", et, ep, xt, xp, q, p, n)
    >>> # two qty_step rounding fragments of one fill: same entry AND same exit
    >>> a = mk(1, 100, 10.0, 200, 12.0, 0.01, 0.02)
    >>> b = mk(2, 100, 10.0, 200, 12.0, 0.99, 1.98)
    >>> # a distinct later trade (different entry bar + price) must NOT merge
    >>> c = mk(3, 300, 11.0, 400, 13.0, 1.00, 2.00)
    >>> out = consolidate_fragments([a, b, c])
    >>> [(round(t.qty, 4), round(t.pnl, 4), t.exit_price) for t in out]
    [(1.0, 2.0, 12.0), (1.0, 2.0, 13.0)]
    >>> # FIFO grid: ONE entry drained by two DIFFERENT exit lots -> one deal,
    >>> # exit = qty-weighted average price at the final close time
    >>> d = mk(4, 100, 10.0, 150, 12.0, 0.5, 1.0)
    >>> e = mk(5, 100, 10.0, 250, 14.0, 0.5, 2.0)
    >>> g = consolidate_fragments([d, e])
    >>> len(g), g[0].qty, g[0].exit_price, g[0].exit_time
    (1, 1.0, 13.0, 250)
    """
    groups: dict[tuple[int, float, str], list[TradePair]] = {}
    order: list[tuple[int, float, str]] = []
    for t in pairs:
        key = (t.entry_time, t.entry_price, t.direction)
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(t)

    out: list[TradePair] = []
    for key in order:
        members = groups[key]
        if len(members) == 1:
            out.append(members[0])
            continue
        qty = sum(m.qty for m in members)
        denom = qty if qty else 1.0
        rep = members[0]
        if len({m.exit_price for m in members}) == 1:
            # Shared-exit fragments (pure qty_step rounding): keep the exact
            # shared exit so the merge is bit-for-bit identical to a single fill.
            exit_price = rep.exit_price
            exit_time = rep.exit_time
        else:
            # FIFO partial-close lots: blend like a TV deal — qty-weighted
            # average exit price, settled at the final close time.
            exit_price = sum(m.exit_price * m.qty for m in members) / denom
            exit_time = max(m.exit_time for m in members)
        out.append(TradePair(
            direction=rep.direction,
            entry_time=rep.entry_time,
            entry_price=rep.entry_price,
            exit_time=exit_time,
            exit_price=exit_price,
            qty=qty,
            pnl=sum(m.pnl for m in members),
            trade_num=min(m.trade_num for m in members),
            pnl_pct=sum(m.pnl_pct * m.qty for m in members) / denom,
            mfe=sum(m.mfe for m in members),
            mae=sum(m.mae for m in members),
        ))
    out.sort(key=lambda t: t.entry_time)
    return out


def has_cross_entry_fifo_allocation(pairs: list[TradePair]) -> bool:
    """Return whether raw rows expose a genuinely ambiguous FIFO schedule.

    A repeated entry key alone is not enough.  Margin calls and other partial
    exits split one ordinary (pyramiding=0) position into several rows too,
    but there is no cross-entry allocation ambiguity in that shape.  The
    schedule-level rescue is justified only when BOTH facts are present:

    1. one entry fill drains through multiple distinct exit events; and
    2. one exact exit event closes quantity owned by multiple distinct entry
       fills (the defining FIFO-grid allocation collision).

    Exit identity deliberately matches :func:`schedule_exit_metrics` so the
    eligibility predicate and the metric it enables cannot disagree about
    which broker event is shared.
    """
    entry_exits: dict[
        tuple[int, float, str], set[tuple[int, float, str]]
    ] = {}
    exit_owners: dict[
        tuple[int, float, str], set[tuple[int, float, str]]
    ] = {}
    for trade in pairs:
        entry_key = (trade.entry_time, trade.entry_price, trade.direction)
        exit_key = (trade.exit_time, round(trade.exit_price, 6), trade.direction)
        entry_exits.setdefault(entry_key, set()).add(exit_key)
        exit_owners.setdefault(exit_key, set()).add(entry_key)

    multi_exit_entries = {
        entry_key for entry_key, exits in entry_exits.items() if len(exits) > 1
    }
    return any(
        len(owners) > 1 and bool(owners & multi_exit_entries)
        for owners in exit_owners.values()
    )


def schedule_rows_for_gate(
    tv_raw: list[TradePair],
    eng_raw: list[TradePair],
    tv_gate: list[TradePair],
    eng_gate: list[TradePair],
    gating_matched: list[tuple[TradePair, TradePair]],
) -> tuple[list[TradePair], list[TradePair]]:
    """Select the scored raw-entry groups for schedule comparison.

    Canonical alignment permits a TV/engine entry pair to differ by up to the
    match window.  Bounding both raw schedules only by independently gated
    entry times can then drop one half of an already scored boundary pair.
    Select each side's gate entries plus its half of every scored match.  Exact
    entry keys preserve every raw fragment without re-importing unrelated
    out-of-gate rows that happen to lie between two boundary timestamps.
    """
    def entry_key(trade: TradePair) -> tuple[int, float, str]:
        return (trade.entry_time, trade.entry_price, trade.direction)

    tv_keys = {entry_key(trade) for trade in tv_gate}
    eng_keys = {entry_key(trade) for trade in eng_gate}
    for tv_trade, eng_trade in gating_matched:
        tv_keys.add(entry_key(tv_trade))
        eng_keys.add(entry_key(eng_trade))
    if not tv_keys and not eng_keys:
        return tv_raw, eng_raw
    return (
        [trade for trade in tv_raw if entry_key(trade) in tv_keys],
        [trade for trade in eng_raw if entry_key(trade) in eng_keys],
    )


def schedule_exit_metrics(tv_raw: list[TradePair], eng_raw: list[TradePair]) -> tuple[list[float], list[float]]:
    """Position-level exit schedule metrics for fragmented FIFO drains.

    Entry-group consolidation is useful for true split rows, but for dense
    FIFO grid exits the exact same position-level close schedule can be sliced
    across entry lots differently.  Compare the exit schedule directly:
    exact (time, rounded price, side) qty matches contribute 0 exit delta;
    unmatched schedule qty contributes a conservative 100% miss.  PnL is
    compared in aggregate over the same raw window, with TV cent-rounding
    tolerated by the normal epsilon.
    """
    def build(rows: list[TradePair]) -> dict[tuple[int, float, str], float]:
        out: dict[tuple[int, float, str], float] = {}
        for t in rows:
            key = (t.exit_time, round(t.exit_price, 6), t.direction)
            out[key] = out.get(key, 0.0) + t.qty
        return out

    tv_sched = build(tv_raw)
    eng_sched = build(eng_raw)
    exit_deltas: list[float] = []
    for key, tv_qty in tv_sched.items():
        eng_qty = eng_sched.get(key, 0.0)
        matched_qty = min(tv_qty, eng_qty)
        if matched_qty > 1e-9:
            exit_deltas.append(0.0)
        if tv_qty - matched_qty > 1e-9:
            exit_deltas.append(1.0)
        if eng_qty - matched_qty > 1e-9:
            exit_deltas.append(1.0)
    for key, eng_qty in eng_sched.items():
        if key not in tv_sched and eng_qty > 1e-9:
            exit_deltas.append(1.0)

    tv_pnl = sum(t.pnl for t in tv_raw)
    eng_pnl = sum(e.pnl for e in eng_raw)
    pnl_deltas: list[float] = []
    if abs(tv_pnl) >= PNL_NEAR_ZERO_USD:
        abs_diff = abs(tv_pnl - eng_pnl)
        pnl_deltas.append(0.0 if abs_diff < TV_PNL_ROUNDING_EPSILON_USD else abs_diff / abs(tv_pnl))
    return exit_deltas, pnl_deltas


def load_strategy_metadata(strategy_dir: Path) -> dict:
    inputs_path = strategy_dir / "inputs.json"
    if not inputs_path.exists():
        return {}
    import json
    with inputs_path.open(encoding="utf-8") as f:
        return json.load(f)


def _apply_declared_tier_override(label: str, meta: dict) -> str:
    """Apply documented-divergence metadata without masking a real TV match.

    ``expected_tier`` owns explicit anomaly/engine-only declarations.  The
    canonical ``validation_overrides.expect_tv_match=false`` spelling then
    resolves to engine-only, matching the historical precedence when both are
    present.  Metadata is evaluated before the excellent guards, preserving
    the normal path's fail-closed behavior for malformed override objects.
    """
    expected_tier = str(meta.get("expected_tier", "")).strip().lower()
    if expected_tier in {"anomaly", "engine_only"} and label != "excellent":
        label = expected_tier

    val_overrides = meta.get("validation_overrides") or {}
    if (not bool(val_overrides.get("expect_tv_match", True))
            and label != "excellent"):
        label = "engine_only"
    return label


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


def analyze_strategy(strategy_dir: Path) -> VerificationResult:
    """Analyze one strategy using the canonical parity rubric.

    This is the single structured API for both the CLI and generated reports.
    Keep all tier-affecting and metric-affecting logic inside this function.
    """
    rel = strategy_dir.name
    if strategy_dir.parent.name in {"basic", "community", "validation", "parity-anomalies"}:
        rel = f"{strategy_dir.parent.name}/{strategy_dir.name}"
    meta = load_strategy_metadata(strategy_dir)
    tv_path = strategy_dir / str(meta.get("tv_trades_csv", "tv_trades.csv"))
    eng_path = strategy_dir / "engine_trades.csv"
    if not tv_path.exists() or not eng_path.exists():
        return VerificationResult(
            strategy_dir=strategy_dir,
            rel=rel,
            label="missing",
            notes="tv_trades.csv or engine_trades.csv missing",
            tv_path=tv_path,
            eng_path=eng_path,
            tv_exists=tv_path.exists(),
            eng_exists=eng_path.exists(),
        )

    tv_raw_all = parse_trades(tv_path, tz=tv_tzinfo(meta))
    eng_raw_all = parse_trades(eng_path, tz=timezone.utc)
    tv = list(tv_raw_all)
    eng = list(eng_raw_all)
    # Reunite TradingView/engine fragment rows (qty_step rounding remainders or
    # FIFO partial-close lots of one fill) into a single logical trade BEFORE
    # pairing, symmetrically on both sides, so the entry-time matcher does not
    # cross-pair same-entry lots. No-op for un-fragmented strategies.
    tv = consolidate_fragments(tv)
    eng = consolidate_fragments(eng)
    matched = align_by_time(tv, eng)
    tv_cmp, eng_cmp = trim_to_common_match_window(tv, eng, matched)
    matched = align_by_time(tv_cmp, eng_cmp)

    if not matched:
        label = "excellent" if len(tv_cmp) == 0 and len(eng_cmp) == 0 else "minimal"
        # The historical both-empty Excellent branch returned before reading
        # override metadata. Keep that exact behavior while fixing declared
        # divergence for the non-excellent zero-alignment branch.
        if label != "excellent":
            label = _apply_declared_tier_override(label, meta)
        return VerificationResult(
            strategy_dir=strategy_dir,
            rel=rel,
            label=label,
            profile=resolve_profile(strategy_dir, meta),
            notes=str(meta.get("notes", "")).strip() or "no aligned trades",
            tv_path=tv_path,
            eng_path=eng_path,
            no_aligned_trades=True,
            tv_count=len(tv_cmp),
            eng_count=len(eng_cmp),
            tv_raw_count=len(tv),
            eng_raw_count=len(eng),
            count_delta=relative_max(len(tv_cmp), len(eng_cmp)),
            count_abs_delta=abs(len(tv_cmp) - len(eng_cmp)),
            count_ok=len(tv_cmp) == len(eng_cmp),
        )

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

    count_abs_delta = abs(len(tv_gate) - len(eng_gate))
    count_delta = relative_max(len(tv_gate), len(eng_gate))
    entry_deltas = [relative_max(t.entry_price, e.entry_price) for t, e in gating_matched]
    exit_deltas  = [relative_max(t.exit_price,  e.exit_price)  for t, e in gating_matched]
    # PnL p90 uses *relative-to-tv_pnl*, with near-zero trades excluded so
    # scratch trades don't blow up the ratio. Mirrors canonical line ~1136.
    pnl_deltas: list[float] = []
    for t, e in gating_matched:
        if abs(t.pnl) < PNL_NEAR_ZERO_USD:
            continue
        abs_diff = abs(t.pnl - e.pnl)
        if (t.qty > 1e-9 and e.qty > 1e-9
                and abs(t.qty / e.qty - 1.0) <= QTY_NORM_BAND):
            # Qty-normalized comparison isolates per-unit price/commission
            # parity from tiny equity-derived sizing drift between two
            # independently compounded simulations — but ONLY inside
            # QTY_NORM_BAND: a large sizing divergence is a bug the PnL gate
            # must surface, not rescale away.
            abs_diff = min(abs_diff, abs(t.pnl - e.pnl * (t.qty / e.qty)))
        if abs_diff < TV_PNL_ROUNDING_EPSILON_USD:
            pnl_deltas.append(0.0)
            continue
        # Mechanistic exit-coupled forgiveness (replaces the former heuristic
        # escape hatches): a per-trade PnL miss is forgiven ONLY when it is
        # arithmetically explained by this same trade's exit-price drift AND
        # that drift is itself inside the profile's exit tolerance. The exit
        # gate owns exit-price correctness; the PnL gate then fails only on
        # UNEXPLAINED money differences (commission, funding, sizing,
        # rounding) — each gate points at a distinct bug class.
        exit_rel = relative_max(t.exit_price, e.exit_price)
        explained_bound = (max(t.qty, e.qty) * abs(t.exit_price - e.exit_price)
                           + TV_PNL_ROUNDING_EPSILON_USD)
        if exit_rel < thresh["exit"] and abs_diff <= explained_bound:
            pnl_deltas.append(0.0)
            continue
        pnl_deltas.append(abs_diff / abs(t.pnl))

    entry_p90 = percentile(entry_deltas, 0.90)
    exit_p90  = percentile(exit_deltas,  0.90)
    pnl_p90   = percentile(pnl_deltas,   0.90) if pnl_deltas else 0.0

    # Fragmented FIFO grids can have exact entry/count parity and exact
    # position-level close events while entry-grouped consolidation smears exit
    # prices across different FIFO lot boundaries. Under a strict guard, score
    # exit/PnL on the raw exit schedule instead of the consolidated deal blend.
    tv_sched_rows, eng_sched_rows = schedule_rows_for_gate(
        tv_raw_all, eng_raw_all, tv_gate, eng_gate, gating_matched
    )
    fragmented_fifo = (
        has_cross_entry_fifo_allocation(tv_sched_rows)
        or has_cross_entry_fifo_allocation(eng_sched_rows)
    )
    if fragmented_fifo and entry_p90 < 1e-12 and count_delta < STRONG_COUNT_DELTA:
        sched_exit_deltas, sched_pnl_deltas = schedule_exit_metrics(tv_sched_rows, eng_sched_rows)
        if sched_exit_deltas:
            sched_exit_p90 = percentile(sched_exit_deltas, 0.90)
            if sched_exit_p90 <= exit_p90:
                exit_deltas = sched_exit_deltas
                exit_p90 = sched_exit_p90
        if sched_pnl_deltas:
            sched_pnl_p90 = percentile(sched_pnl_deltas, 0.90)
            if sched_pnl_p90 <= pnl_p90:
                pnl_deltas = sched_pnl_deltas
                pnl_p90 = sched_pnl_p90

    # --- Report-only field-coverage deltas (NOT gated) ---
    # Extends the historical 4-dimension gate (count/entry/exit/pnl) to qty,
    # pnl_pct, MFE, MAE, plus p100 worst-case and an unmatched-in-window count,
    # so tail / masked divergences are surfaced rather than absorbed. These do
    # NOT affect the tier label (report-only field coverage).
    qty_deltas    = [relative_max(t.qty, e.qty) for t, e in gating_matched]
    pnlpct_deltas = [abs(t.pnl_pct - e.pnl_pct) for t, e in gating_matched]  # pct-points
    mfe_deltas    = [relative_max(t.mfe, e.mfe) for t, e in gating_matched if abs(t.mfe) > 1e-9]
    mae_deltas    = [relative_max(t.mae, e.mae) for t, e in gating_matched if abs(t.mae) > 1e-9]
    entry_p100  = max(entry_deltas)  if entry_deltas  else 0.0
    exit_p100   = max(exit_deltas)   if exit_deltas   else 0.0
    pnl_p100    = max(pnl_deltas)    if pnl_deltas    else 0.0
    qty_p100    = max(qty_deltas)    if qty_deltas    else 0.0
    pnlpct_p100 = max(pnlpct_deltas) if pnlpct_deltas else 0.0
    mfe_p90     = percentile(mfe_deltas, 0.90) if mfe_deltas else 0.0
    mae_p90     = percentile(mae_deltas, 0.90) if mae_deltas else 0.0
    unmatched_in_window = max(len(tv_gate), len(eng_gate)) - len(gating_matched)

    entry_ok = entry_p90  < thresh["entry"]
    # Count mismatch is a primary reproduction signal. Percent thresholds can
    # hide dozens of missing/extra trades in large strategies, so excellent
    # requires exact gated TV/engine count parity. Any non-zero absolute count
    # mismatch is an engine/codegen gap unless separately proven as a TV/export
    # anomaly.
    count_ok = count_abs_delta == 0
    # HARD GATES ONLY. The former heuristic escape hatches (tiny_exit_pnl_noise,
    # strong_exit_pnl_coupling, pnl_validated_exit_noise, and the mae_* waivers)
    # are gone. Every remaining tolerance is principled and visible:
    #   - the declared per-class profile (strict/production, printed in the
    #     report) absorbs TV's sub-bar broker-emulator drift for trail_* exits;
    #   - the mechanistic exit-coupled PnL forgiveness (pnl_deltas loop above)
    #     stops double-counting a tolerated exit drift as a PnL failure;
    #   - TV-side impossibilities go through the documented anomaly pipeline
    #     (inputs.json expected_tier + ANOMALY.md proof), never hidden gates.
    # A failing gate therefore names a distinct bug class:
    #   count    -> missing/extra trades (signal logic, order lifecycle)
    #   entry    -> entry fill semantics
    #   exit     -> exit fill semantics
    #   pnl      -> UNEXPLAINED money drift (commission, funding, sizing, rounding)
    #   coverage -> engine reproduces only a slice of TV's trading history
    exit_ok  = exit_p90 < thresh["exit"]
    pnl_ok   = pnl_p90 < thresh["pnl"]

    # Coverage: matched fraction of ALL closed TV trades (interior-trimmed when
    # declared), NOT of the self-selected common match window — the
    # anti-window-collapse gate.
    # VARIANT: drop STILL-OPEN TV trades from the coverage denominator ONLY
    # when they are UNMATCHED (no engine trade opened at that entry). A matched
    # open row (engine opened AND closed the position for real past window) is
    # a legitimate matched trade and is kept. This never touches the matcher,
    # the count gate, or entry/exit/pnl -- only the coverage denominator.
    _open_nums = still_open_trade_nums(tv_path)
    _open_keys = {(t.entry_time, t.entry_price, t.direction)
                  for t in tv_raw_all if t.trade_num in _open_nums}
    _matched_tv_ids = {id(t) for t, _ in matched}
    def _is_dropped_open(t):
        return ((t.entry_time, t.entry_price, t.direction) in _open_keys
                and id(t) not in _matched_tv_ids)
    if bounds is not None:
        tv_cov_denom = [t for t in tv
                        if is_interior(t.entry_time * 1000, bounds) and not _is_dropped_open(t)]
        cov_matched = len(gating_matched)
    else:
        tv_cov_denom = [t for t in tv if not _is_dropped_open(t)]
        cov_matched = len(matched)
    unmatched_total = max(len(tv_cov_denom) - cov_matched, 0)
    coverage = (cov_matched / len(tv_cov_denom)) if tv_cov_denom else 1.0
    cov_excellent = coverage >= COVERAGE_EXCELLENT or unmatched_total <= 1
    cov_strong    = coverage >= COVERAGE_STRONG or unmatched_total <= 1
    cov_moderate  = coverage >= COVERAGE_MODERATE

    all_ok   = count_ok and entry_ok and exit_ok and pnl_ok and cov_excellent
    if all_ok:
        label = "excellent"
    elif (
        cov_strong
        and count_delta < STRONG_COUNT_DELTA
        and entry_p90 < STRONG_ENTRY_DELTA
        and exit_p90 < STRONG_EXIT_DELTA
        and pnl_p90 < STRONG_PNL_DELTA
    ):
        label = "strong"
    elif cov_moderate and len(gating_matched) / max(len(tv_gate), 1) >= 0.90:
        label = "moderate"
    elif gating_matched:
        label = "weak"
    else:
        label = "minimal"

    # Documented-divergence metadata is shared with the no-alignment path.
    # Keeping the precedence in one helper prevents an early-return branch
    # from silently turning declared engine-only/anomaly probes into Minimal.
    label = _apply_declared_tier_override(label, meta)

    notes = ""
    if label != "excellent":
        notes = str(meta.get("notes", "")).strip()
        if not notes:
            failures = []
            if not count_ok:
                failures.append(f"count Δ {count_delta*100:.2f}%")
            if not entry_ok:
                failures.append(f"entry p90 {entry_p90*100:.4f}%")
            if not exit_ok:
                failures.append(f"exit p90 {exit_p90*100:.4f}%")
            if not pnl_ok:
                failures.append(f"pnl p90 {pnl_p90*100:.4f}%")
            if not cov_excellent:
                failures.append(f"coverage {coverage*100:.1f}%")
            notes = "; ".join(failures) if failures else "non-excellent"

    return VerificationResult(
        strategy_dir=strategy_dir,
        rel=rel,
        label=label,
        profile=profile,
        notes=notes,
        tv_path=tv_path,
        eng_path=eng_path,
        tv_count=len(tv_cmp),
        eng_count=len(eng_cmp),
        tv_raw_count=len(tv),
        eng_raw_count=len(eng),
        matched_count=len(matched),
        gating_matched_count=len(gating_matched),
        tv_gate_count=len(tv_gate),
        eng_gate_count=len(eng_gate),
        count_delta=count_delta,
        count_abs_delta=count_abs_delta,
        entry_p90=entry_p90,
        exit_p90=exit_p90,
        pnl_p90=pnl_p90,
        coverage=coverage,
        unmatched_total=unmatched_total,
        coverage_tv_count=len(tv_cov_denom),
        trim_bars=trim_bars,
        warmup_bars=warmup_bars,
        bounds=bounds,
        count_ok=count_ok,
        entry_ok=entry_ok,
        exit_ok=exit_ok,
        pnl_ok=pnl_ok,
        coverage_ok=cov_excellent,
        unmatched_in_window=unmatched_in_window,
        entry_p100=entry_p100,
        exit_p100=exit_p100,
        pnl_p100=pnl_p100,
        qty_p100=qty_p100,
        pnlpct_p100=pnlpct_p100,
        mfe_p90=mfe_p90,
        mae_p90=mae_p90,
        matched=matched,
        entry_deltas=entry_deltas,
        exit_deltas=exit_deltas,
        pnl_deltas=pnl_deltas,
        qty_deltas=qty_deltas,
        pnlpct_deltas=pnlpct_deltas,
    )


def _print_verification(result: VerificationResult, *, show_diffs: int = 0) -> None:
    if result.label == "missing":
        print(
            f"{result.rel}\n"
            f"  MISSING (tv: {result.tv_exists}, engine: {result.eng_exists})"
        )
        return
    if result.no_aligned_trades:
        print(
            f"{result.rel}: TV={result.tv_raw_count} engine={result.eng_raw_count} "
            f"matched=0  (no aligned trades)\n  -> {result.label}"
        )
        return

    check = lambda b: "OK" if b else "X"
    match_pct = 100.0 * result.matched_count / max(result.tv_raw_count, 1)
    interior_line = ""
    if result.bounds is not None:
        interior_line = (
            f"  Interior-only: tv={result.tv_gate_count} eng={result.eng_gate_count} "
            f"matched={result.gating_matched_count} "
            f"(trim_bars={result.trim_bars}, warmup_bars={result.warmup_bars})\n"
        )
    print(
        f"{result.rel}\n"
        f"  Profile:       {result.profile}\n"
        f"  TV trades:     {result.tv_count}  (raw {result.tv_raw_count})\n"
        f"  Engine trades: {result.eng_count}  (raw {result.eng_raw_count})\n"
        f"  Matched:       {result.matched_count} ({match_pct:.1f}% of TV)\n"
        f"{interior_line}"
        f"  Count delta:           {result.count_delta * 100:8.4f}%  ({check(result.count_ok)}; abs={result.count_abs_delta})\n"
        f"  Entry-price p90 delta: {result.entry_p90  * 100:8.4f}%  ({check(result.entry_ok)})\n"
        f"  Exit-price  p90 delta: {result.exit_p90   * 100:8.4f}%  ({check(result.exit_ok)})\n"
        f"  PnL         p90 delta: {result.pnl_p90    * 100:8.4f}%  ({check(result.pnl_ok)})\n"
        f"  Coverage:              {result.coverage * 100:8.1f}%  ({check(result.coverage_ok)}; unmatched={result.unmatched_total} of {result.coverage_tv_count} TV)\n"
        f"  -- report-only (not gated; MAE/MFE are intrabar-path-limited) --\n"
        f"  Entry/Exit  p99 delta:  {percentile(result.entry_deltas,0.99)*100:.4f}% / {percentile(result.exit_deltas,0.99)*100:.4f}%\n"
        f"  Entry/Exit/PnL p100:   {result.entry_p100*100:.4f}% / {result.exit_p100*100:.4f}% / {result.pnl_p100*100:.4f}%\n"
        f"  Qty   p90/p100 delta:  {percentile(result.qty_deltas,0.90)*100:.4f}% / {result.qty_p100*100:.4f}%\n"
        f"  PnL%  p90/p100 (pts):  {percentile(result.pnlpct_deltas,0.90):.4f} / {result.pnlpct_p100:.4f}\n"
        f"  MFE/MAE p90 delta:     {result.mfe_p90*100:.4f}% / {result.mae_p90*100:.4f}%\n"
        f"  Unmatched-in-window:   {result.unmatched_in_window}\n"
        f"  -> {result.label}"
    )
    if show_diffs > 0:
        # Preserve the historical diagnostic display. Metrics and the tier are
        # already fixed in ``result``; this formatting cannot affect grading.
        ranked = sorted(
            zip(result.matched, result.entry_deltas, result.exit_deltas, result.pnl_deltas),
            key=lambda x: -max(x[1], x[2], x[3]),
        )
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


def verify_one(strategy_dir: Path, *, verbose: bool = True, show_diffs: int = 0) -> str:
    """Compatibility wrapper returning the tier and optionally printing it."""
    result = analyze_strategy(strategy_dir)
    if verbose:
        _print_verification(result, show_diffs=show_diffs)
    return result.label


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
