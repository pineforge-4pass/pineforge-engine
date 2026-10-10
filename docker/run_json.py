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
          "exit_bar_index":  int,     # ABI v2: script-bar index of exit fill
          "open_at_end":     bool,    # ABI v3: range-end close of a position still
                                      # open after the final bar (TV accounting)
          "entry_incarnation": int    # run-scoped physical-entry provenance;
                                        # 0 when the strategy lacks the accessor
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
        "token":  "<base64(canonical provenance JSON)>",  # b64decode -> authoritative UTF-8 bytes
        "digest": "sha256:<hex of those bytes>",  # hash token bytes; do not assume json.loads round-trip
        "provenance": {
          "engine":   { version_string, major, minor, patch, commit_sha },
          "feed":     { canonicalization, source_values_sha256 },
          "codegen":  { version, generated_cpp_sha256, transpiled_from_pine },
          "strategy": { ...all strategy() params, effective... },
          "inputs":   { "<title>": { type, default, value }, ... },
          "applied":  { "inputs": {...}, "overrides": {...} },  # user deltas
          "runtime":  { ...same fields as applied_runtime... }
        }
      }
    }

applied_runtime (and so provenance.runtime) also holds
"syminfo": {"qty_step": float, "mincontract": float} when --syminfo set a lot
grid from syminfo.mincontract; without one the key is absent.

applied_runtime also holds "symbol_feeds" when --symbol-feeds installed other
symbols' bars for request.security (see load_symbol_feeds):
    {"canonicalization": "pf-symbol-feed-barc-close-le-v1",
     "symbols": {"<symbol string>": {
         "facts": {"canonical": str, "type": str, ..., "mintick": float},
         "feeds": {"<timeframe>": {"bars": int, "first_ts": int, "last_ts": int,
                                   "source_values_sha256": "<hex>"}}}}}
Without --symbol-feeds (or with an index naming no symbol) the key is absent.

--outputs reads what a library that records outputs recorded (the group
pf_outputs of pineforge.h; docs/outputs.md): the report gains "outputs",
written just before "fingerprint", and applied_runtime (and so
provenance.runtime) holds "outputs": true. See build_outputs_block:
    "outputs": {
      "schema_version":  "pineforge-outputs/v1",
      "message_format":  "pineforge/v1",
      "manifest_sha256": "<sha256 of the manifest bytes the library returned>",
      "manifest":        { ...the library's outputs manifest... },
      "bars":      {"open_ms": [int, ...], "close_ms": [int, ...]},
      "series":    [{"slot": int, "output": "<id>", "values": [number|null, ...]}, ...],
      "constants": [number|null, ...],            # one per run-constant index
      "hlines":    [{"output": "<id>", "price": number|null}, ...],
      "events":    [{"sequence": int, "output": "<id>", "bar_index": int,
                     "bar_open_ms": int, "bar_close_ms": int, "ordinal_in_bar": int,
                     "phase": "batch" | "warmup" | "realtime",
                     "value": number|null, "message": str|null,
                     "freq": str}, ...]           # freq: an alert output's events only
    }
A double that is not finite is null, as everywhere in the report; a time
equal to INT64_MIN is null; a slot or constant whose manifest encoding is
"rgba-u32" is written as an integer. --bench --outputs records in the timed
runs too. Without --outputs nothing of this is read or written.

A failed run prints one line instead, exit status 1 (2 for a command line
argparse refuses):
    {"engine":"pineforge","error":"<text>","code":"<code>","args":{...}}
That is the run's own error, a --syminfo the harness rejects (see
apply_syminfo), a --symbol-feeds it cannot install (see load_symbol_feeds), a
setting the strategy refuses, an --outputs it cannot honour (see OutputsError),
or any other failure of the harness (see failure_line and main). "code" is a stable code of the closed vocabulary
docker/run_failure_codes.json and "args" its typed arguments. The engine's code
is read only from strategy_get_last_error_code and its args from
strategy_get_last_error_args. A run failure from a library without the code
getter prints a line without "code" and "args": the earlier
{"engine":"pineforge","error":"<text>"} line byte for byte, or, for a run status
of 1 with no text, that line with the text "the run did not complete and the
engine reported no error". A report that cannot be written whole (a closed
pipe, a full disk) ends with exit status 1 and no line after it (see _main).

NaN convention: any metric with an empty/zero denominator is null (JSON has no
NaN); a real computed 0 stays 0. See the report-schema + metrics reference docs
for the per-field meaning of every metrics.* key.

Selected-window mode (selected-window wire v1.3, full/v1 report, no digest). Opt in
with --report-policy selected-window/v1, --window-start-ms T, --window-end-ms E,
--preroll-bars N, --fed-start-ms F and an explicit --input-tf and --script-tf (the
primary chart token); --request-feed-inventory names the inventory bound to --so.
The request is admitted first, from the inventory and the build's candidate support
constants, before any feed, library or strategy is touched. The native planner then
plans the window; the engine runs the retained rows (pre-roll plus window) and
supplies the selected trades, curve and metrics. The report gains report_window (R),
report_shape, diagnostics.phase_timing, fingerprint.version 2 and
fingerprint.provenance.schema_version 2; R is also applied_runtime.report_window and
fingerprint.provenance.runtime.report_window. --validate-window-only prints
{"validation_only": true, "report_window": R} with applied=false and runs nothing.
--so LIB --capabilities-json prints this runtime's capabilities. --run-phase-fd N
sends the phase records (selected and ordinary runs). Without any of these options a
run is the ordinary run described above, byte for byte.
"""
from __future__ import annotations

import argparse
import base64
import calendar
import csv
import ctypes
import hashlib
import io
import json
import math
import os
import re
import struct
import sys
import time
import traceback
import types
from datetime import datetime, timezone
from pathlib import Path


# >>> fingerprint helpers (DUPLICATED verbatim in scripts/run_strategy.py;
#     scripts/ is .dockerignore'd so this cannot be a shared module.
#     scripts/fingerprint_self_test.py asserts both copies stay identical.)
try:
    from importlib import metadata as _ilmd
except ImportError:  # pragma: no cover
    _ilmd = None

# Canonical strategy() defaults: the member defaults of
# source::PineStrategyConfig (include/pineforge/source/pine_adapter.hpp),
# which a generated constructor fills and hands to configure_pine_strategy.
# The constructor declares only what the script (or, for Pine v6, TradingView's
# default) sets -- a script that omits process_orders_on_close or
# close_entries_rule leaves them to the struct -- so this seed supplies the
# rest. KEEP IN SYNC with PineStrategyConfig.
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

# PineStrategyConfig member -> provenance key: the generated constructor's
# `cfg.<member> = <value>;` lines (pineforge-codegen since R4-C).
_CFG_FIELD_KEY = {
    "initial_capital": "initial_capital",
    "process_orders_on_close": "process_orders_on_close",
    "default_qty_type": "default_qty_type",
    "default_qty_value": "default_qty_value",
    "pyramiding": "pyramiding",
    "commission_type": "commission_type",
    "commission_value": "commission_value",
    "slippage": "slippage",
    "close_entries_rule_any": "close_entries_rule",
}
_QTY_TYPE_INDEX = {0: "fixed", 1: "percent_of_equity", 2: "cash"}
_COMM_TYPE_INDEX = {0: "percent", 1: "cash_per_order", 2: "cash_per_contract"}
_CFG_DECL_RE = re.compile(r"\bPineStrategyConfig\s+(\w+)\s*(?:\{\s*\}|\(\s*\))?\s*;")

# Pre-R4-C generated.cpp ctor member write -> provenance key.
_STRAT_FIELD_KEY = {
    "initial_capital_": "initial_capital",
    "process" + "_orders_on_close_": "process_orders_on_close",
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


def _strategy_value(key: str, rhs: str):
    """One declared strategy() value as the provenance spells it. The generated
    constructor stores PineStrategyConfig's enum members as int
    (`static_cast<int>(QtyType::FIXED)`); a pre-R4-C one assigned the enum."""
    rhs = rhs.strip()
    cast = re.fullmatch(r"static_cast<\s*\w+\s*>\((.*)\)", rhs, re.DOTALL)
    if cast:
        rhs = cast.group(1).strip()
    if key in ("default_qty_type", "commission_type"):
        names, index = ((_QTY_TYPE, _QTY_TYPE_INDEX) if key == "default_qty_type"
                        else (_COMM_TYPE, _COMM_TYPE_INDEX))
        value = _coerce_scalar(rhs)
        if isinstance(value, int) and not isinstance(value, bool):
            return index.get(value, rhs)
        return names.get(rhs.split("::")[-1], rhs)
    if key == "close_entries_rule":
        return "ANY" if _coerce_scalar(rhs) is True else "FIFO"
    return _coerce_scalar(rhs)


def parse_strategy_params(cpp_text: str) -> dict:
    """Parse strategy() header defaults from the constructor body only: the
    PineStrategyConfig the generated constructor fills (`cfg.<member> = ...;`),
    or a pre-R4-C constructor's member writes (`<member>_ = ...;`)."""
    out: dict = {}
    body = _ctor_body(cpp_text)
    decl = _CFG_DECL_RE.search(body)
    if decl:
        field = re.compile(r"\b" + re.escape(decl.group(1)) + r"\.(\w+)\s*=\s*([^;]+);")
        for fld, rhs in field.findall(body):
            key = _CFG_FIELD_KEY.get(fld)
            if key:
                out[key] = _strategy_value(key, rhs)
    for fld, rhs in re.findall(r"(\w+_)\s*=\s*([^;]+);", body):
        key = _STRAT_FIELD_KEY.get(fld)
        if key:
            out[key] = _strategy_value(key, rhs)
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


# Product accepted-type domain for exact Python integers: JavaScript
# Number.MAX/MIN_SAFE_INTEGER (= ±(2**53 - 1)). Rejecting larger exact ints
# guarantees unique lossless integer identity across generic ECMAScript
# consumers. This is not a claim that every such magnitude must round as
# binary64 — e.g. float(2**53) is exactly representable and remains legal
# on the float path, while exact int(2**53) is deliberately outside the
# product integer domain. Out-of-domain provenance yields no fingerprint
# under existing callers (exception → fingerprint None / skipped).
# Booleans are handled separately (bool subclasses int).
_JS_MAX_SAFE_INTEGER = 9007199254740991
_JS_MIN_SAFE_INTEGER = -9007199254740991


def _canonical_json_number(num: float) -> str:
    """Serialize a finite IEEE-754 float via ECMAScript NumberToString.

    Matches ECMAScript NumberToString (RFC 8785 / JCS numeric form):
    integral values have no trailing ``.0``, ``±0`` is ``0``, and
    scientific notation uses ES exponent thresholds (``e`` when the
    exponent is < -6 or >= 21). Non-finite values raise ValueError.
    Float subclasses are normalized via base ``float.__float__`` first so
    hooks such as ``__float__``/``__abs__``/comparisons/``__repr__`` cannot
    alter the underlying binary64 value used for math and emission.
    """
    # Plain built-in float: ignore subclass __float__/__abs__/__lt__/...
    num = float.__float__(num)
    if not math.isfinite(num):
        raise ValueError(
            "non-finite numbers are not permitted in fingerprint JSON")
    if num == 0.0:
        return "0"
    negative = num < 0
    r = repr(abs(num))
    if "e" in r or "E" in r:
        mant, exp_s = r.lower().split("e")
        exp = int(exp_s)
        if "." in mant:
            whole, frac = mant.split(".")
            digits_raw = whole + frac
            n = exp + len(whole)
        else:
            digits_raw = mant
            n = exp + len(digits_raw)
    else:
        if "." in r:
            whole, frac = r.split(".")
            digits_raw = whole + frac
            n = len(whole)
        else:
            digits_raw = r
            n = len(digits_raw)
    # Leading-zero strip adjusts n so value = int(digits)*10^(n-k) holds.
    lead = len(digits_raw) - len(digits_raw.lstrip("0"))
    digits = digits_raw.lstrip("0") or "0"
    if digits != "0":
        n -= lead
    while len(digits) > 1 and digits[-1] == "0":
        digits = digits[:-1]
    k = len(digits)
    sign = "-" if negative else ""
    if 0 < n <= 21:
        if k <= n:
            return sign + digits + ("0" * (n - k))
        return sign + digits[:n] + "." + digits[n:]
    if -6 < n <= 0:
        return sign + "0." + ("0" * (-n)) + digits
    exp = n - 1
    exp_s = f"+{exp}" if exp >= 0 else str(exp)
    if k == 1:
        return sign + digits + "e" + exp_s
    return sign + digits[0] + "." + digits[1:] + "e" + exp_s


def _reject_unpaired_surrogates(s: str, *, what: str) -> None:
    """Fail closed on unpaired UTF-16 surrogates (invalid I-JSON / UTF-8)."""
    for ch in s:
        cp = ord(ch)
        if 0xD800 <= cp <= 0xDFFF:
            raise ValueError(
                f"unpaired UTF-16 surrogate in fingerprint JSON {what}")


def _canonical_json_string(s: str) -> str:
    """Serialize a string in RFC 8785 / JCS string form.

    JSON control characters, quotes, and backslashes are escaped; valid
    Unicode (including non-ASCII, emoji, and U+2028/U+2029) is emitted as
    raw code points (not ``ensure_ascii`` ``\\uXXXX`` escapes). Unpaired
    surrogates raise ValueError rather than producing invalid I-JSON.
    Str subclasses are normalized via base ``str.__str__`` first so
    ``__str__``/``__iter__`` hooks cannot redirect iteration or emission.
    """
    # Plain built-in str: ignore subclass __str__/__iter__/...
    s = str.__str__(s)
    _reject_unpaired_surrogates(s, what="string")
    return json.dumps(s, ensure_ascii=False)


def _utf16_code_unit_key(s: str) -> bytes:
    """Object-key sort key matching JCS / ECMAScript UTF-16 code unit order.

    Str subclasses are normalized via base ``str.__str__`` first so
    ``__str__``/``__iter__``/``encode`` hooks cannot corrupt key order or
    hide unpaired surrogates.
    """
    # Plain built-in str: ignore subclass __str__/__iter__/encode hooks.
    s = str.__str__(s)
    _reject_unpaired_surrogates(s, what="object key")
    return s.encode("utf-16-be")


def _canonical_fingerprint_json(value) -> str:
    """Canonical JSON text for fingerprint token/digest bytes.

    Direct RFC 8785 / JCS-style canonical writer over the accepted Python
    value tree. The resulting UTF-8 token bytes are authoritative for
    ``digest``; verifiers should hash those bytes rather than assuming plain
    ``JSON.stringify`` is itself JCS (it does not sort keys or implement
    full JCS). Over values parsed under a JavaScript / IEEE-754 binary64
    number model, this matches a JCS direct encoder for the accepted types:
    - objects: each key is normalized once via base ``str.__str__`` to a
      plain built-in str; keys sorted by UTF-16 code unit order on those
      normalized names; no whitespace. Duplicate normalized names raise
      ValueError (fail closed — distinct str-subclass keys can override
      ``__eq__``/``__hash__`` so both coexist in a Python dict while
      ``str.__str__`` yields the same text; emitting both would produce
      duplicate JSON names that JS silently drops). Non-str keys raise
      TypeError. The retained original key is used only for value lookup.
    - arrays: element order preserved; no whitespace
    - numbers (float): ECMAScript NumberToString for every finite IEEE-754
      value after base ``float.__float__`` normalization (subclass hooks
      ignored); non-finite numbers raise ValueError
    - numbers (int): decimal digits only for exact integers inside the
      product safe-integer domain [-(2**53-1), 2**53-1]. This is a strict
      accepted-type policy guaranteeing unique lossless integer identity
      across generic ECMAScript consumers — not a claim that every larger
      individual value is unrepresentable as binary64. Exact integers
      outside the domain raise ValueError (e.g. int 2**53); isolated
      binary64 floats such as float(2**53) remain legal via the float path.
      Int subclasses (e.g. IntEnum) are normalized via base ``int.__index__``
      to a plain built-in int before domain checks and digit emission, so
      ``__index__``/``__int__``/comparison/``__str__``/``__repr__``/
      ``__format__`` hooks cannot change the value or bypass rejection.
      Booleans are not integers.
    - strings: JCS form — control/quote/backslash escapes, raw valid
      Unicode; unpaired surrogates raise ValueError. Str subclasses are
      normalized via base ``str.__str__`` so ``__str__``/``__iter__``/
      ``encode`` hooks cannot change emission, key order, or bypass
      surrogate rejection.
    - bools/null: ``true`` / ``false`` / ``null``
    """
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, str):
        return _canonical_json_string(value)
    if isinstance(value, int) and not isinstance(value, bool):
        # Plain built-in int via base slot: subclass __index__/__int__/
        # comparison hooks must not bypass domain checks or alter digits
        # (Python 3.9 IntEnum str was "Enum.NAME").
        value = int.__index__(value)
        if value < _JS_MIN_SAFE_INTEGER or value > _JS_MAX_SAFE_INTEGER:
            raise ValueError(
                "integers outside the JavaScript safe-integer range "
                f"[{_JS_MIN_SAFE_INTEGER}, {_JS_MAX_SAFE_INTEGER}] "
                "are not permitted in fingerprint JSON")
        return int.__repr__(value)
    if isinstance(value, float):
        return _canonical_json_number(value)
    if isinstance(value, list):
        return "[" + ",".join(
            _canonical_fingerprint_json(v) for v in value) + "]"
    if isinstance(value, dict):
        # Normalize each original key exactly once to a plain built-in str.
        # Distinct str subclasses can override __eq__/__hash__ so two keys
        # coexist while str.__str__ yields the same text; emit the
        # normalized name, look up values via the retained original key,
        # and fail closed on duplicate normalized names.
        items = []  # (normalized_key, original_key)
        seen_normalized = set()
        for k in value.keys():
            if not isinstance(k, str):
                raise TypeError(
                    "fingerprint JSON object keys must be str, "
                    f"got {type(k).__name__}")
            nk = str.__str__(k)
            if nk in seen_normalized:
                raise ValueError(
                    "duplicate fingerprint JSON object key after "
                    f"str normalization: {nk!r}")
            seen_normalized.add(nk)
            items.append((nk, k))
        parts = []
        for nk, orig in sorted(items, key=lambda ik: _utf16_code_unit_key(ik[0])):
            parts.append(
                _canonical_json_string(nk) + ":"
                + _canonical_fingerprint_json(value[orig]))
        return "{" + ",".join(parts) + "}"
    raise TypeError(
        f"fingerprint JSON cannot encode {type(value).__name__}")


def build_fingerprint(provenance: dict) -> dict:
    """Build ``{token, digest, provenance}`` for a provenance dict.

    ``token`` is base64 of the canonical UTF-8 JSON bytes; ``digest`` is
    ``sha256:`` + hex of those same bytes. Verifiers should treat the decoded
    token bytes as authoritative and hash them directly. Re-canonicalizing a
    decoded value tree requires an RFC 8785/JCS direct encoder with an
    IEEE-754 binary64 number model — default Python ``json.loads`` may yield
    ints outside the product integer domain for tokens such as ``1e20``
    (``100000000000000000000``). Project tests that rebuild from token text
    use ``json.loads(text, parse_int=float)``. Out-of-domain inputs raise
    here; existing callers yield no fingerprint (``None`` / skip).
    """
    canonical = _canonical_fingerprint_json(provenance)
    raw = canonical.encode("utf-8")
    return {
        "token": base64.b64encode(raw).decode("ascii"),
        "digest": "sha256:" + hashlib.sha256(raw).hexdigest(),
        "provenance": provenance,
    }
# <<< fingerprint helpers


_RELEASE_OVERRIDE_TYPES = {
    "initial_capital": "float",
    "commission_value": "float",
    "default_qty_value": "float",
    "pyramiding": "int",
    "slippage": "int",
    "process_orders_on_close": "bool",
    "calc_on_order_fills": "bool",
    "close_entries_rule": "string",
    "default_qty_type": "string",
    "commission_type": "string",
}
_RELEASE_ENUM_WORDS = {
    "close_entries_rule": ("FIFO", "ANY"),
    "default_qty_type": ("fixed", "percent_of_equity", "cash"),
    "commission_type": ("percent", "cash_per_order", "cash_per_contract"),
}


def _release_scalar(value, declared_type):
    if declared_type in ("string", "source", "enum", "unknown"):
        return str(value)
    if declared_type == "bool":
        if type(value) is bool:
            return value
        if value in ("true", "1"):
            return True
        if value in ("false", "0"):
            return False
        raise ValueError("unrepresentable declared boolean")
    scalar = _coerce_scalar(str(value))
    if type(scalar) not in (int, float):
        raise ValueError("unrepresentable declared number")
    if declared_type in ("int", "int64"):
        if type(scalar) is float and not scalar.is_integer():
            raise ValueError("nonintegral declared integer")
        return int(scalar)
    if declared_type in ("float", "double"):
        result = float(scalar)
        if not math.isfinite(result):
            raise ValueError("nonfinite declared float")
        return result
    raise ValueError("unsupported declared scalar type")


def _release_legacy_number(text, declared_type):
    """Use the same C conversions underlying std::stoi/stoll/stod."""
    library = ctypes.CDLL(None, use_errno=True)
    integer = declared_type in ("int", "int64")
    function = library.strtoll if integer else library.strtod
    function.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    function.restype = ctypes.c_longlong if integer else ctypes.c_double
    if integer:
        function.argtypes.append(ctypes.c_int)
    buffer = ctypes.create_string_buffer(text.encode("utf-8"))
    end = ctypes.c_void_p()
    ctypes.set_errno(0)
    arguments = [buffer, ctypes.byref(end)]
    if integer:
        arguments.append(10)
    result = function(*arguments)
    if end.value == ctypes.addressof(buffer) or ctypes.get_errno():
        raise ValueError("legacy numeric conversion failed")
    if declared_type == "int" and not -(2 ** 31) <= result < 2 ** 31:
        raise ValueError("legacy integer conversion out of range")
    return result


# Exact character predicates from the pinned producer's Lexer._read_ident /
# identifier start and NamingHelper._cpp_string_escape inverse. The real-image
# class matrix binds both producer source blobs and exercises every class.
def _release_codegen_identifier(value):
    return bool(value) and (value[0].isalpha() or value[0] == "_") and all(
        char.isalnum() or char == "_" for char in value)


_RELEASE_EMITTED_ESCAPES = {"\\": "\\", '"': '"', "n": "\n", "r": "\r", "t": "\t"}


def _release_cpp_input_name(spelling):
    """Decode an emitted narrow C++ literal, then apply its C-string boundary."""
    simple = {key: ord(value) for key, value in _RELEASE_EMITTED_ESCAPES.items()}
    # Preserve the existing pre-transpiled C++ literal extensions as well.
    simple.update({"a": 7, "b": 8, "f": 12, "v": 11, "'": 39, "?": 63})
    pieces = re.findall(r'\\(?:[0-7]{1,3}|x[0-9a-fA-F]+|u[0-9a-fA-F]{4}'
                        r'|U[0-9a-fA-F]{8}|[\s\S]|$)|[^\\]+', spelling)
    raw = bytearray()
    for piece in pieces:
        if not piece.startswith("\\"):
            raw.extend(piece.encode("utf-8"))
            continue
        escape = piece[1:]
        if escape in simple:
            raw.append(simple[escape])
        elif re.fullmatch(r'[0-7]{1,3}|x[0-9a-fA-F]+', escape):
            # Out-of-byte-range escapes are implementation-defined; refuse
            # them instead of inventing a native name.
            raw.append(int(escape[1:], 16) if escape.startswith("x") else int(escape, 8))
        elif re.fullmatch(r'u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}', escape):
            raw.extend(chr(int(escape[1:], 16)).encode("utf-8"))
        else:
            raise ValueError("unsupported C++ input-name escape")
    return bytes(raw).split(b"\0", 1)[0].decode("utf-8")


