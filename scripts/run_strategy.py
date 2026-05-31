#!/usr/bin/env python3
"""Run a compiled PineForge strategy `.so` against an OHLCV bar feed and
emit `engine_trades.csv` in TradingView-compatible format.

This script is the user-facing piece of the reproducibility kit: given
the open-source runtime and the codegen-emitted `generated.cpp`, anyone
can compile a `strategy.so` (see `corpus/CMakeLists.txt`) and drive it
through this harness to regenerate the exact same `engine_trades.csv`
file shipped in the corpus.

It binds *only* the C ABI declared in `<pineforge/pineforge.h>` via
ctypes. There is no transpiler dependency. The struct layouts here are
mirrors of the C declarations and pinned by static_asserts in the
runtime library — if either side drifts, the runtime fails to link
rather than corrupting reads.

Usage examples
--------------

    # Single strategy (auto-finds strategy.so next to generated.cpp)
    python scripts/run_strategy.py corpus/basic/greedy

    # Custom OHLCV input
    python scripts/run_strategy.py corpus/basic/greedy \\
        --ohlcv corpus/data/ohlcv_ETH-USDT-USDT_15m.csv

    # Don't overwrite engine_trades.csv if it already exists
    python scripts/run_strategy.py corpus/basic/greedy --no-overwrite

    # All strategies (matches `bash scripts/run_corpus.sh`)
    for d in corpus/*/*/; do python scripts/run_strategy.py "$d"; done

    # Warm indicators on pre-window bars but suppress order execution
    # until the comparison window opens (prevents warmup-period trades
    # from polluting engine_trades.csv in strategies with early signals):
    python scripts/run_strategy.py corpus/basic/greedy \\
        --disable-trading-before-window
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
REFERENCE_OHLCV = REPO_ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_15m.csv"
WARMUP_OHLCV = REPO_ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_15m_warmup6m.csv"
DEFAULT_OHLCV = WARMUP_OHLCV if WARMUP_OHLCV.exists() else REFERENCE_OHLCV

# Keys in inputs.json that are validator/harness metadata, not Pine input()
# values. Mirrors the canonical validator's VALIDATION_INPUT_META_KEYS so
# they are not forwarded to ``strategy_set_input`` (which would either
# silently no-op or pollute the strategy's input table).
_VALIDATION_META_KEYS = frozenset({
    "tv_trades_csv_tz",
    "tv_trades_csv",
    "runtime_overrides",
    "validation_overrides",
    "ohlcv_csv",
    "ohlcv_start_ms",
    "script_tf",
    "input_tf",
    "chart_timezone",
})


# --- ctypes mirror of <pineforge/pineforge.h> -------------------------
#
# Field order, types, and widths must match the C struct exactly. The
# runtime library has corresponding static_assert(sizeof(...) == ...) and
# offsetof checks in src/c_abi.cpp; if any of these structs drift, the
# library fails to compile.

class BarC(ctypes.Structure):
    _fields_ = [
        ("open", ctypes.c_double),
        ("high", ctypes.c_double),
        ("low", ctypes.c_double),
        ("close", ctypes.c_double),
        ("volume", ctypes.c_double),
        ("timestamp", ctypes.c_int64),
    ]


class TradeC(ctypes.Structure):
    _fields_ = [
        ("entry_time", ctypes.c_int64),
        ("exit_time", ctypes.c_int64),
        ("entry_price", ctypes.c_double),
        ("exit_price", ctypes.c_double),
        ("pnl", ctypes.c_double),
        ("pnl_pct", ctypes.c_double),
        ("is_long", ctypes.c_int),
        ("max_runup", ctypes.c_double),
        ("max_drawdown", ctypes.c_double),
        ("qty", ctypes.c_double),
    ]


class SecurityDiagC(ctypes.Structure):
    _fields_ = [
        ("sec_id", ctypes.c_int),
        ("feed_count", ctypes.c_int64),
        ("eval_complete_count", ctypes.c_int64),
        ("eval_partial_count", ctypes.c_int64),
    ]


class TraceEntryC(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("bar_index", ctypes.c_int32),
        ("name_id", ctypes.c_int32),
        ("value", ctypes.c_double),
    ]


class ReportC(ctypes.Structure):
    _fields_ = [
        ("total_trades", ctypes.c_int),
        ("trades", ctypes.POINTER(TradeC)),
        ("trades_len", ctypes.c_int),
        ("net_profit", ctypes.c_double),
        ("input_bars_processed", ctypes.c_int64),
        ("script_bars_processed", ctypes.c_int64),
        ("security_feeds_total", ctypes.c_int64),
        ("security_eval_complete_total", ctypes.c_int64),
        ("security_eval_partial_total", ctypes.c_int64),
        ("magnifier_sub_bars_total", ctypes.c_int64),
        ("magnifier_sample_ticks_total", ctypes.c_int64),
        ("input_tf_seconds", ctypes.c_int),
        ("script_tf_seconds", ctypes.c_int),
        ("script_tf_ratio", ctypes.c_int),
        ("needs_aggregation", ctypes.c_int),
        ("bar_magnifier_enabled", ctypes.c_int),
        ("security_diag", ctypes.POINTER(SecurityDiagC)),
        ("security_diag_len", ctypes.c_int),
        ("trace", ctypes.POINTER(TraceEntryC)),
        ("trace_len", ctypes.c_int),
        ("trace_names", ctypes.POINTER(ctypes.c_char_p)),
        ("trace_names_len", ctypes.c_int),
    ]


# --- Strategy harness --------------------------------------------------

class Strategy:
    """Thin ctypes wrapper around one strategy.so."""

    def __init__(self, so_path: Path):
        if not so_path.exists():
            raise FileNotFoundError(
                f"strategy library not found: {so_path}\n"
                f"hint: run `cmake --build build --target corpus_strategies` first"
            )
        self.lib = ctypes.CDLL(str(so_path))
        self._setup_signatures()

    def _setup_signatures(self) -> None:
        L = self.lib
        L.strategy_create.argtypes = [ctypes.c_char_p]
        L.strategy_create.restype = ctypes.c_void_p

        L.run_backtest_full.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(BarC), ctypes.c_int,
            ctypes.c_char_p, ctypes.c_char_p,
            ctypes.c_int, ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ReportC),
        ]
        L.run_backtest_full.restype = None

        L.strategy_free.argtypes = [ctypes.c_void_p]
        L.report_free.argtypes = [ctypes.POINTER(ReportC)]
        if hasattr(L, "strategy_get_last_error"):
            L.strategy_get_last_error.argtypes = [ctypes.c_void_p]
            L.strategy_get_last_error.restype = ctypes.c_char_p
        if hasattr(L, "strategy_set_input"):
            L.strategy_set_input.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        if hasattr(L, "strategy_set_trace_enabled"):
            L.strategy_set_trace_enabled.argtypes = [ctypes.c_void_p, ctypes.c_int]
        if hasattr(L, "strategy_set_trade_start_time"):
            L.strategy_set_trade_start_time.argtypes = [ctypes.c_void_p, ctypes.c_int64]
        # ``strategy_set_chart_timezone`` lets the harness tell the engine
        # which IANA wall-clock zone Pine's ``hour`` / ``minute`` /
        # ``dayofweek`` (and the 1-arg function overloads) should produce.
        # Without it the engine stays on UTC and silently diverges by N
        # hours from a TV chart exported under a non-UTC tz. Older .so
        # files predate this export, so we hasattr-guard the wiring and
        # fall back to UTC for them.
        if hasattr(L, "strategy_set_chart_timezone"):
            L.strategy_set_chart_timezone.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            L.strategy_set_chart_timezone.restype = None
        # Optional volume-weighted magnifier toggle. Older .so builds
        # may predate this export — hasattr-guarded so the rest of the
        # harness keeps working with the legacy ABI.
        if hasattr(L, "strategy_set_magnifier_volume_weighted"):
            L.strategy_set_magnifier_volume_weighted.argtypes = [
                ctypes.c_void_p, ctypes.c_int]
            L.strategy_set_magnifier_volume_weighted.restype = None
        # Symbol metadata plumbing (#19). Exchange tz / session feed
        # session.ismarket / time(session); the metadata setter injects
        # fundamental fields (shares_outstanding_*, target_price_*, ...).
        # Older .so builds predate these exports — hasattr-guarded.
        if hasattr(L, "strategy_set_syminfo_timezone"):
            L.strategy_set_syminfo_timezone.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            L.strategy_set_syminfo_timezone.restype = None
        if hasattr(L, "strategy_set_syminfo_session"):
            L.strategy_set_syminfo_session.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            L.strategy_set_syminfo_session.restype = None
        if hasattr(L, "strategy_set_syminfo_metadata"):
            L.strategy_set_syminfo_metadata.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
            L.strategy_set_syminfo_metadata.restype = None

    def run(self, bars_csv: Path, params: dict | None = None,
            *, trace_enabled: bool = False, trade_start_time_ms: int | None = None,
            chart_timezone: str | None = None,
            syminfo_timezone: str | None = None,
            syminfo_session: str | None = None,
            syminfo_metadata: dict | None = None,
            input_tf: str | None = None, script_tf: str | None = None,
            ohlcv_start_ms: int | None = None,
            bar_magnifier: bool = False,
            magnifier_samples: int = 4,
            magnifier_distribution: str = "ENDPOINTS",
            magnifier_volume_weighted: bool = False) -> dict:
        """Read OHLCV from CSV, drive the engine, return a report dict.

        ``ohlcv_start_ms`` (when provided) trims the loaded OHLCV so the
        engine's first bar is at-or-after that timestamp. This is required
        for probes that pin matrix/array warmup depth to the user's TV
        chart history (e.g. ``var matrix<bool>`` accumulators, where
        feeding the full 6-month warmup CSV pre-fills the mask before the
        comparison window begins and the entry gate becomes a no-op).
        Mirrors the per-probe ``inputs.json::ohlcv_start_ms`` metadata
        the validator already honours.
        """
        bars, n = _load_bars(bars_csv, ohlcv_start_ms=ohlcv_start_ms)
        params = params or {}
        params_json = json.dumps(params).encode()

        state = self.lib.strategy_create(params_json)
        report = ReportC()
        try:
            if trace_enabled and hasattr(self.lib, "strategy_set_trace_enabled"):
                self.lib.strategy_set_trace_enabled(state, 1)
            if trade_start_time_ms is not None and hasattr(self.lib, "strategy_set_trade_start_time"):
                self.lib.strategy_set_trade_start_time(state, int(trade_start_time_ms))
            # Wire chart TZ before the run so date builtins (hour/minute/
            # dayofweek + the 1-arg function overloads) land on the same
            # wall clock TV used at export time. Empty/None == leave the
            # engine on its UTC fast path. See validator/_runner.py for
            # the upstream pattern this mirrors.
            if chart_timezone and hasattr(self.lib, "strategy_set_chart_timezone"):
                self.lib.strategy_set_chart_timezone(state, str(chart_timezone).encode())
            # Plumb exchange tz / session / fundamental metadata into syminfo
            # (#19). Exchange tz feeds session.ismarket; for real-session
            # instruments the serving layer should ALSO pass it as
            # chart_timezone so the intraday-cap day rollover aligns.
            if syminfo_timezone and hasattr(self.lib, "strategy_set_syminfo_timezone"):
                self.lib.strategy_set_syminfo_timezone(state, str(syminfo_timezone).encode())
            if syminfo_session and hasattr(self.lib, "strategy_set_syminfo_session"):
                self.lib.strategy_set_syminfo_session(state, str(syminfo_session).encode())
            if syminfo_metadata and hasattr(self.lib, "strategy_set_syminfo_metadata"):
                for mkey, mval in syminfo_metadata.items():
                    try:
                        self.lib.strategy_set_syminfo_metadata(
                            state, str(mkey).encode(), float(mval))
                    except (TypeError, ValueError):
                        continue
            if hasattr(self.lib, "strategy_set_input"):
                for key, value in params.items():
                    if key.startswith("tv_"):
                        continue
                    if key in _VALIDATION_META_KEYS:
                        continue
                    self.lib.strategy_set_input(
                        state,
                        str(key).encode(),
                        str(value).encode(),
                    )
            input_tf_b = (input_tf or "").encode()
            script_tf_b = (script_tf or "").encode()
            # Map magnifier_distribution string → enum int (must mirror
            # MagnifierDistribution in include/pineforge/magnifier.hpp and
            # PF_MAGNIFIER_* in include/pineforge/pineforge.h).
            _MAG_DIST_INT = {
                "UNIFORM": 0,
                "COSINE": 1,
                "TRIANGLE": 2,
                "ENDPOINTS": 3,
                "FRONT_LOADED": 4,
                "BACK_LOADED": 5,
                # Volume-weighted is a separate runtime toggle (see
                # strategy_set_magnifier_volume_weighted) but probe inputs
                # often spell it as a distribution; honour both spellings.
                "VOLUME_WEIGHTED": 3,  # falls back to ENDPOINTS for the t-grid
            }
            mag_dist_int = _MAG_DIST_INT.get(
                str(magnifier_distribution or "ENDPOINTS").upper(), 3)
            mag_on = 1 if bar_magnifier else 0
            mag_samples_int = int(magnifier_samples) if magnifier_samples else 4
            # VOLUME_WEIGHTED toggles a separate runtime knob; flip it
            # whenever the caller asked for that distribution OR set the
            # explicit ``magnifier_volume_weighted`` flag.
            vw_on = bool(magnifier_volume_weighted) or (
                str(magnifier_distribution or "").upper() == "VOLUME_WEIGHTED")
            if mag_on and vw_on and hasattr(self.lib, "strategy_set_magnifier_volume_weighted"):
                self.lib.strategy_set_magnifier_volume_weighted(state, 1)
            self.lib.run_backtest_full(
                state, bars, n,
                input_tf_b, script_tf_b,  # empty -> auto-detect input_tf, default script_tf=input_tf
                mag_on, mag_samples_int, mag_dist_int,
                ctypes.byref(report),
            )
            if hasattr(self.lib, "strategy_get_last_error"):
                err_ptr = self.lib.strategy_get_last_error(state)
                if err_ptr:
                    err_msg = err_ptr.decode("utf-8", "replace")
                    if err_msg:
                        raise RuntimeError(
                            "pineforge engine rejected run: " + err_msg
                        )
            return _report_to_dict(report)
        finally:
            self.lib.report_free(ctypes.byref(report))
            self.lib.strategy_free(state)


def _load_bars(csv_path: Path, *, ohlcv_start_ms: int | None = None) -> tuple[ctypes.Array, int]:
    """Read OHLCV CSV (timestamp, open, high, low, close, volume) into BarC[].

    When ``ohlcv_start_ms`` is given, drop bars whose timestamp is below
    that bound. Used by probes that pin warmup depth to the user's TV
    chart history (per-probe ``inputs.json::ohlcv_start_ms`` metadata).
    """
    rows: list[tuple[float, float, float, float, float, int]] = []
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ts = int(row["timestamp"])
            if ohlcv_start_ms is not None and ts < ohlcv_start_ms:
                continue
            rows.append((
                float(row["open"]),
                float(row["high"]),
                float(row["low"]),
                float(row["close"]),
                float(row["volume"]),
                ts,
            ))
    n = len(rows)
    bars = (BarC * n)()
    for i, (o, h, l, c, v, ts) in enumerate(rows):
        bars[i].open = o
        bars[i].high = h
        bars[i].low = l
        bars[i].close = c
        bars[i].volume = v
        bars[i].timestamp = ts
    return bars, n


def _report_to_dict(r: ReportC) -> dict:
    trades = []
    for i in range(r.trades_len):
        t = r.trades[i]
        trades.append({
            "entry_time": int(t.entry_time),
            "exit_time": int(t.exit_time),
            "entry_price": float(t.entry_price),
            "exit_price": float(t.exit_price),
            "pnl": float(t.pnl),
            "pnl_pct": float(t.pnl_pct),
            "is_long": bool(t.is_long),
            "max_runup": float(t.max_runup),
            "max_drawdown": float(t.max_drawdown),
            "qty": float(t.qty),
        })
    trace_names: list[str] = []
    for i in range(r.trace_names_len):
        raw = r.trace_names[i]
        trace_names.append(raw.decode() if raw else "")
    trace = []
    for i in range(r.trace_len):
        e = r.trace[i]
        name = trace_names[e.name_id] if 0 <= e.name_id < len(trace_names) else ""
        trace.append({
            "timestamp": int(e.timestamp),
            "bar_index": int(e.bar_index),
            "name": name,
            "value": float(e.value),
        })
    return {
        "total_trades": int(r.total_trades),
        "net_profit": float(r.net_profit),
        "input_bars_processed": int(r.input_bars_processed),
        "script_bars_processed": int(r.script_bars_processed),
        "magnifier_sub_bars_total": int(r.magnifier_sub_bars_total),
        "magnifier_sample_ticks_total": int(r.magnifier_sample_ticks_total),
        "bar_magnifier_enabled": int(r.bar_magnifier_enabled),
        "trades": trades,
        "trace": trace,
        "trace_names": trace_names,
    }


# --- TradingView-compatible CSV writer --------------------------------

def _fmt_time_utc(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")


def _load_window_ms(csv_path: Path) -> tuple[int, int]:
    """Return first/last bar timestamps from an OHLCV CSV."""
    first: int | None = None
    last: int | None = None
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ts = int(row["timestamp"])
            if first is None:
                first = ts
            last = ts
    if first is None or last is None:
        raise ValueError(f"empty OHLCV CSV: {csv_path}")
    return first, last


def _filter_trades_to_window(trades: list[dict], window: tuple[int, int] | None) -> list[dict]:
    """Keep trades whose entries occur inside the comparison window."""
    if window is None:
        return trades
    start_ms, end_ms = window
    return [
        t for t in trades
        if start_ms <= int(t["entry_time"]) <= end_ms
    ]


def _filter_trace_to_window(trace: list[dict], window: tuple[int, int] | None) -> list[dict]:
    if window is None:
        return trace
    start_ms, end_ms = window
    return [
        e for e in trace
        if start_ms <= int(e["timestamp"]) <= end_ms
    ]


def write_engine_trades_csv(trades: list[dict], path: Path) -> None:
    """Emit one row per trade *side* (exit then entry) in reverse-chronological
    order — byte-for-byte alignable with TradingView's `trades.csv` export."""
    cum_pnls: dict[int, float] = {}
    running = 0.0
    for n, t in enumerate(trades, 1):
        running += t["pnl"]
        cum_pnls[n] = running

    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "Trade #", "Type", "Date and time", "Price", "Qty",
            "Net PnL", "Net PnL %", "MFE", "MAE", "Cumulative PnL",
        ])
        for n, t in reversed(list(enumerate(trades, 1))):
            direction = "long" if t["is_long"] else "short"
            cum = cum_pnls[n]
            for side, time_key, price_key in (
                (f"Exit {direction}", "exit_time", "exit_price"),
                (f"Entry {direction}", "entry_time", "entry_price"),
            ):
                w.writerow([
                    n, side,
                    _fmt_time_utc(t[time_key]),
                    f"{t[price_key]:.6f}",
                    f"{t['qty']:g}",
                    f"{t['pnl']:.6f}",
                    f"{t['pnl_pct']:.4f}",
                    f"{t['max_runup']:.6f}",
                    f"{t['max_drawdown']:.6f}",
                    f"{cum:.6f}",
                ])



