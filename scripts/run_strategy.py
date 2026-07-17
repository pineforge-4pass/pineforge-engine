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
        --ohlcv corpus/data/derived/ohlcv_ETH-USDT-USDT_15m_window.csv

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
import base64
import csv
import ctypes
import hashlib
import json
import os
import re
import struct
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
# The corpus ships a single committed feed (full-history 1m, Git LFS);
# the 15m chart feeds are derived from it locally. ensure_derived() is
# called from main() — importing this module stays side-effect free for
# consumers that only want the ABI mirrors.
from derive_corpus_feeds import (  # noqa: E402
    DERIVED_15M, DERIVED_15M_WINDOW, ensure_derived)
REFERENCE_OHLCV = DERIVED_15M_WINDOW
WARMUP_OHLCV = DERIVED_15M
DEFAULT_OHLCV = WARMUP_OHLCV

# Keys in inputs.json that are validator/harness metadata, not Pine input()
# values. Mirrors the canonical validator's VALIDATION_INPUT_META_KEYS so
# they are not forwarded to ``strategy_set_input`` (which would either
# silently no-op or pollute the strategy's input table).
_VALIDATION_META_KEYS = frozenset({
    "tv_trades_csv_tz",
    "tv_trades_csv",
    "runtime_overrides",
    "strategy_overrides",
    "validation_overrides",
    "ohlcv_csv",
    "ohlcv_start_ms",
    "script_tf",
    "input_tf",
    "chart_timezone",
})

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

# Canonical primary-feed identity. Hash the numeric BarC values in source-row
# order, before any validation-only start/end slicing. The domain prefix makes
# the byte contract versioned and prevents cross-domain hash reuse.
SOURCE_FEED_CANONICALIZATION = "pf-ohlcv-barc-le-v1"
_SOURCE_FEED_HASH_PREFIX = b"pineforge:ohlcv:barc-le:v1\0"
_SOURCE_FEED_RECORD = struct.Struct("<5dq")


def _new_source_feed_hasher():
    h = hashlib.sha256()
    h.update(_SOURCE_FEED_HASH_PREFIX)
    return h


def _update_source_feed_hash(h, row) -> None:
    h.update(_SOURCE_FEED_RECORD.pack(*row))


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
                     runtime: dict | None, *, source_feed_sha256: str) -> dict:
    if not isinstance(source_feed_sha256, str) or not re.fullmatch(
            r"[0-9a-f]{64}", source_feed_sha256):
        raise ValueError("source_feed_sha256 must be a lowercase SHA-256 hex digest")
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
        "feed": {
            "canonicalization": SOURCE_FEED_CANONICALIZATION,
            "source_values_sha256": source_feed_sha256,
        },
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


def build_runtime_provenance(run_kwargs: dict, trade_start_ms: int | None) -> dict:
    """Return the effective runtime settings that participate in a fingerprint."""
    return {
        "input_tf": run_kwargs.get("input_tf") or "",
        "script_tf": run_kwargs.get("script_tf") or "",
        "bar_magnifier": bool(run_kwargs.get("bar_magnifier")),
        "magnifier_samples": int(run_kwargs.get("magnifier_samples") or 4),
        "magnifier_distribution": run_kwargs.get("magnifier_distribution") or "ENDPOINTS",
        "chart_timezone": run_kwargs.get("chart_timezone") or "",
        "trade_start_ms": None if trade_start_ms is None else int(trade_start_ms),
    }


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


class TradeTickC(ctypes.Structure):
    """Mirror of pf_trade_tick_t for historical -> realtime handoff."""
    _fields_ = [
        ("timestamp", ctypes.c_int64),
        ("sequence", ctypes.c_uint64),
        ("price", ctypes.c_double),
        ("quantity", ctypes.c_double),
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
        ("commission", ctypes.c_double),
        ("entry_bar_index", ctypes.c_int32),
        ("exit_bar_index", ctypes.c_int32),
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
        ("metrics", MetricsC),
        ("equity_curve", ctypes.POINTER(EquityPointC)),
        ("equity_curve_len", ctypes.c_int64),  # int64 in the C header, NOT c_int
    ]