class _ReleaseUnreadable(ValueError):
    """A literal, comment or conditional the lexical scan cannot delimit."""


class _ReleaseTokens(list):
    """Scanned tokens; .groups maps a conditional instance to its branches."""

    groups = None


# Positive allowlist of the legacy declaration reader. A value is certified
# only when every token it relies on is in one of these forms; any other
# occurrence refuses (resolution reason), never "no bad shape detected".
_RELEASE_GETTERS = {
    "get_input_int": "int", "get_input_int64": "int64", "get_input_double": "double",
    "get_input_bool": "bool", "get_input_string": "string", "get_input_source": "source",
}
# Native receipt `type` a getter is compared against; never coerced to it.
_RELEASE_RECEIPT_TYPES = {
    "int": ("int", "enum"), "int64": ("int",), "double": ("float",),
    "bool": ("bool",), "string": ("string",), "source": ("source",),
}
# PineScheduler::source_series: the finite native source selector vocabulary.
_RELEASE_SOURCE_SELECTORS = ("open", "high", "low", "close", "volume",
                             "hl2", "hlc3", "ohlc4", "hlcc4")
_RELEASE_DECIMAL = re.compile(
    r'[+-]?(?:(?:0|[1-9][0-9]*)(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?')
_RELEASE_INTEGER = re.compile(r'[+-]?(?:0|[1-9][0-9]*)')
# Tokens a recognized getter call follows: the producer's expression contexts.
# (`>`, `>>` and `}` are left out: they can end the type of a declaration.)
_RELEASE_CALL_CONTEXT = frozenset({
    "=", "(", ",", "?", ":", "return", "{", "[", "!", "+", "-", "*", "/", "%",
    "<", "<=", ">=", "==", "!=", "&&", "||", ";", "&", "|", "^", "~",
    "<<", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="})
# Keywords whose parenthesis opens an expression, never a declarator.
_RELEASE_EXPRESSION_KEYWORDS = frozenset({
    "if", "while", "switch", "for", "return", "sizeof", "alignof", "decltype",
    "static_assert", "case", "noexcept"})
# The only conditional branch a recognized getter or symbol use may sit in:
# the producer's checked-settings metadata. Anything else is unrecognized.
_RELEASE_SETTINGS_BRANCH = ("ifdef", "PF_SETTINGS_API_VERSION")
# The producer's guard around another symbol's request
# (emit_top.py _emit_foreign_security_registration): every branch is code, and
# a getter in it is certified only when all branches agree or a native receipt
# row picks exactly one.
_RELEASE_SECURITY_GUARD = ("ifdef", "PINEFORGE_HAS_SYMBOL_SECURITY_EVAL_V1")
# C-style casts the producer emits (helpers.py, tables.py, drawing.py); one may
# stand right before a getter call. Any other cast refuses.
_RELEASE_PRODUCER_CASTS = frozenset({"int", "double", "int64_t"})
_RELEASE_BUILTIN_TYPES = frozenset({
    "int", "double", "float", "bool", "char", "short", "long", "unsigned", "signed",
    "int64_t", "int32_t", "int16_t", "int8_t", "uint64_t", "uint32_t", "uint16_t",
    "uint8_t", "size_t", "auto", "wchar_t", "char8_t", "char16_t", "char32_t"})
# Every #define line the pinned producer emits, verbatim (emit_top.py
# _emit_includes; run_stops.py). A macro name alone does not authenticate a
# replacement list: any other #define, any #undef, refuses.
_RELEASE_PRODUCER_DEFINES = frozenset(r'''#define _PF_NO_DATA_STOP(function, call, line, english) ::pineforge::pine_no_data_stop(function, call, line, english)
#define _PF_OTHER_SYMBOL_STOP(function, symbol, call, line, english) ::pineforge::pine_other_symbol_stop(function, symbol, call, line, english)
#define _PF_ARRAY_STOP(reason, method, english) ::pineforge::pine_array_stop(reason, method, std::string(english).c_str())
#define _PF_COLLECTION_STOP(reason, object, english) ::pineforge::pine_collection_stop(object, reason, english)
#define _PF_NA_STOP(object, english) ::pineforge::pine_na_stop(object, english)
#define _PF_LIMIT_STOP(limit, max, english) ::pineforge::pine_limit_stop(limit, max, english)
#define _PF_UNSUPPORTED_STOP(reason, line, english) ::pineforge::pine_unsupported_stop(reason, line, english)
#define _PF_STRING_STOP(reason, english) ::pineforge::pine_string_stop(reason, english)
#define _PF_ENGINE_INVARIANT(english, legacy_type) ::pineforge::pine_engine_invariant(english)
#define _PF_INVARIANT_AT(container, index) _pf_invariant_at(container, index)
#define _PF_SETTING_FAILURE(strategy, entrypoint, message, reason) (strategy)->_pf_record_setting_failure(entrypoint, message, [] { return reason ? ::pineforge::RunFailureInfo(::pineforge::RunFailureCode::setting_rejected, {{"entrypoint", entrypoint}, {"reason", reason}}) : ::pineforge::RunFailureInfo(::pineforge::RunFailureCode::setting_rejected, {{"entrypoint", entrypoint}}); })
#define _PF_NO_DATA_STOP(function, call, line, english) pine_runtime_error(std::string(english))
#define _PF_OTHER_SYMBOL_STOP(function, symbol, call, line, english) pine_runtime_error(std::string(english))
#define _PF_ARRAY_STOP(reason, method, english) pine_runtime_error(english)
#define _PF_COLLECTION_STOP(reason, object, english) pine_runtime_error(english)
#define _PF_NA_STOP(object, english) throw std::runtime_error(english)
#define _PF_LIMIT_STOP(limit, max, english) throw std::length_error(english)
#define _PF_UNSUPPORTED_STOP(reason, line, english) pine_runtime_error(std::string(english))
#define _PF_STRING_STOP(reason, english) pine_runtime_error(std::string(english))
#define _PF_ENGINE_INVARIANT(english, legacy_type) throw legacy_type(english)
#define _PF_INVARIANT_AT(container, index) (container).at(index)
#define _PF_SETTING_FAILURE(strategy, entrypoint, message, reason) (strategy)->_pf_record_setting_failure(entrypoint, message)
#define PF_PINE_TIME_SESSION_DAY_ARGS(tz, sess) , tz, sess
#define PF_PINE_TIME_SESSION_DAY_ARGS(tz, sess)
#define PF_VWAP_SESSION_ANCHOR_ARGS(tz, sess) , tz, sess
#define PF_VWAP_SESSION_ANCHOR_ARGS(tz, sess)'''.splitlines())
# Macro spellings used by the producer subset and its standard-library calls.
# This is not a table of every macro available to arbitrary C++; foreign
# sources require native receipt confirmation independently of this scanner.
_RELEASE_MACROS = frozenset(
    re.match(r"#define ([A-Za-z_][A-Za-z0-9_]*)\(", line).group(1)
    for line in _RELEASE_PRODUCER_DEFINES) | frozenset({
        "assert", "offsetof", "setjmp", "va_arg", "va_copy", "va_end", "va_start",
        "INT8_C", "INT16_C", "INT32_C", "INT64_C", "INTMAX_C",
        "UINT8_C", "UINT16_C", "UINT32_C", "UINT64_C", "UINTMAX_C"})
_RELEASE_OPERATORS = ("...", "->*", "::", "&&", "||", "==", "!=", "<=", ">=",
                      "++", "--", "->", ".*", "+=", "-=", "*=", "/=", "%=",
                      "&=", "|=", "^=", "<<", ">>", "##")
_RELEASE_STRATEGY_ENUMS = {
    "default_qty_type": ("QtyType", ("FIXED", "PERCENT_OF_EQUITY", "CASH")),
    "commission_type": ("CommissionType", ("PERCENT", "CASH_PER_ORDER", "CASH_PER_CONTRACT")),
}
# The producer's adapter hooks, the only other constructor statements it emits.
_RELEASE_ADAPTER_STATEMENTS = tuple(
    ["pineforge", "::", "source", "::", "PineStrategyHost", "::", hook, "(", ")", ";"]
    for hook in ("attach_pine_execution_adapter", "enable_pine_intraday_cap"))
# The producer's other PineStrategyConfig members: recognized, not provenance.
_RELEASE_CONFIG_OTHER = frozenset({"margin_long", "margin_short", "src_series_active"})


def _release_domain(value):
    """The product scalar domain; known out-of-domain evidence is refused."""
    if type(value) is int and not _JS_MIN_SAFE_INTEGER <= value <= _JS_MAX_SAFE_INTEGER:
        raise ValueError("native integer outside the product domain")
    if type(value) is float and not math.isfinite(value):
        raise ValueError("native number outside the product domain")
    return value


def _release_receipt_value(text, receipt_type):
    """One receipt scalar parsed by the receipt's own declared type, or None."""
    if not isinstance(text, str):
        return None
    if receipt_type in ("int", "enum"):
        return int(text) if re.fullmatch(r'[+-]?[0-9]+', text) else None
    if receipt_type == "float":
        # The settings receipt spells a non-finite native number "na".
        if text == "na":
            return math.nan
        if text.strip().lower().lstrip("+-") in ("inf", "infinity", "nan"):
            return float(text)
        return float(text) if _RELEASE_DECIMAL.fullmatch(text) else None
    if receipt_type == "bool":
        return {"true": True, "false": False}.get(text)
    if receipt_type in ("string", "source"):
        return text
    return None


def _release_cpp_tokens(text):
    """Flat lexical scan: no C++ evaluation, scopes or name lookup.

    Each token is (text, start, end, kind, directive, branches): kind is
    ident/number/string (unprefixed ordinary literal)/literal/op, directive
    marks a preprocessor line and branches the enclosing conditional groups.
    Returns (tokens, trusted). trusted is False for a line splice, a backslash
    outside a literal (universal-character names), or a directive the producer
    never emits. Raises _ReleaseUnreadable for an undelimitable context."""
    tokens = _ReleaseTokens()
    tokens.groups = {}
    trusted = re.search(r'\\[ \t\f\v]*(?:\r\n|\n|\r)', text) is None
    stack = []
    position = 0
    line_start = True
    directive = None
    settings_include = False
    inactive_settings = set()
    length = len(text)

    def close_directive():
        nonlocal trusted, settings_include
        words = [token[0] for token in directive]
        name = words[1] if len(words) > 1 else ""
        if name in ("if", "ifdef", "ifndef"):
            opener = tuple([name] + words[2:])
            instance = len(tokens.groups)
            tokens.groups[instance] = [opener]
            if opener == _RELEASE_SETTINGS_BRANCH and not settings_include:
                inactive_settings.add(instance)
            stack.append((opener, opener, instance))
        elif name in ("elif", "else"):
            if not stack:
                raise _ReleaseUnreadable("unbalanced conditional")
            opener, _, instance = stack[-1]
            branch = tuple([name] + words[2:])
            tokens.groups[instance].append(branch)
            stack[-1] = (opener, branch, instance)
        elif name == "endif":
            if not stack:
                raise _ReleaseUnreadable("unbalanced conditional")
            stack.pop()
        elif name == "include":
            # Only the producer's engine and standard headers, never a path
            # that could reach a header of the caller's.
            header = "".join(words[3:-1])
            if not (len(words) > 3 and words[2] == "<" and words[-1] == ">"
                    and ".." not in header
                    and re.fullmatch(r"pineforge/[A-Za-z0-9_/]+\.hpp|[a-z_]+", header)):
                trusted = False
            # The pinned producer includes this header before emitting any
            # settings guard. Its first include defines PF_SETTINGS_API_VERSION.
            if (header == "pineforge/checked_settings.hpp" and
                    (not stack or all(opener == branch == (
                        "if", "__has_include", "(", "<", "pineforge", "/",
                        "checked_settings", ".", "hpp", ">", ")")
                                      for opener, branch, _ in stack))):
                settings_include = True
        elif name == "define":
            line = re.sub(r"\s+", " ", text[directive[0][1]:directive[-1][2]])
            if line not in _RELEASE_PRODUCER_DEFINES:
                trusted = False
        elif name not in ("", "error"):
            trusted = False

    while position < length:
        start = position
        char = text[position]
        if char in "\r\n":
            if directive is not None:
                close_directive()
                directive = None
            line_start = True
            position += 1
            continue
        if char.isspace():
            position += 1
            continue
        if text.startswith("//", position):
            end = text.find("\n", position)
            position = length if end < 0 else end
            continue
        if text.startswith("/*", position):
            end = text.find("*/", position + 2)
            if end < 0:
                raise _ReleaseUnreadable("unterminated C++ comment")
            if directive is not None and "\n" in text[position:end]:
                raise _ReleaseUnreadable("multi-line comment in a directive")
            position = end + 2
            continue
        kind = "op"
        if char == "#" and line_start and directive is None:
            directive = []
            position += 1
        elif char.isalpha() or char == "_":
            position += 1
            while position < length and (text[position].isalnum() or text[position] == "_"):
                position += 1
            word = text[start:position]
            kind = "ident"
            if position < length and text[position] == '"' and word in ("R", "u8R", "uR", "UR", "LR"):
                match = re.compile(r'"([^ ()\\\t\v\f\r\n]{0,16})\(').match(text, position)
                if not match:
                    raise _ReleaseUnreadable("malformed raw string literal")
                end = text.find(")" + match.group(1) + '"', match.end())
                if end < 0:
                    raise _ReleaseUnreadable("unterminated raw string literal")
                position = end + len(match.group(1)) + 2
                kind = "literal"
            elif position < length and text[position] in "\"'" and word in ("u8", "u", "U", "L"):
                kind = None
        elif "0" <= char <= "9" or (char == "." and position + 1 < length
                                    and "0" <= text[position + 1] <= "9"):
            position += 1
            while position < length:
                current = text[position]
                if current in "+-" and text[position - 1] in "eEpP":
                    position += 1
                elif current == "'":
                    # No producer number contains a digit separator. Refuse
                    # before a quote can be misread as the start of a literal.
                    trusted = False
                    position += 1
                elif current.isascii() and (current.isalnum() or current in "_."):
                    position += 1
                else:
                    break
            kind = "number"
        elif char not in "\"'":
            if char == "\\":
                trusted = False
            # Digraphs (%: <% %> <: :>) would hide a directive or a brace;
            # `<::` not followed by `:` or `>` is the ordinary `<` `::`.
            pair = text[position:position + 2]
            if pair in ("%:", "<%", "%>", ":>") or (
                    pair == "<:" and (text[position:position + 3] != "<::"
                                      or text[position + 3:position + 4] in (":", ">"))):
                trusted = False
            operator = next((op for op in _RELEASE_OPERATORS
                             if text.startswith(op, position)), char)
            position += len(operator)
        if kind is None or (kind == "op" and char in "\"'"):
            quote = text[position]
            kind = "string" if start == position and quote == '"' else "literal"
            position += 1
            while position < length and text[position] != quote:
                if text[position] in "\r\n":
                    raise _ReleaseUnreadable("unterminated C++ literal")
                position += 2 if text[position] == "\\" else 1
            if position >= length:
                raise _ReleaseUnreadable("unterminated C++ literal")
            position += 1
        line_start = False
        token = (text[start:position], start, position, kind,
                 directive is not None, tuple(stack))
        if directive is not None:
            directive.append(token)
        # Before the defining include this metadata branch is inactive. Do
        # not treat its getters, symbol reads or constructor text as code.
        if token[4] or not any(instance in inactive_settings for _, _, instance in stack):
            tokens.append(token)
    if directive is not None:
        close_directive()
    if stack:
        raise _ReleaseUnreadable("unterminated conditional")
    brackets = []
    closing = {"(": ")", "[": "]", "{": "}"}
    for token in tokens:
        if token[4] or token[3] != "op":
            continue
        if token[0] in closing:
            brackets.append(closing[token[0]])
        elif token[0] in (")", "]", "}"):
            if not brackets or token[0] != brackets[-1]:
                trusted = False
            else:
                brackets.pop()
    # Alternative preprocessor arms can repeat an opening function brace
    # before a shared closing brace. This flat scan checks mismatched closers,
    # not the balance of the concatenation of mutually exclusive arms.
    # The producer's leading-comma macros are invoked only as
    # `NAME(syminfo_.timezone, syminfo_.session)` (tables.py, visit_call.py,
    # visit_expr.py); any other invocation could splice a declarator.
    for index, token in enumerate(tokens):
        if (token[3] == "ident" and not token[4] and token[0] in (
                "PF_PINE_TIME_SESSION_DAY_ARGS", "PF_VWAP_SESSION_ANCHOR_ARGS")
                and [item[0] for item in tokens[index + 1:index + 10]] != [
                    "(", "syminfo_", ".", "timezone", ",", "syminfo_", ".", "session", ")"]):
            trusted = False
    # Names a certified form relies on are never redeclared or aliased: `std`
    # and `checked_settings` only qualify, `pineforge` only qualifies or is the
    # producer's `using namespace pineforge;`.
    for index, token in enumerate(tokens):
        if token[3] != "ident" or token[0] not in ("std", "pineforge", "checked_settings"):
            continue
        following = tokens[index + 1][0] if index + 1 < len(tokens) else None
        preceding = [item[0] for item in tokens[max(0, index - 2):index]]
        if token[4] and (preceding[-1:] in (["<"], ["/"]) and following in ("/", ".", ">")):
            continue  # a header-name component of #include / __has_include
        if following == "::" and (token[0] != "checked_settings" or preceding[-1:] == ["::"]):
            continue
        if (token[0] == "pineforge" and following == ";" and preceding == ["using", "namespace"]
                and not token[4] and token[5] == ()):
            continue
        trusted = False
    return tokens, trusted


def _release_code_context(token):
    """Code the producer emits: no directive line, and only inside the first
    branch of its settings guard or the #ifdef / #else branch of its security
    guard."""
    return not token[4] and all(
        (opener == _RELEASE_SETTINGS_BRANCH and branch == opener)
        or (opener == _RELEASE_SECURITY_GUARD and branch in (opener, ("else",)))
        for opener, branch, _ in token[5])


def _release_previous(tokens, index):
    """The previous code token, skipping preprocessor lines."""
    index -= 1
    while index >= 0 and tokens[index][4]:
        index -= 1
    return tokens[index] if index >= 0 else None


def _release_closing(tokens, index, directives=False):
    """Index of the token closing the bracket opened at index, or None. A
    preprocessor line inside refuses unless directives (a body) are allowed."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    expected = []
    for position in range(index, len(tokens)):
        word = tokens[position][0]
        if tokens[position][4]:
            if directives:
                continue
            return None
        if word in pairs and tokens[position][3] == "op":
            expected.append(pairs[word])
        elif word in (")", "]", "}") and tokens[position][3] == "op":
            if not expected or expected.pop() != word:
                return None
            if not expected:
                return position
    return None


def _release_declarator_risk(tokens, index):
    """True when the call at index could be spelled as a declarator instead:
    right after a comma at statement level (an init-declarator list), or
    wrapped only in parentheses that follow `>` or a (qualified) name at the
    start of a statement, as in `T (get_input_int("x", 1));`."""
    previous = _release_previous_index(tokens, index)
    if previous >= 0 and tokens[previous][0] == ",":
        depth = 0
        position = _release_previous_index(tokens, previous)
        while position >= 0:
            token = tokens[position]
            if token[3] == "op" and token[0] in (")", "]", "}"):
                depth += 1
            elif token[3] == "op" and token[0] in ("(", "[", "{"):
                if not depth:
                    if token[0] == "{":
                        return True
                    # for/if/switch/while parentheses admit a declaration.
                    opener = _release_previous(tokens, position)
                    if token[0] == "(" and opener is not None and opener[0] in (
                            "for", "if", "switch", "while"):
                        return True
                    break
                depth -= 1
            elif token[0] == ";" and not depth:
                return True
            position = _release_previous_index(tokens, position)
        else:
            return True
    while previous >= 0 and tokens[previous][0] == "(":
        before = _release_previous_index(tokens, previous)
        if before >= 0 and tokens[before][0] == "(":
            previous = before
            continue
        if before >= 0 and tokens[before][0] in (">", ">>"):
            return True
        if before < 0 or tokens[before][3] != "ident" or tokens[before][0] in \
                _RELEASE_EXPRESSION_KEYWORDS:
            return False
        head = before
        while True:
            prior = _release_previous_index(tokens, head)
            if prior >= 0 and tokens[prior][0] == "::":
                qualifier = _release_previous_index(tokens, prior)
                if qualifier >= 0 and tokens[qualifier][3] == "ident":
                    head = qualifier
                    continue
                prior = qualifier
            return prior < 0 or tokens[prior][0] in (";", "{", "}", ":")
    return False


def _release_getter_calls(tokens):
    """Recognized getter calls, and whether every getter-family token is one.

    A recognized call is one of the six producer getters in code context,
    after an expression token, with a plain literal title and exactly one
    balanced default argument: name ( "title" , default ). A getter call of
    that shape inside a macro argument is returned apart (never certified)."""
    calls = []
    macro_calls = []
    intact = True
    # Code tokens inside an argument of a function-like macro invocation.
    in_macro = [False] * len(tokens)
    stack = []
    for index, token in enumerate(tokens):
        if token[4]:
            continue
        in_macro[index] = any(marker for _, marker in stack)
        if token[3] == "op" and token[0] in ("(", "[", "{"):
            previous = _release_previous(tokens, index)
            stack.append(({"(": ")", "[": "]", "{": "}"}[token[0]],
                          token[0] == "(" and previous is not None and previous[3] == "ident"
                          and previous[0] in _RELEASE_MACROS))
        elif token[3] == "op" and token[0] in (")", "]", "}"):
            if not stack or stack[-1][0] != token[0]:
                intact = False
            else:
                stack.pop()
    for index, token in enumerate(tokens):
        if token[3] != "ident" or not token[0].startswith("get_input_"):
            continue
        if in_macro[index]:
            close = (_release_closing(tokens, index + 1)
                     if index + 4 < len(tokens) and tokens[index + 1][0] == "(" else None)
            if (token[0] in _RELEASE_GETTERS and _release_code_context(token)
                    and close is not None and tokens[index + 2][3] == "string"
                    and tokens[index + 3][0] == "," and close > index + 4):
                macro_calls.append((index, close, _RELEASE_GETTERS[token[0]],
                                    tokens[index + 2][0][1:-1], tokens[index + 4:close]))
            else:
                intact = False
            continue
        previous = _release_previous(tokens, index)
        position = _release_previous_index(tokens, index)
        if previous is not None and previous[0] == ")" and previous[3] == "op":
            # `( <producer cast type> ) get_input_*(...)`: read the context
            # before the cast; the cast is not part of the input.
            kind = _release_previous_index(tokens, position)
            opening = _release_previous_index(tokens, kind) if kind >= 0 else -1
            if (kind >= 0 and tokens[kind][0] in _RELEASE_PRODUCER_CASTS
                    and opening >= 0 and tokens[opening][0] == "("):
                previous = _release_previous(tokens, opening)
        elif previous is not None and previous[0] == "(":
            # A function-style cast `int(get_input_*(...))` is not on the list.
            callee = _release_previous(tokens, position)
            if callee is not None and callee[0] in _RELEASE_BUILTIN_TYPES:
                previous = None
        close = (_release_closing(tokens, index + 1)
                 if index + 4 < len(tokens) and tokens[index + 1][0] == "(" else None)
        if (token[0] not in _RELEASE_GETTERS or not _release_code_context(token)
                or previous is None or previous[0] not in _RELEASE_CALL_CONTEXT
                or previous[3] not in ("op", "ident")
                or close is None or tokens[index + 2][3] != "string"
                or tokens[index + 3][0] != "," or close <= index + 4
                or _release_declarator_risk(tokens, index)):
            intact = False
            continue
        default = tokens[index + 4:close]
        depth = 0
        for item in default:
            if item[3] == "op" and item[0] in ("(", "[", "{"):
                depth += 1
            elif item[3] == "op" and item[0] in (")", "]", "}"):
                depth -= 1
            elif depth == 0 and item[0] == ",":
                intact = False
                break
        else:
            calls.append((index, close, _RELEASE_GETTERS[token[0]], tokens[index + 2][0][1:-1],
                          default))
    return calls, intact, macro_calls


def _release_previous_index(tokens, index):
    """Index of the previous code token, skipping preprocessor lines, or -1."""
    index -= 1
    while index >= 0 and tokens[index][4]:
        index -= 1
    return index


def _release_pure_read(tokens, index):
    """The closed list of producer read positions for a symbolic default.

    R1 `auto _pna_l|_pna_r = ( SYMBOL ) ;` (visit_expr.py _emit_na_relational):
    the parenthesis follows `=`, so it is the initializer's expression, never
    a declarator. R2 `( __switch_val_<n> == SYMBOL ) {` (visit_stmt.py
    _visit_switch / _visit_if_switch_expr): an equality operand is an
    expression, never a declarator."""
    def words(start, stop):
        if start < 0 or stop > len(tokens) or any(tokens[i][4] for i in range(start, stop)):
            return None
        return [tokens[i][0] for i in range(start, stop)]
    if (words(index - 4, index) in (["auto", "_pna_l", "=", "("], ["auto", "_pna_r", "=", "("])
            and words(index + 1, index + 3) == [")", ";"]):
        return True
    before = words(index - 3, index)
    return (before is not None and before[0] == "(" and before[2] == "=="
            and re.fullmatch(r"__switch_val_[0-9]+", before[1]) is not None
            and words(index + 1, index + 3) == [")", "{"])


def _release_symbol_occurrences(tokens, symbol, calls):
    """Classify every occurrence of symbol: (declarations, other count).

    Recognized: the global `[static] const|constexpr int SYMBOL = ...;`, a
    whole numeric getter default, the producer's checked-settings metadata
    default `{"title", "enum", ::pineforge::checked_settings::number(SYMBOL),`,
    and the pure reads of _release_pure_read. Every other occurrence, in any
    syntactic position, is counted against it."""
    uses = {call[4][0][1] for call in calls
            if len(call[4]) == 1 and call[4][0][0] == symbol
            and call[2] in ("int", "int64", "double")}
    metadata = ("{", None, ",", '"enum"', ",", "::", "pineforge", "::",
                "checked_settings", "::", "number", "(")
    declarations = []
    others = 0
    depth = 0
    for index, token in enumerate(tokens):
        if not token[4] and token[3] == "op" and token[0] in ("{", "}"):
            depth += 1 if token[0] == "{" else -1
        if token[0] != symbol or token[3] != "ident":
            continue
        if token[1] in uses:
            continue
        if _release_code_context(token) and _release_pure_read(tokens, index):
            continue
        window = tokens[index - 12:index] if index >= 12 else []
        if (_release_code_context(token) and window
                and all(not item[4] and expected in (None, item[0])
                        for expected, item in zip(metadata, window))
                and window[1][3] == "string" and window[3][3] == "string"
                and index + 2 < len(tokens)
                and tokens[index + 1][0] == ")" and tokens[index + 2][0] == ","):
            continue
        end = index + 2
        while end < len(tokens) and tokens[end][0] != ";" and not tokens[end][4]:
            end += 1
        kind = _release_previous_index(tokens, index)
        qualifier = _release_previous_index(tokens, kind) if kind >= 0 else -1
        head = _release_previous_index(tokens, qualifier) if qualifier >= 0 else -1
        if head >= 0 and tokens[head][0] == "static":
            head = _release_previous_index(tokens, head)
        if (not token[4] and token[5] == () and depth == 0
                and kind >= 0 and tokens[kind][0] == "int"
                and qualifier >= 0 and tokens[qualifier][0] in ("const", "constexpr")
                and (head < 0 or tokens[head][0] in (";", "}"))
                and index + 2 < end < len(tokens) and tokens[index + 1][0] == "="
                and tokens[end][0] == ";"):
            declarations.append((index, tokens[index + 2:end]))
        else:
            others += 1
    return declarations, others


def _release_symbolic_default(tokens, trusted, calls, symbol, declared_type):
    """A unique, positively recognized global integer constant, or a reason."""
    if not trusted:
        return None, "unsupported_binding"
    declarations, others = _release_symbol_occurrences(tokens, symbol, calls)
    if len(declarations) + others > 1:
        return None, "ambiguous_binding"
    if len(declarations) != 1:
        return None, "unsupported_binding"
    words = "".join(item[0] for item in declarations[0][1])
    if (not all(item[3] in ("number", "op") for item in declarations[0][1])
            or not _RELEASE_INTEGER.fullmatch(words)
            or not -(2 ** 31) <= int(words) < 2 ** 31):
        return None, "unsupported_default"
    return _release_scalar(words, declared_type), None


def _release_source_bound(tokens, calls, symbol):
    """`_src_<selector>_` is only a getter default or a member access."""
    defaults = {call[4][0][1] for call in calls
                if len(call[4]) == 1 and call[4][0][0] == symbol}
    for index, token in enumerate(tokens):
        if token[0] == symbol and token[3] == "ident" and token[1] not in defaults:
            if token[4] or index + 1 >= len(tokens) or tokens[index + 1][0] != ".":
                return False
    return True


def _release_literal_default(words, kinds, declared_type):
    """A recognized literal default for a getter type, or a refusal reason."""
    text = "".join(words)
    if declared_type in ("int", "int64", "double"):
        if (not 1 <= len(words) <= 2 or kinds[-1] != "number"
                or (len(words) == 2 and words[0] not in "+-")
                or not _RELEASE_DECIMAL.fullmatch(text)):
            return None, "unsupported_default"
        value = float(text)
        _release_domain(value)
        if declared_type == "double":
            return value, None
        if not value.is_integer():
            return None, "unsupported_default"
        integer = int(text) if _RELEASE_INTEGER.fullmatch(text) else int(value)
        bits = 32 if declared_type == "int" else 64
        if not -(2 ** (bits - 1)) <= integer < 2 ** (bits - 1):
            return None, "unsupported_default"
        return _release_domain(integer), None
    if declared_type == "bool":
        if words in (["true"], ["false"]):
            return words == ["true"], None
        return None, "unsupported_default"
    if declared_type == "string":
        if kinds == ["string"]:
            literal = words[0]
        elif (words[:4] == ["std", "::", "string", "("] and len(words) == 6
                and kinds[4] == "string" and words[5] == ")"):
            literal = words[4]
        else:
            return None, "unsupported_default"
        try:
            return _release_cpp_input_name(literal[1:-1]), None
        except ValueError:
            return None, "unsupported_default"
    return None, "unsupported_default"


def _release_resolve_default(tokens, trusted, calls, declared_type, words, default):
    """One getter occurrence's default: (value, reason, symbolic)."""
    kinds = [item[3] for item in default]
    if declared_type == "source":
        selector = re.fullmatch(r"_src_([a-z0-9]+)_", words[0]) if len(words) == 1 else None
        if not selector or selector.group(1) not in _RELEASE_SOURCE_SELECTORS:
            return None, "unsupported_default", False
        if not _release_source_bound(tokens, calls, words[0]):
            return None, "unsupported_binding", False
        return selector.group(1), None, False
    if (declared_type in ("int", "int64", "double") and len(words) == 1
            and kinds == ["ident"] and _release_codegen_identifier(words[0])):
        value, reason = _release_symbolic_default(tokens, trusted, calls, words[0], declared_type)
        return value, reason, True
    value, reason = _release_literal_default(list(words), kinds, declared_type)
    return value, reason, False


def _release_branch_dependent(tokens, occurrences):
    """True when an occurrence sits in the producer's security guard and some
    branch of that conditional (an implicit empty #else included) lacks an
    identical (getter type, default) occurrence of the same title."""
    for declared_type, words, _, _, branches in occurrences:
        for opener, _, instance in branches:
            if opener != _RELEASE_SECURITY_GUARD:
                continue
            arms = tokens.groups.get(instance, [opener])
            if arms != [opener, ("else",)]:
                # Coverage needs the producer's explicit #else; otherwise a
                # branch (an implicit empty one included) is missing.
                return True
            for arm in arms:
                if not any(other[0] == declared_type and other[1] == words
                           and (opener, arm, instance) in other[4] for other in occurrences):
                    return True
    return False


def _release_legacy_declarations(cpp_text, receipt=None, *, allow_unresolved=False):
    """Typed legacy defaults certified by the positive allowlist above.

    Returns {native name: {"type", "default", "_raw_default", "_reason"}}; an
    uncertified default is None with a stable reason. A title read by getters
    that disagree, or whose getter depends on a security-guard branch, carries
    its resolvable `_candidates` for receipt arbitration in normalization.
    Raises ValueError for a reason unless allow_unresolved, and for known
    out-of-domain evidence."""
    try:
        tokens, trusted = _release_cpp_tokens(cpp_text)
    except _ReleaseUnreadable:
        if not allow_unresolved:
            raise
        return None
    calls, intact, macro_calls = _release_getter_calls(tokens)
    for _, _, declared_type, _, default in calls + macro_calls:
        if declared_type in ("int", "int64", "double"):
            # Concrete literal evidence meets the domain before any refusal.
            _release_literal_default([item[0] for item in default],
                                     [item[3] for item in default], declared_type)
    in_macros = {}
    for _, _, declared_type, spelling, default in macro_calls:
        try:
            name = _release_cpp_input_name(spelling)
        except ValueError:
            intact = False
            continue
        in_macros.setdefault(name, (declared_type, cpp_text[default[0][1]:default[-1][2]]))
    rows = {}
    for index, close, declared_type, spelling, default in calls:
        try:
            name = _release_cpp_input_name(spelling)
        except ValueError:
            intact = False
            continue
        words = [item[0] for item in default]
        raw = cpp_text[default[0][1]:default[-1][2]]
        rows.setdefault(name, []).append(
            (declared_type, tuple(words), raw, default, tokens[index][5]))
    declared = {}
    for name, occurrences in rows.items():
        declared_type, words, raw, default, _ = occurrences[0]
        metadata = {"type": declared_type, "default": None, "_raw_default": raw,
                    "_reason": None, "_types": sorted({item[0] for item in occurrences})}
        pairs = []
        for occurrence in occurrences:
            if all(occurrence[:2] != pair[:2] for pair in pairs):
                pairs.append(occurrence)
        if not intact or not trusted:
            metadata["_reason"] = "unsupported_binding"
        elif len(pairs) > 1 or _release_branch_dependent(tokens, occurrences):
            # Never first-wins: the native receipt must pick exactly one.
            candidates = []
            failures = []
            for pair_type, pair_words, pair_raw, pair_default, _ in pairs:
                value, reason, _ = _release_resolve_default(
                    tokens, trusted, calls, pair_type, list(pair_words), pair_default)
                if reason is not None:
                    failures.append(reason)
                elif all((pair_type, repr(value)) != (kind, repr(seen))
                         for kind, seen, _ in candidates):
                    candidates.append((pair_type, value, pair_raw))
            metadata["_candidates"] = candidates
            # With no readable declaration the first one's own refusal stands
            # (as before this round); otherwise the receipt has to choose.
            metadata["_reason"] = (failures[0] if failures and not candidates
                                   else "ambiguous_binding")
        else:
            metadata["default"], metadata["_reason"], symbolic = _release_resolve_default(
                tokens, trusted, calls, declared_type, list(words), default)
            if symbolic:
                metadata["_symbolic"] = True
        if name in in_macros:
            # A getter of this title sits in a macro argument, which the
            # replacement list may drop or repeat: no declaration to certify.
            metadata.pop("_candidates", None)
            metadata.pop("_symbolic", None)
            metadata["_reason"] = "macro_argument"
        if metadata["_reason"]:
            metadata["default"] = None
            if not allow_unresolved:
                raise ValueError(metadata["_reason"])
        declared[name] = metadata
    for name, (declared_type, raw) in in_macros.items():
        if name not in declared:
            if not allow_unresolved:
                raise ValueError("macro_argument")
            declared[name] = {"type": declared_type, "default": None, "_raw_default": raw,
                              "_reason": "macro_argument", "_types": [declared_type]}
    return declared


def _release_settings_receipt(lib, state, checked):
    if not hasattr(lib, "strategy_get_effective_settings"):
        return None
    try:
        required = ctypes.c_size_t()
        error = ctypes.create_string_buffer(_SETTINGS_ERROR_CAPACITY)
        getter = lib.strategy_get_effective_settings
        if getter(state, None, 0, ctypes.byref(required), error,
                  _SETTINGS_ERROR_CAPACITY) != PF_SETTINGS_BUFFER_TOO_SMALL:
            return None
        if not 0 < required.value <= 16 * 1024 * 1024:
            return None
        buffer = ctypes.create_string_buffer(required.value)
        if getter(state, buffer, required.value, ctypes.byref(required), error,
                  _SETTINGS_ERROR_CAPACITY) != PF_SETTINGS_OK:
            return None
        receipt = json.loads(buffer.value.decode("utf-8"))
        if not isinstance(receipt, dict) or receipt.get("version") != 1:
            return None
        return receipt
    except (AttributeError, TypeError, ValueError, RecursionError):
        return None


def _release_receipt_rows(receipt, section):
    rows = receipt.get(section)
    if not isinstance(rows, list):
        raise ValueError("missing checked settings section")
    result = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("name"), str):
            raise ValueError("malformed checked setting")
        name = row["name"]
        if name in result or type(row.get("supported")) is not bool:
            raise ValueError("ambiguous or malformed checked setting")
        if not all(isinstance(row.get(key), str)
                   for key in ("type", "default", "effective_value")):
            raise ValueError("malformed checked scalar")
        result[name] = row
    return result


class _ReleaseRefusal:
    """An uncertified legacy strategy() default: a stable reason and raw token."""

    __slots__ = ("reason", "raw")

    def __init__(self, reason, raw=None):
        self.reason = reason
        self.raw = raw


def _release_statements(tokens, start, end):
    """Top-level statements of a body, as token index lists (no directives)."""
    statements = []
    statement = []
    depth = 0
    for index in range(start, end):
        token = tokens[index]
        if token[4]:
            continue
        statement.append(index)
        if token[3] == "op" and token[0] in ("(", "[", "{"):
            depth += 1
        elif token[3] == "op" and token[0] in (")", "]", "}"):
            depth -= 1
            following = index + 1
            while following < end and tokens[following][4]:
                following += 1
            if (depth == 0 and token[0] == "}"
                    and (following >= end or tokens[following][0] not in (";", ",", ")"))):
                statements.append(statement)
                statement = []
        elif depth == 0 and token[0] == ";":
            statements.append(statement)
            statement = []
    if statement:
        statements.append(statement)
    return statements


def _release_strategy_value(key, rhs, enum_bound):
    """One recognized strategy() default, or a _ReleaseRefusal."""
    words = [token[0] for token in rhs]
    kinds = [token[3] for token in rhs]
    declared_type = _RELEASE_OVERRIDE_TYPES[key]
    if key in _RELEASE_STRATEGY_ENUMS:
        enum_name, members = _RELEASE_STRATEGY_ENUMS[key]
        names = _QTY_TYPE if key == "default_qty_type" else _COMM_TYPE
        inner = words
        if inner[:5] == ["static_cast", "<", "int", ">", "("] and inner[-1:] == [")"]:
            inner = inner[5:-1]
        for prefix in ([], ["::"], ["pineforge", "::"], ["::", "pineforge", "::"]):
            if (enum_bound and len(inner) == len(prefix) + 3
                    and inner[:len(prefix)] == prefix
                    and inner[len(prefix):len(prefix) + 2] == [enum_name, "::"]
                    and inner[-1] in members):
                return names[inner[-1]]
        if words in (["0"], ["1"], ["2"]) and kinds == ["number"]:
            return names[members[int(words[0])]]
        return _ReleaseRefusal("unsupported_default")
    if declared_type == "bool" or key == "close_entries_rule":
        # close_entries_rule_any is a native bool: only true/false are read.
        if words in (["true"], ["false"]):
            value = words == ["true"]
            return ("ANY" if value else "FIFO") if key == "close_entries_rule" else value
        return _ReleaseRefusal("unsupported_default")
    text = "".join(words)
    if (not 1 <= len(words) <= 2 or kinds[-1] != "number"
            or (len(words) == 2 and words[0] not in "+-")
            or not _RELEASE_DECIMAL.fullmatch(text)):
        return _ReleaseRefusal("unsupported_default")
    # A recognized literal is concrete evidence: its domain failure refuses.
    value = _release_domain(_release_scalar(text, declared_type))
    if declared_type == "int" and not -(2 ** 31) <= value < 2 ** 31:
        # PineStrategyConfig's pyramiding and slippage are native int.
        return _ReleaseRefusal("unsupported_default")
    return value


def _release_legacy_strategy(cpp_text):
    """strategy() defaults from the one generated constructor, by allowlist.

    Every release key maps to a typed value (the PineStrategyConfig seed when
    the constructor leaves it alone) or a _ReleaseRefusal. Recognized: one
    `GeneratedStrategy() [: init] { ... }` holding one
    `PineStrategyConfig cfg;` with top-level `cfg.<field> = <rhs>;` statements
    before its single closing `configure_pine_strategy(cfg);`. Pre-R4-C
    member writes and any other occurrence refuse."""
    keys = list(_RELEASE_OVERRIDE_TYPES)

    def refuse(reason):
        return {key: _ReleaseRefusal(reason) for key in keys}

    try:
        tokens, trusted = _release_cpp_tokens(cpp_text)
    except _ReleaseUnreadable:
        return refuse("unsupported_binding")
    if not trusted:
        return refuse("unsupported_binding")
    bodies = []
    for index, token in enumerate(tokens):
        previous = _release_previous(tokens, index)
        if (token[0] != "GeneratedStrategy" or token[3] != "ident" or token[4]
                or index + 1 >= len(tokens) or tokens[index + 1][0] != "("
                or (previous is not None and previous[0] == "~")):
            continue
        close = _release_closing(tokens, index + 1)
        if close is None or close + 1 >= len(tokens):
            return refuse("unsupported_binding")
        opening = None
        if tokens[close + 1][0] in ("{", ":") and close != index + 2:
            # The producer's constructor takes no parameters.
            return refuse("unsupported_binding")
        if tokens[close + 1][0] == "{":
            opening = close + 1
        elif tokens[close + 1][0] == ":":
            position = close + 2
            while position < len(tokens):
                if tokens[position][3] == "op" and tokens[position][0] in ("(", "{"):
                    if tokens[position][0] == "{" and tokens[position - 1][0] in (")", "}"):
                        opening = position
                        break
                    skipped = _release_closing(tokens, position, directives=True)
                    if skipped is None:
                        break
                    position = skipped + 1
                    continue
                position += 1
            if opening is None:
                return refuse("unsupported_binding")
        else:
            continue
        closing = _release_closing(tokens, opening, directives=True)
        if closing is None:
            return refuse("unsupported_binding")
        bodies.append((opening, closing))
    if len(bodies) != 1:
        return refuse("ambiguous_binding" if bodies else "unsupported_binding")
    opening, closing = bodies[0]
    # The engine's config type and its one configure call, never a local one.
    for index, token in enumerate(tokens):
        if token[0] == "PineStrategyConfig" and (
                token[4] or [item[0] for item in tokens[max(0, index - 4):index]]
                != ["pineforge", "::", "source", "::"]):
            return refuse("unsupported_binding")
    if sum(token[0] == "configure_pine_strategy" for token in tokens) > 1:
        return refuse("unsupported_binding")
    enum_bound = {}
    for key, (enum_name, members) in _RELEASE_STRATEGY_ENUMS.items():
        enum_bound[key] = all(
            not token[4] and index + 2 < len(tokens) and tokens[index + 1][0] == "::"
            and tokens[index + 2][0] in members
            for index, token in enumerate(tokens) if token[0] == enum_name)
    cfg_fields = dict(_CFG_FIELD_KEY, calc_on_order_fills="calc_on_order_fills")
    member_fields = dict(_STRAT_FIELD_KEY, calc_on_order_fills_="calc_on_order_fills")
    statements = _release_statements(tokens, opening + 1, closing)
    words_of = [[tokens[index][0] for index in statement] for statement in statements]
    declarations = []
    declaring = set()
    for number, (statement, words) in enumerate(zip(statements, words_of)):
        if "PineStrategyConfig" not in words:
            continue
        at = words.index("PineStrategyConfig")
        prefix, rest = words[:at], words[at + 1:]
        if (prefix in (["pineforge", "::", "source", "::"],
                       ["::", "pineforge", "::", "source", "::"])
                and len(rest) >= 2 and tokens[statement[at + 1]][3] == "ident"
                and rest[1:] in ([";"], ["{", "}", ";"], ["(", ")", ";"])
                and words.count("PineStrategyConfig") == 1):
            declarations.append(rest[0])
            declaring.add(number)
        else:
            return refuse("unsupported_binding")
    if len(declarations) > 1:
        return refuse("ambiguous_binding")
    variable = declarations[0] if declarations else None
    assigned = {}
    configured = 0
    for number, (statement, words) in enumerate(zip(statements, words_of)):
        if words in _RELEASE_ADAPTER_STATEMENTS:
            continue
        # Nothing else of the constructor may sit in a conditional, the
        # declaration included.
        if any(tokens[index][5] != () for index in statement):
            return refuse("unsupported_binding")
        if number in declaring:
            if assigned or configured:
                return refuse("unsupported_binding")
            continue
        if variable is not None and words == ["configure_pine_strategy", "(", variable, ")", ";"]:
            if number != len(statements) - 1:
                return refuse("unsupported_binding")
            configured += 1
            continue
        if (variable is not None and len(words) >= 6 and words[0] == variable
                and words[1] == "." and words[3] == "="
                and (words[2] in cfg_fields or words[2] in _RELEASE_CONFIG_OTHER)
                and words[-1] == ";" and words.count(variable) == 1
                and not any(word in member_fields for word in words)):
            if configured:
                return refuse("unsupported_binding")
            if words[2] in cfg_fields:
                assigned[cfg_fields[words[2]]] = statement[4:-1]
            continue
        # The producer's constructor holds nothing else: refuse the rest.
        return refuse("unsupported_binding")
    if variable is None or configured != 1:
        # Pre-R4-C member writes are not certified: nothing establishes which
        # member an unqualified field write reaches.
        return refuse("unsupported_binding")
    result = dict(STRATEGY_SEED, calc_on_order_fills=False)
    raws = {}
    for key in keys:
        if key in assigned:
            rhs = [tokens[index] for index in assigned[key]]
            value = _release_strategy_value(key, rhs, enum_bound.get(key, True))
            raws[key] = cpp_text[rhs[0][1]:rhs[-1][2]]
            if isinstance(value, _ReleaseRefusal):
                value.raw = raws[key]
            result[key] = value
    result["_raws"] = raws
    return result


def _release_legacy_receipt_rows(receipt, section="inputs"):
    """Legacy receipt rows by native name: (unique rows, native-ambiguous
    names with their number of distinct uncoerced (type, default) pairs, every
    row, or None when the section is missing or malformed). An empty list
    therefore means a completely parsed empty section, not a parse failure.
    A malformed row makes the receipt unusable; a name the receipt lists
    more than once with differing settings is native-ambiguous (several inputs
    share that title) and is never used to certify."""
    rows = receipt.get(section) if isinstance(receipt, dict) else None
    if not isinstance(rows, list):
        return {}, {}, None
    by_name = {}
    for row in rows:
        if (not isinstance(row, dict) or not isinstance(row.get("name"), str)
                or type(row.get("supported")) is not bool
                or not all(isinstance(row.get(key), str)
                           for key in ("type", "default", "effective_value"))):
            return {}, {}, None
        by_name.setdefault(row["name"], []).append(row)
    unique = {}
    ambiguous = {}
    for name, items in by_name.items():
        if len({(item["type"], item["default"], item["effective_value"], item["supported"])
                for item in items}) == 1:
            unique[name] = items[0]
        else:
            ambiguous[name] = len({(item["type"], item["default"]) for item in items})
    return unique, ambiguous, rows


def normalize_release_provenance(provenance, cpp_text, receipt, checked):
    """Normalize only the fresh release document, before its fingerprint."""
    # This is the entrypoint's internal run fact, set only by its successful
    # Pine transpile branch. Source contents never establish producer origin.
    producer_source = provenance["codegen"].get("transpiled_from_pine") is True
    if not producer_source and (not isinstance(receipt, dict)
                                or type(receipt.get("version")) is not int
                                or receipt["version"] != 1):
        receipt = None
    applied_inputs = provenance["applied"]["inputs"]
    applied_overrides = provenance["applied"]["overrides"]
    input_rows = {}
    override_rows = {}
    if checked:
        if receipt is None:
            raise ValueError("checked settings receipt unavailable")
        input_rows = _release_receipt_rows(receipt, "inputs")
        override_rows = _release_receipt_rows(receipt, "overrides")

    if checked:
        # Receipt names are native decoded identities. Getter regex captures
        # contain C++ escapes and must never be joined to these verbatim.
        declared = {}
        for name, row in input_rows.items():
            declared_type = ("source" if row.get("kind") == "source"
                             else row["type"])
            scalar_type = ("int" if declared_type == "enum" and row["supported"]
                           else declared_type)
            value = _release_scalar(row["effective_value"], scalar_type)
            # Unsupported means the setter cannot honour an override. Its
            # getter still evaluates the default; metadata can be a placeholder.
            default = (_release_scalar(row["default"], scalar_type)
                       if row["supported"] else value)
            if declared_type == "source" and not (
                    value in _RELEASE_SOURCE_SELECTORS and default in _RELEASE_SOURCE_SELECTORS):
                raise ValueError("noncanonical checked source selector")
            if not producer_source and row["supported"] is not True:
                declared[name] = {"type": declared_type, "default": None, "value": None,
                                  "resolution": {"status": "unresolved",
                                      "reason": "foreign_unverified_source",
                                      "raw_default": row["default"]}}
                continue
            declared[name] = {"type": declared_type, "default": default,
                              "value": value}
        for name, text in applied_inputs.items():
            # ctypes passes UTF-8 C strings: embedded NUL terminates a key.
            # Keep the raw applied key, but use the native key for its value.
            native_name = name.split("\0", 1)[0]
            if native_name in declared:
                if "resolution" not in declared[native_name]:
                    applied_inputs[name] = declared[native_name]["value"]
            else:
                declared[name] = {"type": "unknown", "default": None,
                                  "value": str(text)}
        provenance["inputs"] = declared
    else:
        native_inputs = {name.split("\0", 1)[0]: value.split("\0", 1)[0]
                         for name, value in applied_inputs.items()}
        legacy_rows, native_ambiguous, receipt_rows = _release_legacy_receipt_rows(receipt)
        _, _, override_receipt_rows = _release_legacy_receipt_rows(receipt, "overrides")
        # Known native scalars meet the product domain before any refusal.
        for row in (receipt_rows or []) + (override_receipt_rows or []):
            if row["type"] in ("int", "float") or (row["type"] == "enum" and row["supported"]):
                for key in ("default", "effective_value"):
                    # Parse by the row's own type (an integer row's leading
                    # zeros included) and as a number; check whatever parses.
                    native_value = _release_receipt_value(row[key], "float")
                    if native_value is not None:
                        _release_domain(native_value)
                        if row["type"] != "float" and native_value.is_integer():
                            _release_domain(int(native_value))
                    if row["type"] != "float":
                        exact = _release_receipt_value(row[key], "int")
                        if exact is not None:
                            _release_domain(exact)
        declared = _release_legacy_declarations(cpp_text, receipt, allow_unresolved=True)
        unreadable = declared is None
        declared = declared or {}
        for name, metadata in declared.items():
            raw_default = metadata.pop("_raw_default")
            reason = metadata.pop("_reason")
            symbolic = metadata.pop("_symbolic", False)
            candidates = metadata.pop("_candidates", None)
            # A numeric override the recognized getter consumes is concrete
            # evidence even when the default itself is refused.
            for kind in metadata.pop("_types", ()):
                if kind in ("int", "int64", "double") and name in native_inputs:
                    try:
                        converted = _release_legacy_number(native_inputs[name], kind)
                    except ValueError:
                        continue
                    _release_domain(_release_scalar(converted, kind))
            native = legacy_rows.get(name)
            distinct = native_ambiguous.get(name, 0)
            if name in native_ambiguous:
                # The native settings list this title more than once with
                # different settings: no single declaration to certify.
                candidates = None
                # TOP ruling 2026-10-07 22:58: two or more distinct native
                # (type, default) pairs share the title; name it, with the count.
                reason = "duplicate_title" if distinct >= 2 else reason or "ambiguous_binding"
            if candidates is not None and native is not None and native["supported"] is True:
                # The receipt arbitrates: exactly one (getter type, default)
                # must match its own type and default, without coercion.
                native_default = _release_receipt_value(native["default"], native["type"])
                matches = [(kind, value) for kind, value, _ in candidates
                           if native["type"] in _RELEASE_RECEIPT_TYPES[kind]
                           and type(native_default) is type(value) and native_default == value]
                if len(matches) == 1:
                    metadata["type"], metadata["default"] = matches[0]
                    reason = None
            declared_type = metadata["type"]
            default = metadata["default"]
            value = default
            if reason is None and name in native_inputs:
                text = native_inputs[name]
                if declared_type == "bool":
                    value = (True if text in ("true", "1") else
                             False if text in ("false", "0") else default)
                elif declared_type in ("int", "int64", "double"):
                    try:
                        value = _release_legacy_number(text, declared_type)
                    except ValueError:
                        value = default
                    value = _release_domain(_release_scalar(value, declared_type))
                elif declared_type == "source":
                    # The native getter falls back from any other selector.
                    if text not in _RELEASE_SOURCE_SELECTORS:
                        reason = "unsupported_override"
                    value = text
                else:
                    value = text
            if reason is None and native is not None:
                # Each side keeps its own declared type: never coerce one
                # through the other to hide a type, default or value conflict.
                if native["type"] not in _RELEASE_RECEIPT_TYPES[declared_type]:
                    reason = "ambiguous_binding"
                elif native["supported"] is True:
                    receipt_default = _release_receipt_value(native["default"], native["type"])
                    receipt_value = _release_receipt_value(
                        native["effective_value"], native["type"])
                    if (type(receipt_default) is not type(default) or receipt_default != default
                            or type(receipt_value) is not type(value) or receipt_value != value):
                        reason = "ambiguous_binding"
            if reason is None and symbolic and (native is None or native["supported"] is not True):
                reason = "receipt_unavailable"
            if not producer_source and reason != "duplicate_title":
                confirmed = (native is not None and native["supported"] is True
                             and native["type"] in _RELEASE_RECEIPT_TYPES[declared_type]
                             and type(_release_receipt_value(native["default"], native["type"])) is type(default)
                             and _release_receipt_value(native["default"], native["type"]) == default
                             and type(_release_receipt_value(native["effective_value"], native["type"])) is type(value)
                             and _release_receipt_value(native["effective_value"], native["type"]) == value)
                if not confirmed:
                    reason = "foreign_unverified_source"
            if reason is None:
                metadata["default"] = default
                metadata["value"] = value
            else:
                metadata.update(default=None, value=None, resolution={
                    "status": "unresolved", "reason": reason, "raw_default": raw_default})
                if reason == "duplicate_title":
                    metadata["resolution"]["distinct_native_inputs"] = distinct
        declared_names = frozenset(declared)
        for raw_name, text in applied_inputs.items():
            native_name = raw_name.split("\0", 1)[0]
            if native_name in declared_names:
                if "resolution" not in declared[native_name]:
                    applied_inputs[raw_name] = declared[native_name]["value"]
            elif unreadable or (not producer_source and (
                    receipt_rows is None or native_name in legacy_rows
                    or native_name in native_ambiguous)):
                declared[raw_name] = {"type": "unknown", "default": None, "value": None,
                                      "resolution": {"status": "unresolved",
                                                     "reason": ("unsupported_binding" if producer_source
                                                                else "foreign_unverified_source"),
                                                     "raw_default": None}}
            else:
                declared[raw_name] = {"type": "unknown", "default": None,
                                      "value": str(text)}
        provenance["inputs"] = declared

    legacy_defaults = None if checked else _release_legacy_strategy(cpp_text)
    legacy_raws = legacy_defaults.pop("_raws", {}) if legacy_defaults else {}
    legacy_overrides, ambiguous_overrides, _ = (
        ({}, frozenset(), []) if checked else _release_legacy_receipt_rows(receipt, "overrides"))
    unresolved = {}
    for name, declared_type in _RELEASE_OVERRIDE_TYPES.items():
        if checked:
            row = override_rows.get(name)
            if (row is None or row["type"] != declared_type
                    or row["supported"] is not True):
                raise ValueError("declared override missing or mistyped in receipt")
            value = _release_scalar(row["effective_value"], declared_type)
            if name in _RELEASE_ENUM_WORDS and value not in _RELEASE_ENUM_WORDS[name]:
                raise ValueError("noncanonical checked strategy enum")
        else:
            value = legacy_defaults[name]
            if not isinstance(value, _ReleaseRefusal):
                value = _release_scalar(value, declared_type)
            # A later alias may be a no-op. Replay the setter order instead
            # of collapsing aliases and losing the preceding accepted value.
            for raw_name, raw_text in applied_overrides.items():
                if raw_name.split("\0", 1)[0] != name:
                    continue
                text = raw_text.split("\0", 1)[0]
                if declared_type == "bool":
                    value = text in ("true", "1")
                elif name == "close_entries_rule":
                    value = "ANY" if text in ("ANY", "any", "1") else "FIFO"
                elif name in _RELEASE_ENUM_WORDS:
                    prefix = ("strategy.commission." if name == "commission_type"
                              else "strategy.")
                    for index, word in enumerate(_RELEASE_ENUM_WORDS[name]):
                        if text in (word, prefix + word, str(index)):
                            value = word
                            break
                else:
                    converted = _release_legacy_number(text, declared_type)
                    ignored = (converted < 0 if declared_type == "int"
                               else math.isnan(converted))
                    if not ignored:
                        value = _release_domain(_release_scalar(converted, declared_type))
            native_override = legacy_overrides.get(name)
            if not isinstance(value, _ReleaseRefusal) and (
                    name in ambiguous_overrides or (
                        native_override is not None and native_override["supported"] is True
                        and (native_override["type"] != declared_type
                             or type(_release_receipt_value(native_override["effective_value"],
                                                            declared_type)) is not type(value)
                             or _release_receipt_value(native_override["effective_value"],
                                                       declared_type) != value))):
                # The native receipt disagrees: no strategy() value is certified.
                value = _ReleaseRefusal("ambiguous_binding", legacy_raws.get(name))
            if not producer_source:
                confirmed = (not isinstance(value, _ReleaseRefusal)
                             and native_override is not None and native_override["supported"] is True
                             and native_override["type"] == declared_type
                             and type(_release_receipt_value(native_override["effective_value"],
                                                            declared_type)) is type(value)
                             and _release_receipt_value(native_override["effective_value"],
                                                       declared_type) == value)
                if not confirmed:
                    value = _ReleaseRefusal("foreign_unverified_source", legacy_raws.get(name))
            if isinstance(value, _ReleaseRefusal):
                # Applied overrides keep their wire strings; no value is guessed.
                provenance["strategy"][name] = None
                unresolved[name] = {"status": "unresolved", "reason": value.reason,
                                    "raw_default": value.raw}
                continue
        provenance["strategy"][name] = value
        for raw_name in applied_overrides:
            if raw_name.split("\0", 1)[0] == name:
                applied_overrides[raw_name] = value
    if unresolved:
        provenance["strategy_resolution"] = unresolved
    return provenance


# --- The failure line -------------------------------------------------------
#
# Every failure prints ONE line on stdout, written by failure_line:
#     {"engine":"pineforge","error":"<text>","code":"<code>","args":{...}}
# "engine" and "error" lead, so the line still starts with
# {"engine":"pineforge","error":" and "error" is the English text, unchanged
# wherever one existed before codes. "code" is a code of the closed vocabulary
# docker/run_failure_codes.json and "args" its typed arguments. The code is the
# engine's (strategy_get_last_error_code / _args), or run_json's own for a
# failure it finds itself (RunFailure): never read from any text. A run failure
# from a library without the code getter prints neither key, so that line is
# the earlier one byte for byte (a consumer keeps its text rules for it).
#
# Caps keep the line well inside the 64 KiB a consumer reads: the text is cut at
# 16 KiB and each string argument at 1 KiB of UTF-8, on a character boundary;
# a line still over 60 KiB (only text that JSON escapes heavily, control
# characters are six bytes each) has its text cut further until it fits, once
# the arguments are dropped when they alone overflow it. A lone surrogate (a
# non-UTF-8 byte Python kept from the command line or a path) is printed as
# U+FFFD, never as an escape such as \udcff that a strict JSON parser rejects.

ERROR_TEXT_MAX = 16 * 1024
ERROR_ARG_TEXT_MAX = 1024
ERROR_LINE_MAX = 60 * 1024
_CODE_RE = re.compile(r"[a-z][a-z0-9_]{2,47}")
_LONE_SURROGATE_RE = re.compile("[\ud800-\udfff]")

# A failed run that reported neither a text nor a code (strategy_last_run_status
# 1 and nothing else): the line's text, with run_json's own
# engine_unclassified_error when the library has the code getter, without a code
# when it has none.
RUN_STATUS_FAILED_TEXT = "the run did not complete and the engine reported no error"


class RunFailure(Exception):
    """A failure run_json reports itself: the line's text, its code, the code's
    typed arguments and the exit status. The arguments are code_args because
    BaseException.args is the exception's own tuple. The code is chosen where
    the failure is raised, never derived from a text."""

    def __init__(self, text, code, code_args=None, *, exit_status=1):
        super().__init__(text)
        self.code = code
        self.code_args = dict(code_args or {})
        self.exit_status = exit_status


class StrategyLibraryError(RunFailure, RuntimeError):
    """The strategy library does not match the harness (a load failure, an ABI
    or a missing export): strategy_library_incompatible."""

    def __init__(self, text, code_args):
        super().__init__(text, "strategy_library_incompatible", code_args)


def _cut_utf8(text: str, limit: int) -> str:
    """text cut to at most limit bytes of UTF-8, on a character boundary (a lone
    surrogate counts as its three bytes and survives)."""
    raw = text.encode("utf-8", "surrogatepass")
    if len(raw) <= limit:
        return text
    while limit > 0 and (raw[limit] & 0xC0) == 0x80:  # inside a character
        limit -= 1
    return raw[:limit].decode("utf-8", "surrogatepass")


def _no_surrogates(text: str) -> str:
    """text with every lone surrogate replaced by U+FFFD (three UTF-8 bytes, as
    the surrogate counted), so json.dump never writes an escape such as \\udcff."""
    return _LONE_SURROGATE_RE.sub("\ufffd", text)


def _dump_line(doc: dict) -> str:
    # The writer the failure line has always used: json.dump with these
    # separators and json's default ensure_ascii, so the line is ASCII.
    out = io.StringIO()
    json.dump(doc, out, separators=(",", ":"))
    return out.getvalue()


def failure_line(text, code=None, code_args=None) -> str:
    """The one failure line, newline included. Without a code (a run error from
    a library without strategy_get_last_error_code) it is exactly the earlier
    {"engine":"pineforge","error":"<text>"} line. When the line is over
    ERROR_LINE_MAX, arguments that overflow it on their own are dropped first
    (the code stays), then the text is cut to the longest prefix that fits."""
    full = _no_surrogates(str(text))
    doc = {"engine": "pineforge", "error": _cut_utf8(full, ERROR_TEXT_MAX)}
    if code is not None:
        doc["code"] = code
        doc["args"] = {
            (_no_surrogates(name) if isinstance(name, str) else name):
            (_cut_utf8(_no_surrogates(value), ERROR_ARG_TEXT_MAX)
             if isinstance(value, str) else value)
            for name, value in (code_args or {}).items()}
    line = _dump_line(doc)
    if len(line) > ERROR_LINE_MAX:
        if code is not None and len(_dump_line({**doc, "error": ""})) > ERROR_LINE_MAX:
            doc["args"] = {}  # arguments no registry could hold: the code stays
        low, high = 0, len(doc["error"].encode("utf-8", "surrogatepass"))
        while low < high:  # the longest cut whose line fits
            mid = (low + high + 1) // 2
            doc["error"] = _cut_utf8(full, mid)
            if len(_dump_line(doc)) <= ERROR_LINE_MAX:
                low = mid
            else:
                high = mid - 1
        doc["error"] = _cut_utf8(full, low)
        line = _dump_line(doc)
    return line + "\n"


def write_failure(text, code=None, code_args=None) -> None:
    sys.stdout.write(failure_line(text, code, code_args))
    sys.stdout.flush()


def _c_text(raw) -> str:
    """A const char* result (bytes, or None for NULL) as text."""
    if raw is None:
        return ""
    if isinstance(raw, str):
        return raw
    return bytes(raw).decode("utf-8", "replace")


def _engine_args(raw) -> dict:
    """strategy_get_last_error_args re-parsed: a JSON object whose values are all
    strings, integers, finite numbers, booleans or null; anything else is {}."""
    try:
        doc = json.loads(raw.decode("utf-8") if isinstance(raw, bytes) else raw)
    except (TypeError, ValueError, AttributeError, RecursionError):
        return {}
    if not isinstance(doc, dict):
        return {}
    for value in doc.values():
        if isinstance(value, float) and not math.isfinite(value):
            return {}
        if value is not None and not isinstance(value, (str, int, float)):
            return {}
    return doc


def engine_failure_code(lib, strat):
    """The engine's (code, args) for the last failure on strat: None when the
    library has no strategy_get_last_error_code, ("", {}) when it recorded no
    failure. A code that is not a code name reads engine_unclassified_error."""
    if not hasattr(lib, "strategy_get_last_error_code"):
        return None
    code = _c_text(lib.strategy_get_last_error_code(strat))
    if not code:
        return "", {}
    if not _CODE_RE.fullmatch(code):
        return "engine_unclassified_error", {}
    args = {}
    if hasattr(lib, "strategy_get_last_error_args"):
        args = _engine_args(lib.strategy_get_last_error_args(strat))
    return code, args


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
        ("open_at_end",     ctypes.c_int32),   # ABI v3: range-end close row
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
        # The C field names. _stats_dict writes these two under their JSON
        # report keys sharpe_tv / sortino_tv (ADR-0001, "Deprecated public
        # spellings"), so the report schema does not change.
        ("sharpe_monthly", ctypes.c_double), ("sortino_monthly", ctypes.c_double),
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
        ("broker_state_hash",            ctypes.POINTER(ctypes.c_uint64)),
        ("broker_state_hash_len",        ctypes.c_int64),
    ]