def _tv_timezone_offset(meta: dict) -> int:
    tz_name = str(meta.get("tv_trades_csv_tz", "")).lower()
    return {"utc_plus_8": 8, "asia_taipei": 8, "utc": 0}.get(tz_name, 8)


def _parse_trade_dt(s: str, tz_offset_hours: int) -> int:
    from datetime import timedelta
    tz = timezone(timedelta(hours=tz_offset_hours))
    return int(datetime.strptime(s, "%Y-%m-%d %H:%M").replace(tzinfo=tz).timestamp() * 1000)


def _load_tv_entry_window(strategy_dir: Path, meta: dict, bar_interval_ms: int) -> tuple[int, int] | None:
    tv_name = str(meta.get("tv_trades_csv", "tv_trades.csv"))
    tv_path = strategy_dir / tv_name
    if not tv_path.exists():
        return None
    tz_offset = _tv_timezone_offset(meta)
    entries: list[int] = []
    with tv_path.open(encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if str(row.get("Type", "")).startswith("Entry"):
                entries.append(_parse_trade_dt(row["Date and time"], tz_offset))
    if not entries:
        return None
    return min(entries) - bar_interval_ms, max(entries)


def _infer_bar_interval_ms(csv_path: Path) -> int:
    prev: int | None = None
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ts = int(row["timestamp"])
            if prev is not None:
                return max(1, ts - prev)
            prev = ts
    return 15 * 60 * 1000

# --- CLI ---------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("strategy_dir", type=Path,
                    help="Path to corpus/<cat>/<name>/ — must contain strategy.so")
    ap.add_argument("--ohlcv", type=Path, default=DEFAULT_OHLCV,
                    help=f"OHLCV CSV (default: {DEFAULT_OHLCV.relative_to(REPO_ROOT)})")
    ap.add_argument("--so-name", default="strategy.so",
                    help="Library filename inside strategy_dir (default: strategy.so)")
    ap.add_argument("-o", "--output", type=Path, default=None,
                    help="Write engine_trades.csv to this path instead of "
                         "strategy_dir/engine_trades.csv. Lets callers run "
                         "a strategy.so against alternate OHLCV without "
                         "polluting the strategy folder.")
    ap.add_argument("--no-overwrite", action="store_true",
                    help="Skip if the destination CSV already exists.")
    ap.add_argument("--trace-json", type=Path, default=None,
                    help="Enable @pf-trace capture and write per-bar trace records to JSON.")
    ap.add_argument("--emit-window-ohlcv", type=Path, default=None,
                    help="Trim emitted trades/traces to entries inside this OHLCV window. "
                         "Defaults to the strategy TV trade window when available, else the reference OHLCV window.")
    ap.add_argument("--no-trim-output", action="store_true",
                    help="Emit all trades from the full OHLCV input, including warmup trades.")
    ap.add_argument("--disable-trading-before-window", action="store_true",
                    help="Warm indicators on pre-window bars but ignore strategy order commands until the emit window starts.")
    ap.add_argument("--chart-tz", default="",
                    help="IANA timezone the engine should use for Pine date builtins "
                         "(hour/minute/dayofweek and their 1-arg function overloads) "
                         "and intraday-cap rollover. Empty (default) keeps the engine "
                         "on its UTC fast path, matching how the corpus's TV exports "
                         "were recorded. Pass an IANA name (e.g. 'Asia/Taipei') only "
                         "for probes that genuinely need a non-UTC chart-tz. "
                         "Per-probe override: set 'chart_timezone' in inputs.json.")
    args = ap.parse_args()

    strategy_dir = args.strategy_dir.resolve()
    out_path = (args.output.resolve() if args.output
                else strategy_dir / "engine_trades.csv")
    if args.no_overwrite and out_path.exists():
        print(f"SKIP (exists): {out_path}")
        return 0
    out_path.parent.mkdir(parents=True, exist_ok=True)

    so_path = strategy_dir / args.so_name
    if not so_path.exists():
        for alt in ("strategy.dylib", "strategy.so", "strategy.dll"):
            cand = strategy_dir / alt
            if cand.exists():
                so_path = cand
                break

    started = time.time()
    strat = Strategy(so_path)
    inputs_path = strategy_dir / "inputs.json"
    params = {}
    if inputs_path.exists():
        with inputs_path.open(encoding="utf-8") as f:
            params = json.load(f)
    # Per-probe inputs.json::ohlcv_csv override wins over --ohlcv. Used by
    # probes that need a different OHLCV than the global default — e.g.
    # ``request.security_lower_tf`` probes that need a real 1m feed
    # routed through the engine's input-passthrough LTF path instead of
    # the synthesis path's waypoint walk. Mirrors the canonical
    # validator's per-probe override (validate.py docstring lines 63-71).
    if isinstance(params, dict) and "ohlcv_csv" in params:
        _csv_val = params["ohlcv_csv"]
        ohlcv_path = (Path(_csv_val) if str(_csv_val).startswith("/")
                      else (strategy_dir / _csv_val)).resolve()
    else:
        ohlcv_path = args.ohlcv.resolve()
    tv_window_used = False
    if args.no_trim_output:
        emit_window = None
    elif args.emit_window_ohlcv is not None:
        emit_window = _load_window_ms(args.emit_window_ohlcv.resolve())
    else:
        emit_window = _load_tv_entry_window(strategy_dir, params, _infer_bar_interval_ms(ohlcv_path))
        tv_window_used = emit_window is not None
        if emit_window is None:
            emit_window = _load_window_ms(REFERENCE_OHLCV)
    trade_start_ms = emit_window[0] if (emit_window is not None and (tv_window_used or args.disable_trading_before_window)) else None
    # Per-probe inputs.json override wins over the CLI default. Empty
    # string is honoured (treated as "use engine UTC fast path").
    if isinstance(params, dict) and "chart_timezone" in params:
        chart_tz = str(params.get("chart_timezone") or "")
    else:
        chart_tz = args.chart_tz or ""
    # Optional per-probe input_tf / script_tf overrides. Used together
    # with ``ohlcv_csv`` when the override OHLCV is a finer feed than
    # the strategy's native chart TF — e.g. feeding 1m bars to a 15m
    # strategy so request.security_lower_tf("1", ...) takes the
    # input-passthrough LTF path. Empty string => engine uses its
    # auto-detect (input_tf) / input_tf default (script_tf).
    input_tf_override = (str(params.get("input_tf") or "")
                        if isinstance(params, dict) else "")
    script_tf_override = (str(params.get("script_tf") or "")
                          if isinstance(params, dict) else "")
    # Per-probe ``ohlcv_start_ms`` trims the loaded OHLCV so the engine's
    # first bar matches the user's TV chart history start. Required for
    # probes whose state accumulators (var matrix<bool>, var array<>, etc.)
    # would behave differently when fed extra warmup bars not present on
    # the original TV chart at export time.
    ohlcv_start_ms_val: int | None = None
    if isinstance(params, dict) and "ohlcv_start_ms" in params:
        try:
            ohlcv_start_ms_val = int(params["ohlcv_start_ms"])
        except (TypeError, ValueError):
            ohlcv_start_ms_val = None
    # Per-probe ``runtime_overrides`` block forwards engine-level knobs
    # (bar magnifier toggle, magnifier sample distribution, etc.) that
    # don't have Pine ``input.*()`` counterparts. Mirrors the canonical
    # validator's ``runtime_overrides`` schema. Unknown keys are silently
    # ignored so future runtime knobs don't break older harness builds.
    runtime_overrides = (params.get("runtime_overrides") or {}) \
        if isinstance(params, dict) else {}
    if not isinstance(runtime_overrides, dict):
        runtime_overrides = {}
    bar_magnifier_flag = bool(runtime_overrides.get("bar_magnifier", False))
    magnifier_dist_str = str(runtime_overrides.get("magnifier_distribution",
                                                  "ENDPOINTS") or "ENDPOINTS")
    try:
        magnifier_samples_int = int(runtime_overrides.get("magnifier_samples", 4) or 4)
    except (TypeError, ValueError):
        magnifier_samples_int = 4
    magnifier_vw_flag = bool(runtime_overrides.get("magnifier_volume_weighted", False))
    # Symbol metadata overrides (#19): exchange tz / session feed
    # session.ismarket; ``syminfo_metadata`` injects fundamental fields.
    syminfo_tz = str(runtime_overrides.get("timezone") or "") or None
    syminfo_session = str(runtime_overrides.get("session") or "") or None
    syminfo_metadata = runtime_overrides.get("syminfo_metadata")
    if not isinstance(syminfo_metadata, dict):
        syminfo_metadata = None
    report = strat.run(ohlcv_path, params=params,
                       trace_enabled=args.trace_json is not None,
                       trade_start_time_ms=trade_start_ms,
                       chart_timezone=chart_tz,
                       syminfo_timezone=syminfo_tz,
                       syminfo_session=syminfo_session,
                       syminfo_metadata=syminfo_metadata,
                       input_tf=input_tf_override or None,
                       script_tf=script_tf_override or None,
                       ohlcv_start_ms=ohlcv_start_ms_val,
                       bar_magnifier=bar_magnifier_flag,
                       magnifier_samples=magnifier_samples_int,
                       magnifier_distribution=magnifier_dist_str,
                       magnifier_volume_weighted=magnifier_vw_flag)
    raw_trade_count = len(report["trades"])
    trades_to_write = _filter_trades_to_window(report["trades"], emit_window)
    write_engine_trades_csv(trades_to_write, out_path)
    if args.trace_json is not None:
        trace_to_write = _filter_trace_to_window(report["trace"], emit_window)
        args.trace_json.parent.mkdir(parents=True, exist_ok=True)
        with args.trace_json.open("w", encoding="utf-8") as f:
            json.dump({
                "strategy": str(strategy_dir),
                "ohlcv": str(ohlcv_path),
                "emit_window": None if emit_window is None else {"start_ms": emit_window[0], "end_ms": emit_window[1]},
                "trace_names": report["trace_names"],
                "trace": trace_to_write,
            }, f)
    elapsed = time.time() - started

    try:
        rel = strategy_dir.relative_to(REPO_ROOT)
    except ValueError:
        rel = strategy_dir
    print(
        f"{rel}: "
        f"{len(trades_to_write)} trades"
        f"{'' if raw_trade_count == len(trades_to_write) else f' ({raw_trade_count} raw)'}, "
        f"net_profit={report['net_profit']:.2f}, "
        f"bars={report['input_bars_processed']}, "
        f"{elapsed:.2f}s -> {out_path.name}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
