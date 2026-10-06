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

A failed run prints one line instead, exit status 1 (2 for a command line
argparse refuses):
    {"engine":"pineforge","error":"<text>","code":"<code>","args":{...}}
That is the run's own error, a --syminfo the harness rejects (see
apply_syminfo), a --symbol-feeds it cannot install (see load_symbol_feeds), a
setting the strategy refuses, or any other failure of the harness (see
failure_line and main). "code" is a stable code of the closed vocabulary
docker/run_failure_codes.json and "args" its typed arguments. The engine's code
is read only from strategy_get_last_error_code and its args from
strategy_get_last_error_args; for a run error from a library without those
getters the line has no "code" and no "args" and is byte-identical to the
earlier {"engine":"pineforge","error":"<text>"} line.

NaN convention: any metric with an empty/zero denominator is null (JSON has no
NaN); a real computed 0 stays 0. See the report-schema + metrics reference docs
for the per-field meaning of every metrics.* key.
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
import re
import struct
import sys
import time
import traceback
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


# --- The failure line -------------------------------------------------------
#
# Every failure prints ONE line on stdout, written by failure_line:
#     {"engine":"pineforge","error":"<text>","code":"<code>","args":{...}}
# "engine" and "error" lead, so the line still starts with
# {"engine":"pineforge","error":" and "error" is the English text, unchanged
# wherever one existed before codes. "code" is a code of the closed vocabulary
# docker/run_failure_codes.json and "args" its typed arguments. The code is the
# engine's (strategy_get_last_error_code / _args), or run_json's own for a
# failure it finds itself (RunFailure): never read from any text. A run error
# from a library without the code getter prints neither key, so that line is
# the earlier one byte for byte (the app keeps its text rules for it).
#
# Caps keep the line well inside the 64 KiB the app reads: the text is cut at
# 16 KiB and each string argument at 1 KiB of UTF-8, on a character boundary;
# a line still over 60 KiB (only text that JSON escapes heavily, control
# characters are six bytes each) has its text cut further until it fits.

ERROR_TEXT_MAX = 16 * 1024
ERROR_ARG_TEXT_MAX = 1024
ERROR_LINE_MAX = 60 * 1024
_CODE_RE = re.compile(r"[a-z][a-z0-9_]{2,47}")

# A failed run that reported neither a text nor a code (strategy_last_run_status
# 1 and nothing else): run_json's own engine_unclassified_error.
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


def _dump_line(doc: dict) -> str:
    # The writer the failure line has always used: json.dump with these
    # separators and json's default ensure_ascii, so the line is ASCII.
    out = io.StringIO()
    json.dump(doc, out, separators=(",", ":"))
    return out.getvalue()


def failure_line(text, code=None, code_args=None) -> str:
    """The one failure line, newline included. Without a code (a run error from
    a library without strategy_get_last_error_code) it is exactly the earlier
    {"engine":"pineforge","error":"<text>"} line."""
    full = str(text)
    doc = {"engine": "pineforge", "error": _cut_utf8(full, ERROR_TEXT_MAX)}
    if code is not None:
        doc["code"] = code
        doc["args"] = {name: _cut_utf8(value, ERROR_ARG_TEXT_MAX)
                       if isinstance(value, str) else value
                       for name, value in (code_args or {}).items()}
    line = _dump_line(doc)
    if len(line) > ERROR_LINE_MAX:
        low, high = 0, len(doc["error"].encode("utf-8", "surrogatepass"))
        while low < high:  # the longest cut whose line fits
            mid = (low + high + 1) // 2
            doc["error"] = _cut_utf8(full, mid)
            if len(_dump_line(doc)) <= ERROR_LINE_MAX:
                low = mid
            else:
                high = mid - 1
        doc["error"] = _cut_utf8(full, low)
        if code is not None and len(_dump_line(doc)) > ERROR_LINE_MAX:
            doc["args"] = {}  # arguments no registry could hold: the code stays
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

    lib.strategy_free.argtypes = [ctypes.c_void_p]
    lib.report_free.argtypes   = [ctypes.POINTER(ReportC)]
    return lib


# --- Creating a strategy and applying the run's settings ----------------------
#
# A library exporting the checked settings API (strategy_settings_api_version()
# == 1, docs/checked-settings.md) is created and configured through it, so a
# setting the strategy cannot honour fails the run before it starts: an unknown
# input or override key, an invalid enum, an unparseable value, which the
# legacy setters drop silently. A library without it keeps the legacy setters.

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


def uses_checked_settings(lib) -> bool:
    """True when lib exports the checked settings API at version 1."""
    if not all(hasattr(lib, name) for name in _CHECKED_SETTINGS_EXPORTS):
        return False
    return lib.strategy_settings_api_version() == 1


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
            if not isinstance(file, str) or not file:
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
        },
        "trades": trades,
        "metrics": metrics,
        "equity_curve": equity_curve,
    }