class OutputEventC(ctypes.Structure):
    """Mirror of pf_output_event_v1_t (strategy_outputs_event_get)."""
    _fields_ = [
        ("struct_version", ctypes.c_uint32),
        ("size",           ctypes.c_uint32),
        ("sequence",       ctypes.c_uint64),
        ("output_index",   ctypes.c_int32),
        ("bar_index",      ctypes.c_int32),
        ("bar_open_ms",    ctypes.c_int64),
        ("bar_close_ms",   ctypes.c_int64),
        ("ordinal_in_bar", ctypes.c_uint32),
        ("phase",          ctypes.c_uint32),
        ("confirmed",      ctypes.c_uint32),
        ("reserved0",      ctypes.c_uint32),
        ("value",          ctypes.c_double),
        ("message_hash64", ctypes.c_uint64),
        ("message",        ctypes.c_char_p),
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
# v4 appended the live-runtime accessors and grew pf_report_t with the
# broker_state_hash array after equity_curve_len (ReportC above already
# carries both fields).
EXPECTED_PF_ABI = 4


def check_abi(lib: ctypes.CDLL) -> None:
    try:
        lib.pf_abi_version.restype = ctypes.c_int
        abi = lib.pf_abi_version()
    except AttributeError:
        raise StrategyLibraryError(
            "strategy .so predates pf_abi_version (ABI v1); rebuild it against "
            "the current pineforge runtime (pf_report_t grew).",
            {"reason": "abi_missing"}) from None
    if abi != EXPECTED_PF_ABI:
        raise StrategyLibraryError(
            f"pineforge ABI mismatch: .so reports {abi}, harness expects "
            f"{EXPECTED_PF_ABI}; rebuild.",
            {"reason": "abi_mismatch", "abi": int(abi)})


# --- helpers ----------------------------------------------------------

class ChartBarsError(RunFailure, ValueError):
    """An --ohlcv tape the harness cannot read: chart_bars_unreadable{reason}."""

    def __init__(self, text, reason):
        super().__init__(text, "chart_bars_unreadable", {"reason": reason})


def _bars_unreadable(text: str, reason: str) -> ChartBarsError:
    return ChartBarsError(text, reason)


def load_bars(csv_path: Path) -> tuple[ctypes.Array, int, str]:
    """Load the source tape once and return bars, count, and canonical hash.
    A file it cannot read is a ChartBarsError (a ValueError),
    chart_bars_unreadable{reason}: io (the file cannot be opened or read),
    columns (a row lacks a column), value (a value is not a number, or the file
    is not a UTF-8 CSV). An empty tape is returned as zero bars (main refuses
    it: reason empty)."""
    rows: list[tuple[float, float, float, float, float, int]] = []
    feed_hasher = _new_source_feed_hasher()
    try:
        with csv_path.open(newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    parsed = (
                        float(row["open"]),
                        float(row["high"]),
                        float(row["low"]),
                        float(row["close"]),
                        float(row["volume"]),
                        int(row["timestamp"]),
                    )
                except KeyError as e:
                    raise _bars_unreadable(
                        f"--ohlcv: {csv_path}: no column {e.args[0]}", "columns") from None
                except (TypeError, ValueError):
                    raise _bars_unreadable(
                        f"--ohlcv: {csv_path} line {reader.line_num}: not a number",
                        "value") from None
                _update_source_feed_hash(feed_hasher, parsed)
                rows.append(parsed)
    except OSError as e:
        raise _bars_unreadable(f"--ohlcv: {csv_path}: {e.strerror or e}", "io") from None
    except (UnicodeDecodeError, csv.Error) as e:
        raise _bars_unreadable(f"--ohlcv: {csv_path}: not a UTF-8 CSV ({e})", "value") from None
    n = len(rows)
    bars = (BarC * n)()
    for i, (o, h, l, c, v, ts) in enumerate(rows):
        bars[i].open      = o
        bars[i].high      = h
        bars[i].low       = l
        bars[i].close     = c
        bars[i].volume    = v
        bars[i].timestamp = ts
    return bars, n, feed_hasher.hexdigest()


# The exports every run calls; a library without one is
# strategy_library_incompatible{reason: symbol_missing, missing: <name>}.
_REQUIRED_EXPORTS = ("strategy_create", "strategy_set_input", "strategy_set_override",
                     "run_backtest_full", "strategy_free", "report_free")


def load_strategy(so_path: Path) -> ctypes.CDLL:
    try:
        lib = ctypes.CDLL(str(so_path))
    except OSError as e:
        raise StrategyLibraryError(f"cannot load the strategy library: {e}",
                                   {"reason": "load_failed"}) from None
    check_abi(lib)
    for name in _REQUIRED_EXPORTS:
        if not hasattr(lib, name):
            raise StrategyLibraryError(f"the strategy library has no {name}",
                                       {"reason": "symbol_missing", "missing": name})

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
    # The failure's code and arguments (engine 1.4.0+), and whether the last
    # run completed (ABI v4). hasattr-guarded: an older library has none.
    for _n in ("strategy_get_last_error_code", "strategy_get_last_error_args"):
        if hasattr(lib, _n):
            getattr(lib, _n).argtypes = [ctypes.c_void_p]
            getattr(lib, _n).restype = ctypes.c_char_p
    if hasattr(lib, "strategy_last_run_status"):
        lib.strategy_last_run_status.argtypes = [ctypes.c_void_p]
        lib.strategy_last_run_status.restype = ctypes.c_int
    if hasattr(lib, "strategy_closed_trade_entry_incarnation"):
        lib.strategy_closed_trade_entry_incarnation.argtypes = [
            ctypes.c_void_p, ctypes.c_int]
        lib.strategy_closed_trade_entry_incarnation.restype = ctypes.c_uint64
    # The checked settings API of a generated strategy (docs/checked-settings.md),
    # used when present (see uses_checked_settings).
    if hasattr(lib, "strategy_settings_api_version"):
        lib.strategy_settings_api_version.argtypes = []
        lib.strategy_settings_api_version.restype = ctypes.c_uint32
    if hasattr(lib, "strategy_create_checked"):
        lib.strategy_create_checked.argtypes = [
            ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p, ctypes.c_size_t]
        lib.strategy_create_checked.restype = ctypes.c_int
    for _n in ("strategy_set_input_checked", "strategy_set_override_checked"):
        if hasattr(lib, _n):
            getattr(lib, _n).argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                                         ctypes.c_char_p, ctypes.c_size_t]
            getattr(lib, _n).restype = ctypes.c_int
    if hasattr(lib, "strategy_get_effective_settings"):
        lib.strategy_get_effective_settings.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
            ctypes.c_char_p, ctypes.c_size_t]
        lib.strategy_get_effective_settings.restype = ctypes.c_int

    # syminfo setters — declare argtypes so ctypes does not default the float
    # args to c_int (which would truncate mintick=0.5 to 0). Guarded with
    # hasattr in case an older strategy.so predates these symbols.
    for _n in ("strategy_set_syminfo_mintick", "strategy_set_syminfo_pointvalue"):
        if hasattr(lib, _n):
            getattr(lib, _n).argtypes = [ctypes.c_void_p, ctypes.c_double]
    for _n in ("strategy_set_syminfo_timezone", "strategy_set_syminfo_session"):
        if hasattr(lib, _n):
            getattr(lib, _n).argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    if hasattr(lib, "strategy_set_syminfo_metadata"):
        lib.strategy_set_syminfo_metadata.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
        lib.strategy_set_syminfo_metadata.restype = None
    # Other symbols' data for request.security (engine 1.0.0+, see
    # load_symbol_feeds). hasattr-guarded: install_symbol_feeds fails by name
    # on a library without them.
    if hasattr(lib, "strategy_set_symbol_feed"):
        lib.strategy_set_symbol_feed.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.POINTER(BarC), ctypes.POINTER(ctypes.c_int64), ctypes.c_int]
        lib.strategy_set_symbol_feed.restype = ctypes.c_int
    if hasattr(lib, "strategy_set_symbol_facts"):
        lib.strategy_set_symbol_facts.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
        lib.strategy_set_symbol_facts.restype = ctypes.c_int

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

    # Recorded outputs (--outputs). The readers are runtime exports of every
    # library built on an engine that has them; the manifest and the version
    # are a recording library's own. All hasattr-guarded: require_outputs
    # refuses --outputs by name on a library that lacks them.
    _bind_outputs(lib)

    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.report_free.argtypes   = [ctypes.POINTER(ReportC)]
    return lib


