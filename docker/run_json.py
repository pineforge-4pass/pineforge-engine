#!/usr/bin/env python3
"""PineForge container harness — load strategy.so, run against an
OHLCV CSV, emit a JSON report on stdout.

Schema:
    {
      "engine": "pineforge",
      "input": {
        "ohlcv":      "<path>",
        "bars":       int,
        "first_ts":   int,           # unix ms
        "last_ts":    int,           # unix ms
        "first_time": "YYYY-MM-DD HH:MM UTC",
        "last_time":  "YYYY-MM-DD HH:MM UTC"
      },
      "elapsed_seconds": float,
      "summary": {
        "total_trades": int,
        "wins":         int,
        "losses":       int,
        "win_rate_pct": float,
        "net_pnl":      float,
        "avg_trade":    float,
        "best_trade":   float,
        "worst_trade":  float,
        "max_drawdown": float,
        "bars_processed": int
      },
      "trades": [
        {
          "n":            int,
          "side":         "long" | "short",
          "entry_time":   int,       # unix ms
          "exit_time":    int,       # unix ms
          "entry_price":  float,
          "exit_price":   float,
          "qty":          float,
          "pnl":          float,
          "pnl_pct":      float,
          "max_runup":    float,
          "max_drawdown": float,
          "commission":      float,   # ABI v2
          "entry_bar_index": int,     # ABI v2: script-bar index of entry fill
          "exit_bar_index":  int      # ABI v2: script-bar index of exit fill
        },
        ...
      ],
      "metrics": {                     # ABI v2 computed trading metrics
        "all":    { ...pf_trade_stats_t... },   # all closed trades
        "longs":  { ...pf_trade_stats_t... },   # long trades only
        "shorts": { ...pf_trade_stats_t... },   # short trades only
        "equity": { ...pf_equity_stats_t... }   # sharpe/sortino/cagr/calmar/...
      },                               # any NaN statistic -> null (see _num)
      "equity_curve": [                # ABI v2: one point per script bar
        { "time_ms": int, "equity": float, "open_profit": float },
        ...
      ],
      "fingerprint": {                 # decode-able backtest provenance
        "token":  "<base64(canonical provenance JSON)>",  # b64decode -> JSON
        "digest": "sha256:<hex>",      # stable run id over canonical JSON
        "provenance": {
          "engine":   { version_string, major, minor, patch, commit_sha },
          "codegen":  { version, generated_cpp_sha256, transpiled_from_pine },
          "strategy": { ...all strategy() params, effective... },
          "inputs":   { "<title>": { type, default, value }, ... },
          "applied":  { "inputs": {...}, "overrides": {...} },  # user deltas
          "runtime":  { ...same fields as applied_runtime... }
        }
      }
    }

NaN convention: any metric with an empty/zero denominator is null (JSON has no
NaN); a real computed 0 stays 0. See the report-schema + metrics reference docs
for the per-field meaning of every metrics.* key.
"""
from __future__ import annotations

import argparse
import base64
import csv
import ctypes
import hashlib
import json
import math
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


# >>> fingerprint helpers (DUPLICATED verbatim in scripts/run_strategy.py;
#     scripts/ is .dockerignore'd so this cannot be a shared module.
#     scripts/fingerprint_self_test.py asserts both copies stay identical.)
try:
    from importlib import metadata as _ilmd
except ImportError:  # pragma: no cover
    _ilmd = None

# Canonical strategy() defaults. Mirrors the engine base-class defaults in
# include/pineforge/engine.hpp (initial_capital_, process_orders_on_close_,
# default_qty_type_, default_qty_value_, pyramiding_, commission_type_,
# commission_value_, slippage_, close_entries_rule_any_). The codegen ctor
# emits only a subset (it omits process_orders_on_close + close_entries_rule),
# so this seed supplies the rest. KEEP IN SYNC with engine.hpp.
STRATEGY_SEED = {
    "initial_capital": 1000000.0,
    "process_orders_on_close": False,
    "default_qty_type": "fixed",
    "default_qty_value": 1.0,
    "pyramiding": 1,
    "commission_type": "percent",
    "commission_value": 0.0,
    "slippage": 0,
    "close_entries_rule": "FIFO",
}

_QTY_TYPE = {"FIXED": "fixed", "PERCENT_OF_EQUITY": "percent_of_equity", "CASH": "cash"}
_COMM_TYPE = {"PERCENT": "percent", "CASH_PER_ORDER": "cash_per_order",
              "CASH_PER_CONTRACT": "cash_per_contract"}

# generated.cpp ctor field name -> provenance key.
_STRAT_FIELD_KEY = {
    "initial_capital_": "initial_capital",
    "process_orders_on_close_": "process_orders_on_close",
    "default_qty_type_": "default_qty_type",
    "default_qty_value_": "default_qty_value",
    "pyramiding_": "pyramiding",
    "commission_type_": "commission_type",
    "commission_value_": "commission_value",
    "slippage_": "slippage",
    "close_entries_rule_any_": "close_entries_rule",
}

_INPUT_RE = re.compile(
    r'get_input_(\w+)\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*((?:[^();]|\([^()]*\))*?)\s*\)')