def _request_invalid(text: str, option: str, exit_status: int = 1) -> RunFailure:
    return RunFailure(text, "run_request_invalid", {"option": option},
                      exit_status=exit_status)


def parse_kv_json(s: str | None, label: str) -> dict[str, str]:
    """Parse a JSON object of {key: value} into a {str: str} map.
    Empty / None / "{}" → {}. Non-object payloads abort with a clear
    error so junk env vars don't silently noop: run_request_invalid{option}
    (the label without its dashes)."""
    if not s or s.strip() in ("", "{}"):
        return {}
    option = label.lstrip("-").replace("-", "_")
    try:
        obj = json.loads(s)
    except (json.JSONDecodeError, RecursionError) as e:
        raise _request_invalid(f"error: {label} is not valid JSON: {e}", option) from None
    if not isinstance(obj, dict):
        raise _request_invalid(
            f"error: {label} must be a JSON object, got {type(obj).__name__}", option)
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
    empty message fails: text "", code strategy_runtime_error. With a library
    without strategy_get_last_error_code a failure with a text keeps the earlier
    line (no code); a failure reported with no code at all (status 1 alone, or a
    text the code getter does not name) is run_json's engine_unclassified_error."""
    text = ""
    if hasattr(lib, "strategy_get_last_error"):
        text = _c_text(lib.strategy_get_last_error(strat))
    engine = engine_failure_code(lib, strat)
    status = (lib.strategy_last_run_status(strat)
              if hasattr(lib, "strategy_last_run_status") else 0)
    code, args = engine if engine is not None else (None, None)
    if not (text or code or status == 1):
        return None
    if engine is None and text:
        return text, None, None
    if code:
        return text, code, args
    return text or RUN_STATUS_FAILED_TEXT, "engine_unclassified_error", {}


def main(argv=None) -> int:
    """Run the harness. Every failure ends as the one failure line on stdout:
    run_json's own (RunFailure) with its code, and anything unexpected as
    harness_internal_error (its traceback on stderr). Exit status 1, or 2 for a
    command line argparse refuses."""
    try:
        return _main(argv)
    except RunFailure as failure:
        write_failure(str(failure), failure.code, failure.code_args)
        return failure.exit_status
    except Exception as error:
        traceback.print_exc(file=sys.stderr)
        write_failure(f"harness internal error: {type(error).__name__}: {error}",
                      "harness_internal_error")
        return 1


def _main(argv=None) -> int:
    ap = _ArgumentParser(description=__doc__,
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
    ap.add_argument("--symbol-feeds", type=Path, default=None,
                    help="JSON index of other symbols' bars (and syminfo) that "
                         "request.security reads, keyed by the exact symbol string "
                         "and timeframe (strategy_set_symbol_feed / _facts); see "
                         "load_symbol_feeds.")
    args = ap.parse_args(argv)

    inputs    = parse_kv_json(args.inputs,    "--inputs")
    overrides = parse_kv_json(args.overrides, "--overrides")
    input_tf  = args.input_tf.strip().encode()
    script_tf = args.script_tf.strip().encode()
    bar_magnifier = 1 if parse_bool(args.bar_magnifier) else 0
    magnifier_samples = max(2, int(args.magnifier_samples))
    magnifier_dist = parse_magnifier_dist(args.magnifier_dist)

    bars, n, source_feed_sha256 = load_bars(args.ohlcv)
    if n == 0:
        raise _bars_unreadable(f"--ohlcv: {args.ohlcv}: no bars", "empty")
    first_ts, last_ts = bars[0].timestamp, bars[n - 1].timestamp

    lib = load_strategy(args.so)
    checked = uses_checked_settings(lib)

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
    started = time.time()
    try:
        _run(state, report)
        elapsed = time.time() - started
        failure = run_failure(lib, state)
        if failure is not None:
            write_failure(*failure)
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
        if syminfo_applied:
            applied_runtime["syminfo"] = syminfo_applied
        if symbol_feeds:
            applied_runtime["symbol_feeds"] = symbol_feeds_record(symbol_feeds)
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
        try:
            out["fingerprint"] = build_fingerprint(build_provenance(
                engine_version(lib),
                args.generated_cpp,
                parse_bool(args.transpiled),
                inputs,
                overrides,
                applied_runtime,
                source_feed_sha256=source_feed_sha256,
            ))
        except Exception:
            out["fingerprint"] = None
        # Serialized whole before any byte is written (the same json.dump), so
        # a failure while it is built never follows half a report.
        buffer = io.StringIO()
        json.dump(out, buffer, separators=(",", ":"))
        buffer.write("\n")
    finally:
        lib.report_free(ctypes.byref(report))
        lib.strategy_free(state)
    sys.stdout.write(buffer.getvalue())
    return 0


if __name__ == "__main__":
    sys.exit(main())