def _bind_outputs(lib) -> None:
    signatures = {
        "strategy_outputs_set_enabled": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_int]),
        "strategy_outputs_series_count": (ctypes.c_int, [ctypes.c_void_p]),
        "strategy_outputs_bars_len": (ctypes.c_int64, [ctypes.c_void_p]),
        "strategy_outputs_bar_times_copy": (ctypes.c_int, [
            ctypes.c_void_p, ctypes.c_int64, ctypes.POINTER(ctypes.c_int64),
            ctypes.POINTER(ctypes.c_int64), ctypes.c_int64, ctypes.POINTER(ctypes.c_int64)]),
        "strategy_outputs_series_copy": (ctypes.c_int, [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int64, ctypes.POINTER(ctypes.c_double),
            ctypes.c_int64, ctypes.POINTER(ctypes.c_int64)]),
        "strategy_outputs_events_len": (ctypes.c_int, [ctypes.c_void_p]),
        "strategy_outputs_event_get": (ctypes.c_int, [
            ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(OutputEventC), ctypes.c_size_t]),
        "strategy_outputs_events_clear": (None, [ctypes.c_void_p]),
        "strategy_outputs_constants_copy": (ctypes.c_int, [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.c_int]),
        "strategy_outputs_api_version": (ctypes.c_uint32, []),
        "strategy_outputs_manifest": (ctypes.c_int, [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
            ctypes.c_char_p, ctypes.c_size_t]),
        "strategy_signal_safety_receipt": (ctypes.c_int, [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
            ctypes.c_char_p, ctypes.c_size_t]),
    }
    for name, (restype, argtypes) in signatures.items():
        if hasattr(lib, name):
            fn = getattr(lib, name)
            fn.restype = restype
            fn.argtypes = argtypes