# ABI version this harness mirrors (PF_ABI_VERSION in pineforge.h).
# pf_report_t is CALLER-allocated: running an old .so against the v2
# ReportC mirror (or vice versa) silently corrupts memory, so the .so's
# pf_abi_version() export is asserted before any run.
EXPECTED_PF_ABI = 2


def _check_abi(lib: ctypes.CDLL) -> None:
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


# --- Strategy harness --------------------------------------------------

def find_strategy_lib(strategy_dir: Path, so_name: str = "strategy.so") -> Path:
    """Resolve the compiled strategy library inside ``strategy_dir``.

    Prefers ``so_name`` when present, otherwise falls back to the
    platform-specific alternatives (.dylib / .so / .dll)."""
    so_path = strategy_dir / so_name
    if not so_path.exists():
        for alt in ("strategy.dylib", "strategy.so", "strategy.dll"):
            cand = strategy_dir / alt
            if cand.exists():
                return cand
    return so_path


def inputs_run_kwargs(params, strategy_dir: Path, default_ohlcv: Path,
                      default_chart_tz: str = "") -> tuple[Path, dict]:
    """Resolve per-probe ``inputs.json`` metadata into the OHLCV path and the
    keyword arguments for :meth:`Strategy.run`.

    This is the single source of truth for how harnesses honour
    ``ohlcv_csv`` / ``input_tf`` / ``script_tf`` / ``ohlcv_start_ms`` /
    ``chart_timezone`` / ``runtime_overrides`` — ``main()`` below and
    ``scripts/crossvalidate_metrics.py`` both consume it, so a probe that
    runs under one harness runs identically under the other. ``params``
    itself must still be passed to ``Strategy.run(params=...)`` so Pine
    ``input()`` values reach ``strategy_set_input``.
    """
    if not isinstance(params, dict):
        params = {}
    # Per-probe inputs.json::ohlcv_csv override wins over the caller default.
    if "ohlcv_csv" in params:
        _csv_val = str(params["ohlcv_csv"])
        ohlcv_path = (Path(_csv_val) if _csv_val.startswith("/")
                      else (strategy_dir / _csv_val)).resolve()
    else:
        ohlcv_path = Path(default_ohlcv).resolve()
    # Per-probe chart tz override wins over the caller default. Empty
    # string is honoured (engine UTC fast path).
    if "chart_timezone" in params:
        chart_tz = str(params.get("chart_timezone") or "")
    else:
        chart_tz = default_chart_tz or ""
    ohlcv_start_ms: int | None = None
    if "ohlcv_start_ms" in params:
        try:
            ohlcv_start_ms = int(params["ohlcv_start_ms"])
        except (TypeError, ValueError):
            ohlcv_start_ms = None
    runtime_overrides = params.get("runtime_overrides") or {}
    if not isinstance(runtime_overrides, dict):
        runtime_overrides = {}
    strategy_overrides = params.get("strategy_overrides") or {}
    if not isinstance(strategy_overrides, dict):
        strategy_overrides = {}
    try:
        magnifier_samples = int(runtime_overrides.get("magnifier_samples", 4) or 4)
    except (TypeError, ValueError):
        magnifier_samples = 4
    syminfo_metadata = runtime_overrides.get("syminfo_metadata")
    if not isinstance(syminfo_metadata, dict):
        syminfo_metadata = None

    def _num(v):
        try:
            return float(v)
        except (TypeError, ValueError):
            return None

    # Per-instrument forced-liquidation lot step. Accept it either as a
    # top-level runtime_overrides.qty_step or inside syminfo_metadata, and
    # route it through the existing syminfo_metadata channel (key "qty_step")
    # so the engine quantizes margin-call lots without a new C-ABI export.
    qty_step = _num(runtime_overrides.get("qty_step"))
    if qty_step is None and isinstance(syminfo_metadata, dict):
        qty_step = _num(syminfo_metadata.get("qty_step"))
    if qty_step is not None and qty_step > 0.0:
        if not isinstance(syminfo_metadata, dict):
            syminfo_metadata = {}
        syminfo_metadata = dict(syminfo_metadata)
        syminfo_metadata["qty_step"] = qty_step

    kwargs = dict(
        strategy_overrides=strategy_overrides or None,
        chart_timezone=chart_tz,
        syminfo_timezone=str(runtime_overrides.get("timezone") or "") or None,
        syminfo_session=str(runtime_overrides.get("session") or "") or None,
        syminfo_metadata=syminfo_metadata,
        syminfo_mintick=_num(runtime_overrides.get("mintick")),
        syminfo_pointvalue=_num(runtime_overrides.get("pointvalue")),
        input_tf=str(params.get("input_tf") or "") or None,
        script_tf=str(params.get("script_tf") or "") or None,
        ohlcv_start_ms=ohlcv_start_ms,
        bar_magnifier=bool(runtime_overrides.get("bar_magnifier", False)),
        magnifier_samples=magnifier_samples,
        magnifier_distribution=str(runtime_overrides.get(
            "magnifier_distribution", "ENDPOINTS") or "ENDPOINTS"),
        magnifier_volume_weighted=bool(
            runtime_overrides.get("magnifier_volume_weighted", False)),
    )
    return ohlcv_path, kwargs