def _ctor_body(cpp_text: str) -> str:
    """Return the GeneratedStrategy constructor body, or '' if not found.

    Scoping to the ctor is load-bearing: set_strategy_override() also contains
    `initial_capital_ = std::stod(value);` lines that must NOT be parsed as
    defaults. The member-init list (`_ta_ema_1(5)`) has no `=` so it cannot
    false-match the field regex."""
    m = re.search(r"GeneratedStrategy\s*\([^)]*\)\s*(?::[^{]*)?\{", cpp_text)
    if not m:
        return ""
    i = m.end() - 1  # index of the opening '{'
    depth = 0
    for j in range(i, len(cpp_text)):
        c = cpp_text[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return cpp_text[i + 1:j]
    return ""


def _coerce_scalar(rhs: str):
    rhs = rhs.strip()
    if rhs in ("true", "false"):
        return rhs == "true"
    if re.fullmatch(r"[+-]?\d+", rhs):
        return int(rhs)
    try:
        f = float(rhs)
        return f if (f == f and f not in (float("inf"), float("-inf"))) else rhs
    except ValueError:
        return rhs


def _unwrap_std_string(expr: str) -> str:
    """Codegen wraps string input defaults as std::string("..."); unwrap to the
    inner literal so the recorded default is the value, not the C++ expression."""
    m = re.fullmatch(r'std::string\((.*)\)', expr.strip(), re.DOTALL)
    return m.group(1).strip() if m else expr


def parse_strategy_params(cpp_text: str) -> dict:
    """Parse strategy() header defaults from the constructor body only."""
    out: dict = {}
    body = _ctor_body(cpp_text)
    for fld, rhs in re.findall(r"(\w+_)\s*=\s*([^;]+);", body):
        key = _STRAT_FIELD_KEY.get(fld)
        if not key:
            continue
        rhs = rhs.strip()
        if fld == "default_qty_type_":
            out[key] = _QTY_TYPE.get(rhs.split("::")[-1], rhs)
        elif fld == "commission_type_":
            out[key] = _COMM_TYPE.get(rhs.split("::")[-1], rhs)
        elif fld == "close_entries_rule_any_":
            out[key] = "ANY" if _coerce_scalar(rhs) is True else "FIFO"
        else:
            out[key] = _coerce_scalar(rhs)
    return out


def effective_strategy(cpp_text: str, overrides: dict | None) -> dict:
    """Canonical seed -> ctor-parsed defaults -> user overrides (string wins)."""
    s = dict(STRATEGY_SEED)
    s.update(parse_strategy_params(cpp_text))
    for k, v in (overrides or {}).items():
        s[k] = v
    return s


def parse_inputs(cpp_text: str) -> dict:
    """Parse every get_input_*("title", default) call; dedup by title (first wins)."""
    out: dict = {}
    for typ, title, dflt in _INPUT_RE.findall(cpp_text):
        if title in out:
            continue
        d = _unwrap_std_string(dflt.strip())
        if d.startswith('"') and d.endswith('"') and len(d) >= 2:
            val = d[1:-1]
        elif typ == "source":
            val = d
        else:
            val = _coerce_scalar(d)
        out[title] = {"type": typ, "default": val}
    return out


def effective_inputs(cpp_text: str, inputs_applied: dict | None) -> dict:
    """All declared inputs with {type, default, value}; value = override or default.
    Applied inputs with no matching declaration are appended best-effort."""
    applied = inputs_applied or {}
    out: dict = {}
    for title, meta in parse_inputs(cpp_text).items():
        out[title] = {
            "type": meta["type"],
            "default": meta["default"],
            "value": applied.get(title, meta["default"]),
        }
    for title, v in applied.items():
        if title not in out:
            out[title] = {"type": "unknown", "default": None, "value": v}
    return out


def _sha256_file(path) -> str | None:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def _codegen_version() -> str:
    if _ilmd is None:
        return "unknown"
    try:
        return _ilmd.version("pineforge-codegen")
    except Exception:
        return "unknown"


def build_provenance(engine: dict, cpp_path, transpiled: bool,
                     inputs_applied: dict, overrides_applied: dict,
                     runtime: dict | None) -> dict:
    cpp_text = ""
    cpp_sha = None
    if cpp_path:
        cpp_sha = _sha256_file(cpp_path)
        try:
            with open(cpp_path, "r", encoding="utf-8", errors="replace") as f:
                cpp_text = f.read()
        except OSError:
            cpp_text = ""
    return {
        "engine": engine,
        "codegen": {
            "version": _codegen_version(),
            "generated_cpp_sha256": cpp_sha,
            "transpiled_from_pine": bool(transpiled),
        },
        "strategy": effective_strategy(cpp_text, overrides_applied),
        "inputs": effective_inputs(cpp_text, inputs_applied),
        "applied": {
            "inputs": dict(inputs_applied or {}),
            "overrides": dict(overrides_applied or {}),
        },
        "runtime": runtime or {},
    }


def build_fingerprint(provenance: dict) -> dict:
    canonical = json.dumps(provenance, sort_keys=True, separators=(",", ":"))
    raw = canonical.encode("utf-8")
    return {
        "token": base64.b64encode(raw).decode("ascii"),
        "digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
        "provenance": provenance,
    }
# <<< fingerprint helpers


# --- ctypes mirror of <pineforge/pineforge.h> -------------------------

class BarC(ctypes.Structure):
    _fields_ = [
        ("open",      ctypes.c_double),
        ("high",      ctypes.c_double),
        ("low",       ctypes.c_double),
        ("close",     ctypes.c_double),
        ("volume",    ctypes.c_double),
        ("timestamp", ctypes.c_int64),
    ]


class TradeC(ctypes.Structure):
    _fields_ = [
        ("entry_time",   ctypes.c_int64),
        ("exit_time",    ctypes.c_int64),
        ("entry_price",  ctypes.c_double),
        ("exit_price",   ctypes.c_double),
        ("pnl",          ctypes.c_double),
        ("pnl_pct",      ctypes.c_double),
        ("is_long",      ctypes.c_int),
        ("max_runup",    ctypes.c_double),
        ("max_drawdown", ctypes.c_double),
        ("qty",          ctypes.c_double),
        ("commission",      ctypes.c_double),
        ("entry_bar_index", ctypes.c_int32),
        ("exit_bar_index",  ctypes.c_int32),
    ]


class TradeStatsC(ctypes.Structure):
    """Mirror of pf_trade_stats_t (ABI v2)."""
    _fields_ = [
        ("num_trades", ctypes.c_int32), ("num_wins", ctypes.c_int32),
        ("num_losses", ctypes.c_int32), ("num_even", ctypes.c_int32),
        ("percent_profitable", ctypes.c_double),
        ("net_profit", ctypes.c_double), ("net_profit_pct", ctypes.c_double),
        ("gross_profit", ctypes.c_double), ("gross_profit_pct", ctypes.c_double),
        ("gross_loss", ctypes.c_double), ("gross_loss_pct", ctypes.c_double),
        ("profit_factor", ctypes.c_double),
        ("avg_trade", ctypes.c_double), ("avg_trade_pct", ctypes.c_double),
        ("avg_win", ctypes.c_double), ("avg_win_pct", ctypes.c_double),
        ("avg_loss", ctypes.c_double), ("avg_loss_pct", ctypes.c_double),
        ("ratio_avg_win_avg_loss", ctypes.c_double),
        ("largest_win", ctypes.c_double), ("largest_win_pct", ctypes.c_double),
        ("largest_loss", ctypes.c_double), ("largest_loss_pct", ctypes.c_double),
        ("commission_paid", ctypes.c_double),
        ("expectancy", ctypes.c_double),
        ("max_consecutive_wins", ctypes.c_int32), ("max_consecutive_losses", ctypes.c_int32),
        ("avg_bars_in_trade", ctypes.c_double), ("avg_bars_in_wins", ctypes.c_double),
        ("avg_bars_in_losses", ctypes.c_double),
    ]


class EquityStatsC(ctypes.Structure):
    """Mirror of pf_equity_stats_t (ABI v2)."""
    _fields_ = [
        ("max_equity_drawdown", ctypes.c_double), ("max_equity_drawdown_pct", ctypes.c_double),
        ("max_equity_runup", ctypes.c_double), ("max_equity_runup_pct", ctypes.c_double),
        ("buy_hold_return", ctypes.c_double), ("buy_hold_return_pct", ctypes.c_double),
        ("sharpe_tv", ctypes.c_double), ("sortino_tv", ctypes.c_double),
        ("sharpe_bar", ctypes.c_double), ("sortino_bar", ctypes.c_double),
        ("cagr", ctypes.c_double), ("calmar", ctypes.c_double),
        ("recovery_factor", ctypes.c_double), ("time_in_market_pct", ctypes.c_double),
        ("open_pl", ctypes.c_double),
    ]


class MetricsC(ctypes.Structure):
    """Mirror of pf_metrics_t (ABI v2)."""
    _fields_ = [("all", TradeStatsC), ("longs", TradeStatsC),
                ("shorts", TradeStatsC), ("equity", EquityStatsC)]


class EquityPointC(ctypes.Structure):
    """Mirror of pf_equity_point_t (ABI v2)."""
    _fields_ = [("time_ms", ctypes.c_int64), ("equity", ctypes.c_double),
                ("open_profit", ctypes.c_double)]


class SecurityDiagC(ctypes.Structure):
    _fields_ = [
        ("sec_id",              ctypes.c_int),
        ("feed_count",          ctypes.c_int64),
        ("eval_complete_count", ctypes.c_int64),
        ("eval_partial_count",  ctypes.c_int64),
    ]


class TraceEntryC(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("bar_index", ctypes.c_int32),
        ("name_id",   ctypes.c_int32),
        ("value",     ctypes.c_double),
    ]


class OrderEventC(ctypes.Structure):
    _fields_ = [
        ("transition_sequence", ctypes.c_uint64),
        ("command_revision_id", ctypes.c_uint64),
        ("order_leg_id", ctypes.c_uint64),
        ("priority_sequence", ctypes.c_uint64),
        ("fill_id", ctypes.c_uint64),
        ("entry_lot_id", ctypes.c_uint64),
        ("position_episode_id", ctypes.c_uint64),
        ("event_timestamp", ctypes.c_int64),
        ("event_sequence", ctypes.c_uint64),
        ("input_bar_index", ctypes.c_int64),
        ("script_bar_index", ctypes.c_int32),
        ("command_kind", ctypes.c_int32), ("leg_kind", ctypes.c_int32),
        ("state_before", ctypes.c_int32), ("state_after", ctypes.c_int32),
        ("transition", ctypes.c_int32), ("reason", ctypes.c_int32),
        ("side", ctypes.c_int32), ("oca_type", ctypes.c_int32),
        ("requested_quantity", ctypes.c_double),
        ("remaining_quantity", ctypes.c_double),
        ("filled_quantity", ctypes.c_double),
        ("observed_price", ctypes.c_double), ("stop_price", ctypes.c_double),
        ("limit_price", ctypes.c_double),
        ("trail_activation_price", ctypes.c_double),
        ("trail_watermark", ctypes.c_double), ("fill_price", ctypes.c_double),
        ("position_size_before", ctypes.c_double),
        ("position_size_after", ctypes.c_double),
        ("equity_before", ctypes.c_double), ("equity_after", ctypes.c_double),
        ("id", ctypes.c_char_p), ("from_entry", ctypes.c_char_p),
        ("oca_name", ctypes.c_char_p),
    ]


class ReportC(ctypes.Structure):
    _fields_ = [
        ("total_trades",                 ctypes.c_int),
        ("trades",                       ctypes.POINTER(TradeC)),
        ("trades_len",                   ctypes.c_int),
        ("net_profit",                   ctypes.c_double),
        ("input_bars_processed",         ctypes.c_int64),
        ("script_bars_processed",        ctypes.c_int64),
        ("security_feeds_total",         ctypes.c_int64),
        ("security_eval_complete_total", ctypes.c_int64),
        ("security_eval_partial_total",  ctypes.c_int64),
        ("magnifier_sub_bars_total",     ctypes.c_int64),
        ("magnifier_sample_ticks_total", ctypes.c_int64),
        ("input_tf_seconds",             ctypes.c_int),
        ("script_tf_seconds",            ctypes.c_int),
        ("script_tf_ratio",              ctypes.c_int),
        ("needs_aggregation",            ctypes.c_int),
        ("bar_magnifier_enabled",        ctypes.c_int),
        ("security_diag",                ctypes.POINTER(SecurityDiagC)),
        ("security_diag_len",            ctypes.c_int),
        ("trace",                        ctypes.POINTER(TraceEntryC)),
        ("trace_len",                    ctypes.c_int),
        ("trace_names",                  ctypes.POINTER(ctypes.c_char_p)),
        ("trace_names_len",              ctypes.c_int),
        ("metrics",                      MetricsC),
        ("equity_curve",                 ctypes.POINTER(EquityPointC)),
        ("equity_curve_len",             ctypes.c_int64),  # int64, NOT c_int
        ("order_events",                 ctypes.POINTER(OrderEventC)),
        ("order_events_len",             ctypes.c_int64),
        ("order_event_count",            ctypes.c_uint64),
        ("order_event_hash",             ctypes.c_uint64),
        ("order_event_dropped",          ctypes.c_uint64),
    ]


class PfVersionC(ctypes.Structure):
    """Mirror of pf_version_t (returned by value from pf_version_get)."""
    _fields_ = [("major", ctypes.c_int), ("minor", ctypes.c_int),
                ("patch", ctypes.c_int), ("commit_sha", ctypes.c_char_p)]


def engine_version(lib: ctypes.CDLL) -> dict:
    """Read engine version+sha from the .so (whole-archive exports). The
    fields are hasattr-guarded so an older .so degrades to blanks."""
    eng = {"version_string": "", "major": None, "minor": None,
           "patch": None, "commit_sha": ""}
    if hasattr(lib, "pf_version_string"):
        lib.pf_version_string.restype = ctypes.c_char_p
        s = lib.pf_version_string()
        eng["version_string"] = s.decode("utf-8", "replace") if s else ""
    if hasattr(lib, "pf_version_get"):
        lib.pf_version_get.restype = PfVersionC
        v = lib.pf_version_get()
        eng["major"], eng["minor"], eng["patch"] = int(v.major), int(v.minor), int(v.patch)
        eng["commit_sha"] = v.commit_sha.decode("utf-8", "replace") if v.commit_sha else ""
    return eng


# pf_report_t is CALLER-allocated: a .so built against a different ABI
# writes past (or short of) our ReportC buffer. Assert version up front.
EXPECTED_PF_ABI = 3


def check_abi(lib: ctypes.CDLL) -> None:
    try:
        lib.pf_abi_version.restype = ctypes.c_int
        abi = lib.pf_abi_version()
    except AttributeError:
        raise RuntimeError(
            "strategy .so predates pf_abi_version (ABI v1); rebuild it against "
            "the current pineforge runtime (pf_report_t grew).")
    if abi != EXPECTED_PF_ABI:
        raise RuntimeError(
            f"pineforge ABI mismatch: .so reports {abi}, harness expects "
            f"{EXPECTED_PF_ABI}; rebuild.")


# --- helpers ----------------------------------------------------------

def load_bars(csv_path: Path) -> tuple[ctypes.Array, int]:
    rows: list[tuple[float, float, float, float, float, int]] = []
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append((
                float(row["open"]),
                float(row["high"]),
                float(row["low"]),
                float(row["close"]),
                float(row["volume"]),
                int(row["timestamp"]),
            ))
    n = len(rows)
    bars = (BarC * n)()
    for i, (o, h, l, c, v, ts) in enumerate(rows):
        bars[i].open      = o
        bars[i].high      = h
        bars[i].low       = l
        bars[i].close     = c
        bars[i].volume    = v
        bars[i].timestamp = ts
    return bars, n


def load_strategy(so_path: Path) -> ctypes.CDLL:
    lib = ctypes.CDLL(str(so_path))
    check_abi(lib)

    lib.strategy_create.argtypes = [ctypes.c_char_p]
    lib.strategy_create.restype  = ctypes.c_void_p

    lib.strategy_set_input.argtypes    = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.strategy_set_override.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]

    lib.run_backtest_full.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(BarC), ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ReportC),
    ]
    lib.run_backtest_full.restype = None

    if hasattr(lib, "strategy_get_last_error"):
        lib.strategy_get_last_error.argtypes = [ctypes.c_void_p]
        lib.strategy_get_last_error.restype  = ctypes.c_char_p

    # syminfo setters — declare argtypes so ctypes does not default the float
    # args to c_int (which would truncate mintick=0.5 to 0). Guarded with
    # hasattr in case an older strategy.so predates these symbols.
    for _n in ("strategy_set_syminfo_mintick", "strategy_set_syminfo_pointvalue"):
        if hasattr(lib, _n):
            getattr(lib, _n).argtypes = [ctypes.c_void_p, ctypes.c_double]
    for _n in ("strategy_set_syminfo_timezone", "strategy_set_syminfo_session"):
        if hasattr(lib, _n):
            getattr(lib, _n).argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    # Validation-parity setters mirrored from scripts/run_strategy.py. All
    # hasattr-guarded: trade_start_time + chart_timezone are runtime PF exports;
    # magnifier_volume_weighted is a PER-STRATEGY codegen symbol (may be absent
    # on a .so that didn't emit it) — never call it unconditionally.
    if hasattr(lib, "strategy_set_trade_start_time"):
        lib.strategy_set_trade_start_time.argtypes = [ctypes.c_void_p, ctypes.c_int64]
        lib.strategy_set_trade_start_time.restype = None
    if hasattr(lib, "strategy_set_chart_timezone"):
        lib.strategy_set_chart_timezone.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.strategy_set_chart_timezone.restype = None
    if hasattr(lib, "strategy_set_magnifier_volume_weighted"):
        lib.strategy_set_magnifier_volume_weighted.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.strategy_set_magnifier_volume_weighted.restype = None

    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.report_free.argtypes   = [ctypes.POINTER(ReportC)]
    return lib