# --- Creating a strategy and applying the run's settings ----------------------
#
# A library exporting the checked settings API (strategy_settings_api_version()
# == 1, docs/checked-settings.md) is created and configured through it, so a
# setting the strategy cannot honour fails the run before it starts: an unknown
# input or override key, an invalid enum, an unparseable value, which the
# legacy setters drop silently. A library exporting only part of it, or another
# version of it, is refused before any strategy exists
# (strategy_library_incompatible{reason: settings_api_mismatch}); a library
# exporting none of it keeps the legacy setters.

PF_SETTINGS_OK = 0
PF_SETTINGS_INVALID_ARGUMENT = 1
PF_SETTINGS_UNSUPPORTED = 2
PF_SETTINGS_BUFFER_TOO_SMALL = 4
_SETTINGS_ERROR_CAPACITY = 4096
_CHECKED_SETTINGS_EXPORTS = ("strategy_settings_api_version", "strategy_create_checked",
                             "strategy_set_input_checked", "strategy_set_override_checked")

# The checked setters' own messages (checked_settings.hpp and the generated
# setters, statuses INVALID_ARGUMENT / UNSUPPORTED) -> setting_rejected's reason.
# The message is the API's, never the value the request supplied.
SETTING_REJECTED_REASONS = {
    "expected an integer": "expected_integer",
    "invalid integer exponent": "invalid_integer_exponent",
    "invalid integer or trailing bytes": "invalid_integer_or_trailing_bytes",
    "expected an integral value": "expected_integral_value",
    "integer out of range": "integer_out_of_range",
    "invalid integer sign": "invalid_integer_sign",
    "expected a finite decimal number": "expected_finite_decimal",
    "invalid numeric exponent": "invalid_numeric_exponent",
    "invalid number or trailing bytes": "invalid_number_or_trailing_bytes",
    "number out of finite range": "number_out_of_finite_range",
    "number underflows to zero": "number_underflows_to_zero",
    "number cannot be consumed by the strategy getter": "number_not_consumable",
    "invalid boolean": "invalid_boolean",
    "invalid enum option": "invalid_enum_option",
    "value below minimum": "value_below_minimum",
    "value above maximum": "value_above_maximum",
    "invalid input option": "invalid_input_option",
    "unknown input key": "unknown_key",
    "unknown override key": "unknown_key",
    "ambiguous input key": "ambiguous_key",
}
# A declared input the compiled strategy cannot honour: setting_unsupported.
SETTING_UNSUPPORTED_MESSAGE = "input cannot be honoured by this compiled strategy"
# The setters' host-contract checks: a harness or engine fault, never the
# request's (run_json configures a fresh handle with non-null arguments).
SETTING_INVARIANT_MESSAGES = frozenset({
    "settings are frozen after execution begins",
    "input was not installed",
    "null strategy, key or value",
})


# A library exporting part of the checked settings API, or another version of
# it: strategy_library_incompatible{reason: settings_api_mismatch}.
SETTINGS_API_MISMATCH_TEXT = (
    "checked settings API mismatch: the strategy library must export "
    "strategy_settings_api_version() == 1, strategy_create_checked, "
    "strategy_set_input_checked and strategy_set_override_checked, or none of them; "
    "rebuild.")


def uses_checked_settings(lib) -> bool:
    """True when lib exports the checked settings API at version 1, False when it
    exports none of _CHECKED_SETTINGS_EXPORTS (the legacy setters run). Some but
    not all of them, or a strategy_settings_api_version() other than 1, is a
    StrategyLibraryError settings_api_mismatch: falling back to the legacy setters
    would drop silently the settings the checked API exists to refuse."""
    present = [name for name in _CHECKED_SETTINGS_EXPORTS if hasattr(lib, name)]
    if not present:
        return False
    if (len(present) != len(_CHECKED_SETTINGS_EXPORTS)
            or lib.strategy_settings_api_version() != 1):
        raise StrategyLibraryError(SETTINGS_API_MISMATCH_TEXT,
                                   {"reason": "settings_api_mismatch"})
    return True


def create_strategy(lib, checked: bool):
    """A new strategy handle, through strategy_create_checked when checked. A
    creation that fails (a NULL handle, or a checked status other than OK) is
    strategy_create_failed, whatever the cause: run_json reads no code from the
    checked API's message, it only shows it."""
    if not checked:
        handle = lib.strategy_create(b"{}")
        if not handle:
            raise RunFailure("strategy_create failed", "strategy_create_failed")
        return handle
    out = ctypes.c_void_p()
    error = ctypes.create_string_buffer(_SETTINGS_ERROR_CAPACITY)
    status = lib.strategy_create_checked(None, ctypes.byref(out), error,
                                         _SETTINGS_ERROR_CAPACITY)
    if status == PF_SETTINGS_OK and out.value:
        return out.value
    if out.value:
        lib.strategy_free(out.value)
    message = _c_text(error.value)
    raise RunFailure("strategy_create failed" + (f": {message}" if message else ""),
                     "strategy_create_failed")


def receipt_input_titles(lib, strat) -> frozenset:
    """The input titles strategy_get_effective_settings lists (each the literal
    the transpiler emitted), or none when the receipt is unavailable."""
    if not hasattr(lib, "strategy_get_effective_settings"):
        return frozenset()
    try:
        required = ctypes.c_size_t(0)
        error = ctypes.create_string_buffer(_SETTINGS_ERROR_CAPACITY)
        if lib.strategy_get_effective_settings(
                strat, None, 0, ctypes.byref(required), error,
                _SETTINGS_ERROR_CAPACITY) != PF_SETTINGS_BUFFER_TOO_SMALL:
            return frozenset()
        receipt = ctypes.create_string_buffer(required.value)
        if lib.strategy_get_effective_settings(
                strat, receipt, required.value, ctypes.byref(required), error,
                _SETTINGS_ERROR_CAPACITY) != PF_SETTINGS_OK:
            return frozenset()
        rows = json.loads(receipt.value.decode("utf-8"))["inputs"]
        return frozenset(row["name"] for row in rows
                         if isinstance(row, dict) and isinstance(row.get("name"), str))
    except (TypeError, ValueError, KeyError, AttributeError, RecursionError):
        return frozenset()


def setting_failure(lib, strat, entrypoint: str, key: str, status: int,
                    message: str) -> RunFailure:
    """The failure of a checked setter that returned status with message. The
    text is "<entrypoint>: <message>", as the generated setters latch theirs.
    setting_rejected names its reason (SETTING_REJECTED_REASONS; another
    INVALID_ARGUMENT message is unparseable_value) and, for an input, the input
    when the key is a title the receipt lists; setting_unsupported has no
    arguments (also for another UNSUPPORTED message); an exception or a latched
    failure (RUN_FAILED) is engine_unclassified_error."""
    text = f"{entrypoint}: {message}"
    if status not in (PF_SETTINGS_INVALID_ARGUMENT, PF_SETTINGS_UNSUPPORTED):
        return RunFailure(text, "engine_unclassified_error")
    if message in SETTING_INVARIANT_MESSAGES:
        return RunFailure(text, "engine_invariant")
    reason = SETTING_REJECTED_REASONS.get(message)
    if message == SETTING_UNSUPPORTED_MESSAGE or (
            reason is None and status == PF_SETTINGS_UNSUPPORTED):
        return RunFailure(text, "setting_unsupported")
    args = {"entrypoint": entrypoint, "reason": reason or "unparseable_value"}
    if entrypoint == "strategy_set_input" and key in receipt_input_titles(lib, strat):
        args["input"] = key
    return RunFailure(text, "setting_rejected", args)


def apply_settings(lib, strat, inputs: dict, overrides: dict, checked: bool) -> None:
    """Every input, then every override. Through the checked setters the first
    one refused fails the run (setting_failure); the legacy setters report
    nothing here."""
    if not checked:
        for k, v in inputs.items():
            lib.strategy_set_input(strat, k.encode(), v.encode())
        for k, v in overrides.items():
            lib.strategy_set_override(strat, k.encode(), v.encode())
        return
    for entrypoint, setter, settings in (
            ("strategy_set_input", lib.strategy_set_input_checked, inputs),
            ("strategy_set_override", lib.strategy_set_override_checked, overrides)):
        for key, value in settings.items():
            error = ctypes.create_string_buffer(_SETTINGS_ERROR_CAPACITY)
            status = setter(strat, key.encode(), value.encode(), error,
                            _SETTINGS_ERROR_CAPACITY)
            if status != PF_SETTINGS_OK:
                raise setting_failure(lib, strat, entrypoint, key, status,
                                      _c_text(error.value))


class SyminfoError(RunFailure, ValueError):
    """A --syminfo file the harness cannot apply as given, with its code:
    lot_grid_rejected, syminfo_unreadable{reason} or
    strategy_library_incompatible{reason: setter_missing, missing}. main()
    reports it as the one failure line (exit 1), never as a traceback."""


class OutputsError(RunFailure, ValueError):
    """An --outputs the harness cannot honour, with its code: outputs_rejected
    {reason} for a library that records no outputs (or whose switch the engine
    refuses: the engine's own code), strategy_library_incompatible{reason:
    outputs_api_mismatch | outputs_manifest_invalid} for a recording library
    whose outputs exports the harness cannot read. main() reports it as the
    one failure line (exit 1)."""


# This file is vendored: pineforge-release copies it from the pineforge-engine
# tag at every release. Keep the lot-grid handling (mincontract) identical in
# both repos, or a sync drops it.
def apply_syminfo(lib, strat, syminfo_path):
    """Apply syminfo.json (data-worker schema) via strategy_set_syminfo_*.
    Tolerant: missing keys skipped. Accepts {"syminfo": {...}} or a flat dict.

    mincontract (TradingView's syminfo.mincontract, the instrument's lot size)
    is strict: absent or null applies nothing; anything else must be a positive
    finite JSON number, else SyminfoError lot_grid_rejected before any setter
    runs. A valid one is set first, as the metadata key qty_step (the engine
    floors order quantities to that grid) and as mincontract (what
    syminfo.mincontract reads return).

    Every value is read before any setter runs. A file that cannot be read, is
    not JSON or holds no syminfo object, and a mintick, pointvalue, timezone or
    session the setters cannot take, is SyminfoError
    syminfo_unreadable{reason: io|not_json|not_object|value_type}; a library
    without the setter a key needs is strategy_library_incompatible{reason:
    setter_missing, missing}.
    Returns what main() records in applied_runtime["syminfo"]:
    {"qty_step": v, "mincontract": v}, or {} when no grid was applied."""
    def unreadable(text, reason):
        return SyminfoError(text, "syminfo_unreadable", {"reason": reason})

    def setter(name, key):
        if not hasattr(lib, name):
            raise SyminfoError(
                f"the strategy library has no {name}, so syminfo.{key} cannot be applied",
                "strategy_library_incompatible", {"reason": "setter_missing", "missing": name})
        return getattr(lib, name)

    try:
        with open(syminfo_path) as f:
            text = f.read()
    except OSError as e:
        raise unreadable(f"--syminfo: {syminfo_path}: {e.strerror or e}", "io") from None
    except ValueError as e:  # not text in the file's encoding
        raise unreadable(f"--syminfo: {syminfo_path} is not JSON: {e}", "not_json") from None
    try:
        doc = json.loads(text)
    except (ValueError, RecursionError) as e:
        raise unreadable(f"--syminfo: {syminfo_path} is not JSON: {e}", "not_json") from None
    si = doc.get("syminfo", doc) if isinstance(doc, dict) else None
    if not isinstance(si, dict):
        raise unreadable(f"--syminfo: {syminfo_path}: the syminfo is not a JSON object",
                         "not_object")
    applied = {}
    calls = []
    lot = si.get("mincontract")
    if lot is not None:
        try:
            v = (float(lot) if isinstance(lot, (int, float))
                 and not isinstance(lot, bool) else math.nan)
        except OverflowError:  # an int beyond binary64
            v = math.nan
        if not (math.isfinite(v) and v > 0):
            raise SyminfoError(
                "syminfo.mincontract must be a positive finite number, got "
                + json.dumps(lot)[:80], "lot_grid_rejected")
        meta = setter("strategy_set_syminfo_metadata", "mincontract")
        calls += [(meta, b"qty_step", v), (meta, b"mincontract", v)]
        applied = {"qty_step": v, "mincontract": v}
    for key, name, present, convert, kind in (
            ("mintick", "strategy_set_syminfo_mintick", "mintick" in si, float, "a number"),
            ("pointvalue", "strategy_set_syminfo_pointvalue", "pointvalue" in si, float,
             "a number"),
            ("timezone", "strategy_set_syminfo_timezone", bool(si.get("timezone")),
             lambda x: str(x).encode(), "UTF-8 text"),
            ("session", "strategy_set_syminfo_session", bool(si.get("session")),
             lambda x: str(x).encode(), "UTF-8 text")):
        if not present:
            continue
        try:
            value = convert(si[key])
        except (TypeError, ValueError, OverflowError):
            raise unreadable(f"syminfo.{key} must be {kind}, got {json.dumps(si[key])[:80]}",
                             "value_type") from None
        calls.append((setter(name, key), value))
    for call in calls:
        call[0](strat, *call[1:])
    return applied


# --- Other symbols' bars for request.security (--symbol-feeds) --------------
#
# request.security on another symbol reads that symbol's own bars, never the
# chart's (engine and codegen 1.0.0+, C ABI strategy_set_symbol_feed /
# strategy_set_symbol_facts). The engine keys a feed by the exact symbol string
# the script passes at run time and by timeframe, and aggregates nothing: a
# request at "1D" needs a "1D" feed. --symbol-feeds names them in one index:
#
#   {"symbols": {"BINANCE:ETHUSDT": {
#       "syminfo": {<the symbol's catalog syminfo object, flat or wrapped>},
#       "feeds": {"240": "ethusdt-240.csv", "1D": "ethusdt-1D.csv"}}}}

SYMBOL_FEED_CANONICALIZATION = "pf-symbol-feed-barc-close-le-v1"
_SYMBOL_FEED_HASH_PREFIX = b"pineforge:symbol-feed:barc-close-le:v1\0"
_SYMBOL_FEED_RECORD = struct.Struct("<5dqq")
# The requests manifest's timeframe spelling and caps (scripts/run_strategy.py).
_SYMBOL_TF_RE = re.compile(r"(?:[1-9][0-9]{0,4}|[1-9][0-9]{0,3}[DWMS])")
_SYMBOL_FEEDS_MAX = 256
_SYMBOL_KEY_MAX = 256
_SYMBOL_STAMP_MAX = 2**53 - 1  # unix ms; the record is fingerprinted as a JSON number
# Catalog syminfo key -> the strategy_set_symbol_facts field it sets.
_SYMBOL_FACT_KEYS = (("tickerid", "canonical"), ("type", "type"), ("timezone", "timezone"),
                     ("session", "session"), ("currency", "currency"), ("mintick", "mintick"))
_SYMBOL_FEED_SETTERS = ("strategy_set_symbol_facts", "strategy_set_symbol_feed")


class SymbolFeedsError(RunFailure, ValueError):
    """A --symbol-feeds index or feed the harness cannot install as given; main()
    reports it as the one failure line (exit 1). Its code is
    symbol_feeds_refused{reason}, one reason per refusal; a feed the engine
    refused carries the engine's own code instead when the library reports one
    (install_symbol_feeds)."""

    def __init__(self, text, reason):
        super().__init__(text, "symbol_feeds_refused", {"reason": reason})


def _shown(value) -> str:
    return json.dumps(value)[:80]


def _file_name_ok(name: str) -> bool:
    """name can name a file: it holds no NUL and the file system encoding takes
    it (a lone surrogate from a JSON escape may not), so opening it raises no
    ValueError."""
    if "\x00" in name:
        return False
    try:
        os.fsencode(name)
    except UnicodeEncodeError:
        return False
    return True


def _symbol_text(value, what: str) -> str:
    try:
        if isinstance(value, str):
            value.encode("utf-8")
    except UnicodeEncodeError:
        value = None
    if (not isinstance(value, str) or not value or len(value) > _SYMBOL_KEY_MAX
            or any(ord(ch) < 0x20 for ch in value)):
        raise SymbolFeedsError(
            f"--symbol-feeds: {what} must be a non-empty string of at most "
            f"{_SYMBOL_KEY_MAX} characters without control characters, got {_shown(value)}",
            "symbol_text_invalid")
    return value