class Strategy:
    """Thin ctypes wrapper around one strategy.so."""

    def __init__(self, so_path: Path):
        if not so_path.exists():
            raise FileNotFoundError(
                f"strategy library not found: {so_path}\n"
                f"hint: run `cmake --build build --target corpus_strategies` first"
            )
        self.lib = ctypes.CDLL(str(so_path))
        _check_abi(self.lib)
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
        if hasattr(L, "strategy_stream_begin"):
            L.strategy_stream_begin.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
                ctypes.c_char_p, ctypes.c_char_p]
            L.strategy_stream_begin.restype = ctypes.c_int
            L.strategy_stream_push_tick.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(TradeTickC)]
            L.strategy_stream_push_tick.restype = ctypes.c_int
            L.strategy_stream_push_ticks.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(TradeTickC), ctypes.c_int]
            L.strategy_stream_push_ticks.restype = ctypes.c_int
            L.strategy_stream_advance_time.argtypes = [
                ctypes.c_void_p, ctypes.c_int64]
            L.strategy_stream_advance_time.restype = ctypes.c_int
            L.strategy_stream_end.argtypes = [ctypes.c_void_p, ctypes.c_int]
            L.strategy_stream_end.restype = ctypes.c_int
            L.strategy_stream_fill_report.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(ReportC)]
            L.strategy_stream_fill_report.restype = ctypes.c_int
        if hasattr(L, "strategy_set_input"):
            L.strategy_set_input.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        if hasattr(L, "strategy_set_override"):
            L.strategy_set_override.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
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
        if hasattr(L, "strategy_set_syminfo_mintick"):
            L.strategy_set_syminfo_mintick.argtypes = [ctypes.c_void_p, ctypes.c_double]
            L.strategy_set_syminfo_mintick.restype = None
        if hasattr(L, "strategy_set_syminfo_pointvalue"):
            L.strategy_set_syminfo_pointvalue.argtypes = [ctypes.c_void_p, ctypes.c_double]
            L.strategy_set_syminfo_pointvalue.restype = None
        if hasattr(L, "pf_version_get"):
            L.pf_version_get.restype = PfVersionC
        if hasattr(L, "pf_version_string"):
            L.pf_version_string.restype = ctypes.c_char_p

    def run(self, bars_csv: Path, params: dict | None = None,
            *, trace_enabled: bool = False, trade_start_time_ms: int | None = None,
            strategy_overrides: dict | None = None,
            chart_timezone: str | None = None,
            syminfo_timezone: str | None = None,
            syminfo_session: str | None = None,
            syminfo_metadata: dict | None = None,
            syminfo_mintick: float | None = None,
            syminfo_pointvalue: float | None = None,
            input_tf: str | None = None, script_tf: str | None = None,
            ohlcv_start_ms: int | None = None,
            ohlcv_end_ms: int | None = None,
            bar_magnifier: bool = False,
            magnifier_samples: int = 4,
            magnifier_distribution: str = "ENDPOINTS",
            magnifier_volume_weighted: bool = False,
            preloaded_bars: "tuple | None" = None,
            on_report=None) -> dict:
        """Read OHLCV from CSV, drive the engine, return a report dict.

        ``preloaded_bars`` (a ``(BarC[], n)`` tuple, already trimmed) lets a
        batch driver parse the shared feed ONCE and reuse it across every
        strategy/candidate — skipping the per-run CSV parse + ctypes array build
        that otherwise dominates wall time. When given, ``bars_csv`` /
        ``ohlcv_start_ms`` / ``ohlcv_end_ms`` are ignored (the caller has already
        sliced). Behaviour is otherwise identical to the CSV path.

        ``ohlcv_start_ms`` (when provided) trims the loaded OHLCV so the
        engine's first bar is at-or-after that timestamp. This is required
        for probes that pin matrix/array warmup depth to the user's TV
        chart history (e.g. ``var matrix<bool>`` accumulators, where
        feeding the full 6-month warmup CSV pre-fills the mask before the
        comparison window begins and the entry gate becomes a no-op).
        Mirrors the per-probe ``inputs.json::ohlcv_start_ms`` metadata
        the validator already honours.

        ``ohlcv_end_ms`` symmetrically drops bars after that timestamp
        (inclusive bound), letting validation tooling test alternate
        spans without a trimmed CSV copy.

        CSV-backed runs also return ``source_feed_sha256``: the canonical
        identity of the full parsed source tape before either bound is applied.
        Preloaded-bar callers retain their established return shape and do not
        receive an inferred identity for an already-sliced buffer.

        ``on_report`` (when given) is invoked with the live ``ReportC``
        after the engine-error check and BEFORE ``report_free``, so
        callers can read report fields the summary dict does not carry
        (``metrics.equity``, the raw ``equity_curve``, ...).
        """
        source_feed_sha256 = None
        if preloaded_bars is not None:
            # Preserve the established (bars, n) contract. A future
            # fingerprinting caller using preloaded bars must supply the
            # source-tape identity through an explicit API rather than hashing
            # an already-sliced subset here.
            bars, n = preloaded_bars
        else:
            bars, n, source_feed_sha256 = _load_bars(
                bars_csv, ohlcv_start_ms=ohlcv_start_ms,
                ohlcv_end_ms=ohlcv_end_ms)
        params = params or {}
        params_json = json.dumps(params).encode()

        state = self.lib.strategy_create(params_json)
        report = ReportC()
        try:
            # Strategy-property overrides (commission/slippage/pyramiding/...)
            # via the per-strategy ``strategy_set_override`` export. Keys and
            # value grammar: codegen emit_top.py set_strategy_override —
            # e.g. commission_type "percent"/"cash_per_order"/"cash_per_contract",
            # commission_value/initial_capital/default_qty_value as decimals,
            # slippage/pyramiding as ints. Applied before any input/run call.
            if strategy_overrides and hasattr(self.lib, "strategy_set_override"):
                for okey, oval in strategy_overrides.items():
                    self.lib.strategy_set_override(
                        state, str(okey).encode(), str(oval).encode())
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
            if syminfo_mintick is not None and hasattr(self.lib, "strategy_set_syminfo_mintick"):
                self.lib.strategy_set_syminfo_mintick(state, float(syminfo_mintick))
            if syminfo_pointvalue is not None and hasattr(self.lib, "strategy_set_syminfo_pointvalue"):
                self.lib.strategy_set_syminfo_pointvalue(state, float(syminfo_pointvalue))
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
            if on_report is not None:
                on_report(report)
            result = _report_to_dict(report)
            if source_feed_sha256 is not None:
                result["source_feed_sha256"] = source_feed_sha256
            return result
        finally:
            self.lib.report_free(ctypes.byref(report))
            self.lib.strategy_free(state)