def apply_syminfo(lib, strat, syminfo_path):
    """Apply syminfo.json (data-worker schema) via strategy_set_syminfo_*.
    Tolerant: missing keys skipped. Accepts {"syminfo": {...}} or a flat dict."""
    import json
    doc = json.loads(open(syminfo_path).read())
    si = doc.get("syminfo", doc)
    if "mintick" in si:    lib.strategy_set_syminfo_mintick(strat, float(si["mintick"]))
    if "pointvalue" in si: lib.strategy_set_syminfo_pointvalue(strat, float(si["pointvalue"]))
    if si.get("timezone"): lib.strategy_set_syminfo_timezone(strat, str(si["timezone"]).encode())
    if si.get("session"):  lib.strategy_set_syminfo_session(strat, str(si["session"]).encode())


def fmt_utc(ms: int) -> str:
    return datetime.fromtimestamp(
        ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def _num(x):
    """JSON-safe float. The engine's metric NaN convention (empty / zero
    denominator -> NaN, never 0) cannot survive JSON: json.dump emits a bare
    `NaN` token that a strict downstream JSON.parse (the MCP layer) rejects.
    Collapse every non-finite double to null so the report stays valid JSON."""
    f = float(x)
    return f if math.isfinite(f) else None


def _stats_dict(s) -> dict:
    """Serialize a pf_trade_stats_t / pf_equity_stats_t ctypes struct to a dict,
    keying off each field's ctype: integer counters stay ints, every double is
    sanitized through _num. Driven by _fields_ so it tracks the struct verbatim."""
    out = {}
    for name, ctype in s._fields_:
        v = getattr(s, name)
        out[name] = _num(v) if ctype is ctypes.c_double else int(v)
    return out


def build_report_dict(report: ReportC, ohlcv_path: Path,
                      n_bars: int, first_ts: int, last_ts: int,
                      elapsed: float,
                      applied_inputs: dict[str, str],
                      applied_overrides: dict[str, str],
                      applied_runtime: dict[str, object] | None = None) -> dict:
    trades = []
    pnls: list[float] = []
    for i in range(report.trades_len):
        t = report.trades[i]
        pnls.append(float(t.pnl))
        trades.append({
            "n":            i + 1,
            "side":         "long" if t.is_long else "short",
            "entry_time":   int(t.entry_time),
            "exit_time":    int(t.exit_time),
            "entry_price":  float(t.entry_price),
            "exit_price":   float(t.exit_price),
            "qty":          float(t.qty),
            "pnl":          float(t.pnl),
            "pnl_pct":      float(t.pnl_pct),
            "max_runup":    float(t.max_runup),
            "max_drawdown": float(t.max_drawdown),
            "commission":      float(t.commission),
            "entry_bar_index": int(t.entry_bar_index),
            "exit_bar_index":  int(t.exit_bar_index),
        })

    n = len(pnls)
    wins   = sum(1 for p in pnls if p > 0)
    losses = sum(1 for p in pnls if p < 0)

    cum, peak, max_dd = 0.0, 0.0, 0.0
    for p in pnls:
        cum += p
        peak = max(peak, cum)
        max_dd = min(max_dd, cum - peak)

    # Computed trading metrics (ABI v2): all/longs/shorts trade stats + the
    # equity-curve-derived block (sharpe/sortino/cagr/calmar/...). See the
    # report-schema + metrics reference pages for per-field definitions.
    m = report.metrics
    metrics = {
        "all":    _stats_dict(m.all),
        "longs":  _stats_dict(m.longs),
        "shorts": _stats_dict(m.shorts),
        "equity": _stats_dict(m.equity),
    }

    # Per-script-bar equity curve (ABI v2). equity_curve may be NULL if a
    # mid-run exception truncated it (len then 0); guard the pointer deref.
    equity_curve = []
    if report.equity_curve:
        for i in range(int(report.equity_curve_len)):
            p = report.equity_curve[i]
            equity_curve.append({
                "time_ms":     int(p.time_ms),
                "equity":      _num(p.equity),
                "open_profit": _num(p.open_profit),
            })

    order_events = []
    for i in range(int(report.order_events_len)):
        e = report.order_events[i]
        text = lambda p: p.decode("utf-8", "replace") if p else ""
        order_events.append({
            "transition_sequence": int(e.transition_sequence),
            "command_revision_id": int(e.command_revision_id),
            "order_leg_id": int(e.order_leg_id),
            "priority_sequence": int(e.priority_sequence),
            "fill_id": int(e.fill_id),
            "entry_lot_id": int(e.entry_lot_id),
            "position_episode_id": int(e.position_episode_id),
            "event_timestamp": int(e.event_timestamp),
            "event_sequence": int(e.event_sequence),
            "script_bar_index": int(e.script_bar_index),
            "command_kind": int(e.command_kind), "leg_kind": int(e.leg_kind),
            "state_before": int(e.state_before), "state_after": int(e.state_after),
            "transition": int(e.transition), "reason": int(e.reason),
            "id": text(e.id), "from_entry": text(e.from_entry),
            "oca_name": text(e.oca_name),
            "filled_quantity": _num(e.filled_quantity),
            "observed_price": _num(e.observed_price),
            "fill_price": _num(e.fill_price),
        })

    return {
        "engine": "pineforge",
        "input": {
            "ohlcv":      str(ohlcv_path),
            "bars":       n_bars,
            "first_ts":   int(first_ts),
            "last_ts":    int(last_ts),
            "first_time": fmt_utc(first_ts),
            "last_time":  fmt_utc(last_ts),
        },
        "applied_inputs":    applied_inputs,
        "applied_overrides": applied_overrides,
        "applied_runtime":   applied_runtime or {},
        "elapsed_seconds":   round(elapsed, 4),
        "summary": {
            "total_trades":   n,
            "wins":           wins,
            "losses":         losses,
            "win_rate_pct":   round((wins / n * 100.0) if n else 0.0, 4),
            "net_pnl":        float(report.net_profit),
            "avg_trade":      (float(report.net_profit) / n) if n else 0.0,
            "best_trade":     max(pnls) if pnls else 0.0,
            "worst_trade":    min(pnls) if pnls else 0.0,
            "max_drawdown":   max_dd,
            "bars_processed": int(report.input_bars_processed),
        },
        "diagnostics": {
            "input_bars_processed":         int(report.input_bars_processed),
            "script_bars_processed":        int(report.script_bars_processed),
            "magnifier_sub_bars_total":     int(report.magnifier_sub_bars_total),
            "magnifier_sample_ticks_total": int(report.magnifier_sample_ticks_total),
            "bar_magnifier_enabled":        bool(report.bar_magnifier_enabled),
            "order_event_count":            int(report.order_event_count),
            "order_event_hash":             f"{int(report.order_event_hash):016x}",
            "order_event_dropped":          int(report.order_event_dropped),
        },
        "trades": trades,
        "metrics": metrics,
        "equity_curve": equity_curve,
        "order_events": order_events,
    }


def parse_kv_json(s: str | None, label: str) -> dict[str, str]:
    """Parse a JSON object of {key: value} into a {str: str} map.
    Empty / None / "{}" → {}. Non-object payloads abort with a clear
    error so junk env vars don't silently noop."""
    if not s or s.strip() in ("", "{}"):
        return {}
    try:
        obj = json.loads(s)
    except json.JSONDecodeError as e:
        sys.exit(f"error: {label} is not valid JSON: {e}")
    if not isinstance(obj, dict):
        sys.exit(f"error: {label} must be a JSON object, got {type(obj).__name__}")
    return {str(k): str(v) for k, v in obj.items()}


# MagnifierDistribution enum values mirror include/pineforge/magnifier.hpp.
MAGNIFIER_DISTS = {
    "uniform":      0,
    "cosine":       1,
    "triangle":     2,
    "endpoints":    3,
    "front_loaded": 4,
    "back_loaded":  5,
}


def parse_magnifier_dist(s: str) -> int:
    if not s:
        return 3
    key = s.strip().lower()
    if key in MAGNIFIER_DISTS:
        return MAGNIFIER_DISTS[key]
    if key.isdigit() and 0 <= int(key) <= 5:
        return int(key)
    sys.exit(
        f"error: --magnifier-dist must be one of "
        f"{sorted(MAGNIFIER_DISTS)} or 0-5, got {s!r}"
    )


def parse_bool(s: str) -> bool:
    return s.strip().lower() in ("1", "true", "yes", "on")


def _timing_block(samples_ns, *, warmup, repeats, bar_magnifier,
                  magnifier_samples, magnifier_dist, volume_weighted) -> dict:
    """diagnostics.timing payload from raw per-repeat run_backtest_full samples.
    Pure (no engine handle) so the --bench contract is unit-testable; consumed
    by benchmarks/speed/time_pineforge_docker.py."""
    return {
        "mode": "run_backtest_full",
        "warmup": int(warmup),
        "repeats": int(repeats),
        "samples_ns": list(samples_ns),
        "magnifier": {
            "enabled": bool(bar_magnifier),
            "samples": magnifier_samples,
            "dist": magnifier_dist,
            "volume_weighted": volume_weighted,
        },
    }


def _throughput_block(items_processed, samples_ns, *, bar_magnifier) -> dict:
    """diagnostics.throughput payload. magnifier_mode mirrors the GBench
    benchmark split (with_magnifier vs no_magnifier); consumed by
    benchmarks/throughput/time_throughput_docker.py."""
    return {
        "items_processed": int(items_processed),
        "samples_ns": list(samples_ns),
        "magnifier_mode": "with_magnifier" if bar_magnifier else "no_magnifier",
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so",        type=Path, required=True, help="strategy.so path")
    ap.add_argument("--ohlcv",     type=Path, required=True, help="OHLCV CSV path")
    ap.add_argument("--inputs",    default="",
                    help='JSON object overriding input.*() values, e.g. \'{"Fast Length": "8"}\'')
    ap.add_argument("--overrides", default="",
                    help='JSON object overriding strategy() header, e.g. \'{"default_qty_value": "5"}\'')
    ap.add_argument("--input-tf", default="",
                    help="Chart bar timeframe (e.g. '1', '5', '15', '60', 'D'). "
                         "Empty = auto-detect from bar timestamps.")
    ap.add_argument("--script-tf", default="",
                    help="Strategy timeframe. Empty = same as input_tf. "
                         "Must be coarser than or equal to input_tf; the engine "
                         "rejects finer values via strategy_get_last_error.")
    ap.add_argument("--bar-magnifier", default="",
                    help="Enable intra-bar price-path sampling for stop/limit fills "
                         "(true/false, default false).")
    ap.add_argument("--magnifier-samples", type=int, default=4,
                    help="Sub-bar sample count when --bar-magnifier=true (default 4).")
    ap.add_argument("--magnifier-dist", default="endpoints",
                    help="Sample distribution: uniform, cosine, triangle, "
                         "endpoints (default), front_loaded, back_loaded.")
    ap.add_argument("--generated-cpp", type=Path, default=None,
                    help="Path to the compiled generated.cpp; hashed and parsed "
                         "for the report fingerprint (strategy()/input() provenance).")
    ap.add_argument("--transpiled", default="",
                    help="'true' if generated.cpp came from a .pine transpile this "
                         "run, 'false' if a user-supplied .cpp. Recorded in the "
                         "fingerprint as codegen.transpiled_from_pine.")
    ap.add_argument("--syminfo", type=Path, default=None,
                    help="syminfo.json to apply via strategy_set_syminfo_*")
    ap.add_argument("--trade-start-ms", type=int, default=None,
                    help="Suppress order execution before this unix-ms timestamp "
                         "(strategy_set_trade_start_time). Mirrors the validation "
                         "harness tv-window gate. Unset = no gate.")
    ap.add_argument("--chart-tz", default="",
                    help="IANA timezone for Pine date builtins (hour/minute/dayofweek) "
                         "+ intraday-cap rollover (strategy_set_chart_timezone). "
                         "Empty = engine UTC fast path.")
    ap.add_argument("--magnifier-volume-weighted", action="store_true",
                    help="Volume-weighted bar-magnifier sub-bar sampling; only effective "
                         "with --bar-magnifier (strategy_set_magnifier_volume_weighted).")
    ap.add_argument("--bench", action="store_true",
                    help="Timing mode: warm up, then time ONLY run_backtest_full over N "
                         "repeats; emit diagnostics.timing.samples_ns + diagnostics.throughput. "
                         "Raw samples only — no median/ratio is computed in the image.")
    ap.add_argument("--warmup", type=int, default=3, help="Bench warmup runs (default 3).")
    ap.add_argument("--repeats", type=int, default=20, help="Bench timed repeats (default 20).")
    args = ap.parse_args()

    inputs    = parse_kv_json(args.inputs,    "--inputs")
    overrides = parse_kv_json(args.overrides, "--overrides")
    input_tf  = args.input_tf.strip().encode()
    script_tf = args.script_tf.strip().encode()
    bar_magnifier = 1 if parse_bool(args.bar_magnifier) else 0
    magnifier_samples = max(2, int(args.magnifier_samples))
    magnifier_dist = parse_magnifier_dist(args.magnifier_dist)

    bars, n = load_bars(args.ohlcv)
    first_ts, last_ts = bars[0].timestamp, bars[n - 1].timestamp

    lib = load_strategy(args.so)

    # Volume-weighted magnifier only meaningful when the magnifier is on.
    vw_on = bool(args.magnifier_volume_weighted) and bar_magnifier == 1

    def _make_state():
        """Create + fully configure a fresh strategy state — everything EXCEPT the
        timed run_backtest_full call. Mirrors scripts/run_strategy.py's setup so the
        engine behaves identically to the ctypes validation harness."""
        st = lib.strategy_create(b"{}")
        for k, v in inputs.items():
            lib.strategy_set_input(st, k.encode(), v.encode())
        for k, v in overrides.items():
            lib.strategy_set_override(st, k.encode(), v.encode())
        if args.syminfo:
            apply_syminfo(lib, st, args.syminfo)
        if args.trade_start_ms is not None and hasattr(lib, "strategy_set_trade_start_time"):
            lib.strategy_set_trade_start_time(st, int(args.trade_start_ms))
        if args.chart_tz and hasattr(lib, "strategy_set_chart_timezone"):
            lib.strategy_set_chart_timezone(st, args.chart_tz.encode())
        if vw_on and hasattr(lib, "strategy_set_magnifier_volume_weighted"):
            lib.strategy_set_magnifier_volume_weighted(st, 1)
        return st

    def _run(st, rep):
        lib.run_backtest_full(
            st, bars, n,
            input_tf, script_tf,
            bar_magnifier, magnifier_samples, magnifier_dist,
            ctypes.byref(rep),
        )

    # --- Bench mode: warm up, then time ONLY run_backtest_full over N repeats. ---
    # Setup (create/set_input/free) is OUTSIDE the timed region so the sample
    # isolates the engine hot loop (closest to the GBench harness). dlopen
    # already happened above (load_strategy), outside any loop.
    timing = None
    if args.bench:
        warmup = max(0, int(args.warmup))
        repeats = max(1, int(args.repeats))
        for _ in range(warmup):
            st = _make_state(); rep = ReportC()
            try:
                _run(st, rep)
            finally:
                lib.report_free(ctypes.byref(rep)); lib.strategy_free(st)
        samples_ns: list[int] = []
        for _ in range(repeats):
            st = _make_state(); rep = ReportC()
            try:
                t0 = time.perf_counter_ns(); _run(st, rep); t1 = time.perf_counter_ns()
                samples_ns.append(t1 - t0)
            finally:
                lib.report_free(ctypes.byref(rep)); lib.strategy_free(st)
        timing = _timing_block(
            samples_ns, warmup=warmup, repeats=repeats,
            bar_magnifier=bar_magnifier, magnifier_samples=magnifier_samples,
            magnifier_dist=args.magnifier_dist.strip().lower() or "endpoints",
            volume_weighted=vw_on)

    # --- Body run: one configured run for trades / metrics / diagnostics. ---
    state = _make_state()
    report = ReportC()
    started = time.time()
    try:
        _run(state, report)
        elapsed = time.time() - started
        err_msg = ""
        if hasattr(lib, "strategy_get_last_error"):
            err_ptr = lib.strategy_get_last_error(state)
            err_msg = err_ptr.decode("utf-8", "replace") if err_ptr else ""
        if err_msg:
            json.dump({"engine": "pineforge", "error": err_msg},
                      sys.stdout, separators=(",", ":"))
            sys.stdout.write("\n")
            return 1
        applied_runtime = {
            "input_tf":           input_tf.decode() if input_tf else "",
            "script_tf":          script_tf.decode() if script_tf else "",
            "input_tf_seconds":   int(report.input_tf_seconds),
            "script_tf_seconds":  int(report.script_tf_seconds),
            "script_tf_ratio":    int(report.script_tf_ratio),
            "needs_aggregation":  bool(report.needs_aggregation),
            "bar_magnifier":      bool(bar_magnifier),
            "magnifier_samples":  magnifier_samples,
            "magnifier_dist":     args.magnifier_dist.strip().lower() or "endpoints",
            "magnifier_volume_weighted": vw_on,
            "trade_start_ms":     args.trade_start_ms,
            "chart_tz":           args.chart_tz or "",
        }
        out = build_report_dict(report, args.ohlcv, n, first_ts, last_ts,
                                elapsed, inputs, overrides, applied_runtime)
        if timing is not None:
            out["diagnostics"]["timing"] = timing
            out["diagnostics"]["throughput"] = _throughput_block(
                report.input_bars_processed, timing["samples_ns"],
                bar_magnifier=bar_magnifier)
        try:
            out["fingerprint"] = build_fingerprint(build_provenance(
                engine_version(lib),
                args.generated_cpp,
                parse_bool(args.transpiled),
                inputs,
                overrides,
                applied_runtime,
            ))
        except Exception:
            out["fingerprint"] = None
        json.dump(out, sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