def symbol_timeframe(tf) -> str:
    """The engine's one feed timeframe spelling: whole minutes as a bare integer
    ("240", never "4h"), days, weeks, months and seconds as <n>D|W|M|S; Pine's
    bare D/W/M/S fold to 1D/1W/1M/1S, as the engine folds a request's."""
    if isinstance(tf, str) and tf in ("D", "W", "M", "S"):
        tf = "1" + tf
    if not (isinstance(tf, str) and _SYMBOL_TF_RE.fullmatch(tf)):
        raise SymbolFeedsError(
            "--symbol-feeds: a timeframe is whole minutes (\"15\", \"240\") or "
            f"<n>D|W|M|S (\"1D\", \"1W\"), got {_shown(tf)}", "timeframe_invalid")
    return tf


def _bar_close_ms(open_ms: int, tf: str) -> int:
    """A bar's close when its feed has no time_close column: its open plus the
    timeframe, n calendar months (UTC) for M. Right for a 24x7 symbol; a
    session-bound one must carry time_close."""
    n = int(tf) if tf.isdigit() else int(tf[:-1])
    unit = "" if tf.isdigit() else tf[-1]
    if unit != "M":
        return open_ms + n * {"": 60_000, "S": 1_000, "D": 86_400_000,
                              "W": 604_800_000}[unit]
    secs, ms = divmod(open_ms, 1000)
    try:
        t = datetime.fromtimestamp(secs, tz=timezone.utc)
        month = t.month - 1 + n
        year, month = t.year + month // 12, month % 12 + 1
        day = min(t.day, calendar.monthrange(year, month)[1])
        return int(t.replace(year=year, month=month, day=day).timestamp()) * 1000 + ms
    except (ValueError, OverflowError, OSError):
        return None  # out of the calendar's range: refused by the caller


def _load_symbol_feed(path: Path, symbol: str, tf: str) -> dict:
    """One feed CSV -> ctypes bars and closes plus its record. Columns:
    timestamp (open, unix ms), open, high, low, close, optional volume (empty or
    NaN when the symbol publishes none) and optional time_close (unix ms); other
    columns are ignored."""
    where = f"--symbol-feeds: feed {symbol}@{tf} ({path})"
    rows, lines = [], []
    try:
        with path.open(newline="", encoding="utf-8-sig") as f:
            reader = csv.DictReader(f)
            columns = reader.fieldnames or []
            missing = [c for c in ("timestamp", "open", "high", "low", "close")
                       if c not in columns]
            if missing:
                raise SymbolFeedsError(f"{where}: no column {', '.join(missing)}",
                                       "feed_columns_missing")
            for row in reader:
                line = reader.line_num
                try:
                    ts = int(row["timestamp"])
                    o, h, l, c = (float(row[k]) for k in ("open", "high", "low", "close"))
                    vol = (row.get("volume") or "").strip()
                    v = float(vol) if vol else math.nan
                    cell = (row.get("time_close") or "").strip()
                    close = int(cell) if cell else None  # empty: open + timeframe
                except (TypeError, ValueError):
                    raise SymbolFeedsError(f"{where} line {line}: not a number",
                                           "feed_value_not_number") from None
                if not all(math.isfinite(x) for x in (o, h, l, c)) or v < 0 or math.isinf(v):
                    raise SymbolFeedsError(
                        f"{where} line {line}: prices must be finite and volume "
                        "nonnegative or empty", "feed_value_invalid")
                if close is None:
                    close = _bar_close_ms(ts, tf)
                if not all(x is not None and abs(x) <= _SYMBOL_STAMP_MAX for x in (ts, close)):
                    raise SymbolFeedsError(
                        f"{where} line {line}: a time must be unix milliseconds "
                        f"within +-{_SYMBOL_STAMP_MAX}", "feed_time_out_of_range")
                rows.append((o, h, l, c, v, ts, close))
                lines.append(line)
    except OSError as e:
        raise SymbolFeedsError(f"{where}: {e.strerror or e}", "feed_unreadable") from None
    except (UnicodeDecodeError, csv.Error) as e:
        raise SymbolFeedsError(f"{where}: not a UTF-8 CSV ({e})", "feed_not_utf8_csv") from None
    n = len(rows)
    bars = (BarC * n)()
    closes = (ctypes.c_int64 * n)()
    hasher = hashlib.sha256(_SYMBOL_FEED_HASH_PREFIX)
    for i, (o, h, l, c, v, ts, close) in enumerate(rows):
        next_open = rows[i + 1][5] if i + 1 < n else None
        if next_open is not None and next_open <= ts:
            raise SymbolFeedsError(f"{where} line {lines[i + 1]}: timestamps must increase",
                                   "feed_not_increasing")
        if close <= ts or (next_open is not None and close > next_open):
            raise SymbolFeedsError(
                f"{where} line {lines[i]}: its close {close} is not after its open {ts} "
                "and at or before the next bar's open (is the timeframe right?)",
                "feed_close_time_invalid")
        bars[i].open, bars[i].high, bars[i].low, bars[i].close = o, h, l, c
        bars[i].volume, bars[i].timestamp = v, ts
        closes[i] = close
        hasher.update(_SYMBOL_FEED_RECORD.pack(o, h, l, c, v, ts, close))
    # A header-only feed is installed as the engine documents it: its requests
    # read na on every bar (a symbol with no bars in the window).
    record = {"bars": n, "source_values_sha256": hasher.hexdigest()}
    if n:
        record.update(first_ts=rows[0][5], last_ts=rows[-1][5])
    return {"timeframe": tf, "bars": bars, "close_ms": closes, "n": n, "record": record}


def _symbol_facts(doc, symbol: str) -> list:
    """The strategy_set_symbol_facts (field, value) pairs of a catalog syminfo
    object: tickerid (as canonical), type, timezone, session, currency, mintick.
    Absent, null or empty keys set nothing; other keys are ignored."""
    if doc is None:
        return []
    si = doc.get("syminfo", doc) if isinstance(doc, dict) else None
    if not isinstance(si, dict):
        raise SymbolFeedsError(f"--symbol-feeds: {symbol}: syminfo must be an object",
                               "syminfo_not_object")
    facts = []
    for key, field in _SYMBOL_FACT_KEYS:
        value = si.get(key)
        if value is None or value == "":
            continue
        if field == "mintick":
            try:
                ok = (isinstance(value, (int, float)) and not isinstance(value, bool)
                      and math.isfinite(float(value)) and value > 0)
            except OverflowError:  # an int beyond binary64
                ok = False
            if not ok:
                raise SymbolFeedsError(
                    f"--symbol-feeds: {symbol}: syminfo.mintick must be a positive "
                    f"finite number, got {_shown(value)}", "syminfo_mintick_invalid")
            facts.append((field, float(value)))
        else:
            facts.append((field, _symbol_text(value, f"{symbol}: syminfo.{key}")))
    return facts


def load_symbol_feeds(index_path: Path) -> list:
    """Read and check the --symbol-feeds index and every feed it names before
    any strategy state exists. Each symbol key is the exact string a script's
    request.security passes (prefix and suffix included: "BINANCE:ETHUSDT",
    "ETHUSDT" and "BINANCE:ETHUSDT.P" are three symbols). An entry holds "feeds"
    ({timeframe: CSV path, relative to the index}) and optionally "syminfo".
    Returns one entry per symbol: {"symbol", "facts", "feeds"}. Any problem is a
    SymbolFeedsError naming it."""
    def unique(pairs):
        out = {}
        for k, v in pairs:
            if k in out:
                raise SymbolFeedsError(f"--symbol-feeds: duplicate key {_shown(k)}",
                                       "index_duplicate_key")
            out[k] = v
        return out
    try:
        doc = json.loads(index_path.read_text(encoding="utf-8"), object_pairs_hook=unique)
    except OSError as e:
        raise SymbolFeedsError(f"--symbol-feeds: {index_path}: {e.strerror or e}",
                               "index_unreadable") from None
    except (ValueError, RecursionError) as e:
        if isinstance(e, SymbolFeedsError):
            raise
        raise SymbolFeedsError(f"--symbol-feeds: {index_path} is not JSON: {e}",
                               "index_not_json") from None
    symbols = doc.get("symbols") if isinstance(doc, dict) else None
    if not isinstance(symbols, dict):
        raise SymbolFeedsError('--symbol-feeds: the index must be {"symbols": {...}}',
                               "index_shape")
    if len(symbols) > _SYMBOL_FEEDS_MAX:
        raise SymbolFeedsError(f"--symbol-feeds: more than {_SYMBOL_FEEDS_MAX} symbols",
                               "too_many_symbols")
    out, total = [], 0
    for symbol, entry in symbols.items():
        _symbol_text(symbol, "a symbol")
        if not isinstance(entry, dict) or set(entry) - {"syminfo", "feeds"}:
            raise SymbolFeedsError(
                f'--symbol-feeds: {symbol}: an entry is {{"feeds": {{...}}, "syminfo": {{...}}}}',
                "entry_shape")
        feeds = entry.get("feeds", {})
        if not isinstance(feeds, dict):
            raise SymbolFeedsError(f"--symbol-feeds: {symbol}: feeds must be an object",
                                   "feeds_not_object")
        total += len(feeds)
        if total > _SYMBOL_FEEDS_MAX:
            raise SymbolFeedsError(f"--symbol-feeds: more than {_SYMBOL_FEEDS_MAX} feeds",
                                   "too_many_feeds")
        named = {}
        for tf, file in feeds.items():
            canonical = symbol_timeframe(tf)
            if canonical in named:
                raise SymbolFeedsError(
                    f"--symbol-feeds: {symbol}: two feeds at timeframe {canonical}",
                    "duplicate_timeframe")
            if not isinstance(file, str) or not file or not _file_name_ok(file):
                raise SymbolFeedsError(
                    f"--symbol-feeds: {symbol}@{canonical}: the feed must name a CSV file",
                    "feed_path_invalid")
            named[canonical] = index_path.parent / file
        facts = _symbol_facts(entry.get("syminfo"), symbol)
        out.append({"symbol": symbol, "facts": facts,
                    "feeds": [_load_symbol_feed(path, symbol, tf)
                              for tf, path in named.items()]})
    return out


def install_symbol_feeds(lib, strat, symbols) -> None:
    """Install what load_symbol_feeds read: each symbol's facts, then its feeds.
    The engine copies the arrays, so one load serves every state of a run.
    A setter's refusal keeps run_json's text, with the engine's code and
    arguments for it when the library reports one (the engine names why it
    refused: a specific symbol_feeds_refused reason, out_of_memory, an engine
    fault), else symbol_feeds_refused{reason: engine_refused}."""
    missing = [n for n in _SYMBOL_FEED_SETTERS if not hasattr(lib, n)]
    if missing:
        raise SymbolFeedsError(
            f"--symbol-feeds: the strategy library has no {', '.join(missing)}, so "
            "other symbols' bars cannot be installed (engine 1.0.0 or later)",
            "library_without_symbol_feeds")

    def refused(what):
        detail = ""
        if hasattr(lib, "strategy_get_last_error"):
            err = lib.strategy_get_last_error(strat)
            detail = err.decode("utf-8", "replace") if err else ""
        error = SymbolFeedsError(f"--symbol-feeds: the engine refused {what}"
                                 + (f": {detail}" if detail else ""), "engine_refused")
        engine = engine_failure_code(lib, strat)
        if engine and engine[0]:
            error.code, error.code_args = engine
        raise error

    for sym in symbols:
        key = sym["symbol"].encode()
        for field, value in sym["facts"]:
            text = repr(value) if isinstance(value, float) else value
            if lib.strategy_set_symbol_facts(strat, key, field.encode(), text.encode()) != 0:
                refused(f"the {field} of {sym['symbol']}")
        for feed in sym["feeds"]:
            if lib.strategy_set_symbol_feed(strat, key, feed["timeframe"].encode(),
                                            feed["bars"], feed["close_ms"], feed["n"]) != 0:
                refused(f"the feed {sym['symbol']}@{feed['timeframe']}")


def symbol_feeds_record(symbols) -> dict:
    """applied_runtime["symbol_feeds"]: what was installed, so a run with other
    symbols' bars has its own fingerprint digest."""
    return {"canonicalization": SYMBOL_FEED_CANONICALIZATION,
            "symbols": {sym["symbol"]: {
                "facts": dict(sym["facts"]),
                "feeds": {feed["timeframe"]: feed["record"] for feed in sym["feeds"]},
            } for sym in symbols}}


# --- Recorded outputs (--outputs) --------------------------------------------
#
# A library that records outputs (docs/outputs.md) exports
# strategy_outputs_api_version and its manifest; every library built on an
# engine with the group has the readers. The harness switches recording on
# before the run, then reads the rows, the run constants and the events once,
# after it: a batch caller never clears.

OUTPUTS_API_VERSION = 1
OUTPUTS_SCHEMA_VERSION = "pineforge-outputs/v1"
OUTPUTS_MESSAGE_FORMAT = "pineforge/v1"
OUTPUT_PHASES = {0: "batch", 1: "warmup", 2: "realtime"}
_INT64_MIN = -(2 ** 63)
_OUTPUTS_EXPORTS = (
    "strategy_outputs_manifest", "strategy_outputs_set_enabled",
    "strategy_outputs_series_count", "strategy_outputs_bars_len",
    "strategy_outputs_bar_times_copy", "strategy_outputs_series_copy",
    "strategy_outputs_events_len", "strategy_outputs_event_get",
    "strategy_outputs_constants_copy")
_NO_OUTPUTS = ("--outputs: this library records no outputs (compile the script as an "
               "indicator, or with outputs on)")


def _outputs_incompatible(text: str, reason: str) -> OutputsError:
    return OutputsError("--outputs: " + text, "strategy_library_incompatible",
                        {"reason": reason})


def require_outputs(lib) -> None:
    """OutputsError unless `lib` records outputs, has every export recording
    reads, and answers the outputs API version this harness reads."""
    if not hasattr(lib, "strategy_outputs_api_version"):
        raise OutputsError(_NO_OUTPUTS, "outputs_rejected", {"reason": "not_declared"})
    for name in _OUTPUTS_EXPORTS:
        if not hasattr(lib, name):
            raise _outputs_incompatible(
                f"the library lacks {name}, which recording needs; rebuild.",
                "outputs_api_mismatch")
    version = int(lib.strategy_outputs_api_version())
    if version != OUTPUTS_API_VERSION:
        raise _outputs_incompatible(
            f"the library's outputs API version is {version}, the harness reads "
            f"{OUTPUTS_API_VERSION}; rebuild.", "outputs_api_mismatch")


def enable_outputs(lib, strat) -> None:
    """Switch recording on for `strat`. A refusal is the engine's own failure,
    its text after "--outputs: " and its code from the getters."""
    if lib.strategy_outputs_set_enabled(strat, 1) == 0:
        return
    text = ""
    if hasattr(lib, "strategy_get_last_error"):
        text = _c_text(lib.strategy_get_last_error(strat))
    engine = engine_failure_code(lib, strat)
    code, args = engine if engine is not None and engine[0] else (
        "outputs_rejected", {"reason": "not_declared"})
    raise OutputsError("--outputs: " + (text or "the library refused to record"), code, args)


def _outputs_manifest(lib, strat) -> bytes:
    required = ctypes.c_size_t(0)
    error = ctypes.create_string_buffer(512)
    lib.strategy_outputs_manifest(strat, None, 0, ctypes.byref(required), error, len(error))
    status = -1
    buffer = None
    if required.value > 0:
        buffer = ctypes.create_string_buffer(required.value)
        status = lib.strategy_outputs_manifest(strat, buffer, required.value,
                                               ctypes.byref(required), error, len(error))
    if status != 0 or buffer is None:
        raise _outputs_incompatible(
            "the library returned no outputs manifest: "
            + error.value.decode("utf-8", "replace"), "outputs_manifest_invalid")
    return buffer.raw[:required.value - 1]


def make_outputs_reader(lib, strat):
    """The reader build_outputs_block takes: callables over the C readers of
    `strat`. Each copies when called; an event's message is copied out with
    the event (the library lends it only until the next call that runs or
    clears)."""

    def bar_times():
        n = int(lib.strategy_outputs_bars_len(strat))
        opens = (ctypes.c_int64 * max(n, 1))()
        closes = (ctypes.c_int64 * max(n, 1))()
        written = ctypes.c_int64(0)
        if n > 0 and lib.strategy_outputs_bar_times_copy(
                strat, 0, opens, closes, n, ctypes.byref(written)) != 0:
            raise _outputs_incompatible("the bar times could not be read",
                                        "outputs_api_mismatch")
        return list(opens[:written.value]), list(closes[:written.value])

    def series(slot):
        n = int(lib.strategy_outputs_bars_len(strat))
        values = (ctypes.c_double * max(n, 1))()
        written = ctypes.c_int64(0)
        if lib.strategy_outputs_series_copy(strat, slot, 0, values, n,
                                            ctypes.byref(written)) != 0:
            raise _outputs_incompatible(f"series slot {slot} could not be read",
                                        "outputs_api_mismatch")
        return list(values[:written.value])

    def constants():
        n = int(lib.strategy_outputs_constants_copy(strat, None, 0))
        values = (ctypes.c_double * max(n, 1))()
        if n > 0:
            lib.strategy_outputs_constants_copy(strat, values, n)
        return list(values[:max(n, 0)])

    def events():
        out = []
        for index in range(int(lib.strategy_outputs_events_len(strat))):
            event = OutputEventC()
            if lib.strategy_outputs_event_get(strat, index, ctypes.byref(event),
                                              ctypes.sizeof(event)) != 0:
                raise _outputs_incompatible(f"event {index} could not be read",
                                            "outputs_api_mismatch")
            # A c_char_p field reads as a bytes copy: taken here, while the
            # library still lends the text.
            out.append(types.SimpleNamespace(
                **{name: getattr(event, name) for name, _ in OutputEventC._fields_}))
        return out

    return types.SimpleNamespace(
        manifest=lambda: _outputs_manifest(lib, strat),
        bar_times=bar_times,
        series_count=lambda: int(lib.strategy_outputs_series_count(strat)),
        series=series,
        constants=constants,
        events=events,
    )


def _output_value(value, encoding):
    """A slot or constant value: null when not finite, an integer under the
    rgba-u32 encoding, else the double."""
    number = _num(value)
    if number is not None and encoding == "rgba-u32":
        return int(number)
    return number


def _output_time(ms) -> int | None:
    ms = int(ms)
    return None if ms == _INT64_MIN else ms


def _manifest_entries(manifest: dict, key: str, fields: tuple) -> list:
    """Validate the indices and output IDs this reader consumes before use."""
    entries = manifest.get(key, [])
    if not isinstance(entries, list) or not all(
            isinstance(e, dict) and all(f in e for f in fields) for e in entries):
        raise _outputs_incompatible(
            f"the outputs manifest's {key} is not a list of entries with {', '.join(fields)}",
            "outputs_manifest_invalid")
    for entry in entries:
        for field in fields:
            value = entry[field]
            if field in ("index", "slot"):
                valid = type(value) is int and value >= 0
            else:
                valid = isinstance(value, str) and bool(value)
            if not valid:
                raise _outputs_incompatible(
                    f"the outputs manifest's {key} entry has an invalid {field}",
                    "outputs_manifest_invalid")
    return entries


def build_outputs_block(reader) -> dict:
    """The report's "outputs" block, from a reader of callables (see
    make_outputs_reader): manifest() -> the raw manifest bytes,
    bar_times() -> (opens, closes), series_count() -> int,
    series(slot) -> [double], constants() -> [double], events() -> records
    with the fields of pf_output_event_v1_t (message as bytes or None).

    series[] has one entry per manifest series[] entry, in slot order;
    hlines[] one per hline output, its price the run constant its manifest
    entry names (null when na or not written, as in a run with no rows);
    an event of an alert output also carries the output's freq. A manifest
    that is not a JSON object, names a slot the library does not record, or
    lacks an output an event names is an OutputsError."""
    raw = reader.manifest()
    try:
        manifest = json.loads(raw)
    except (ValueError, UnicodeDecodeError, RecursionError) as error:
        raise _outputs_incompatible(f"the outputs manifest is not JSON: {error}",
                                    "outputs_manifest_invalid") from None
    if not isinstance(manifest, dict):
        raise _outputs_incompatible("the outputs manifest is not a JSON object",
                                    "outputs_manifest_invalid")
    output_entries = _manifest_entries(manifest, "outputs", ("index", "id"))
    series_entries = _manifest_entries(manifest, "series", ("slot", "output"))
    constant_entries = _manifest_entries(manifest, "constants", ("index",))
    outputs = {entry["index"]: entry for entry in output_entries}
    opens, closes = reader.bar_times()
    slots = reader.series_count()
    series = []
    for entry in sorted(series_entries, key=lambda e: e["slot"]):
        slot = entry["slot"]
        if not isinstance(slot, int) or not 0 <= slot < slots:
            raise _outputs_incompatible(
                f"the outputs manifest names series slot {slot}, which the library "
                f"does not record", "outputs_manifest_invalid")
        encoding = entry.get("encoding")
        series.append({"slot": slot, "output": entry.get("output"),
                       "values": [_output_value(v, encoding) for v in reader.series(slot)]})
    encodings = {entry["index"]: entry.get("encoding") for entry in constant_entries}
    constants = [_output_value(v, encodings.get(k)) for k, v in enumerate(reader.constants())]
    hlines = []
    for entry in output_entries:
        if entry.get("kind") != "hline":
            continue
        price = entry.get("price")
        index = price.get("constant") if isinstance(price, dict) else None
        hlines.append({"output": entry.get("id"),
                       "price": (constants[index] if isinstance(index, int)
                                 and 0 <= index < len(constants) else None)})
    events = []
    for event in reader.events():
        output_index = int(event.output_index)
        if output_index not in outputs:
            raise _outputs_incompatible(
                f"the outputs manifest lists no output {output_index}, which an event "
                f"names", "outputs_manifest_invalid")
        output = outputs[output_index]
        message = event.message
        if isinstance(message, bytes):
            message = message.decode("utf-8", "replace")
        row = {
            "sequence": int(event.sequence),
            "output": output.get("id"),
            "bar_index": int(event.bar_index),
            "bar_open_ms": _output_time(event.bar_open_ms),
            "bar_close_ms": _output_time(event.bar_close_ms),
            "ordinal_in_bar": int(event.ordinal_in_bar),
            "phase": OUTPUT_PHASES.get(int(event.phase), str(int(event.phase))),
            "value": _num(event.value),
            "message": message,
        }
        if output.get("kind") == "alert":
            row["freq"] = output.get("freq")
        events.append(row)
    return {
        "schema_version": OUTPUTS_SCHEMA_VERSION,
        "message_format": manifest.get("message_format", OUTPUTS_MESSAGE_FORMAT),
        "manifest_sha256": hashlib.sha256(raw).hexdigest(),
        "manifest": manifest,
        "bars": {"open_ms": [_output_time(t) for t in opens],
                 "close_ms": [_output_time(t) for t in closes]},
        "series": series,
        "constants": constants,
        "hlines": hlines,
        "events": events,
    }


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


# The JSON report keys of pf_equity_stats_t's two monthly ratios. They are
# report-schema names: the C fields are sharpe_monthly / sortino_monthly (their
# pre-1.0 spellings were removed for 1.0), the keys did not change (ADR-0001,
# "Deprecated public spellings").
EQUITY_REPORT_KEYS = {"sharpe_monthly": "sharpe_tv", "sortino_monthly": "sortino_tv"}