def _load_bars(csv_path: Path, *, ohlcv_start_ms: int | None = None,
               ohlcv_end_ms: int | None = None) -> tuple[ctypes.Array, int, str]:
    """Read OHLCV CSV (timestamp, open, high, low, close, volume) into BarC[].

    When ``ohlcv_start_ms`` is given, drop bars whose timestamp is below
    that bound. Used by probes that pin warmup depth to the user's TV
    chart history (per-probe ``inputs.json::ohlcv_start_ms`` metadata).
    ``ohlcv_end_ms`` symmetrically drops bars whose timestamp is above
    that bound (inclusive keep).

    Returns ``(effective_bars, effective_count, source_feed_sha256)``. The hash
    covers every parsed source row before either bound is applied.
    """
    # Vectorized load: parsing the CSV and building the BarC[] with a per-bar
    # Python/ctypes loop is ~99% of a run's wall time (the C++ backtest itself is
    # ~20ms at ~27M bar/s). numpy parses the whole feed at once and BarC[] shares
    # its exact memory layout (5x float64 + 1x int64, no padding), so a single
    # from_buffer_copy builds the array with zero Python-level per-bar work
    # (measured: 1m feed 4.93s -> 0.62s, byte-identical output). Falls back to the
    # explicit loop if numpy is unavailable.
    try:
        import numpy as _np
    except ImportError:
        _np = None
    if _np is not None:
        with csv_path.open(newline="", encoding="utf-8") as f:
            header = f.readline().strip().split(",")
        col = {name: header.index(name)
               for name in ("open", "high", "low", "close", "volume", "timestamp")}
        a = _np.loadtxt(csv_path, delimiter=",", skiprows=1, ndmin=2)
        source_n = len(a)
        dt = _np.dtype([("open", "<f8"), ("high", "<f8"), ("low", "<f8"),
                        ("close", "<f8"), ("volume", "<f8"), ("timestamp", "<i8")])
        if dt.itemsize != _SOURCE_FEED_RECORD.size:  # pragma: no cover - invariant
            raise RuntimeError("canonical OHLCV dtype does not match <5dq>")
        source_rows = _np.empty(source_n, dtype=dt)
        for name in ("open", "high", "low", "close", "volume"):
            source_rows[name] = a[:, col[name]] if source_n else []
        source_rows["timestamp"] = (
            a[:, col["timestamp"]].astype("<i8") if source_n else [])
        feed_hasher = _new_source_feed_hasher()
        feed_hasher.update(memoryview(source_rows))
        source_feed_sha256 = feed_hasher.hexdigest()

        rows = source_rows
        if a.size and (ohlcv_start_ms is not None or ohlcv_end_ms is not None):
            ts = a[:, col["timestamp"]]
            keep = _np.ones(len(a), dtype=bool)
            if ohlcv_start_ms is not None:
                keep &= ts >= ohlcv_start_ms
            if ohlcv_end_ms is not None:
                keep &= ts <= ohlcv_end_ms
            rows = source_rows[keep]
        n = len(rows)
        bars = (BarC * n).from_buffer_copy(rows.tobytes()) if n else (BarC * 0)()
        return bars, n, source_feed_sha256
    # Fallback (no numpy): explicit parse + per-bar build.
    rows: list[tuple[float, float, float, float, float, int]] = []
    feed_hasher = _new_source_feed_hasher()
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ts = int(row["timestamp"])
            parsed = (float(row["open"]), float(row["high"]), float(row["low"]),
                      float(row["close"]), float(row["volume"]), ts)
            _update_source_feed_hash(feed_hasher, parsed)
            if ohlcv_start_ms is not None and ts < ohlcv_start_ms:
                continue
            if ohlcv_end_ms is not None and ts > ohlcv_end_ms:
                continue
            rows.append(parsed)
    n = len(rows)
    bars = (BarC * n)()
    for i, (o, h, l, c, v, ts) in enumerate(rows):
        bars[i].open = o
        bars[i].high = h
        bars[i].low = l
        bars[i].close = c
        bars[i].volume = v
        bars[i].timestamp = ts
    return bars, n, feed_hasher.hexdigest()


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
            "commission": float(t.commission),
            "entry_bar_index": int(t.entry_bar_index),
            "exit_bar_index": int(t.exit_bar_index),
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
    order — byte-for-byte alignable with TradingView's `trades.csv` export.

    Excursion columns use TradingView's export convention: favorable excursion
    is a non-negative total-USD run-up, adverse excursion is emitted as a
    NEGATIVE total-USD drawdown (TV exports e.g. -8579.36). The engine ABI's
    `max_drawdown` stays positive — that mirrors Pine's
    `strategy.*trades.max_drawdown` accessors — so the sign flip happens only
    here, in the TV-compatible export representation."""
    cum_pnls: dict[int, float] = {}
    running = 0.0
    for n, t in enumerate(trades, 1):
        running += t["pnl"]
        cum_pnls[n] = running

    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "Trade #", "Type", "Date and time", "Price", "Qty",
            "Net PnL", "Net PnL %",
            "Favorable excursion USD", "Adverse excursion USD",
            "Cumulative PnL",
        ])
        for n, t in reversed(list(enumerate(trades, 1))):
            direction = "long" if t["is_long"] else "short"
            cum = cum_pnls[n]
            # TV convention: adverse excursion is negative (or zero).
            adverse = -t["max_drawdown"] or 0.0
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
                    f"{adverse:.6f}",
                    f"{cum:.6f}",
                ])



def _tv_tzinfo(meta: dict):
    """Resolve the TV export tz to a tzinfo. Fixed aliases (utc/utc_plus_8/
    asia_taipei) OR any IANA name (DST-aware, e.g. America/New_York) so the
    emit-window for a DST-bearing exchange is computed correctly."""
    from datetime import timedelta
    name = str(meta.get("tv_trades_csv_tz", "")).strip()
    low = name.lower()
    fixed = {"utc_plus_8": 8, "asia_taipei": 8, "utc": 0}
    if low in fixed:
        return timezone(timedelta(hours=fixed[low]))
    if "/" in name:
        try:
            from zoneinfo import ZoneInfo
            return ZoneInfo(name)
        except Exception:
            pass
    return timezone(timedelta(hours=8))


def _parse_trade_dt(s: str, tz) -> int:
    return int(datetime.strptime(s, "%Y-%m-%d %H:%M").replace(tzinfo=tz).timestamp() * 1000)


def _load_tv_entry_window(strategy_dir: Path, meta: dict, bar_interval_ms: int) -> tuple[int, int] | None:
    tv_name = str(meta.get("tv_trades_csv", "tv_trades.csv"))
    tv_path = strategy_dir / tv_name
    if not tv_path.exists():
        return None
    tz_offset = _tv_tzinfo(meta)
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

# --- docker runner (pineforge-release image) ---------------------------

def _run_via_docker(strategy_dir: Path, ohlcv_path: Path, params: dict,
                    run_kwargs: dict, trade_start_ms, image) -> dict:
    """Run the COMMITTED generated.cpp through the pineforge-release container and
    return a report dict shaped like Strategy.run / _report_to_dict, so the
    host-side emit-window filter + write_engine_trades_csv work UNCHANGED.

    Mirrors Strategy.run's engine setup exactly: inputs filter, strategy_overrides,
    input_tf/script_tf, magnifier (incl. the VOLUME_WEIGHTED->ENDPOINTS+vw mapping),
    trade_start_time, chart_timezone, syminfo. ohlcv_start_ms is applied by
    pre-trimming the CSV fed to the container (the ctypes path trims in _load_bars).
    NEVER re-transpiles .pine (uses the committed cpp, no codegen variance)."""
    import pf_release_run as _rel
    generated_cpp = strategy_dir / "generated.cpp"
    if not generated_cpp.exists():
        raise FileNotFoundError(f"generated.cpp not found for --runner docker: {generated_cpp}")

    # ohlcv_start_ms: ctypes trims bars in _load_bars; the container needs an
    # already-trimmed CSV (M8 — feed the same trimmed feed downstream).
    ohlcv_for_image = ohlcv_path
    tmp_ohlcv = None
    start_ms = run_kwargs.get("ohlcv_start_ms")
    if start_ms is not None:
        import tempfile
        fd, p = tempfile.mkstemp(suffix=".csv", prefix="pf_ohlcv_")
        tmp_ohlcv = Path(p)
        with ohlcv_path.open(newline="", encoding="utf-8") as fin, \
                os.fdopen(fd, "w", newline="") as fout:
            r = csv.DictReader(fin)
            w = csv.DictWriter(fout, fieldnames=r.fieldnames)
            w.writeheader()
            for row in r:
                if int(row["timestamp"]) >= int(start_ms):
                    w.writerow(row)
        ohlcv_for_image = tmp_ohlcv

    # Magnifier dist/vw: mirror Strategy.run. 'VOLUME_WEIGHTED' is not a geometric
    # distribution — fall back to ENDPOINTS for the t-grid AND toggle vw.
    dist = str(run_kwargs.get("magnifier_distribution") or "ENDPOINTS")
    vw = bool(run_kwargs.get("magnifier_volume_weighted")) or dist.upper() == "VOLUME_WEIGHTED"
    if dist.upper() == "VOLUME_WEIGHTED":
        dist = "ENDPOINTS"

    # Inputs forwarded to the engine: same filter as Strategy.run (drop tv_*/meta).
    inputs_for_image = {
        str(k): str(v) for k, v in params.items()
        if not str(k).startswith("tv_") and k not in _VALIDATION_META_KEYS
    }
    # syminfo from runtime_overrides. apply_syminfo covers mintick/pointvalue/
    # timezone/session; syminfo_metadata (fundamentals) is NOT covered — 0 corpus
    # probes use it (documented gap).
    syminfo: dict = {}
    if run_kwargs.get("syminfo_timezone"):
        syminfo["timezone"] = run_kwargs["syminfo_timezone"]
    if run_kwargs.get("syminfo_session"):
        syminfo["session"] = run_kwargs["syminfo_session"]
    if run_kwargs.get("syminfo_mintick") is not None:
        syminfo["mintick"] = run_kwargs["syminfo_mintick"]
    if run_kwargs.get("syminfo_pointvalue") is not None:
        syminfo["pointvalue"] = run_kwargs["syminfo_pointvalue"]

    kw = dict(
        inputs=inputs_for_image,
        overrides=run_kwargs.get("strategy_overrides") or {},
        input_tf=run_kwargs.get("input_tf") or "",
        script_tf=run_kwargs.get("script_tf") or "",
        bar_magnifier=bool(run_kwargs.get("bar_magnifier")),
        magnifier_samples=int(run_kwargs.get("magnifier_samples") or 4),
        magnifier_dist=dist,
        magnifier_volume_weighted=vw,
        trade_start_ms=trade_start_ms,
        chart_tz=run_kwargs.get("chart_timezone") or "",
        syminfo=syminfo or None,
    )
    if image:
        kw["image"] = image
    try:
        raw = _rel.run_release(generated_cpp, ohlcv_for_image, **kw)
    finally:
        if tmp_ohlcv is not None:
            tmp_ohlcv.unlink(missing_ok=True)
    return {
        "trades": _rel.report_trades_to_runstrategy_shape(raw),
        "net_profit": float(raw.get("summary", {}).get("net_pnl", 0.0)),
        "input_bars_processed": int(raw.get("diagnostics", {}).get("input_bars_processed", 0)),
        "trace": [],
        "trace_names": [],
    }


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
    ap.add_argument("--allow-trading-before-window", action="store_true",
                    help="When tv_trades_csv defines an emit window, keep broker order execution active before that window. "
                         "This matches TV exports that carry positions opened before the displayed date range.")
    ap.add_argument("--inputs-json", type=Path, default=None,
                    help="Use this inputs.json instead of strategy_dir/inputs.json. "
                         "Lets ad-hoc validation runs override strategy properties "
                         "(strategy_overrides block) without touching the corpus probe.")
    ap.add_argument("--chart-tz", default="",
                    help="IANA timezone the engine should use for Pine date builtins "
                         "(hour/minute/dayofweek and their 1-arg function overloads) "
                         "and intraday-cap rollover. Empty (default) keeps the engine "
                         "on its UTC fast path, matching how the corpus's TV exports "
                         "were recorded. Pass an IANA name (e.g. 'Asia/Taipei') only "
                         "for probes that genuinely need a non-UTC chart-tz. "
                         "Per-probe override: set 'chart_timezone' in inputs.json.")
    ap.add_argument("--fingerprint-json", type=Path, default=None,
                    help="Write a {token,digest,provenance} fingerprint of this "
                         "run to PATH. Off by default (keeps corpus output and "
                         "run_corpus.sh parity untouched).")
    ap.add_argument("--runner", choices=["ctypes", "docker"], default="ctypes",
                    help="Engine backend. 'ctypes' (default) loads the prebuilt "
                         "strategy.so in-process. 'docker' runs the committed "
                         "generated.cpp through the pineforge-release image (no host "
                         "C++ toolchain) — same engine_trades.csv, verified by "
                         "verify_corpus.py tolerance tiers.")
    ap.add_argument("--image", default=None,
                    help="pineforge-release image for --runner docker (default: "
                         "$PINEFORGE_RELEASE_IMAGE or ghcr .../pineforge-release:latest).")
    args = ap.parse_args()

    ensure_derived()

    strategy_dir = args.strategy_dir.resolve()
    out_path = (args.output.resolve() if args.output
                else strategy_dir / "engine_trades.csv")
    if args.no_overwrite and out_path.exists():
        print(f"SKIP (exists): {out_path}")
        return 0
    out_path.parent.mkdir(parents=True, exist_ok=True)

    started = time.time()
    inputs_path = (args.inputs_json.resolve() if args.inputs_json
                   else strategy_dir / "inputs.json")
    params = {}
    if inputs_path.exists():
        with inputs_path.open(encoding="utf-8") as f:
            params = json.load(f)
    # Per-probe inputs.json metadata (ohlcv_csv / chart_timezone /
    # input_tf / script_tf / ohlcv_start_ms / runtime_overrides) is
    # resolved by the shared helper — see inputs_run_kwargs docstring.
    # Per-probe overrides win over --ohlcv / --chart-tz CLI defaults.
    ohlcv_path, run_kwargs = inputs_run_kwargs(
        params, strategy_dir, args.ohlcv.resolve(),
        default_chart_tz=args.chart_tz or "")
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
    trade_start_ms = None
    if emit_window is not None and not args.allow_trading_before_window:
        if tv_window_used or args.disable_trading_before_window:
            trade_start_ms = emit_window[0]

    if args.runner == "docker":
        if args.trace_json is not None:
            sys.exit("error: --trace-json needs --emit-plots (deferred); not supported with --runner docker.")
        if args.fingerprint_json is not None:
            sys.exit("error: --fingerprint-json is not supported with --runner docker.")
        strat = None
        report = _run_via_docker(strategy_dir, ohlcv_path, params, run_kwargs,
                                 trade_start_ms, args.image)
    else:
        so_path = find_strategy_lib(strategy_dir, args.so_name)
        strat = Strategy(so_path)
        report = strat.run(ohlcv_path, params=params,
                           trace_enabled=args.trace_json is not None,
                           trade_start_time_ms=trade_start_ms,
                           **run_kwargs)
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
    if args.fingerprint_json is not None:
        try:
            cpp_path = strategy_dir / "generated.cpp"
            # Inputs actually forwarded to the engine (drop tv_*/validator meta keys).
            inputs_applied = {
                str(k): str(v) for k, v in params.items()
                if not str(k).startswith("tv_") and k not in _VALIDATION_META_KEYS
            }
            overrides_applied = {
                str(k): str(v) for k, v in (run_kwargs.get("strategy_overrides") or {}).items()
            }
            runtime = build_runtime_provenance(run_kwargs, trade_start_ms)
            fp = build_fingerprint(build_provenance(
                engine_version(strat.lib),
                cpp_path if cpp_path.exists() else None,
                False,  # run_strategy.py drives a prebuilt .so; no transpile this run
                inputs_applied,
                overrides_applied,
                runtime,
                source_feed_sha256=report.get("source_feed_sha256"),
            ))
            args.fingerprint_json.parent.mkdir(parents=True, exist_ok=True)
            with args.fingerprint_json.open("w", encoding="utf-8") as f:
                json.dump(fp, f, indent=2)
            print(f"  fingerprint -> {args.fingerprint_json} ({fp['digest']})")
        except Exception as e:
            print(f"  fingerprint: skipped ({e})", file=sys.stderr)
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