def _stats_dict(s) -> dict:
    """Serialize a pf_trade_stats_t / pf_equity_stats_t ctypes struct to a dict,
    keying off each field's ctype: integer counters stay ints, every double is
    sanitized through _num. Driven by _fields_ so it tracks the struct verbatim;
    an equity struct's two monthly ratios take their EQUITY_REPORT_KEYS."""
    keys = EQUITY_REPORT_KEYS if isinstance(s, EquityStatsC) else {}
    out = {}
    for name, ctype in s._fields_:
        v = getattr(s, name)
        out[keys.get(name, name)] = _num(v) if ctype is ctypes.c_double else int(v)
    return out


def build_report_dict(report: ReportC, ohlcv_path: Path,
                      n_bars: int, first_ts: int, last_ts: int,
                      elapsed: float,
                      applied_inputs: dict[str, str],
                      applied_overrides: dict[str, str],
                      applied_runtime: dict[str, object] | None = None,
                      trade_entry_incarnations: list[int] | None = None) -> dict:
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
            "open_at_end":     bool(t.open_at_end),
            "entry_incarnation": (
                int(trade_entry_incarnations[i])
                if trade_entry_incarnations is not None
                and i < len(trade_entry_incarnations) else 0
            ),
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

    return {
        "engine": "pineforge",
        "input": {
            "ohlcv":      str(ohlcv_path),
            "bars":       n_bars,
            # None only for a selected-window run whose evaluated feed is empty
            # (first_ts/last_ts None); an ordinary run always passes integers.
            "first_ts":   None if first_ts is None else int(first_ts),
            "last_ts":    None if last_ts is None else int(last_ts),
            "first_time": None if first_ts is None else fmt_utc(first_ts),
            "last_time":  None if last_ts is None else fmt_utc(last_ts),
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
        },
        "trades": trades,
        "metrics": metrics,
        "equity_curve": equity_curve,
    }


def _request_invalid(text: str, option: str, exit_status: int = 1) -> RunFailure:
    return RunFailure(text, "run_request_invalid", {"option": option},
                      exit_status=exit_status)


def _utf8_option(text: str, label: str) -> bytes:
    """A command-line option's text as UTF-8 bytes. A non-UTF-8 byte in it (which
    Python keeps from argv as a lone surrogate) is run_request_invalid{option}."""
    try:
        return text.encode("utf-8")
    except UnicodeEncodeError:
        raise _request_invalid(f"error: {label} must hold UTF-8 text",
                               label.lstrip("-").replace("-", "_")) from None


def _json_kind(value) -> str:
    """A JSON value that is neither a string nor a number, as a refusal names it."""
    if value is None or isinstance(value, bool):
        return json.dumps(value)
    return "an array" if isinstance(value, list) else "an object"


def parse_kv_json(s: str | None, label: str) -> dict[str, str]:
    """Parse a JSON object of {key: value} into a {str: str} map.
    Empty / None / "{}" → {}. Non-object payloads abort with a clear
    error so junk env vars don't silently noop: run_request_invalid{option}
    (the label without its dashes). A value is a string or a number; a number
    is passed as str() spells it, as it always was ("5", "0.5", "1000.0" for
    1e3). A boolean, null, array or object is refused before any setter runs,
    rather than passed as Python spells it ("True", "None")."""
    if not s or s.strip() in ("", "{}"):
        return {}
    option = label.lstrip("-").replace("-", "_")
    try:
        obj = json.loads(s)
    except (ValueError, RecursionError) as e:  # ValueError: also an int over 4300 digits
        raise _request_invalid(f"error: {label} is not valid JSON: {e}", option) from None
    if not isinstance(obj, dict):
        raise _request_invalid(
            f"error: {label} must be a JSON object, got {type(obj).__name__}", option)
    for key, value in obj.items():
        if isinstance(value, bool) or not isinstance(value, (str, int, float)):
            raise _request_invalid(
                f"error: {label}: {_shown(key)} must be a string or a number, "
                f"got {_json_kind(value)}", option)
    out = {str(k): str(v) for k, v in obj.items()}
    for text in (*out, *out.values()):
        try:
            text.encode("utf-8")
        except UnicodeEncodeError:  # a lone surrogate escape
            raise _request_invalid(f"error: {label} must hold UTF-8 text", option) from None
    return out


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
    try:
        if key.isdigit() and 0 <= int(key) <= 5:
            return int(key)
    except ValueError:  # a digit int() does not read, such as "²"
        pass
    raise _request_invalid(
        f"error: --magnifier-dist must be one of "
        f"{sorted(MAGNIFIER_DISTS)} or 0-5, got {s!r}",
        "magnifier_dist"
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


# run_request_invalid's option for a command line argparse refuses: the flag the
# error names (dashes to underscores) when it is one of these, else "arguments"
# (an unknown flag, several missing ones).
_RUN_REQUEST_OPTIONS = frozenset({
    "so", "ohlcv", "inputs", "overrides", "input_tf", "script_tf", "bar_magnifier",
    "magnifier_samples", "magnifier_dist", "generated_cpp", "transpiled", "syminfo",
    "trade_start_ms", "chart_tz", "magnifier_volume_weighted", "bench", "warmup",
    "repeats", "symbol_feeds",
})


def argparse_option(message: str) -> str:
    """run_request_invalid's option for an argparse error message."""
    m = (re.match(r"argument (--[a-z][a-z-]*): ", message)
         or re.fullmatch(r"the following arguments are required: (--[a-z][a-z-]*)", message))
    option = m.group(1)[2:].replace("-", "_") if m else "arguments"
    return option if option in _RUN_REQUEST_OPTIONS else "arguments"


class _ArgumentParser(argparse.ArgumentParser):
    """argparse whose usage error is also the failure line,
    run_request_invalid{option}; it still prints the usage and the message to
    stderr and exits 2."""

    def error(self, message):
        self.print_usage(sys.stderr)
        text = f"{self.prog}: error: {message}"
        write_failure(text, "run_request_invalid", {"option": argparse_option(message)})
        self.exit(2, text + "\n")


def run_failure(lib, strat):
    """(text, code, args) of the failure line for the run just made on strat, or
    None when it succeeded. A run failed when the engine reports a text, a code
    or a run status of 1 (strategy_last_run_status), so runtime.error() with an
    empty message fails: text "", code strategy_runtime_error. A library without
    strategy_get_last_error_code gets a line without a code: the earlier line of
    its text, or RUN_STATUS_FAILED_TEXT for a status of 1 without one. With the
    getter, a failure reported with no code (status 1 alone, or a text the getter
    does not name) is run_json's engine_unclassified_error."""
    text = ""
    if hasattr(lib, "strategy_get_last_error"):
        text = _c_text(lib.strategy_get_last_error(strat))
    engine = engine_failure_code(lib, strat)
    status = (lib.strategy_last_run_status(strat)
              if hasattr(lib, "strategy_last_run_status") else 0)
    code, args = engine if engine is not None else (None, None)
    if not (text or code or status == 1):
        return None
    if engine is None:
        return text or RUN_STATUS_FAILED_TEXT, None, None
    if code:
        return text, code, args
    return text or RUN_STATUS_FAILED_TEXT, "engine_unclassified_error", {}


# --- Selected-window mode (selected-window/v1: full/v1 report, no digest) -------------------
#
# A request that names --report-policy selected-window/v1, a window option or
# --validate-window-only is run by _selected_run. Every other request follows the ordinary body
# of _main, which gains only the optional phase records of --run-phase-fd (they change no report
# byte). docker/selected_window_report.py holds the pure pieces. The sibling modules
# run_phase_transport, run_execution_observer, request_feed_inventory and selected_window_plan
# are imported lazily, and only on the paths that need them, so an ordinary run never loads them
# and never reads an inventory, a policy or the planner library.
#
# The order of a selected run; each step ends the run with its typed failure line:
#   1. validate the flags and the request: nothing is read, nothing is loaded;
#   2. open the phase writer (its descriptor is checked before any strategy code loads);
#   3. ADMISSION from the request-feed inventory and the candidate support constants, before
#      load_bars, load_symbol_feeds, any ctypes load (the planner included), any strategy
#      construction and the first phase record;
#   4. phase preflight; load the primary feed once; plan with the native planner; trim by a
#      pointer offset; hash. --validate-window-only ends here, applied=false, no strategy run;
#   5. phase execution, BEFORE the strategy library loads; load it, configure the selected-window
#      ABI, attach the observer to the phase writer, run;
#   6. the OBSERVER (never this file) moves the phase to result_assembly during the native call.
#      Afterwards the retained callback exception is raised first, the native counts must equal
#      the plan, the report is assembled (the engine supplies the selected trades, curve and
#      metrics; nothing is recalculated here) and serialized once the serialization phase has
#      begun; completed follows the stdout flush.

_TYPED_ERROR_NAMES = frozenset({"SelectedWindowError", "InventoryAdmissionError",
                                "PlanBridgeError", "ObserverBindingError",
                                "PhaseTransportError"})


def _typed_failure(error):
    """The RunFailure for an exception raised by one of the selected-window helper modules
    (code, message and the code's arguments as that module defines them), or None for any other
    exception. Matched by class name so this file imports none of the modules at load."""
    if type(error).__name__ not in _TYPED_ERROR_NAMES:
        return None
    code = getattr(error, "code", None)
    if not isinstance(code, str) or _CODE_RE.fullmatch(code) is None:
        return None
    code_args = getattr(error, "code_args", None)
    if code_args is None:
        code_args = getattr(error, "detail", None)  # InventoryAdmissionError
    text = getattr(error, "message", None) or str(error)
    return RunFailure(text, code, dict(code_args) if isinstance(code_args, dict) else {})


def _engine_invariant(text: str) -> RunFailure:
    return RunFailure(text, "engine_invariant")


def trusted_planner_path() -> str:
    """The absolute path of the trusted standalone planner helper of this installation:
    <prefix>/lib/libpineforge_window_plan.so, where <prefix> is the parent of the directory
    this run_json.py is installed in (bin). Never a strategy library and never a path search."""
    return str(Path(__file__).resolve().parent.parent / "lib" / "libpineforge_window_plan.so")


def _selected_requested(args) -> bool:
    """Whether the command line opts into selected-window mode: the policy, any of the four
    window options or the validation-only switch. A window option without the policy is not
    legacy: it is refused (window_request_invalid, option report_policy)."""
    return (args.report_policy is not None or args.window_start_ms is not None
            or args.window_end_ms is not None or args.preroll_bars is not None
            or args.fed_start_ms is not None or bool(args.validate_window_only))


class _Phases:
    """The run's RunPhaseWriter with its error type: a transport failure of any advance() is
    the typed harness fault (harness_internal_error), never a strategy outcome."""

    def __init__(self, writer, error_type):
        self.writer = writer
        self._error_type = error_type

    def advance(self, phase: str) -> None:
        try:
            self.writer.advance(phase)
        except self._error_type as error:
            raise _typed_failure(error) from None


def _open_phases(fd_text, *, always: bool):
    """The run's phase session, or None. fd_text is --run-phase-fd as written: None gives
    None for an ordinary run (always=False) and a writer that exports nothing but still
    tracks the phases for a selected run (always=True). A descriptor that is not a number or
    not a usable socket is a preflight harness_internal_error (phase transport v1)."""
    if fd_text is None and not always:
        return None
    import run_phase_transport
    fd = None
    if fd_text is not None:
        if re.fullmatch(r"[0-9]{1,10}", fd_text) is None:
            raise RunFailure(f"--run-phase-fd must be a descriptor number from 3 up, got "
                             f"{_shown(fd_text)}", "harness_internal_error")
        fd = int(fd_text)
    try:
        writer = run_phase_transport.RunPhaseWriter(fd)
    except run_phase_transport.PhaseTransportError as error:
        raise _typed_failure(error) from None
    return _Phases(writer, run_phase_transport.PhaseTransportError)


def _advance(phases, phase: str) -> None:
    if phases is not None:
        phases.advance(phase)


def _require_result_assembly(phases) -> None:
    """After a successful native run the engine's observer must have moved the writer to
    result_assembly. It is never announced from here: anything else is a harness fault, not
    a strategy outcome."""
    if phases.writer.phase != "result_assembly":
        raise RunFailure(
            "the run ended without the engine announcing the end of execution (phase "
            f"{phases.writer.phase!r})", "harness_internal_error")


# Post-execution classification (the documented selected-window contract). Once the engine's
# observer has moved the REAL phase writer to result_assembly, strategy execution is over: a
# failure that is otherwise unclassified is the engine's, non-billable, and is
# report_post_execution_failed{phase, reason}, never a strategy error or timeout. The phase is
# read from the writer; it is never inferred from Python's _run returning. A specific typed
# fault (a RunFailure, a helper module's typed error, an engine failure with its own code) keeps
# its code, and a failure while the writer is still in `execution` is left to the rules that
# applied before this section existed.
_POST_BOUNDARY_PHASES = ("result_assembly", "results_digest", "serialization")
POST_EXECUTION_TEXT = ("The engine could not finish producing the result after strategy "
                       "execution completed.")


def _boundary_phase(phases):
    """The phase of the real writer when the end of execution has been acknowledged
    (result_assembly or later), else None (no writer, or still executing)."""
    if phases is None:
        return None
    phase = phases.writer.phase
    return phase if phase in _POST_BOUNDARY_PHASES else None


def _post_boundary_failure(error, phases):
    """report_post_execution_failed for an exception that is neither a RunFailure nor a typed
    helper error, raised after the acknowledged boundary: reason resource for MemoryError, io
    for OSError, exception for everything else. None when the boundary was not acknowledged
    or the error is a specific typed fault."""
    phase = _boundary_phase(phases)
    if phase is None or isinstance(error, RunFailure) or _typed_failure(error) is not None:
        return None
    if isinstance(error, MemoryError):
        reason = "resource"
    elif isinstance(error, OSError):
        reason = "io"
    else:
        reason = "exception"
    return RunFailure(POST_EXECUTION_TEXT, "report_post_execution_failed",
                      {"phase": phase, "reason": reason})


def _post_boundary_engine_failure(failure, phases):
    """The (text, code, args) of an engine failure line. After the acknowledged boundary an
    engine out_of_memory (reason resource) and an engine_unclassified_error (reason exception)
    are report_post_execution_failed; every other engine code, and every failure
    before the boundary (a strategy failure during execution included), is returned unchanged."""
    phase = _boundary_phase(phases)
    if phase is None:
        return failure
    code = failure[1]
    if code == "out_of_memory":
        reason = "resource"
    elif code == "engine_unclassified_error":
        reason = "exception"
    else:
        return failure
    return POST_EXECUTION_TEXT, "report_post_execution_failed", {"phase": phase, "reason": reason}


def _observer_binding(lib, state, writer):
    import run_execution_observer
    return run_execution_observer.ExecutionObserverBinding(lib, state, writer)


def _detach_observer(binding):
    """Unregister the observer in cleanup. The error, if any, is returned instead of raised:
    the frees that follow must still run. The binding keeps its callback alive until the
    caller lets go of it, after the state is freed."""
    if binding is None:
        return None
    try:
        binding.detach()
    except Exception as error:
        return error
    return None


def _read_observation(lib, state) -> dict:
    """The identities (run_generation, attempt_serial, attempt_generation) of the latest
    attempt from strategy_execution_observation_v1."""
    import run_execution_observer as observer
    observation = observer.ObservationC(ctypes.sizeof(observer.ObservationC),
                                        observer.OBSERVER_VERSION)
    status = lib.strategy_execution_observation_v1(state, ctypes.byref(observation))
    if status != 0:
        raise RunFailure(f"strategy_execution_observation_v1 returned {status}",
                         "harness_internal_error")
    return {"run_generation": int(observation.run_generation),
            "attempt_serial": int(observation.attempt_serial),
            "attempt_generation": int(observation.attempt_generation)}


def _syminfo_clock(syminfo_path) -> tuple:
    """(timezone, session) of the --syminfo file as text, "" for an absent or empty member:
    read as data, with no library, in the shape apply_syminfo reads it (flat or wrapped under
    "syminfo"; a member that is present is str()-ed, as apply_syminfo sends it). The planner
    treats "" as UTC and 24x7. A file apply_syminfo would refuse has the same refusal here."""
    def unreadable(text, reason):
        return SyminfoError(text, "syminfo_unreadable", {"reason": reason})

    try:
        with open(syminfo_path) as f:
            text = f.read()
    except OSError as e:
        raise unreadable(f"--syminfo: {syminfo_path}: {e.strerror or e}", "io") from None
    except ValueError as e:
        raise unreadable(f"--syminfo: {syminfo_path} is not JSON: {e}", "not_json") from None
    try:
        doc = json.loads(text)
    except (ValueError, RecursionError) as e:
        raise unreadable(f"--syminfo: {syminfo_path} is not JSON: {e}", "not_json") from None
    si = doc.get("syminfo", doc) if isinstance(doc, dict) else None
    if not isinstance(si, dict):
        raise unreadable(f"--syminfo: {syminfo_path}: the syminfo is not a JSON object",
                         "not_object")
    clock = []
    for key in ("timezone", "session"):
        value = si.get(key)
        value = str(value) if value else ""
        try:
            value.encode("utf-8")
        except UnicodeEncodeError:
            raise unreadable(f"syminfo.{key} must be UTF-8 text", "value_type") from None
        clock.append(value)
    return clock[0], clock[1]


def _write_report(text: str, phases=None) -> int:
    """Write the whole document to stdout and flush: 0, or 1 when stdout cannot take it. No
    failure line (no second JSON document) may follow part of a report, so the reason goes to
    stderr, stdout is pointed at /dev/null, the exit status is 1 and the phase writer is left
    where it is (serialization): the watchdog sees the terminal status and the last phase.
    With a phase session (a run past the boundary) any exception from the write is that
    outcome, and the stderr line names it as report_post_execution_failed with the real
    phase and the reason (resource, io or exception); without one, only OSError and ValueError
    are, and the line is the ordinary one."""
    caught = (OSError, ValueError) if phases is None else Exception
    try:
        sys.stdout.write(text)
        sys.stdout.flush()
    except caught as error:
        _discard_stdout()
        if phases is None:
            print(f"run_json: the report could not be written: {error}", file=sys.stderr)
        else:
            reason = ("resource" if isinstance(error, MemoryError)
                      else "io" if isinstance(error, (OSError, ValueError)) else "exception")
            print("run_json: report_post_execution_failed (phase "
                  f"{phases.writer.phase}, reason {reason}): the report could not be written "
                  f"whole: {error}", file=sys.stderr)
        return 1
    return 0


def _runtime_supports_selected_window(lib) -> bool:
    """Whether this runtime can run selected-window mode: the selected-window ABI and the
    execution observer at version 1 on the given library, the phase transport and the trusted
    planner helper loadable. False on any missing piece, never an exception for a missing
    one; no feed is read and nothing runs."""
    try:
        import run_execution_observer
        import run_phase_transport  # noqa: F401  (present is all the probe asks of it)
        import selected_window_plan
        import selected_window_report as swr
        swr.SelectedWindowAbi(lib)
        for name in ("pf_execution_observer_version", "strategy_set_execution_observer_v1",
                     "strategy_execution_observation_v1"):
            if not hasattr(lib, name):
                return False
        lib.pf_execution_observer_version.argtypes = []
        lib.pf_execution_observer_version.restype = ctypes.c_uint32
        if lib.pf_execution_observer_version() != run_execution_observer.OBSERVER_VERSION:
            return False
        selected_window_plan.SelectedPrimaryPlanner(trusted_planner_path())
    except (ImportError, OSError):
        return False
    except Exception as error:
        if _typed_failure(error) is not None:
            return False
        raise
    return True


def _capabilities_main(args) -> int:
    """--so LIB --capabilities-json: what this runtime supports, without OHLCV and without a
    run. LIB is the build's trusted canary library; it is loaded like any strategy library
    (ABI check, required exports) and no strategy is constructed."""
    import selected_window_report as swr
    lib = load_strategy(args.so)
    document = swr.capabilities_record(_runtime_supports_selected_window(lib))
    return _write_report(json.dumps(document, separators=(",", ":")) + "\n")


def _selected_run(args, inputs, overrides, input_tf, script_tf, bar_magnifier,
                  magnifier_samples, magnifier_dist) -> int:
    import request_feed_inventory
    import selected_window_report as swr

    # 1. The request. Nothing is read or loaded until admission has passed.
    request = swr.validate_request(
        policy=args.report_policy, window_start_ms=args.window_start_ms,
        window_end_ms=args.window_end_ms, preroll_bars=args.preroll_bars,
        fed_start_ms=args.fed_start_ms, trade_start_ms=args.trade_start_ms,
        input_tf=input_tf.decode("utf-8"), script_tf=script_tf.decode("utf-8"))
    if args.bench:
        raise _request_invalid(
            "error: --bench cannot be combined with selected-window mode", "bench")
    # 2. The phase transport is checked before any strategy code loads. Validation only runs
    #    no strategy and has no phases.
    phases = None if args.validate_window_only else _open_phases(args.run_phase_fd, always=True)
    # 3. Admission: the candidate support constants against the primary chart token and the
    #    artifact's request-feed inventory (this --so, hashed and never loaded; the inventory
    #    beside it). Raises InventoryAdmissionError (window_mode_unsupported) before any feed,
    #    ctypes library, strategy or phase record. The refusal keeps its code and detail.
    try:
        request_feed_inventory.admit_selected_request(
            request.script_tf, args.request_feed_inventory, args.so, inputs,
            swr.support_policy())
    except request_feed_inventory.InventoryAdmissionError as error:
        raise RunFailure(error.message, error.code, error.detail) from None

    # 4. Preflight: the primary feed is read once and planned by the native planner.
    _advance(phases, "preflight")
    bars, n, source_values_sha256 = load_bars(args.ohlcv)
    source_bytes_sha256 = _sha256_file(args.ohlcv)
    if source_bytes_sha256 is None:
        raise _bars_unreadable(f"--ohlcv: {args.ohlcv}: the file cannot be read", "io")
    timezone_text, session_text = _syminfo_clock(args.syminfo) if args.syminfo else ("", "")
    import selected_window_plan
    planner = selected_window_plan.SelectedPrimaryPlanner(trusted_planner_path())
    plan = planner.plan(
        bars, n, start_ms=request.start_ms, end_ms=request.end_ms,
        fed_start_ms=request.fed_start_ms, preroll_bars=request.preroll_bars,
        input_tf=request.input_tf, script_tf=request.script_tf,
        chart_timezone=args.chart_tz, engine_timezone=timezone_text, session=session_text,
        feed_tolerant=True)
    refusal = swr.refusal_for_plan(
        plan, request, timezone=swr.calendar_timezone(args.chart_tz),
        session=swr.calendar_session(session_text))
    if refusal is not None:
        raise refusal
    # The retained rows are a view of the same allocation; the evaluated hash restarts the
    # existing primary hash domain over them (equal to the supplied hash when nothing is cut).
    retained, fed_count = swr.retained_rows(
        bars, n, plan["trim_index"], plan["fed_input_bars"], BarC)
    evaluated_values_sha256 = source_values_sha256
    if plan["trim_index"]:
        hasher = _new_source_feed_hasher()
        swr.update_rows_hash(hasher, bars, plan["trim_index"], fed_count, BarC,
                             _SOURCE_FEED_RECORD)
        evaluated_values_sha256 = hasher.hexdigest()
    feed = {
        "input_tf_seconds": request.input_tf_seconds,
        "source_bytes_sha256": source_bytes_sha256,
        "source_values_sha256": source_values_sha256,
        "evaluated_source_values_sha256": evaluated_values_sha256,
    }
    calendar = swr.calendar_record(args.chart_tz, session_text)
    if args.validate_window_only:
        # Not execution evidence: the counts are the plan's, no strategy was loaded or run.
        window = swr.build_report_window(request, plan, applied=False, feed=feed,
                                         calendar=calendar)
        return _write_report(json.dumps(
            {"engine": "pineforge", "validation_only": True, "report_window": window},
            separators=(",", ":")) + "\n")
    symbol_feeds = load_symbol_feeds(args.symbol_feeds) if args.symbol_feeds else None

    # 5. Execution begins before the strategy library loads.
    _advance(phases, "execution")
    lib = load_strategy(args.so)
    checked = uses_checked_settings(lib)
    if args.outputs:
        require_outputs(lib)
    abi = swr.SelectedWindowAbi(lib)
    vw_on = bool(args.magnifier_volume_weighted) and bar_magnifier == 1
    syminfo_applied: dict = {}

    def _make_state():
        """A fresh, fully configured strategy state (the ordinary run's setup without the
        legacy trade-start gate, which selected mode refuses); freed again if it fails."""
        nonlocal syminfo_applied
        st = create_strategy(lib, checked)
        try:
            apply_settings(lib, st, inputs, overrides, checked)
            if args.syminfo:
                r = apply_syminfo(lib, st, args.syminfo)
                syminfo_applied = r if isinstance(r, dict) else {}
            if symbol_feeds:
                install_symbol_feeds(lib, st, symbol_feeds)
            if args.chart_tz and hasattr(lib, "strategy_set_chart_timezone"):
                lib.strategy_set_chart_timezone(st, args.chart_tz.encode())
            if vw_on and hasattr(lib, "strategy_set_magnifier_volume_weighted"):
                lib.strategy_set_magnifier_volume_weighted(st, 1)
            if args.outputs:
                enable_outputs(lib, st)
        except BaseException:
            lib.strategy_free(st)
            raise
        return st

    state = _make_state()
    for sym in symbol_feeds or ():
        for feed_record in sym["feeds"]:
            feed_record["bars"] = feed_record["close_ms"] = None
    report = ReportC()
    binding = None
    started = time.time()
    try:
        settings_receipt = _release_settings_receipt(lib, state, checked)
        abi.configure(state, request.start_ms, request.end_ms)
        binding = _observer_binding(lib, state, phases.writer)
        binding.attach()
        lib.run_backtest_full(
            state, retained, fed_count,
            input_tf, script_tf,
            bar_magnifier, magnifier_samples, magnifier_dist,
            ctypes.byref(report))
        elapsed = time.time() - started
        # The callback's retained exception comes before any report access.
        binding.raise_if_failed()
        failure = run_failure(lib, state)
        if failure is not None:
            # After the acknowledged boundary an engine out_of_memory or unclassified failure
            # is report_post_execution_failed; a failure during execution is unchanged.
            write_failure(*_post_boundary_engine_failure(failure, phases))
            return 1
        _require_result_assembly(phases)
        # The engine's own counts must equal the plan's, and belong to the observed attempt.
        swr.check_native_counts(plan, abi.counts(state), _read_observation(lib, state))
        if (int(report.input_bars_processed) != plan["fed_input_bars"]
                or int(report.script_bars_processed) != plan["fed_script_bars"]):
            raise _engine_invariant(
                "the report's processed counts differ from the plan's evaluated-fed counts")
        if int(report.input_tf_seconds) != request.input_tf_seconds:
            raise _engine_invariant(
                f"the engine reports input_tf_seconds {int(report.input_tf_seconds)}, the "
                f"request's input timeframe is {request.input_tf_seconds} seconds")
        if int(report.equity_curve_len) != plan["window_script_bars"] + 1:
            raise _engine_invariant(
                f"the engine's selected curve has {int(report.equity_curve_len)} points, the "
                f"plan says {plan['window_script_bars'] + 1}")
        # The report spells the evaluated first and last bar as UTC dates (fmt_utc).
        for ts in (plan["fed_first_data_ms"], plan["fed_last_data_ms"]):
            if ts is None:
                continue
            try:
                fmt_utc(ts)
            except (ValueError, OverflowError, OSError):
                raise _bars_unreadable(
                    f"--ohlcv: {args.ohlcv}: timestamp {ts} is out of the calendar's range",
                    "value") from None
        window = swr.build_report_window(request, plan, applied=True, feed=feed,
                                         calendar=calendar)
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
            "trade_start_ms":     None,
            "chart_tz":           args.chart_tz or "",
        }
        if syminfo_applied:
            applied_runtime["syminfo"] = syminfo_applied
        if symbol_feeds:
            applied_runtime["symbol_feeds"] = symbol_feeds_record(symbol_feeds)
        outputs_block = None
        if args.outputs:
            outputs_block = build_outputs_block(make_outputs_reader(lib, state))
            applied_runtime["outputs"] = True
        # ONE R object, placed in all three required places (the third is
        # fingerprint.provenance.runtime, which is applied_runtime itself).
        applied_runtime["report_window"] = window
        incarnation_accessor = getattr(lib, "strategy_closed_trade_entry_incarnation", None)
        trade_entry_incarnations = (
            [int(incarnation_accessor(state, i)) for i in range(report.trades_len)]
            if incarnation_accessor is not None else None)
        out = build_report_dict(
            report, args.ohlcv, fed_count, plan["fed_first_data_ms"], plan["fed_last_data_ms"],
            elapsed, inputs, overrides, applied_runtime, trade_entry_incarnations)
        out["report_window"] = window
        out["report_shape"] = swr.report_shape_full(len(out["equity_curve"]),
                                                    len(out["trades"]))
        if outputs_block is not None:
            out["outputs"] = outputs_block
        # The selected fingerprint MUST exist, as version 2. There is no best-effort null here
        # (the ordinary report keeps its own): any failure below ends the run before anything is
        # serialized, as its own typed code, or, when otherwise unclassified, as
        # report_post_execution_failed of the except clause at the end of this try.
        provenance = build_provenance(
            engine_version(lib),
            None,
            parse_bool(args.transpiled),
            inputs,
            overrides,
            applied_runtime,
            source_feed_sha256=source_values_sha256,
        )
        provenance["codegen"]["generated_cpp_sha256"] = (
            _sha256_file(args.generated_cpp) if args.generated_cpp else None)
        cpp_text = ""
        if args.generated_cpp:
            with open(args.generated_cpp, encoding="utf-8", errors="replace") as source:
                cpp_text = source.read()
        provenance = normalize_release_provenance(
            provenance, cpp_text, settings_receipt, checked)
        provenance["schema_version"] = swr.FINGERPRINT_VERSION
        fingerprint = build_fingerprint(provenance)
        fingerprint["version"] = swr.FINGERPRINT_VERSION
        out["fingerprint"] = fingerprint
        # R in all three places, the v2 fingerprint over the same bytes, and the anchor of the
        # native curve at the strategy's initial capital: all checked before serialization.
        swr.check_report_placements(out, window)
        swr.check_selected_curve(out["equity_curve"], request.start_ms,
                                 plan["window_script_bars"],
                                 provenance["strategy"].get("initial_capital"))
        # 6. Serialization begins; the timing is known now and is outside the provenance.
        _advance(phases, "serialization")
        out["diagnostics"]["phase_timing"] = phases.writer.phase_timing()
        buffer = io.StringIO()
        json.dump(out, buffer, separators=(",", ":"))
        buffer.write("\n")
    except Exception as error:
        # Once the engine has acknowledged result_assembly (the REAL writer phase), an
        # unclassified failure is report_post_execution_failed{phase, reason}; typed faults,
        # and anything while still executing, are raised unchanged.
        classified = _post_boundary_failure(error, phases)
        if classified is None:
            raise
        raise classified from error
    finally:
        # Unregister the observer, clear the selection and free, on every path; the binding
        # (callback and descriptor) stays referenced until after the state is freed.
        detach_error = _detach_observer(binding)
        try:
            abi.clear(state)
        except Exception:
            pass
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)
        if detach_error is not None and sys.exc_info()[1] is None:
            raise _typed_failure(detach_error) or detach_error
    status = _write_report(buffer.getvalue(), phases)
    if status:
        return status
    try:
        phases.advance("completed")  # only after the flush
    except RunFailure as error:
        # The whole report is already out, so no failure line may follow it, and nothing may
        # claim success: this is the harness fault (exit status 1, stderr), never a result.
        print(f"run_json: harness_internal_error: the completed phase record could not be "
              f"handed over: {error}", file=sys.stderr)
        return 1
    return 0


def main(argv=None) -> int:
    """Run the harness. Every failure ends as the one failure line on stdout:
    run_json's own (RunFailure) with its code, a selected-window helper's typed error
    (_typed_failure) with its code, and anything unexpected as
    harness_internal_error (its traceback on stderr). Exit status 1, or 2 for a
    command line argparse refuses. The one exception is a success report stdout
    cannot take whole (_main): exit 1 with the reason on stderr and no line after
    the part written."""
    try:
        return _main(argv)
    except RunFailure as failure:
        write_failure(str(failure), failure.code, failure.code_args)
        return failure.exit_status
    except Exception as error:
        typed = _typed_failure(error)
        if typed is not None:
            write_failure(str(typed), typed.code, typed.code_args)
            return typed.exit_status
        traceback.print_exc(file=sys.stderr)
        write_failure(f"harness internal error: {type(error).__name__}: {error}",
                      "harness_internal_error")
        return 1


def _legacy_abbreviations(argv, legacy_long, all_long):
    """argv (sys.argv[1:] when None) as a list, with every FORMERLY UNIQUE legacy long-option
    prefix spelled in full. argparse accepts a prefix of a long option when exactly one option
    begins with it; the selected-window flags (--report-policy, --window-start-ms, ...) make
    some prefixes that were unique ambiguous (--r, --re and --rep for --repeats, --c for
    --chart-tz, --w for --warmup), which would turn a working ordinary command line into an
    argparse error. A token `--name[=value]` whose name is not itself an option of `all_long`
    and is a prefix of exactly one option of `legacy_long` (the long options captured before
    the new flags were added) becomes that option, `=value` kept exactly as written. A prefix
    of several legacy options (ambiguous before, ambiguous still) and a prefix of none (an
    abbreviation of a new flag, left to argparse) are not touched; neither is any token that
    does not begin with `--`, any token after a bare `--`, or any value."""
    tokens = list(sys.argv[1:] if argv is None else argv)
    out = []
    for index, token in enumerate(tokens):
        if token == "--":
            out.extend(tokens[index:])
            break
        if isinstance(token, str) and token.startswith("--"):
            name, equals, value = token.partition("=")
            if name not in all_long:
                matches = [option for option in legacy_long if option.startswith(name)]
                if len(matches) == 1:
                    token = matches[0] + equals + value
        out.append(token)
    return out


def _main(argv=None) -> int:
    ap = _ArgumentParser(description=__doc__,
                         formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--so",        type=Path, required=True, help="strategy.so path")
    ohlcv_option = ap.add_argument("--ohlcv", type=Path, required=True, help="OHLCV CSV path")
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
    ap.add_argument("--symbol-feeds", type=Path, default=None,
                    help="JSON index of other symbols' bars (and syminfo) that "
                         "request.security reads, keyed by the exact symbol string "
                         "and timeframe (strategy_set_symbol_feed / _facts); see "
                         "load_symbol_feeds.")
    ap.add_argument("--outputs", action="store_true",
                    help="Record the library's outputs (a library compiled to record "
                         "them) and write them as the report's \"outputs\" block; see "
                         "build_outputs_block. A failure on a library that records none.")
    # The long options of the command line BEFORE the selected-window flags below; see
    # _legacy_abbreviations (the option strings of the parser, captured here and not typed out).
    legacy_long = tuple(s for s in ap._option_string_actions if s.startswith("--"))
    # Selected-window mode (selected-window/v1; see the section above main). All of these are
    # absent from an ordinary command line, which then behaves exactly as before.
    ap.add_argument("--report-policy", default=None,
                    help="selected-window/v1 opts into the selected-window report; absent = "
                         "the ordinary report, unchanged.")
    ap.add_argument("--window-start-ms", default=None,
                    help="Selected window start T (unix ms, calendar-aligned).")
    ap.add_argument("--window-end-ms", default=None,
                    help="Selected window end E (unix ms, calendar-aligned), T < E.")
    ap.add_argument("--preroll-bars", default=None,
                    help="Requested pre-roll N in script bars (0..5000).")
    ap.add_argument("--fed-start-ms", default=None,
                    help="Frozen start F of the supplied primary feed (unix ms), F <= T.")
    ap.add_argument("--capabilities-json", action="store_true",
                    help="With --so only: print this runtime's selected-window capabilities "
                         "as JSON. Reads no OHLCV and runs nothing.")
    ap.add_argument("--validate-window-only", action="store_true",
                    help="With the selected-window options: validate and plan the loaded "
                         "feed and print report_window (applied=false); runs no strategy.")
    ap.add_argument("--run-phase-fd", default=None,
                    help="N >= 3: an inherited AF_UNIX SOCK_STREAM descriptor that receives "
                         "the run's phase records (phase transport v1).")
    ap.add_argument("--request-feed-inventory", type=Path, default=None,
                    help="The request-feed inventory bound to --so; read only by a "
                         "selected-window request, for its admission.")
    ap.add_argument("--report-shape", default=None,
                    help="This build offers full/v1 only, for selected-window requests.")
    ap.add_argument("--curve-point-budget", default=None,
                    help="Refused: only the compact shape takes a budget, which this build "
                         "does not offer.")
    ap.add_argument("--results-digest", default=None,
                    help="Refused: this build offers no results digest.")
    all_long = tuple(s for s in ap._option_string_actions if s.startswith("--"))
    argv = _legacy_abbreviations(argv, legacy_long, all_long)
    # --capabilities-json is a probe of the runtime: it needs --so and no OHLCV. Every other
    # command line keeps --ohlcv required, exactly as before.
    probe = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    probe.add_argument("--capabilities-json", action="store_true")
    if probe.parse_known_args(argv)[0].capabilities_json:
        ohlcv_option.required = False
    args = ap.parse_args(argv)

    if args.capabilities_json:
        return _capabilities_main(args)
    selected = _selected_requested(args)
    if (selected or args.report_shape is not None or args.curve_point_budget is not None
            or args.results_digest is not None):
        import selected_window_report
        selected_window_report.check_unsupported_options(
            selected=selected, report_shape=args.report_shape,
            curve_point_budget=args.curve_point_budget, results_digest=args.results_digest)

    inputs    = parse_kv_json(args.inputs,    "--inputs")
    overrides = parse_kv_json(args.overrides, "--overrides")
    input_tf  = _utf8_option(args.input_tf.strip(), "--input-tf")
    script_tf = _utf8_option(args.script_tf.strip(), "--script-tf")
    _utf8_option(args.chart_tz, "--chart-tz")  # the setter takes it as UTF-8
    bar_magnifier = 1 if parse_bool(args.bar_magnifier) else 0
    magnifier_samples = max(2, int(args.magnifier_samples))
    magnifier_dist = parse_magnifier_dist(args.magnifier_dist)

    if selected:
        return _selected_run(args, inputs, overrides, input_tf, script_tf, bar_magnifier,
                             magnifier_samples, magnifier_dist)

    # Ordinary mode. --run-phase-fd alone adds the run's phase records (preflight, execution,
    # result_assembly through the engine's observer, serialization, completed after the
    # flush); the report and every other output are unchanged. None without the flag.
    phases = _open_phases(args.run_phase_fd, always=False)
    _advance(phases, "preflight")
    bars, n, source_feed_sha256 = load_bars(args.ohlcv)
    if n == 0:
        raise _bars_unreadable(f"--ohlcv: {args.ohlcv}: no bars", "empty")
    first_ts, last_ts = bars[0].timestamp, bars[n - 1].timestamp

    _advance(phases, "execution")
    lib = load_strategy(args.so)
    checked = uses_checked_settings(lib)
    if args.outputs:
        require_outputs(lib)

    # Volume-weighted magnifier only meaningful when the magnifier is on.
    vw_on = bool(args.magnifier_volume_weighted) and bar_magnifier == 1

    # The lot grid the last _make_state() applied (the body run's), for
    # applied_runtime["syminfo"]; {} when none.
    syminfo_applied: dict = {}
    # Other symbols' bars, read once before the first state (None without
    # --symbol-feeds).
    symbol_feeds = None

    def _make_state():
        """Create + fully configure a fresh strategy state — everything EXCEPT the
        timed run_backtest_full call. Mirrors scripts/run_strategy.py's setup so the
        engine behaves identically to the ctypes validation harness. The handle is
        checked before anything is set on it; a state that fails to configure is
        freed before its failure propagates."""
        nonlocal syminfo_applied
        st = create_strategy(lib, checked)
        try:
            apply_settings(lib, st, inputs, overrides, checked)
            if args.syminfo:
                # A replacement apply_syminfo may return None (or another non-dict).
                r = apply_syminfo(lib, st, args.syminfo)
                syminfo_applied = r if isinstance(r, dict) else {}
            if symbol_feeds:
                install_symbol_feeds(lib, st, symbol_feeds)
            if args.trade_start_ms is not None and hasattr(lib, "strategy_set_trade_start_time"):
                lib.strategy_set_trade_start_time(st, int(args.trade_start_ms))
            if args.chart_tz and hasattr(lib, "strategy_set_chart_timezone"):
                lib.strategy_set_chart_timezone(st, args.chart_tz.encode())
            if vw_on and hasattr(lib, "strategy_set_magnifier_volume_weighted"):
                lib.strategy_set_magnifier_volume_weighted(st, 1)
            if args.outputs:
                enable_outputs(lib, st)
        except BaseException:
            lib.strategy_free(st)
            raise
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
    # already happened above (load_strategy), outside any loop. A rejected
    # --syminfo / --symbol-feeds / setting raises its RunFailure before any
    # stdout; main() prints it.
    timing = None
    if args.symbol_feeds:
        symbol_feeds = load_symbol_feeds(args.symbol_feeds)
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
    # The body state is the last one installed and the engine copied the
    # feeds' arrays: release them before the run (the records stay).
    for sym in symbol_feeds or ():
        for feed in sym["feeds"]:
            feed["bars"] = feed["close_ms"] = None
    report = ReportC()
    binding = None
    started = time.time()
    try:
        settings_receipt = _release_settings_receipt(lib, state, checked)
        if phases is not None:
            binding = _observer_binding(lib, state, phases.writer)
            binding.attach()
        _run(state, report)
        elapsed = time.time() - started
        if binding is not None:
            binding.raise_if_failed()  # the callback's exception, before any report access
        failure = run_failure(lib, state)
        if failure is not None:
            # Unchanged without --run-phase-fd; with it, an out_of_memory or unclassified engine
            # failure after the acknowledged boundary is report_post_execution_failed.
            write_failure(*_post_boundary_engine_failure(failure, phases))
            return 1
        if phases is not None:
            _require_result_assembly(phases)
        # The report spells the first and last bar as UTC dates (fmt_utc).
        # Checked after the run, so the engine's own refusal of the same tape
        # keeps its line.
        for ts in (first_ts, last_ts):
            try:
                fmt_utc(ts)
            except (ValueError, OverflowError, OSError):
                raise _bars_unreadable(
                    f"--ohlcv: {args.ohlcv}: timestamp {ts} is out of the calendar's range",
                    "value") from None
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
        if syminfo_applied:
            applied_runtime["syminfo"] = syminfo_applied
        if symbol_feeds:
            applied_runtime["symbol_feeds"] = symbol_feeds_record(symbol_feeds)
        outputs_block = None
        if args.outputs:
            outputs_block = build_outputs_block(make_outputs_reader(lib, state))
            applied_runtime["outputs"] = True
        incarnation_accessor = getattr(
            lib, "strategy_closed_trade_entry_incarnation", None)
        trade_entry_incarnations = (
            [int(incarnation_accessor(state, i))
             for i in range(report.trades_len)]
            if incarnation_accessor is not None else None
        )
        out = build_report_dict(
            report, args.ohlcv, n, first_ts, last_ts,
            elapsed, inputs, overrides, applied_runtime,
            trade_entry_incarnations)
        if timing is not None:
            out["diagnostics"]["timing"] = timing
            out["diagnostics"]["throughput"] = _throughput_block(
                report.input_bars_processed, timing["samples_ns"],
                bar_magnifier=bar_magnifier)
        if outputs_block is not None:
            out["outputs"] = outputs_block
        try:
            # The frozen helpers' regex readers never see the C++: the release
            # reader below owns every declared value, and only the digest of
            # the file is taken here.
            provenance = build_provenance(
                engine_version(lib),
                None,
                parse_bool(args.transpiled),
                inputs,
                overrides,
                applied_runtime,
                source_feed_sha256=source_feed_sha256,
            )
            provenance["codegen"]["generated_cpp_sha256"] = (
                _sha256_file(args.generated_cpp) if args.generated_cpp else None)
            cpp_text = ""
            if args.generated_cpp:
                with open(args.generated_cpp, encoding="utf-8", errors="replace") as source:
                    cpp_text = source.read()
            provenance = normalize_release_provenance(
                provenance, cpp_text, settings_receipt, checked)
            out["fingerprint"] = build_fingerprint(provenance)
        except Exception:
            out["fingerprint"] = None
        _advance(phases, "serialization")  # phase records only: the report gains no field
        # Serialized whole before any byte is written (the same json.dump), so
        # a failure while it is built never follows half a report.
        buffer = io.StringIO()
        json.dump(out, buffer, separators=(",", ":"))
        buffer.write("\n")
    except Exception as error:
        # Without --run-phase-fd there is no boundary and this re-raises the error unchanged.
        classified = _post_boundary_failure(error, phases)
        if classified is None:
            raise
        raise classified from error
    finally:
        detach_error = _detach_observer(binding)  # None without --run-phase-fd
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)
        if detach_error is not None and sys.exc_info()[1] is None:
            raise _typed_failure(detach_error) or detach_error
    # A closed pipe or a full disk while the report is written: a failure line now would
    # follow part of a report, so none is written (see _write_report).
    status = _write_report(buffer.getvalue(), phases)
    if status:
        return status
    if phases is not None:
        try:
            phases.advance("completed")  # only after the flush
        except RunFailure as error:
            # The whole report is already out: no failure line may follow it, and nothing may
            # claim success; this is the harness fault, exit status 1 on stderr.
            print(f"run_json: harness_internal_error: the completed phase record could not be "
                  f"handed over: {error}", file=sys.stderr)
            return 1
    return 0


def _discard_stdout() -> None:
    """Point stdout's file descriptor at /dev/null after a failed report write,
    so what stdout still buffers is dropped instead of written again at exit."""
    try:
        fd = sys.stdout.fileno()
    except (AttributeError, OSError, ValueError):  # not a file: nothing to drop
        return
    devnull = os.open(os.devnull, os.O_WRONLY)
    try:
        os.dup2(devnull, fd)
    finally:
        os.close(devnull)


if __name__ == "__main__":
    sys.exit(main())
