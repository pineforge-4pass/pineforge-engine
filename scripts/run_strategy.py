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

With a TradingView tape (inputs.json::tv_trades_csv, default
strategy_dir/tv_trades.csv) the emit window is TradingView's own: order
commands are ignored before the chart bar that precedes TV's first entry
bar (the bar the first entry was placed on), and the trades written are
those entered inside the tape's span — _tv_entry_emit_window states the
exact rule.
"""
from __future__ import annotations

import argparse
import base64
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
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import NamedTuple

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
    "aux_security_ohlcv_csv",
    "aux_security_input_tf",
    "native_security_feeds",
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
# include/pineforge/engine.hpp (initial_capital_, close-timing mode,
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


# Timestamped quote->account FX is fingerprinted from the exact values passed
# through the C ABI, not merely from provider-file metadata.  The raw file hash
# remains useful provenance, while this canonical digest proves the effective
# broker input independently of JSON key order or whitespace.
ACCOUNT_CURRENCY_FX_CANONICALIZATION = "pf-account-currency-fx-i64-f64-le-v1"
_ACCOUNT_CURRENCY_FX_HASH_PREFIX = \
    b"pineforge:account-currency-fx:i64-f64-le:v1\0"
_ACCOUNT_CURRENCY_FX_RECORD = struct.Struct("<qd")


def _account_currency_fx_values_sha256(timestamps, rates) -> str:
    if len(timestamps) != len(rates) or not timestamps:
        raise ValueError(
            "account_currency_fx_series requires equal non-empty timestamp/rate arrays")
    h = hashlib.sha256()
    h.update(_ACCOUNT_CURRENCY_FX_HASH_PREFIX)
    previous_timestamp = None
    for raw_timestamp, raw_rate in zip(timestamps, rates):
        timestamp = int(raw_timestamp)
        rate = float(raw_rate)
        if previous_timestamp is not None and timestamp <= previous_timestamp:
            raise ValueError(
                "account_currency_fx_series timestamps must be strictly increasing")
        if not math.isfinite(rate) or rate <= 0.0:
            raise ValueError(
                "account_currency_fx_series rates must be positive and finite")
        h.update(_ACCOUNT_CURRENCY_FX_RECORD.pack(timestamp, rate))
        previous_timestamp = timestamp
    return h.hexdigest()


def _effective_account_currency_fx_scalar(run_kwargs: dict) -> float:
    metadata = run_kwargs.get("syminfo_metadata")
    raw_value = metadata.get("account_currency_fx", 1.0) \
        if isinstance(metadata, dict) else 1.0
    try:
        value = float(raw_value)
    except (TypeError, ValueError):
        return 1.0
    return value if math.isfinite(value) and value > 0.0 else 1.0


def build_runtime_provenance(run_kwargs: dict, trade_start_ms: int | None) -> dict:
    """Return the effective runtime settings that participate in a fingerprint."""
    fx_scalar = _effective_account_currency_fx_scalar(run_kwargs)
    fx_series = run_kwargs.get("account_currency_fx_series")
    fx_summary = None
    if fx_series:
        timestamps, rates = fx_series
        fx_summary = {
            "canonicalization": ACCOUNT_CURRENCY_FX_CANONICALIZATION,
            "effective_values_sha256":
                _account_currency_fx_values_sha256(timestamps, rates),
            "source_file_sha256": run_kwargs.get(
                "account_currency_fx_source_sha256") or "",
            "fallback_account_per_quote": fx_scalar,
            "points": len(timestamps),
            "first_effective_ms": int(timestamps[0]),
            "last_effective_ms": int(timestamps[-1]),
        }
    runtime = {
        "input_tf": run_kwargs.get("input_tf") or "",
        "script_tf": run_kwargs.get("script_tf") or "",
        "bar_magnifier": bool(run_kwargs.get("bar_magnifier")),
        "magnifier_samples": int(run_kwargs.get("magnifier_samples") or 4),
        "magnifier_distribution": run_kwargs.get("magnifier_distribution") or "ENDPOINTS",
        "chart_timezone": run_kwargs.get("chart_timezone") or "",
        "trade_start_ms": None if trade_start_ms is None else int(trade_start_ms),
        "account_currency_fx_scalar": fx_scalar,
        "account_currency_fx_series": fx_summary,
    }
    aux_path = run_kwargs.get("aux_security_ohlcv_csv")
    if aux_path is not None:
        runtime["aux_security_feed"] = {
            "input_tf": run_kwargs.get("aux_security_input_tf") or "",
            "source_path": str(Path(aux_path).resolve()),
            "source_file_sha256": run_kwargs.get(
                "aux_security_source_file_sha256") or "",
            "source_values_sha256": run_kwargs.get(
                "aux_security_source_feed_sha256") or "",
        }
    native_feeds = run_kwargs.get("native_security_feeds")
    if native_feeds:
        # One entry per requested timeframe served by the exchange's own bars
        # (strategy_set_native_security_feed): the file identity resolved at
        # inputs time and, when the run supplied it, the parsed-values digest.
        runtime["native_security_feeds"] = {
            str(tf): {
                "source_path": str(Path(entry["path"]).resolve()),
                "source_file_sha256": entry.get("source_file_sha256") or "",
                "source_values_sha256": entry.get("source_values_sha256") or "",
            }
            for tf, entry in sorted(native_feeds.items())
        }
    return runtime


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
        # ABI v3: 1 on the range-end close of a position still open after the
        # final bar (TradingView's deep-backtest accounting; pineforge.h).
        ("open_at_end", ctypes.c_int32),
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
        ("broker_state_hash", ctypes.POINTER(ctypes.c_uint64)),
        ("broker_state_hash_len", ctypes.c_int64),
    ]


# ABI version this harness mirrors (PF_ABI_VERSION in pineforge.h).
# pf_report_t is CALLER-allocated: running an old .so against the v3
# ReportC mirror (or vice versa) silently corrupts memory, so the .so's
# pf_abi_version() export is asserted before any run. v3 appended
# pf_trade_t::open_at_end (TradeC above); v4 appended the live-runtime
# accessors and grew pf_report_t with the broker_state_hash array after
# equity_curve_len (ReportC above already carries both fields).
# test_run_strategy_range_end.py pins this constant to the header's
# macro, because the campaign's verifier runs every probe through THIS
# harness and a stale guard here is a run-error on every slug (the
# f-1d spark pre-check of 2026-09-02 found exactly that: ".so reports
# 3, harness expects 2" x64).
EXPECTED_PF_ABI = 4


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


class PfFieldDescC(ctypes.Structure):
    """Mirror of pf_field_desc_t (<pineforge/pending_order_mirror.hpp>): one
    row of the self-describing pf_pending_order_v1_t field table returned by
    strategy_pending_order_layout (ABI v4 live-runtime surface, task 7)."""
    _fields_ = [("name", ctypes.c_char_p), ("type", ctypes.c_char_p),
                ("offset", ctypes.c_uint32), ("size", ctypes.c_uint32)]


# C type spelling in a pf_field_desc_t row -> ctypes scalar. `char[N]` is
# handled separately (an N-byte c_char array).
_PF_FIELD_CTYPES = {
    "uint8_t": ctypes.c_uint8,
    "uint32_t": ctypes.c_uint32,
    "int32_t": ctypes.c_int32,
    "int64_t": ctypes.c_int64,
    "uint64_t": ctypes.c_uint64,
    "double": ctypes.c_double,
}
_PF_CHAR_ARRAY_RE = re.compile(r"^char\[(\d+)\]$")
PENDING_ORDER_STRUCT_VERSION = 1
# strategy_pending_order_fill_qty partition codes (pineforge.h, ABI v4 task 8).
FILL_QTY_PARTITIONS = {
    0: "EXPLICIT", 1: "FROZEN_PLACEMENT", 2: "DEFAULT_STOP_PLACEMENT", 3: "AT_FILL"}


def _nan_to_none(x: float) -> float | None:
    """NaN / inf sentinels -> None (JSON null); finite doubles unchanged."""
    x = float(x)
    return x if math.isfinite(x) else None


def _pending_order_layout(lib: ctypes.CDLL) -> list[tuple[str, str, int, int]]:
    """Read strategy_pending_order_layout() into [(name, type, offset, size)]."""
    count = ctypes.c_int(0)
    descs = lib.strategy_pending_order_layout(ctypes.byref(count))
    return [(descs[i].name.decode("ascii"), descs[i].type.decode("ascii"),
             int(descs[i].offset), int(descs[i].size)) for i in range(count.value)]


def build_pending_order_struct(layout: list[tuple[str, str, int, int]]) -> type:
    """Build the ctypes.Structure mirroring pf_pending_order_v1_t FROM THE
    RUNTIME'S OWN FIELD TABLE (strategy_pending_order_layout), never from a
    hand-typed field list: the mirror is append-only and generated from
    engine.hpp (scripts/gen_pending_order_mirror.py), so a reader typed by
    hand would silently desynchronise the first time intent row grows.
    Every ctypes offset/size is cross-checked against the table and a
    mismatch raises rather than mis-reading the book."""
    if not layout:
        raise RuntimeError("strategy_pending_order_layout returned no fields")
    fields = []
    for name, ctype, offset, size in layout:
        m = _PF_CHAR_ARRAY_RE.match(ctype)
        if m:
            ct = ctypes.c_char * int(m.group(1))
        elif ctype in _PF_FIELD_CTYPES:
            ct = _PF_FIELD_CTYPES[ctype]
        else:
            raise RuntimeError(
                f"strategy_pending_order_layout: field {name!r} has unknown C type "
                f"{ctype!r}; extend _PF_FIELD_CTYPES in run_strategy.py")
        if ctypes.sizeof(ct) != size:
            raise RuntimeError(
                f"strategy_pending_order_layout: field {name!r} ({ctype}) is {size} "
                f"bytes in the runtime but {ctypes.sizeof(ct)} in ctypes")
        fields.append((name, ct))
    if fields[0][0] != "struct_version" or fields[1][0] != "size":
        raise RuntimeError(
            "strategy_pending_order_layout: table must start with struct_version, size; "
            f"got {[f[0] for f in fields[:2]]}")
    cls = type("IntentRowV1", (ctypes.Structure,), {"_fields_": fields})
    for name, _ctype, offset, _size in layout:
        got = getattr(cls, name).offset
        if got != offset:
            raise RuntimeError(
                f"strategy_pending_order_layout: field {name!r} is at byte {offset} in the "
                f"runtime but ctypes placed it at {got} (natural alignment differs?)")
    last_name, _, last_off, last_size = layout[-1]
    if ctypes.sizeof(cls) < last_off + last_size:
        raise RuntimeError(
            f"strategy_pending_order_layout: sizeof(IntentRowV1) {ctypes.sizeof(cls)} < "
            f"end of {last_name!r} ({last_off + last_size})")
    return cls


def pending_order_to_dict(rec, layout: list[tuple[str, str, int, int]]) -> dict:
    """One pf_pending_order_v1_t record -> JSON-ready dict, in field order.
    char[N] -> str (up to the NUL); NaN/inf doubles -> None (JSON has no
    NaN; the engine's "not set" sentinel); EVERY uint64_t field (the
    *_hash64 digests, incarnation / *_incarnation, signal_close_mc_fill_seq,
    ...) -> 16-hex-digit string, because a uint64 is not a JS-safe number
    and a consumer must not silently round one; everything else (uint8_t /
    int32_t / int64_t / uint32_t / double) -> int/float."""
    out = {}
    for name, ctype, _offset, _size in layout:
        v = getattr(rec, name)
        if ctype.startswith("char["):
            out[name] = bytes(v).decode("utf-8", "replace")
        elif ctype == "double":
            out[name] = float(v) if math.isfinite(v) else None
        elif ctype == "uint64_t":
            out[name] = format(int(v), "016x")
        else:
            out[name] = int(v)
    return out


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


def _load_account_currency_fx_series_json(path: Path):
    """Load a bar-grain effective-time FX series JSON.

    Accepted shapes (effective timestamps already resolved by the producer):
      {"timestamps_ms": [int, ...], "rates": [float, ...], ...}
      {"points": [{"t"|"timestamp_ms"|"timestamp": int, "rate"|"c2a1": float}, ...]}

    Unlike the daily-close loader, this path does **not** apply a D+1 shift —
    the file is the production as-of curve copied straight into the C ABI.
    """
    raw_bytes = path.read_bytes()
    payload = json.loads(raw_bytes)
    points = []
    if isinstance(payload, dict) and isinstance(payload.get("timestamps_ms"), list)             and isinstance(payload.get("rates"), list):
        timestamps = payload["timestamps_ms"]
        rates = payload["rates"]
        if len(timestamps) != len(rates) or not timestamps:
            raise ValueError(
                f"account_currency_fx_series_json timestamps/rates mismatch: {path}")
        for ts, rate in zip(timestamps, rates):
            try:
                t_ms = int(ts)
                r = float(rate)
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"invalid account-currency FX series point {ts!r}: {rate!r}"
                ) from exc
            if isinstance(rate, bool) or not math.isfinite(r) or r <= 0.0:
                raise ValueError(
                    f"invalid account-currency FX series rate {ts!r}: {rate!r}")
            points.append((t_ms, r))
    elif isinstance(payload, dict) and isinstance(payload.get("points"), list):
        if not payload["points"]:
            raise ValueError(
                f"account_currency_fx_series_json points must be non-empty: {path}")
        for item in payload["points"]:
            if not isinstance(item, dict):
                raise ValueError(
                    f"invalid account-currency FX series point {item!r}: {path}")
            raw_t = item.get("t", item.get("timestamp_ms", item.get("timestamp")))
            raw_r = item.get("rate", item.get("c2a1"))
            try:
                t_ms = int(raw_t)
                r = float(raw_r)
            except (TypeError, ValueError) as exc:
                raise ValueError(
                    f"invalid account-currency FX series point {item!r}"
                ) from exc
            if isinstance(raw_r, bool) or not math.isfinite(r) or r <= 0.0:
                raise ValueError(
                    f"invalid account-currency FX series rate {item!r}")
            points.append((t_ms, r))
    else:
        raise ValueError(
            "account_currency_fx_series_json must contain timestamps_ms+rates "
            f"or points[]: {path}")
    points.sort()
    if any(points[i][0] <= points[i - 1][0] for i in range(1, len(points))):
        raise ValueError(
            f"account_currency_fx_series_json timestamps must be strictly increasing: {path}")
    return (
        ([point[0] for point in points], [point[1] for point in points]),
        hashlib.sha256(raw_bytes).hexdigest(),
    )


def _load_account_currency_fx_daily_closes(path: Path):
    """Load ``{"YYYY-MM-DD": close}`` and shift each close to D+1 00:00Z.

    TradingView applies the previous UTC day's quote/account close. The C ABI
    deliberately accepts effective timestamps instead of provider-specific
    daily semantics; this harness adapter owns the one-day shift.
    """
    raw_bytes = path.read_bytes()

    def reject_duplicate_keys(pairs):
        obj = {}
        for key, value in pairs:
            if key in obj:
                raise ValueError(
                    f"duplicate account-currency FX date {key!r}: {path}")
            obj[key] = value
        return obj

    payload = json.loads(raw_bytes, object_pairs_hook=reject_duplicate_keys)
    if not isinstance(payload, dict) or not payload:
        raise ValueError(
            f"account_currency_fx_daily_close_json must contain a non-empty object: {path}")
    points = []
    for day, raw_rate in payload.items():
        try:
            date = datetime.strptime(str(day), "%Y-%m-%d").replace(
                tzinfo=timezone.utc)
            rate = float(raw_rate)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"invalid account-currency FX point {day!r}: {raw_rate!r}") from exc
        if isinstance(raw_rate, bool) or not math.isfinite(rate) or rate <= 0.0:
            raise ValueError(f"invalid account-currency FX rate {day!r}: {raw_rate!r}")
        effective_ms = int((date + timedelta(days=1)).timestamp() * 1000)
        points.append((effective_ms, rate))
    points.sort()
    if any(points[i][0] <= points[i - 1][0] for i in range(1, len(points))):
        raise ValueError(f"duplicate account-currency FX dates: {path}")
    return (
        ([point[0] for point in points], [point[1] for point in points]),
        hashlib.sha256(raw_bytes).hexdigest(),
    )


def inputs_run_kwargs(params, strategy_dir: Path, default_ohlcv: Path,
                      default_chart_tz: str = "") -> tuple[Path, dict]:
    """Resolve per-probe ``inputs.json`` metadata into the OHLCV path and the
    keyword arguments for :meth:`Strategy.run`.

    This is the single source of truth for how harnesses honour
    ``ohlcv_csv`` / ``input_tf`` / ``script_tf`` / ``ohlcv_start_ms`` /
    ``aux_security_ohlcv_csv`` / ``aux_security_input_tf`` /
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
    aux_csv_present = "aux_security_ohlcv_csv" in params
    aux_tf_present = "aux_security_input_tf" in params
    aux_security_ohlcv_csv = None
    aux_security_input_tf = None
    aux_security_source_file_sha256 = None
    if aux_csv_present or aux_tf_present:
        aux_csv_value = str(params.get("aux_security_ohlcv_csv") or "")
        aux_tf_value = str(params.get("aux_security_input_tf") or "")
        if not aux_csv_value or not aux_tf_value:
            raise ValueError(
                "aux_security_ohlcv_csv and aux_security_input_tf must be set together")
        aux_security_ohlcv_csv = (
            Path(aux_csv_value) if aux_csv_value.startswith("/")
            else strategy_dir / aux_csv_value
        ).resolve()
        if not aux_security_ohlcv_csv.is_file():
            raise FileNotFoundError(
                "auxiliary request.security OHLCV export not found: "
                f"{aux_security_ohlcv_csv}")
        aux_security_input_tf = aux_tf_value
        aux_security_source_file_sha256 = _sha256_file(
            aux_security_ohlcv_csv)
        if aux_security_source_file_sha256 is None:
            raise OSError(
                "cannot hash auxiliary request.security OHLCV export: "
                f"{aux_security_ohlcv_csv}")
    # ``native_security_feeds``: {"D": "<csv>", ...} -- the exchange's own
    # bars of one higher timeframe each, served to the request.security
    # evaluators that request exactly that timeframe (TradingView's daily bar
    # on an intraday CME / NASDAQ / NSE chart is the settlement or official
    # close, which no aggregate of the chart feed reproduces). Paths resolve
    # like ``aux_security_ohlcv_csv``; each file is hashed here so the run can
    # refuse a file that changed between metadata resolution and execution.
    native_security_feeds = None
    if "native_security_feeds" in params:
        raw_feeds = params.get("native_security_feeds")
        if raw_feeds in (None, "", {}):
            raw_feeds = {}
        if not isinstance(raw_feeds, dict):
            raise ValueError(
                "native_security_feeds must map a timeframe to an OHLCV export path")
        native_security_feeds = {}
        for raw_tf, raw_path in raw_feeds.items():
            tf = str(raw_tf or "").strip()
            path_value = str(raw_path or "")
            if not tf or not path_value:
                raise ValueError(
                    "native_security_feeds entries need a timeframe and a path")
            feed_path = (
                Path(path_value) if path_value.startswith("/")
                else strategy_dir / path_value
            ).resolve()
            if not feed_path.is_file():
                raise FileNotFoundError(
                    f"native request.security OHLCV export not found for {tf}: "
                    f"{feed_path}")
            feed_sha = _sha256_file(feed_path)
            if feed_sha is None:
                raise OSError(
                    f"cannot hash native request.security OHLCV export: {feed_path}")
            native_security_feeds[tf] = {
                "path": feed_path,
                "source_file_sha256": feed_sha,
            }
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

    fx_series = None
    fx_source_sha256 = None
    fx_series_json = runtime_overrides.get(
        "account_currency_fx_series_json")
    fx_daily_json = runtime_overrides.get(
        "account_currency_fx_daily_close_json")
    if fx_series_json and fx_daily_json:
        raise ValueError(
            "runtime_overrides may set only one of "
            "account_currency_fx_series_json or "
            "account_currency_fx_daily_close_json")
    if fx_series_json:
        fx_path = Path(str(fx_series_json))
        if not fx_path.is_absolute():
            fx_path = (strategy_dir / fx_path).resolve()
        fx_series, fx_source_sha256 = \
            _load_account_currency_fx_series_json(fx_path)
    elif fx_daily_json:
        fx_path = Path(str(fx_daily_json))
        if not fx_path.is_absolute():
            fx_path = (strategy_dir / fx_path).resolve()
        fx_series, fx_source_sha256 = \
            _load_account_currency_fx_daily_closes(fx_path)

    kwargs = dict(
        strategy_overrides=strategy_overrides or None,
        chart_timezone=chart_tz,
        syminfo_timezone=str(runtime_overrides.get("timezone") or "") or None,
        syminfo_session=str(runtime_overrides.get("session") or "") or None,
        syminfo_type=str(runtime_overrides.get("type") or "") or None,
        syminfo_strings={
            k: str(runtime_overrides.get(k))
            for k in ("ticker", "tickerid", "currency", "basecurrency",
                      "description", "volumetype")
            if runtime_overrides.get(k)
        } or None,
        syminfo_metadata=syminfo_metadata,
        syminfo_mintick=_num(runtime_overrides.get("mintick")),
        syminfo_pointvalue=_num(runtime_overrides.get("pointvalue")),
        account_currency_fx_series=fx_series,
        account_currency_fx_source_sha256=fx_source_sha256,
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
    if aux_security_ohlcv_csv is not None:
        kwargs["aux_security_ohlcv_csv"] = aux_security_ohlcv_csv
        kwargs["aux_security_input_tf"] = aux_security_input_tf
        kwargs["aux_security_source_file_sha256"] = \
            aux_security_source_file_sha256
    if native_security_feeds:
        kwargs["native_security_feeds"] = native_security_feeds
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
        if hasattr(L, "strategy_closed_trade_entry_incarnation"):
            L.strategy_closed_trade_entry_incarnation.argtypes = [
                ctypes.c_void_p, ctypes.c_int]
            L.strategy_closed_trade_entry_incarnation.restype = ctypes.c_uint64
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
        if hasattr(L, "strategy_request_abort"):
            L.strategy_request_abort.argtypes = [ctypes.c_void_p]
            L.strategy_request_abort.restype = None
            L.strategy_last_run_status.argtypes = [ctypes.c_void_p]
            L.strategy_last_run_status.restype = ctypes.c_int
        if hasattr(L, "strategy_set_realtime_tail"):
            L.strategy_set_realtime_tail.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
            L.strategy_set_realtime_tail.restype = None
        if hasattr(L, "strategy_set_probe_suppress_tail_logic"):
            L.strategy_set_probe_suppress_tail_logic.argtypes = [
                ctypes.c_void_p, ctypes.c_int]
            L.strategy_set_probe_suppress_tail_logic.restype = None
        # ABI v4 live-runtime surface (task 4): force the intrabar path order
        # / read the last bar's dual-entry-stop arbitration winner. Older
        # .so builds may predate these exports -- hasattr-guarded like the
        # other live-runtime setters above.
        if hasattr(L, "strategy_set_path_order"):
            L.strategy_set_path_order.argtypes = [ctypes.c_void_p, ctypes.c_int]
            L.strategy_set_path_order.restype = None
        if hasattr(L, "strategy_last_bar_dual_entry_path"):
            L.strategy_last_bar_dual_entry_path.argtypes = [ctypes.c_void_p]
            L.strategy_last_bar_dual_entry_path.restype = ctypes.c_int
        # ABI v4 live-runtime surface (task 6): toggle per-script-bar
        # broker-state hash recording / read the final state's hash. Older
        # .so builds may predate these exports -- hasattr-guarded like the
        # other live-runtime setters above.
        if hasattr(L, "strategy_set_broker_state_hash_recording"):
            L.strategy_set_broker_state_hash_recording.argtypes = [
                ctypes.c_void_p, ctypes.c_int]
            L.strategy_set_broker_state_hash_recording.restype = None
        if hasattr(L, "strategy_broker_state_hash"):
            L.strategy_broker_state_hash.argtypes = [ctypes.c_void_p]
            L.strategy_broker_state_hash.restype = ctypes.c_uint64
        # ABI v4 live-runtime surface (task 7): the resting-order book through
        # the generated POD mirror (pf_pending_order_v1_t). The ctypes struct
        # is built from the runtime's own field table -- see
        # build_pending_order_struct -- so an appended field cannot
        # desynchronise this reader. Older .so builds predate the exports:
        # hasattr-guarded, IntentRowV1 stays None and --dump-book warns.
        self.pending_order_layout: list[tuple[str, str, int, int]] | None = None
        self.IntentRowV1: type | None = None
        if hasattr(L, "strategy_pending_order_layout"):
            L.strategy_pending_orders_len.argtypes = [ctypes.c_void_p]
            L.strategy_pending_orders_len.restype = ctypes.c_int
            L.strategy_pending_order_get.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]
            L.strategy_pending_order_get.restype = ctypes.c_int
            L.strategy_pending_order_layout.argtypes = [ctypes.POINTER(ctypes.c_int)]
            L.strategy_pending_order_layout.restype = ctypes.POINTER(PfFieldDescC)
            self.pending_order_layout = _pending_order_layout(L)
            self.IntentRowV1 = build_pending_order_struct(self.pending_order_layout)
        # ABI v4 live-runtime surface (task 8): engine-computed derived order
        # values (fill qty / partition / close-only, level resolution,
        # effective levels) and the position scalars. hasattr-guarded like
        # the rest; has_order_derived gates the --dump-book enrichment.
        self.has_order_derived = hasattr(L, "strategy_pending_order_fill_qty")
        if self.has_order_derived:
            L.strategy_pending_order_fill_qty.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_double,
                ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_int)]
            L.strategy_pending_order_fill_qty.restype = ctypes.c_int
            L.strategy_pending_order_level_resolved.argtypes = [ctypes.c_void_p, ctypes.c_int]
            L.strategy_pending_order_level_resolved.restype = ctypes.c_int
            L.strategy_pending_order_effective_levels.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_double),
                ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double)]
            L.strategy_pending_order_effective_levels.restype = ctypes.c_int
            L.strategy_trail_best_price.argtypes = [ctypes.c_void_p]
            L.strategy_trail_best_price.restype = ctypes.c_double
            L.strategy_position_avg_price.argtypes = [ctypes.c_void_p]
            L.strategy_position_avg_price.restype = ctypes.c_double
            L.strategy_position_cycle_seq.argtypes = [ctypes.c_void_p]
            L.strategy_position_cycle_seq.restype = ctypes.c_int64
        # ABI v4 live-runtime surface (task 9): closed-trade id / exit-comment
        # / close-cause accessors (report-row scope, spans range-end rows
        # like strategy_closed_trade_entry_incarnation) and the position
        # size / equity / script-bars-processed scalars. Older .so builds
        # predate these exports -- hasattr-guarded like the rest.
        if hasattr(L, "strategy_closed_trade_entry_id"):
            L.strategy_closed_trade_entry_id.argtypes = [ctypes.c_void_p, ctypes.c_int]
            L.strategy_closed_trade_entry_id.restype = ctypes.c_char_p
            L.strategy_closed_trade_exit_id.argtypes = [ctypes.c_void_p, ctypes.c_int]
            L.strategy_closed_trade_exit_id.restype = ctypes.c_char_p
            L.strategy_closed_trade_exit_comment.argtypes = [ctypes.c_void_p, ctypes.c_int]
            L.strategy_closed_trade_exit_comment.restype = ctypes.c_char_p
        if hasattr(L, "strategy_closed_trade_close_cause"):
            L.strategy_closed_trade_close_cause.argtypes = [ctypes.c_void_p, ctypes.c_int]
            L.strategy_closed_trade_close_cause.restype = ctypes.c_int
        if hasattr(L, "strategy_position_size"):
            L.strategy_position_size.argtypes = [ctypes.c_void_p]
            L.strategy_position_size.restype = ctypes.c_double
        if hasattr(L, "strategy_current_equity"):
            L.strategy_current_equity.argtypes = [ctypes.c_void_p]
            L.strategy_current_equity.restype = ctypes.c_double
        if hasattr(L, "strategy_script_bars_processed"):
            L.strategy_script_bars_processed.argtypes = [ctypes.c_void_p]
            L.strategy_script_bars_processed.restype = ctypes.c_int64
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
        # syminfo.type ("forex"/"stock"/"crypto"/...) + the generic string
        # member setter (ticker/tickerid/currency/basecurrency/...). Older
        # .so builds predate these exports — hasattr-guarded like the rest.
        if hasattr(L, "strategy_set_syminfo_type"):
            L.strategy_set_syminfo_type.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            L.strategy_set_syminfo_type.restype = None
        if hasattr(L, "strategy_set_syminfo_string"):
            L.strategy_set_syminfo_string.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
            L.strategy_set_syminfo_string.restype = ctypes.c_int
        if hasattr(L, "strategy_set_syminfo_metadata"):
            L.strategy_set_syminfo_metadata.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
            L.strategy_set_syminfo_metadata.restype = None
        if hasattr(L, "strategy_set_account_currency_fx_series"):
            L.strategy_set_account_currency_fx_series.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(ctypes.c_int64),
                ctypes.POINTER(ctypes.c_double), ctypes.c_int]
            L.strategy_set_account_currency_fx_series.restype = ctypes.c_int
        if hasattr(L, "strategy_set_aux_security_feed"):
            L.strategy_set_aux_security_feed.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(BarC), ctypes.c_int,
                ctypes.c_char_p]
            L.strategy_set_aux_security_feed.restype = ctypes.c_int
        if hasattr(L, "strategy_set_native_security_feed"):
            L.strategy_set_native_security_feed.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(BarC),
                ctypes.c_int]
            L.strategy_set_native_security_feed.restype = ctypes.c_int
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

    def _probe_fill_qty(self, state, index: int, price: float) -> dict | None:
        """One strategy_pending_order_fill_qty probe -> {price, qty, close_only,
        partition, partition_name}; None when the order has no opening size
        (rc 1, an EXIT) or the price is not finite."""
        if price is None or not math.isfinite(price):
            return None
        qty = ctypes.c_double(float("nan"))
        close_only = ctypes.c_int(-1)
        partition = ctypes.c_int(-1)
        rc = self.lib.strategy_pending_order_fill_qty(
            state, index, float(price), ctypes.byref(qty),
            ctypes.byref(close_only), ctypes.byref(partition))
        if rc == 1:
            return None
        if rc != 0:
            raise RuntimeError(
                f"strategy_pending_order_fill_qty({index}, {price}) returned {rc}")
        return {
            "price": float(price),
            "qty": _nan_to_none(qty.value),
            "close_only": int(close_only.value),
            "partition": int(partition.value),
            "partition_name": FILL_QTY_PARTITIONS.get(int(partition.value), "?"),
        }

    def read_order_derived(self, state, index: int,
                           last_close: float | None = None) -> dict | None:
        """ABI v4 task 8: the engine-computed derived values of one resting
        order. ``level_resolved`` and ``effective_levels`` are the engine's
        reads verbatim (NaN -> None). ``fill_qty`` reports the engine-sized
        opening quantity at every finite effective STOP / LIMIT level the
        order itself carries (the prices a priced entry can fill at) and at
        ``last_close`` (the MARKET / gap-through proxy: the next open is
        unknown after a run, the last close is its best stand-in); None for
        an EXIT, which has no opening size. None when the .so predates the
        exports."""
        if not getattr(self, "has_order_derived", False):
            return None
        resolved = int(self.lib.strategy_pending_order_level_resolved(state, index))
        stop = ctypes.c_double(float("nan"))
        limit = ctypes.c_double(float("nan"))
        trail = ctypes.c_double(float("nan"))
        rc = self.lib.strategy_pending_order_effective_levels(
            state, index, ctypes.byref(stop), ctypes.byref(limit), ctypes.byref(trail))
        if rc != 0:
            raise RuntimeError(
                f"strategy_pending_order_effective_levels({index}) returned {rc}")
        levels = {
            "stop": _nan_to_none(stop.value),
            "limit": _nan_to_none(limit.value),
            "trail_activation": _nan_to_none(trail.value),
        }
        fill_qty: dict[str, dict] | None = {}
        for key, price in (("at_stop", levels["stop"]), ("at_limit", levels["limit"]),
                           ("at_last_close", last_close)):
            if price is None:
                continue
            probe = self._probe_fill_qty(state, index, price)
            if probe is None:      # rc 1: an EXIT -- no opening size at all
                fill_qty = None
                break
            fill_qty[key] = probe
        return {
            "level_resolved": resolved,
            "effective_levels": levels,
            "fill_qty": fill_qty,
        }

    def read_position_scalars(self, state) -> dict | None:
        """ABI v4 task 8: strategy_position_avg_price (None when flat),
        strategy_position_cycle_seq (0 when flat), strategy_trail_best_price
        (None until a position filled). None when the .so predates them."""
        if not getattr(self, "has_order_derived", False):
            return None
        return {
            "avg_price": _nan_to_none(self.lib.strategy_position_avg_price(state)),
            "cycle_seq": int(self.lib.strategy_position_cycle_seq(state)),
            "trail_best_price": _nan_to_none(self.lib.strategy_trail_best_price(state)),
        }

    def read_pending_orders(self, state, last_close: float | None = None) -> list[dict]:
        """Snapshot the live handle's resting-order book (ABI v4 task 7):
        strategy_pending_orders_len + one strategy_pending_order_get per
        order, each decoded through the layout-built IntentRowV1. Must be
        called while ``state`` is alive (run() does so before strategy_free).
        Empty list when the .so predates the exports (IntentRowV1 is
        None) -- callers that need to distinguish check that attribute.
        When the .so also exports the task-8 derived accessors each dict
        gains a ``derived`` sub-dict (read_order_derived; ``last_close`` is
        the fill-qty probe price for the MARKET / gap-through case)."""
        if self.IntentRowV1 is None or self.pending_order_layout is None:
            return []
        n = int(self.lib.strategy_pending_orders_len(state))
        book: list[dict] = []
        for i in range(n):
            rec = self.IntentRowV1()
            rc = self.lib.strategy_pending_order_get(
                state, i, ctypes.byref(rec), ctypes.sizeof(rec))
            if rc != 0:
                raise RuntimeError(f"strategy_pending_order_get({i}) returned {rc} (len={n})")
            if int(rec.struct_version) != PENDING_ORDER_STRUCT_VERSION:
                raise RuntimeError(
                    f"pending order {i}: struct_version {int(rec.struct_version)} != "
                    f"{PENDING_ORDER_STRUCT_VERSION}")
            if int(rec.size) != ctypes.sizeof(rec):
                raise RuntimeError(
                    f"pending order {i}: runtime size {int(rec.size)} != "
                    f"layout-built sizeof {ctypes.sizeof(rec)}")
            entry = pending_order_to_dict(rec, self.pending_order_layout)
            derived = self.read_order_derived(state, i, last_close)
            if derived is not None:
                entry["derived"] = derived
            book.append(entry)
        return book

    def run(self, bars_csv: Path, params: dict | None = None,
            *, trace_enabled: bool = False, trade_start_time_ms: int | None = None,
            realtime_tail_horizon: int | None = None,
            probe_suppress_tail: bool = False,
            path_order: str | None = None,
            broker_state_hash_recording: bool = False,
            dump_book: bool = False,
            strategy_overrides: dict | None = None,
            chart_timezone: str | None = None,
            syminfo_timezone: str | None = None,
            syminfo_session: str | None = None,
            syminfo_type: str | None = None,
            syminfo_strings: dict | None = None,
            syminfo_metadata: dict | None = None,
            syminfo_mintick: float | None = None,
            syminfo_pointvalue: float | None = None,
            account_currency_fx_series: "tuple | None" = None,
            account_currency_fx_source_sha256: str | None = None,
            input_tf: str | None = None, script_tf: str | None = None,
            aux_security_ohlcv_csv: Path | None = None,
            aux_security_input_tf: str | None = None,
            aux_security_source_file_sha256: str | None = None,
            native_security_feeds: dict | None = None,
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

        ``dump_book`` adds ``pending_orders`` -- the post-run resting-order
        book as a list of dicts (read_pending_orders, ABI v4 task 7), each
        carrying a ``derived`` sub-dict (fill qty / partition / close-only
        at every finite effective level and at the last close, level
        resolution, effective levels; ABI v4 task 8) -- and ``position``
        (avg_price, cycle_seq, trail_best_price) to the returned dict.
        Read-only: the run itself is unchanged. Omitted (not an empty list)
        when the .so predates the exports; ``derived`` / ``position`` are
        omitted when it predates only the task-8 accessors.
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
        aux_requested = (aux_security_ohlcv_csv is not None
                         or aux_security_input_tf is not None)
        aux_bars = None
        aux_n = 0
        aux_source_feed_sha256 = None
        aux_source_file_sha256 = None
        if aux_requested:
            if aux_security_ohlcv_csv is None or not aux_security_input_tf:
                raise ValueError(
                    "aux_security_ohlcv_csv and aux_security_input_tf must be set together")
            if not hasattr(self.lib, "strategy_set_aux_security_feed"):
                raise RuntimeError(
                    "strategy library lacks auxiliary request.security feed support; rebuild it")
            (aux_bars, aux_n, aux_source_feed_sha256,
             aux_source_file_sha256) = _load_aux_bars_snapshot(
                 Path(aux_security_ohlcv_csv))
            if (aux_security_source_file_sha256 is not None
                    and aux_security_source_file_sha256
                    != aux_source_file_sha256):
                raise RuntimeError(
                    "auxiliary request.security OHLCV export changed after metadata resolution")
            if aux_n <= 0:
                raise ValueError(
                    "auxiliary request.security OHLCV export contains no bars")
        native_feed_arrays = []
        native_feed_report = {}
        if native_security_feeds:
            if not hasattr(self.lib, "strategy_set_native_security_feed"):
                raise RuntimeError(
                    "strategy library lacks native request.security feed support; rebuild it")
            for tf, entry in sorted(native_security_feeds.items()):
                feed_path = Path(entry["path"])
                (feed_bars, feed_n, feed_values_sha256,
                 feed_file_sha256) = _load_aux_bars_snapshot(feed_path)
                expected_sha = entry.get("source_file_sha256")
                if expected_sha is not None and expected_sha != feed_file_sha256:
                    raise RuntimeError(
                        "native request.security OHLCV export changed after "
                        f"metadata resolution: {tf}")
                if feed_n <= 0:
                    raise ValueError(
                        f"native request.security OHLCV export for {tf} contains no bars")
                native_feed_arrays.append((str(tf), feed_bars, feed_n))
                native_feed_report[str(tf)] = {
                    "path": str(feed_path.resolve()),
                    "source_file_sha256": feed_file_sha256,
                    "source_values_sha256": feed_values_sha256,
                }
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
            # ABI v4 live-runtime surface (task 6): per-script-bar
            # broker-state hash recording. Off by default, like trace.
            if (broker_state_hash_recording
                    and hasattr(self.lib, "strategy_set_broker_state_hash_recording")):
                self.lib.strategy_set_broker_state_hash_recording(state, 1)
            if trade_start_time_ms is not None and hasattr(self.lib, "strategy_set_trade_start_time"):
                self.lib.strategy_set_trade_start_time(state, int(trade_start_time_ms))
            # Live-runtime tail (ABI v4, spec §3.1): the last bar of this run
            # is a still-forming bar, not the chart's rightmost historical
            # bar. Off unless a horizon is given.
            if realtime_tail_horizon is not None and hasattr(self.lib, "strategy_set_realtime_tail"):
                self.lib.strategy_set_realtime_tail(state, 1, int(realtime_tail_horizon))
            # Live probe tail suppression (ABI v4, spec §3.2): the last bar of
            # this run only runs dispatch_bar()'s pre-on_bar broker steps and
            # returns, so the run's last-bar fills are the settled book's
            # fills against the forming bar and the post-run book is the
            # in-force book. Off unless requested.
            if probe_suppress_tail and hasattr(self.lib, "strategy_set_probe_suppress_tail_logic"):
                self.lib.strategy_set_probe_suppress_tail_logic(state, 1)
            # ABI v4 live-runtime surface: force this run's intrabar path
            # order (see strategy_set_path_order doc in pineforge.h). None
            # (default) leaves the engine on AUTO.
            if path_order is not None and hasattr(self.lib, "strategy_set_path_order"):
                _PATH_ORDER_INT = {"auto": 0, "high": 1, "low": 2}
                self.lib.strategy_set_path_order(
                    state, _PATH_ORDER_INT[str(path_order).lower()])
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
            # Instrument class (syminfo.type). Unset keeps the engine's
            # "crypto" default byte-identical; per-symbol datasets pass
            # TV's value ("forex" for OANDA:EURUSD, "stock" for NASDAQ:AAPL).
            if syminfo_type and hasattr(self.lib, "strategy_set_syminfo_type"):
                self.lib.strategy_set_syminfo_type(state, str(syminfo_type).encode())
            if syminfo_strings and hasattr(self.lib, "strategy_set_syminfo_string"):
                for skey, sval in syminfo_strings.items():
                    if sval is None or str(sval) == "":
                        continue
                    self.lib.strategy_set_syminfo_string(
                        state, str(skey).encode(), str(sval).encode())
            if syminfo_metadata and hasattr(self.lib, "strategy_set_syminfo_metadata"):
                for mkey, mval in syminfo_metadata.items():
                    try:
                        self.lib.strategy_set_syminfo_metadata(
                            state, str(mkey).encode(), float(mval))
                    except (TypeError, ValueError):
                        continue
            if account_currency_fx_series is not None:
                if not hasattr(self.lib, "strategy_set_account_currency_fx_series"):
                    raise RuntimeError(
                        "strategy library lacks timestamped account-currency FX support; rebuild it")
                fx_timestamps, fx_rates = account_currency_fx_series
                if len(fx_timestamps) != len(fx_rates) or not fx_timestamps:
                    raise ValueError(
                        "account_currency_fx_series requires equal non-empty timestamp/rate arrays")
                fx_timestamps_c = (ctypes.c_int64 * len(fx_timestamps))(
                    *[int(value) for value in fx_timestamps])
                fx_rates_c = (ctypes.c_double * len(fx_rates))(
                    *[float(value) for value in fx_rates])
                rc = self.lib.strategy_set_account_currency_fx_series(
                    state, fx_timestamps_c, fx_rates_c, len(fx_timestamps))
                if rc != 0:
                    raise ValueError(
                        "engine rejected account_currency_fx_series")
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
            if aux_requested:
                rc = self.lib.strategy_set_aux_security_feed(
                    state, aux_bars, aux_n,
                    str(aux_security_input_tf).encode())
                if rc != 0:
                    detail = ""
                    if hasattr(self.lib, "strategy_get_last_error"):
                        err_ptr = self.lib.strategy_get_last_error(state)
                        if err_ptr:
                            detail = err_ptr.decode("utf-8", "replace")
                    raise RuntimeError(
                        "engine rejected auxiliary request.security feed"
                        + (f": {detail}" if detail else ""))
            for feed_tf, feed_bars, feed_n in native_feed_arrays:
                rc = self.lib.strategy_set_native_security_feed(
                    state, feed_tf.encode(), feed_bars, feed_n)
                if rc != 0:
                    detail = ""
                    if hasattr(self.lib, "strategy_get_last_error"):
                        err_ptr = self.lib.strategy_get_last_error(state)
                        if err_ptr:
                            detail = err_ptr.decode("utf-8", "replace")
                    raise RuntimeError(
                        f"engine rejected native request.security feed {feed_tf}"
                        + (f": {detail}" if detail else ""))
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
            incarnation_accessor = getattr(
                self.lib, "strategy_closed_trade_entry_incarnation", None)
            # ABI v4 (task 9): entry_id / exit_id / exit_comment / close_cause
            # are added to the JSON trade dicts ONLY -- never to
            # write_engine_trades_csv's fixed column layout, which the
            # corpus's engine_trades.csv is compared byte-for-byte against
            # (verify_corpus.py). A new CSV column would change that
            # committed byte layout for the flags-off identity task.
            entry_id_accessor = getattr(self.lib, "strategy_closed_trade_entry_id", None)
            exit_id_accessor = getattr(self.lib, "strategy_closed_trade_exit_id", None)
            exit_comment_accessor = getattr(
                self.lib, "strategy_closed_trade_exit_comment", None)
            close_cause_accessor = getattr(
                self.lib, "strategy_closed_trade_close_cause", None)
            for i, trade in enumerate(result["trades"]):
                trade["entry_incarnation"] = (
                    int(incarnation_accessor(state, i))
                    if incarnation_accessor is not None else 0
                )
                # Default like entry_incarnation above: an older .so predating
                # these exports still gets the key, just with the same value
                # an unrecognised/absent close would report (empty string /
                # UNKNOWN), so callers need not special-case a missing key.
                if entry_id_accessor is not None:
                    ptr = entry_id_accessor(state, i)
                    trade["entry_id"] = ptr.decode("utf-8", "replace") if ptr else ""
                else:
                    trade["entry_id"] = ""
                if exit_id_accessor is not None:
                    ptr = exit_id_accessor(state, i)
                    trade["exit_id"] = ptr.decode("utf-8", "replace") if ptr else ""
                else:
                    trade["exit_id"] = ""
                if exit_comment_accessor is not None:
                    ptr = exit_comment_accessor(state, i)
                    trade["exit_comment"] = ptr.decode("utf-8", "replace") if ptr else ""
                else:
                    trade["exit_comment"] = ""
                trade["close_cause"] = (
                    int(close_cause_accessor(state, i))
                    if close_cause_accessor is not None else 0
                )
            if dump_book and self.IntentRowV1 is not None:
                last_close = float(bars[n - 1].close) if n else None
                result["pending_orders"] = self.read_pending_orders(state, last_close)
                position = self.read_position_scalars(state)
                if position is not None:
                    result["position"] = position
            if source_feed_sha256 is not None:
                result["source_feed_sha256"] = source_feed_sha256
            if aux_requested:
                result["aux_security_ohlcv_csv"] = str(
                    Path(aux_security_ohlcv_csv).resolve())
                result["aux_security_input_tf"] = str(
                    aux_security_input_tf)
                result["aux_security_source_file_sha256"] = (
                    aux_source_file_sha256 or "")
                result["aux_security_source_feed_sha256"] = (
                    aux_source_feed_sha256 or "")
            if native_feed_report:
                result["native_security_feeds"] = native_feed_report
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


def _load_aux_bars_snapshot(
        csv_path: Path) -> tuple[ctypes.Array, int, str, str]:
    """Load auxiliary bars and both identities from one immutable byte read.

    The raw SHA and parsed ``BarC`` values must attest the same bytes. Reading
    the path once eliminates the hash-then-reopen window where a replacement
    file could be executed under the previous file's hash.
    """
    with csv_path.open("rb") as handle:
        snapshot = handle.read()
    source_file_sha256 = hashlib.sha256(snapshot).hexdigest()
    lines = snapshot.splitlines()
    if not lines:
        raise ValueError(
            f"auxiliary request.security OHLCV export is empty: {csv_path}")
    header = lines[0].decode("utf-8-sig").strip().split(",")
    col = {name: header.index(name)
           for name in ("open", "high", "low", "close", "volume", "timestamp")}

    try:
        import numpy as _np
    except ImportError:
        _np = None
    if _np is not None:
        if any(line.strip() for line in lines[1:]):
            data = _np.loadtxt(
                io.BytesIO(snapshot), delimiter=",", skiprows=1, ndmin=2)
        else:
            data = _np.empty((0, len(header)), dtype="<f8")
        count = len(data)
        dtype = _np.dtype([
            ("open", "<f8"), ("high", "<f8"), ("low", "<f8"),
            ("close", "<f8"), ("volume", "<f8"), ("timestamp", "<i8"),
        ])
        rows = _np.empty(count, dtype=dtype)
        for name in ("open", "high", "low", "close", "volume"):
            rows[name] = data[:, col[name]] if count else []
        rows["timestamp"] = (
            data[:, col["timestamp"]].astype("<i8") if count else [])
        canonical = _new_source_feed_hasher()
        canonical.update(memoryview(rows))
        bars = ((BarC * count).from_buffer_copy(rows.tobytes())
                if count else (BarC * 0)())
        return bars, count, canonical.hexdigest(), source_file_sha256

    parsed_rows: list[tuple[float, float, float, float, float, int]] = []
    canonical = _new_source_feed_hasher()
    reader = csv.DictReader(io.StringIO(snapshot.decode("utf-8-sig")))
    for row in reader:
        parsed = (
            float(row["open"]), float(row["high"]), float(row["low"]),
            float(row["close"]), float(row["volume"]), int(row["timestamp"]),
        )
        _update_source_feed_hash(canonical, parsed)
        parsed_rows.append(parsed)
    count = len(parsed_rows)
    bars = (BarC * count)()
    for index, (open_, high, low, close, volume, timestamp) in enumerate(
            parsed_rows):
        bars[index].open = open_
        bars[index].high = high
        bars[index].low = low
        bars[index].close = close
        bars[index].volume = volume
        bars[index].timestamp = timestamp
    return bars, count, canonical.hexdigest(), source_file_sha256


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
            # Range-end close of a position still open after the final bar
            # (engine_orders.cpp record_range_end_close_trades). The CSV
            # writer marks the row's exit leg in the trailing
            # ``Engine range-end`` column so the grader can pair it with the
            # tape's own mark (write_engine_trades_csv).
            "open_at_end": bool(t.open_at_end),
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
    # ABI v4 task 6: per-script-bar broker-state hash, empty unless the run
    # enabled broker_state_hash_recording. Python ints are arbitrary
    # precision, so these round-trip through json.dump exactly (unlike a
    # JS/JSON-Number consumer, which loses precision above 2**53).
    broker_state_hash = [int(r.broker_state_hash[i]) for i in range(r.broker_state_hash_len)]
    # Per-script-bar OPEN timestamps, cheap to carry alongside
    # broker_state_hash so a consumer (e.g. --broker-state-hash's JSON
    # output) can align each hash to a bar without a separate --trace-json
    # / equity-curve dump. equity_curve itself stays out of this dict (see
    # on_report's docstring); recording invariant: when
    # broker_state_hash_recording was on, broker_state_hash_len ==
    # equity_curve_len (both are appended once per dispatched script bar,
    # equity_curve unconditionally and broker_state_hash right after it).
    equity_curve_time_ms = [int(r.equity_curve[i].time_ms) for i in range(r.equity_curve_len)]
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
        "broker_state_hash": broker_state_hash,
        "equity_curve_time_ms": equity_curve_time_ms,
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
    """Keep trades whose entry (fill) time lies inside ``window`` (inclusive).

    main() hands it the REPORT window. With a TV tape in use that is
    ``[first TV entry - one feed bar interval, last TV entry]`` — the bound
    trades have always been reported from — and not the wider emit window
    that gates the engine (_tv_entry_emit_window): the signal bar admitted
    ahead of a weekend / session gap places the first entry's order, whose
    fill on TV's first entry bar is reported; a fill on that signal bar
    itself, which TV did not report, lies before the bound on every gap
    tape and stays out; on a gapless feed the signal bar is one interval
    before the first entry and its fills are reported exactly as before.
    An explicit --emit-window-ohlcv / reference-feed window reports what it
    gates."""
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


def _tv_metrics_path(strategy_dir: Path, meta: dict, tv_path: Path) -> Path:
    """Where the tape's metrics.json lives: ``tv_metrics_json`` from the
    probe's inputs.json when set (relative to ``strategy_dir``), else the
    ``metrics.json`` beside the tape — the scrapper's probe directory, the
    case runner's ``data/<group>/<slug>/`` copy and ``lab tv``'s export all
    write the four files (strategy.pine, tv_trades.csv, metrics.json,
    meta.json) side by side."""
    name = str(meta.get("tv_metrics_json", "")).strip()
    if name:
        return strategy_dir / name
    return tv_path.parent / "metrics.json"


def _tv_metrics_range_end(metrics: dict) -> tuple[int, str] | None:
    """TradingView's backtest range END from a tape's metrics.json, in ms,
    with the key it was read from: the ``to`` of the deep-backtest window,
    as 00:00 UTC of that date.

    Two spellings carry it. The ws-report-v1 exporter records the exact
    window it sent to TradingView as ``wsProvenance.requestedRange.to`` (ms
    UTC), and every campaign tape's value is 1777593600000 = 2026-05-01
    00:00 UTC for ``"to": "2026-05-01"``; the browser export (ETH, AAPL 15,
    EURUSD) carries only the date. They agree wherever both exist, and the
    integer is what TradingView actually saw, so it wins. None without a
    range (a hand-made metrics.json, or none)."""
    if not isinstance(metrics, dict):
        return None
    ws = metrics.get("wsProvenance")
    if isinstance(ws, dict):
        requested = ws.get("requestedRange")
        if isinstance(requested, dict):
            to_ms = requested.get("to")
            if isinstance(to_ms, int) and not isinstance(to_ms, bool):
                return int(to_ms), "metrics.json wsProvenance.requestedRange.to"
    to_date = str(metrics.get("to", "") or "").strip()
    if to_date:
        try:
            when = datetime.strptime(to_date, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        except ValueError:
            return None
        return int(when.timestamp() * 1000), "metrics.json to"
    return None


def _tv_metrics_range_end_ms(metrics: dict) -> int | None:
    """The value half of _tv_metrics_range_end."""
    found = _tv_metrics_range_end(metrics)
    return None if found is None else found[0]


def _tv_tape_last_row_ms(tv_path: Path, meta: dict) -> int | None:
    """The latest ``Date and time`` over EVERY row of the tape — entries,
    exits and the row TV writes for a position still open at the range's
    end. None without rows. The fallback bound when metrics.json carries
    no range; see _load_tv_range_end_ms for why it is only a fallback."""
    tz_offset = _tv_tzinfo(meta)
    latest: int | None = None
    with tv_path.open(encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            stamp = str(row.get("Date and time", "")).strip()
            if not stamp:
                continue
            ts = _parse_trade_dt(stamp, tz_offset)
            latest = ts if latest is None else max(latest, ts)
    return latest


def _tv_tape_marks_open_rows(strategy_dir: Path, meta: dict) -> bool:
    """True when the tape MARKS a position still open at the range's end:
    any row whose ``Signal`` is ``open`` (case- and space-insensitive).

    Two exporters write the campaign's tapes, and they print the range-end
    position differently. The ws-report-v1 exporter (286 of the 305 ETH
    tapes; no marker in metrics.json beyond ``tapeChannel``) reports it as
    an ordinary closed trade — exit row on the last bar at its close, EMPTY
    Signal, counted in closedTrades — which is the shape the engine's
    ``open_at_end`` row emulates. The browser export (the scrapper's
    3commas grid-bot tapes among them, 22/22/28 open lots each at
    2026-05-01 08:00 +8; metrics.json without ``tapeChannel``) writes the
    same row with Signal "Open". TV nets commission on BOTH legs of that row
    exactly as it does on a close (3commas-xlm-grid-bot #615: qty 0.0912,
    entry 2189.13, mark 2261.44 -> 6.3511 = TV's 6.35 = the engine's
    6.351137), so per-lot P&L is not the difference; the difference is
    only how the tape spells the row. Both shapes are measured the same
    way (_apply_range_end_regime); the marker is read for the run's log
    line, because the grader's pairing of the engine's marked rows
    (write_engine_trades_csv) with the tape's is what makes a marked tape
    grade as an unmarked one does. Read once per run; False without a
    tape or with a tape of no rows."""
    tv_name = str(meta.get("tv_trades_csv", "tv_trades.csv"))
    tv_path = strategy_dir / tv_name
    if not tv_path.exists():
        return False
    with tv_path.open(encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            if str(row.get("Signal") or "").strip().lower() == "open":
                return True
    return False


def _load_tv_range_end(strategy_dir: Path, meta: dict) -> tuple[int, str] | None:
    """The last bar of TradingView's backtest range, as a feed bound
    (``ohlcv_end_ms``, inclusive) with where it was read from: every bar
    whose OPEN timestamp is at or before the range's ``to`` (00:00 UTC of
    that date) is inside the range.

    Applied to every tape graded through its TV window
    (_apply_range_end_regime), whichever way the tape spells its range-end
    row.

    The rule is TradingView's own, read off the ws-report-v1 tapes'
    ``returnedRange.to`` (the open of the last bar TV reported) against the
    ``requestedRange.to`` = 2026-05-01 00:00 UTC every campaign tape asked
    for, and off where each lane's open-position row sits:

      BINANCE:BTCUSDT / CME_MINI:ES1! / NQ1! / OANDA:XAUUSD 15
                             returned.to = 05-01 00:00 UTC   (== to: the bar
                             OPENING at ``to`` is inside; 00:15 is not — on
                             CME's America/Chicago chart too, so ``to`` is
                             UTC midnight, not the chart tz's);
      NYSE:F 1D              returned.to = 04-30 13:30 UTC   (the 05-01 13:30
                             session bar exists on TV and is excluded);
      CME_MINI:ES1!/NQ1! 1D  returned.to = 04-30 22:00 UTC;
      NSE:NIFTY 15 / 1D      returned.to = 04-30 09:45 / 03:45 UTC;
      OANDA:XAUUSD 1D        returned.to = 04-30 21:00 UTC;
      NYSE:F 15              returned.to = 04-30 19:45 UTC;

    and every open-position row (browser "Open" rows and ws rows with an
    empty Signal alike) sits on exactly that bar: BTC/ES/NQ/XAU/EURUSD 15
    and BINANCE:ETHUSDT.P 15 at 05-01 00:00 UTC (ETH: 228 of 397 scrapper
    tapes, @ 2261.44 = that bar's close), F 1D at 04-30 13:30 (@ 12.08),
    ES/NQ 1D at 04-30 22:00, XAU 1D at 04-30 21:00, NIFTY 1D at 04-30
    03:45, AAPL 15 at 04-30 19:45. Survey of 2026-09-02 over the scrapper's
    17 lane directories (1,356 tapes; every one ``to: 2026-05-01``).

    This bounds the feed the engine measures. The engine books a position
    still open after its FINAL bar as TradingView's range-end close
    (record_range_end_close_trades, engine_orders.cpp), so that final bar
    must be TV's last bar, not the feed's: the ETH chart feed runs on to
    2026-05-04 15:00 UTC, and unbounded the row would be booked on 05-04
    15:00 @ 2365.09 — a different bar, ~4.6% off TV's row — and fail
    exit/pnl against TV's Open row (review finding on the first cut of this
    change, 2026-09-02).

    The bound is the RANGE end, never the tape's last trade row. A first
    cut bounded the feed at the tape's latest stamp, and the full-population
    gate (gate-cand-range-end-close-20260902) failed hard.regression on
    scrapper:data/standard/waranyutrkm-asian-box-breakout-eda-tuned (ETH
    15): 110 -> 109 trades. Its last trade closes 04-29 18:00 UTC, so the
    feed was cut mid-day, and the strategy gates entries on
    request.security(.., "D", close / ta.ema(close, 50), lookahead_on) —
    under the historical lookahead projection every 04-29 chart bar reads
    the 04-29 daily bar's FINAL value. Cut at 18:00 the partial day gave
    d_close 2240.35 < d_ema 2241.85 and no long; the complete day (as TV
    sees it, its range running to 05-01) gives 2251.52 > 2242.29 and TV's
    trade 110 at 09:45. Any probe with a lookahead_on HTF projection and a
    closed last trade inside the final HTF period is exposed the same way,
    and the four other near-range-end closed probes in that shard were
    byte-identical either way (spark smoke-asianbox, 2026-09-02). Trades
    ENTERED after TV's last entry are still dropped by
    _filter_trades_to_window; the range end only decides which bars the
    strategy computes on and where a position still open at the end is
    marked.

    Source: the tape's metrics.json (_tv_metrics_path) —
    ``wsProvenance.requestedRange.to`` (ms) when present, else ``to``
    (YYYY-MM-DD, UTC midnight). A metrics.json without a range (or none at
    all) falls back to the tape's latest row (_tv_tape_last_row_ms), which
    is right whenever that row is the range-end row and merely early
    otherwise. None without a tape, or with a tape of no rows and no range."""
    tv_name = str(meta.get("tv_trades_csv", "tv_trades.csv"))
    tv_path = strategy_dir / tv_name
    if not tv_path.exists():
        return None
    metrics_path = _tv_metrics_path(strategy_dir, meta, tv_path)
    if metrics_path.is_file():
        try:
            with metrics_path.open(encoding="utf-8") as f:
                metrics = json.load(f)
        except (OSError, ValueError):
            metrics = None
        found = _tv_metrics_range_end(metrics)
        if found is not None:
            return found
    last_row_ms = _tv_tape_last_row_ms(tv_path, meta)
    if last_row_ms is None:
        return None
    return last_row_ms, "the tape's last row (metrics.json carries no range)"


def _load_tv_range_end_ms(strategy_dir: Path, meta: dict) -> int | None:
    """The value half of _load_tv_range_end."""
    found = _load_tv_range_end(strategy_dir, meta)
    return None if found is None else found[0]


def _fmt_utc_ms(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


def _apply_range_end_regime(strategy_dir: Path, meta: dict, run_kwargs: dict) -> str:
    """End the measurement where TradingView's range ends: apply the feed
    bound (``run_kwargs["ohlcv_end_ms"]``) and return the one line main()
    logs. One regime for every tape (v5, 2026-09-03):

      -> bound the feed at TradingView's range end (_load_tv_range_end:
         metrics.json ``wsProvenance.requestedRange.to`` / ``to`` as 00:00
         UTC inclusive, the tape's last row as the fallback) so the engine's
         final bar is TV's last bar, and WRITE the range-end close rows,
         each marked in the CSV's ``Engine range-end`` column
         (write_engine_trades_csv). A probe whose inputs.json already sets
         ``ohlcv_end_ms`` keeps its own bound; a tape with no range and no
         rows leaves the feed as given.

    The tape's spelling of that row no longer picks a regime. The
    ws-report-v1 exporter prints the range-end position as an ordinary
    closed trade (empty Signal, counted in closedTrades — the shape the
    engine's ``open_at_end`` row emulates); the browser export prints the
    same row with Signal "Open" (227 of the 396 scraped ETH tapes). Under
    v4 a marked tape was measured as the baseline had measured it — feed
    unbounded three days past the range, the engine's marks withheld —
    because the canonical grader paired the baseline's post-range closes
    with TV's Open rows on entry time and counted the marks' cent rounding
    against pnlP90 (round 3, 2026-09-02). The grader now pairs the engine's
    marked rows with the tape's Open rows lot for lot, gates neither exit
    nor pnl on the pair and keeps both sides out of every aggregate
    (scripts/verify_corpus.py pair_range_end_marks), so the bounded run
    grades a marked tape the way it grades an unmarked one: the 22/22/28
    open lots of the three 3commas grid bots pair exactly (21 of 22 xlm
    marks agree with TV to the cent, the 22nd is a FIFO lot-size split)
    and alexgrover / aborkan89 (one Open row each) grade 100% at every
    p90 = 0 (spark diag/r4, 2026-09-02).

    Only called with a TV tape window in use; an explicit
    --emit-window-ohlcv / --no-trim-output run measures the feed it was
    given and writes every row."""
    marked = _tv_tape_marks_open_rows(strategy_dir, meta)
    spelling = ("tape marks its open rows (Signal 'Open')" if marked
                else "tape unmarked (ws-report-v1 shape)")
    rows = ("range-end close rows written, marked in the "
            f"'{ENGINE_RANGE_END_COLUMN}' column for the grader to pair")
    if run_kwargs.get("ohlcv_end_ms") is not None:
        return (
            f"{spelling} -> range-end regime: the probe's own ohlcv_end_ms "
            f"{_fmt_utc_ms(int(run_kwargs['ohlcv_end_ms']))} kept as the feed "
            f"bound, {rows}")
    found = _load_tv_range_end(strategy_dir, meta)
    if found is None:
        return (
            f"{spelling}, no range in metrics.json and no tape rows -> feed "
            f"unbounded, {rows}")
    range_end_ms, source = found
    run_kwargs["ohlcv_end_ms"] = range_end_ms
    return (
        f"{spelling} -> range-end regime: feed bounded at TV's range end "
        f"{_fmt_utc_ms(range_end_ms)} ({source}), {rows}")


def _trim_ohlcv_csv(ohlcv_path: Path, start_ms: int | None, end_ms: int | None) -> Path | None:
    """Write a copy of ``ohlcv_path`` holding only the bars inside
    ``[start_ms, end_ms]`` (either bound optional, both inclusive) to a
    temporary file and return its path; None when neither bound is set.
    The ctypes runner applies the same bounds in _load_bars; the container
    runner needs an already-trimmed CSV (M8 — feed the same trimmed feed
    downstream). The caller unlinks the file."""
    if start_ms is None and end_ms is None:
        return None
    import tempfile
    fd, p = tempfile.mkstemp(suffix=".csv", prefix="pf_ohlcv_")
    out = Path(p)
    with ohlcv_path.open(newline="", encoding="utf-8") as fin, \
            os.fdopen(fd, "w", newline="") as fout:
        r = csv.DictReader(fin)
        w = csv.DictWriter(fout, fieldnames=r.fieldnames)
        w.writeheader()
        for row in r:
            ts = int(row["timestamp"])
            if start_ms is not None and ts < int(start_ms):
                continue
            if end_ms is not None and ts > int(end_ms):
                continue
            w.writerow(row)
    return out


def format_trade_qty(qty: float) -> str:
    """Lot-faithful quantity text for the TV-alignable export.

    The previous ``f"{qty:g}"`` kept only 6 significant digits, so any
    quantity with more digits than that was silently rewritten on the way
    out: an OANDA:EURUSD all-in lot of 923941.16 units (TV export, 0.01 lot
    step) printed as ``923941``, 897902.68 printed as ``897903`` (rounded UP),
    92293.36 as ``92293.4``, and anything >= 1e6 as ``1e+06``. The ledger held
    the exact value; only the CSV lied, which put a spurious 0.01-0.5 unit
    "qty miss" on 1933/2708 entries of the EURUSD KI-52 all-in probe and on
    every giua64 entry while the fills themselves were exact.

    Eight decimals cover every lot step in use (1 share, 0.01 unit, 0.0001
    contract, satoshi-scale 1e-8) and absorb binary noise from
    floor(q/step)*step (0.30000000000000004 -> ``0.3``); trailing zeros are
    trimmed the way TradingView prints its ``Size (qty)`` column, so every
    value the old formatter rendered faithfully renders byte-identically.
    """
    if not math.isfinite(qty):
        return f"{qty:g}"
    text = f"{qty:.8f}".rstrip("0").rstrip(".")
    if text in ("", "-0", "-"):
        return "0"
    return text


# The trailing column of engine_trades.csv. Its value on the EXIT row of a
# trade the engine booked as its range-end close (``open_at_end``:
# engine_orders.cpp record_range_end_close_trades) is
# ENGINE_RANGE_END_OPEN; every other cell is empty. The grader
# (scripts/verify_corpus.py ENGINE_RANGE_END_COLUMN) pairs such a row with
# the tape's own mark of the same lot — the browser export's Signal "Open".
ENGINE_RANGE_END_COLUMN = "Engine range-end"
ENGINE_RANGE_END_OPEN = "open"


def write_engine_trades_csv(trades: list[dict], path: Path) -> None:
    """Emit one row per trade *side* (exit then entry) in reverse-chronological
    order — byte-for-byte alignable with TradingView's `trades.csv` export.

    Excursion columns use TradingView's export convention: favorable excursion
    is a non-negative total-USD run-up, adverse excursion is emitted as a
    NEGATIVE total-USD drawdown (TV exports e.g. -8579.36). The engine ABI's
    `max_drawdown` stays positive — that mirrors Pine's
    `strategy.*trades.max_drawdown` accessors — so the sign flip happens only
    here, in the TV-compatible export representation.

    A trade flagged ``open_at_end`` (the engine's range-end close of a
    position still open after the final bar) is written like any other
    trade — TradingView's ws-report-v1 tape reports that position as an
    ordinary closed trade whose exit row is the last bar at its close with
    an EMPTY Signal (orb-lite NYSE:F 1D — Exit short 2026-04-30 @ 12.08),
    so the row must look like every other exit for the verifier to pair
    it — with one addition: its exit row says so in the trailing
    ``Engine range-end`` column (ENGINE_RANGE_END_OPEN, "open"), the
    engine's counterpart of the browser export's Signal "Open". The grader
    pairs the two marks of one lot and gates neither exit nor pnl on them
    (verify_corpus.py pair_range_end_marks); every other cell of the column
    is empty, and readers keyed by column name (verify_corpus.parse_trades,
    the lab's loaders) are untouched by it. The registry's projection of
    this CSV (campaign lib-results.mjs parseEngineTrades / airflow
    registry.py parse_engine_trades) pins the header and the field count:
    both parsers accept this 12-column header and the 11-column one
    engines before it wrote, project the cell as ``rangeEnd`` (blank ->
    null, "open" -> "open") and reject any other spelling, so the column
    is a bump of that contract on both sides, not a free addition."""
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
            "Cumulative PnL", "Engine entry incarnation",
            ENGINE_RANGE_END_COLUMN,
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
                    format_trade_qty(float(t["qty"])),
                    f"{t['pnl']:.6f}",
                    f"{t['pnl_pct']:.4f}",
                    f"{t['max_runup']:.6f}",
                    f"{adverse:.6f}",
                    f"{cum:.6f}",
                    str(int(t.get("entry_incarnation", 0)))
                    if (side.startswith("Entry")
                        and int(t.get("entry_incarnation", 0)) > 0) else "",
                    ENGINE_RANGE_END_OPEN
                    if (side.startswith("Exit") and t.get("open_at_end")) else "",
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


def _load_tv_entry_span(strategy_dir: Path, meta: dict) -> tuple[int, int] | None:
    """TradingView's first and last entry time (UTC ms) on the tape named by
    ``meta["tv_trades_csv"]`` (tv_trades.csv by default; rows stamped in
    ``tv_trades_csv_tz``, _tv_tzinfo). None without a tape or without an
    Entry row — the emit window then falls back to the reference feed's
    span, as it always has."""
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
    return min(entries), max(entries)


def _feed_timestamps(csv_path: Path, *, ohlcv_start_ms: int | None = None,
                     ohlcv_end_ms: int | None = None) -> list[int]:
    """The chart feed's bar timestamps as the engine will see them: the CSV's
    rows inside ``[ohlcv_start_ms, ohlcv_end_ms]`` (either bound optional,
    both inclusive) in file order — the selection _load_bars makes for the
    ctypes runner and _trim_ohlcv_csv for the container."""
    try:
        import numpy as _np
    except ImportError:
        _np = None
    if _np is not None:
        import warnings
        with csv_path.open(newline="", encoding="utf-8") as f:
            header = f.readline().strip().split(",")
        with warnings.catch_warnings():
            # An empty feed: _load_bars already warns once for the run.
            warnings.filterwarnings("ignore", message="loadtxt: input contained no data")
            ts = _np.loadtxt(csv_path, delimiter=",", skiprows=1,
                             usecols=header.index("timestamp"), ndmin=1).astype("<i8")
        keep = _np.ones(len(ts), dtype=bool)
        if ohlcv_start_ms is not None:
            keep &= ts >= int(ohlcv_start_ms)
        if ohlcv_end_ms is not None:
            keep &= ts <= int(ohlcv_end_ms)
        return [int(t) for t in ts[keep]]
    out: list[int] = []
    with csv_path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            ts_row = int(row["timestamp"])
            if ohlcv_start_ms is not None and ts_row < int(ohlcv_start_ms):
                continue
            if ohlcv_end_ms is not None and ts_row > int(ohlcv_end_ms):
                continue
            out.append(ts_row)
    return out


class TvEntryWindow(NamedTuple):
    """The window a TV tape defines over the loaded chart feed
    (_tv_entry_emit_window)."""
    start_ms: int             # emit window start: the engine's trade_start_time + the trace
    end_ms: int               # TV's last entry
    report_start_ms: int      # trades are written from here (first entry - one bar interval)
    first_entry_ms: int       # TV's first entry as stamped on the tape
    fill_bar_ms: int | None   # the loaded feed bar at/just before it: TV's first fill bar
    signal_bar_ms: int | None  # the loaded feed bar before that one


def _tv_entry_emit_window(feed_ts: list[int], first_entry_ms: int, last_entry_ms: int,
                          bar_interval_ms: int) -> TvEntryWindow:
    """The emit window a TV tape defines, walked over the loaded chart feed.

    ``end`` is TV's last entry. ``report_start`` is ``first TV entry -
    bar_interval_ms`` (the feed's first-row gap, _infer_bar_interval_ms):
    the bound trades have always been reported from, and the ceiling of
    ``start``. ``start`` — the engine's ``trade_start_time`` (strategy
    commands are ignored before it less one script-TF bar:
    trading_is_active, engine_strategy_commands.cpp) and the trace window —
    is the loaded feed bar BEFORE the bar at or just before the first TV
    entry, i.e. the bar the first entry's order was placed on when the fill
    is the next bar's open, whenever that bar is earlier than
    ``report_start``; never later (widen-only).

    Why: ``first entry - one bar interval`` assumed the signal bar sits one
    interval before the fill bar. Across a weekend, a holiday or an
    overnight session break it does not, and the gate swallowed the
    strategy.entry TradingView filled on the next session's first bar —
    every ladder candidate produced 0 trades on six single-entry tapes
    (round 7, ledger log-20260905t054904z-a9baf07e; signal bar vs the old
    gate): jos-protrader NQ1@1D 2025-06-26 22:00Z vs 06-27 22:00Z (fill
    06-29 22:00Z), rakesh NIFTY@1D 11-14 03:45Z vs 11-15 03:45Z, vasanth
    F@1D 10-03 13:30Z vs 10-04 13:30Z, stockhunter2025 XAUUSD@1D 2026-01-08
    22:00Z vs 01-09 22:00Z, heneralmomo25 XAUUSD@1D 10-30 21:00Z vs 10-31
    22:00Z (the DST shift), ayusattv AAPL@15 04-09 19:45Z vs 04-10 13:00Z.
    Walking the feed the engine is actually fed (after the probe's
    ohlcv_start_ms / range-end bounds) handles every gap shape by
    construction, no calendar arithmetic; on a gapless feed the preceding
    bar IS one interval back and nothing changes.

    Reported trades keep the old bound on purpose: the fill of an order
    placed on the admitted signal bar lands on TV's first entry bar (at or
    after ``report_start``) and is written; a fill on the signal bar
    itself, which TV did not report, lies before ``report_start`` on every
    gap tape above and stays out; on a gapless feed such a fill was inside
    the one-interval bound already. Narrowing the bound to TV's first entry
    bar was rejected: the grader aligns entries inside a one-hour window,
    so an engine first fill one 15m bar early is a matched pair with an
    entry-time delta today and would become an unmatched trade instead.

    Degenerate shapes leave today's window untouched: no loaded bar at or
    before the first entry (an entry earlier than the feed, an empty feed)
    or a first entry on the feed's first row gives no preceding bar; a feed
    whose first-row gap is wider than the gap before the first entry (a
    feed starting on a Friday) keeps the wider legacy start. A first entry
    past the feed's last bar admits the last two bars, which is harmless:
    nothing at or after it exists to report."""
    report_start_ms = first_entry_ms - bar_interval_ms
    fill_index = -1
    for index, ts in enumerate(feed_ts):
        if ts > first_entry_ms:
            break
        fill_index = index
    fill_bar_ms = feed_ts[fill_index] if fill_index >= 0 else None
    signal_bar_ms = feed_ts[fill_index - 1] if fill_index >= 1 else None
    start_ms = (report_start_ms if signal_bar_ms is None
                else min(report_start_ms, signal_bar_ms))
    return TvEntryWindow(start_ms, last_entry_ms, report_start_ms,
                         first_entry_ms, fill_bar_ms, signal_bar_ms)


def _describe_tv_entry_window(w: TvEntryWindow) -> str:
    """The one line main() logs for a TV tape's window."""
    reported = (f"entries reported from {_fmt_utc_ms(w.report_start_ms)} "
                f"to {_fmt_utc_ms(w.end_ms)}")
    if w.fill_bar_ms is None:
        return (f"start {_fmt_utc_ms(w.start_ms)} (TV's first entry "
                f"{_fmt_utc_ms(w.first_entry_ms)} precedes the loaded feed: "
                f"unchanged); {reported}")
    if w.signal_bar_ms is None:
        return (f"start {_fmt_utc_ms(w.start_ms)} (TV's first entry "
                f"{_fmt_utc_ms(w.first_entry_ms)} is on the loaded feed's first "
                f"bar {_fmt_utc_ms(w.fill_bar_ms)}, no earlier bar: unchanged); "
                f"{reported}")
    if w.start_ms < w.report_start_ms:
        return (f"start {_fmt_utc_ms(w.start_ms)} = the feed bar before TV's "
                f"first entry bar {_fmt_utc_ms(w.fill_bar_ms)} (widened from "
                f"{_fmt_utc_ms(w.report_start_ms)}: the gap before the first "
                f"entry exceeds one bar interval); {reported}")
    return (f"start {_fmt_utc_ms(w.start_ms)} (the feed bar before TV's first "
            f"entry bar {_fmt_utc_ms(w.fill_bar_ms)} is "
            f"{_fmt_utc_ms(w.signal_bar_ms)}, not earlier: unchanged); {reported}")


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
    trade_start_time, chart_timezone, syminfo. ohlcv_start_ms / ohlcv_end_ms are
    applied by pre-trimming the CSV fed to the container (the ctypes path trims
    in _load_bars).
    NEVER re-transpiles .pine (uses the committed cpp, no codegen variance)."""
    import pf_release_run as _rel
    if run_kwargs.get("account_currency_fx_series") is not None:
        raise RuntimeError(
            "--runner docker does not support timestamped account-currency FX; "
            "use the ctypes runner with a freshly built strategy library")
    generated_cpp = strategy_dir / "generated.cpp"
    if not generated_cpp.exists():
        raise FileNotFoundError(f"generated.cpp not found for --runner docker: {generated_cpp}")

    # ohlcv_start_ms / ohlcv_end_ms: ctypes trims bars in _load_bars; the
    # container needs an already-trimmed CSV (M8 — feed the same trimmed
    # feed downstream). The end bound, when main() set one
    # (_apply_range_end_regime), is TradingView's range end, so the engine's
    # range-end close lands on TV's last bar.
    tmp_ohlcv = _trim_ohlcv_csv(ohlcv_path, run_kwargs.get("ohlcv_start_ms"),
                                run_kwargs.get("ohlcv_end_ms"))
    ohlcv_for_image = tmp_ohlcv if tmp_ohlcv is not None else ohlcv_path

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
    if run_kwargs.get("syminfo_type"):
        syminfo["type"] = run_kwargs["syminfo_type"]
    for skey, sval in (run_kwargs.get("syminfo_strings") or {}).items():
        syminfo[skey] = sval
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
        "broker_state_hash": [],
        "equity_curve_time_ms": [],
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
    ap.add_argument("--realtime-tail", type=int, default=None, metavar="HORIZON",
                    help="Live-runtime tail (ABI v4, spec §3.1): treat the fed OHLCV's last bar as "
                         "still forming instead of the chart's rightmost historical bar "
                         "(barstate.islast false, no range-end close row) and freeze "
                         "pine_last_bar_index() at HORIZON - 1. Calls strategy_set_realtime_tail "
                         "with on=1 before the run. --runner ctypes only.")
    ap.add_argument("--probe-suppress-tail", action="store_true",
                    help="Live probe tail suppression (ABI v4, spec §3.2): the fed OHLCV's "
                         "last bar runs only the broker's pre-on_bar steps (resting-order "
                         "fills, max-intraday-loss path check, per-trade extremes) and "
                         "on_bar is never invoked for it -- no chart-side fills, no margin "
                         "call / intraday-cap close against the forming bar. Calls "
                         "strategy_set_probe_suppress_tail_logic with on=1 before the run. "
                         "--runner ctypes only.")
    ap.add_argument("--path-order", choices=["auto", "high", "low"], default=None,
                    help="ABI v4 live-runtime surface: force this run's intrabar path order "
                         "instead of the engine's own |H-O| vs |O-L| AUTO rule -- 'high' for "
                         "O->H->L->C, 'low' for O->L->H->C. A live probe runs the same forming "
                         "bar under both forced orders and keeps only the fills that agree. "
                         "Calls strategy_set_path_order before the run. Default (unset) leaves "
                         "the engine on AUTO. --runner ctypes only.")
    ap.add_argument("--broker-state-hash", action="store_true",
                    help="ABI v4 live-runtime surface (task 6): enable per-script-bar "
                         "broker-state hash recording (strategy_set_broker_state_hash_recording) "
                         "and write the array as JSON. Written next to --trace-json as "
                         "'broker_state_hash.json' when that flag is given, else next to the "
                         "engine_trades.csv output. --runner ctypes only.")
    ap.add_argument("--dump-book", type=Path, default=None, metavar="PATH",
                    help="ABI v4 live-runtime surface (task 7): after the run, write the "
                         "engine's resting-order book -- every pf_pending_order_v1_t read "
                         "through strategy_pending_orders_len/strategy_pending_order_get -- "
                         "as JSON to PATH ({struct_version, layout, count, orders[], "
                         "position}). Each order carries a 'derived' sub-dict (task 8): "
                         "level_resolved, effective_levels {stop, limit, trail_activation} "
                         "and fill_qty -- the engine-sized opening qty / partition / "
                         "close_only probed at each finite effective stop/limit level and "
                         "at the last close (null for an EXIT). Read-only; the run is "
                         "unchanged. --runner ctypes only.")
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
    # emit_window gates the engine (trade_start_time) and the trace;
    # report_window selects the trades written. They differ only for a TV
    # tape (_tv_entry_emit_window), whose window is set below once the
    # feed's range-end bound is known.
    emit_window: tuple[int, int] | None = None
    report_window: tuple[int, int] | None = None
    tv_window_used = False
    tv_span: tuple[int, int] | None = None
    if args.no_trim_output:
        pass
    elif args.emit_window_ohlcv is not None:
        emit_window = report_window = _load_window_ms(args.emit_window_ohlcv.resolve())
    else:
        tv_span = _load_tv_entry_span(strategy_dir, params)
        tv_window_used = tv_span is not None
        if tv_span is None:
            emit_window = report_window = _load_window_ms(REFERENCE_OHLCV)
    # The measurement ENDS where TradingView's range ends
    # (_apply_range_end_regime): the feed is bounded at the bars opening at
    # or before the range's `to` from the tape's metrics.json — never the
    # tape's last trade row, which cuts the final HTF period short for a
    # strategy that reads it through a lookahead_on projection
    # (_load_tv_range_end) — so the engine's range-end close (open_at_end)
    # lands on TV's last bar, and the rows are written with their exit leg
    # marked for the grader (write_engine_trades_csv). The same for a tape
    # that marks its own row Signal "Open" and one that does not. Only with
    # a tape in use; an explicit --emit-window-ohlcv / --no-trim-output run
    # measures the feed it was given. The auxiliary request.security feed
    # (the ETH lane's 1m FEED_1M) is left as exported: the engine's routing
    # ignores aux bars labelled past the last chart bar
    # (engine_aux_security.cpp tail prefilter; test_aux_security_feed pins
    # the 15m/1m shape), so the chart's last bar keeps its full slice and
    # no chart bar is lost.
    if tv_window_used:
        print(f"  range-end: {_apply_range_end_regime(strategy_dir, params, run_kwargs)}")
        # The measurement STARTS on the feed bar preceding TV's first entry
        # bar — the bar the first entry's order was placed on — found by
        # walking the feed the engine is fed (both bounds applied), so a
        # weekend / holiday / overnight gap before TV's first fill no longer
        # hides the signal bar behind the gate; never later than the old
        # ``first entry - one bar interval`` start, from which trades keep
        # being reported (_tv_entry_emit_window has the rule and the six
        # round-7 tapes).
        assert tv_span is not None
        tv_window = _tv_entry_emit_window(
            _feed_timestamps(ohlcv_path,
                             ohlcv_start_ms=run_kwargs.get("ohlcv_start_ms"),
                             ohlcv_end_ms=run_kwargs.get("ohlcv_end_ms")),
            tv_span[0], tv_span[1], _infer_bar_interval_ms(ohlcv_path))
        emit_window = (tv_window.start_ms, tv_window.end_ms)
        report_window = (tv_window.report_start_ms, tv_window.end_ms)
        print(f"  emit-window: {_describe_tv_entry_window(tv_window)}")
    trade_start_ms = None
    if emit_window is not None and not args.allow_trading_before_window:
        if tv_window_used or args.disable_trading_before_window:
            trade_start_ms = emit_window[0]

    if args.runner == "docker":
        if args.trace_json is not None:
            sys.exit("error: --trace-json needs --emit-plots (deferred); not supported with --runner docker.")
        if args.fingerprint_json is not None:
            sys.exit("error: --fingerprint-json is not supported with --runner docker.")
        if run_kwargs.get("account_currency_fx_series") is not None:
            sys.exit(
                "error: --runner docker does not support timestamped "
                "account-currency FX; use --runner ctypes with a freshly "
                "built strategy library.")
        if run_kwargs.get("aux_security_ohlcv_csv") is not None:
            sys.exit(
                "error: --runner docker does not support an auxiliary "
                "request.security feed; use --runner ctypes with a freshly "
                "built strategy library.")
        if run_kwargs.get("native_security_feeds"):
            sys.exit(
                "error: --runner docker does not support native "
                "request.security feeds; use --runner ctypes with a freshly "
                "built strategy library.")
        if args.realtime_tail is not None:
            sys.exit(
                "error: --runner docker does not support --realtime-tail; "
                "use --runner ctypes with a freshly built strategy library.")
        if args.probe_suppress_tail:
            sys.exit(
                "error: --runner docker does not support --probe-suppress-tail; "
                "use --runner ctypes with a freshly built strategy library.")
        if args.path_order is not None:
            sys.exit(
                "error: --runner docker does not support --path-order; "
                "use --runner ctypes with a freshly built strategy library.")
        if args.broker_state_hash:
            sys.exit(
                "error: --runner docker does not support --broker-state-hash; "
                "use --runner ctypes with a freshly built strategy library.")
        if args.dump_book is not None:
            sys.exit(
                "error: --runner docker does not support --dump-book; "
                "use --runner ctypes with a freshly built strategy library.")
        strat = None
        report = _run_via_docker(strategy_dir, ohlcv_path, params, run_kwargs,
                                 trade_start_ms, args.image)
    else:
        so_path = find_strategy_lib(strategy_dir, args.so_name)
        strat = Strategy(so_path)
        if args.realtime_tail is not None:
            # Live-runtime tail (ABI v4, spec §3.1). strat is None only under
            # --runner docker, which already sys.exit's above when
            # --realtime-tail is set. Strategy.run's internal call is
            # hasattr-guarded, so a .so predating the export would otherwise
            # run to completion having silently ignored the flag -- for a
            # lane whose whole method is "does the flagged run differ from
            # the base run", a silently ignored flag makes every comparison
            # vacuously pass. Fail hard here, same as --broker-state-hash
            # below, and BEFORE strat.run() -- checking only after the run
            # (task-11 re-review, new finding 1) let a stale .so burn a full
            # backtest (minutes on the default feed) before exiting 1.
            if not hasattr(strat.lib, "strategy_set_realtime_tail"):
                sys.exit(
                    "error: --realtime-tail requires strategy_set_realtime_tail "
                    "(strategy.so predates ABI v4 spec section 3.1; rebuild the engine)")
        if args.broker_state_hash:
            # strat is None only under --runner docker, which already
            # sys.exit's above when --broker-state-hash is set -- so
            # reaching here means strat is the ctypes Strategy. A .so
            # predating the export would otherwise run to completion having
            # recorded nothing -- fail hard (like --realtime-tail above) and
            # BEFORE strat.run(), rather than after burning the backtest
            # time, so downstream consumers (e.g. the live-flags lane) never
            # have to wait out a doomed run.
            if not hasattr(strat.lib, "strategy_set_broker_state_hash_recording"):
                sys.exit(
                    "error: --broker-state-hash requires "
                    "strategy_set_broker_state_hash_recording (strategy.so predates "
                    "ABI v4 task 6; rebuild the engine)")
        if args.probe_suppress_tail:
            # Same rationale as --realtime-tail / --broker-state-hash above
            # (final review F8): strat.run's internal call is hasattr-guarded,
            # so a stale .so would otherwise run to completion having
            # silently ignored the flag. Fail hard here, BEFORE strat.run().
            if not hasattr(strat.lib, "strategy_set_probe_suppress_tail_logic"):
                sys.exit(
                    "error: --probe-suppress-tail requires "
                    "strategy_set_probe_suppress_tail_logic (strategy.so predates "
                    "ABI v4 spec section 3.2; rebuild the engine)")
        if args.path_order is not None:
            # Same rationale (final review F8).
            if not hasattr(strat.lib, "strategy_set_path_order"):
                sys.exit(
                    "error: --path-order requires strategy_set_path_order "
                    "(strategy.so predates ABI v4 spec section 3.3; rebuild the engine)")
        report = strat.run(ohlcv_path, params=params,
                           trace_enabled=args.trace_json is not None,
                           trade_start_time_ms=trade_start_ms,
                           realtime_tail_horizon=args.realtime_tail,
                           probe_suppress_tail=args.probe_suppress_tail,
                           path_order=args.path_order,
                           broker_state_hash_recording=args.broker_state_hash,
                           dump_book=args.dump_book is not None,
                           **run_kwargs)
    raw_trade_count = len(report["trades"])
    trades_to_write = _filter_trades_to_window(report["trades"], report_window)
    # The trades the verifier grades are the CSV's — run_strategy.py writes
    # no report JSON — and the summary line below counts what was written,
    # the range-end close rows among them.
    range_end_rows = sum(1 for t in trades_to_write if t.get("open_at_end"))
    write_engine_trades_csv(trades_to_write, out_path)
    if args.trace_json is not None:
        trace_to_write = _filter_trace_to_window(report["trace"], emit_window)
        args.trace_json.parent.mkdir(parents=True, exist_ok=True)
        with args.trace_json.open("w", encoding="utf-8") as f:
            json.dump({
                "strategy": str(strategy_dir),
                "ohlcv": str(ohlcv_path),
                "emit_window": None if emit_window is None else {"start_ms": emit_window[0], "end_ms": emit_window[1]},
                "report_start_ms": None if report_window is None else report_window[0],
                "trace_names": report["trace_names"],
                "trace": trace_to_write,
            }, f)
    if args.realtime_tail is not None:
        # Receipt: a consumer (scripts/live_flags_lane.py) asserts this line
        # is present in a flagged run's stdout so a future regression that
        # silently no-ops the flag (e.g. an accidental hasattr-guard removal
        # upstream) shows up as a missing receipt, not a quiet P=0. The
        # strategy_set_realtime_tail hard-fail guard runs earlier, right
        # after `strat = Strategy(so_path)` and before strat.run(), so a
        # stale .so is caught before the (possibly multi-minute) backtest
        # rather than after it.
        print(f"realtime-tail: on horizon={args.realtime_tail}")
    if args.probe_suppress_tail:
        # Receipt (final review F8), same rationale/style as --realtime-tail
        # above: the strategy_set_probe_suppress_tail_logic hard-fail guard
        # runs earlier, right after `strat = Strategy(so_path)` and before
        # strat.run(), so a stale .so is caught before the run rather than
        # after a silently no-op'd flag.
        print("probe-suppress-tail: on")
    if args.path_order is not None:
        # Receipt (final review F8); the strategy_set_path_order hard-fail
        # guard runs earlier, same rationale as above.
        print(f"path-order: {args.path_order}")
    if args.broker_state_hash:
        # The strategy_set_broker_state_hash_recording hard-fail guard runs
        # earlier, right after `strat = Strategy(so_path)` and before
        # strat.run() -- see there for why. Reaching here means strat is the
        # ctypes Strategy (--runner docker already sys.exit's above when
        # --broker-state-hash is set) and the export is present.
        # Sibling of --trace-json when given (both are per-script-bar
        # debug arrays); otherwise sibling of the trades CSV output.
        bsh_path = ((args.trace_json.parent if args.trace_json is not None else out_path.parent)
                    / "broker_state_hash.json")
        bsh_path.parent.mkdir(parents=True, exist_ok=True)
        bsh_values = report["broker_state_hash"]
        bsh_times = report["equity_curve_time_ms"]
        # Self-describing: script_bars_processed lets a consumer verify
        # coverage without loading the trades CSV, and each entry pairs
        # its script-bar OPEN timestamp with the hash (hex, not a bare
        # JSON-Number, so no consumer can silently truncate a uint64 to
        # a JS-safe double). Invariant (Strategy.run/_report_to_dict):
        # len(bsh_values) == len(bsh_times) whenever recording was on.
        entries = [
            {"time_ms": bsh_times[i], "hash": format(bsh_values[i], "016x")}
            for i in range(len(bsh_values))
        ]
        with bsh_path.open("w", encoding="utf-8") as f:
            json.dump({
                "strategy": str(strategy_dir),
                "ohlcv": str(ohlcv_path),
                "script_bars_processed": report["script_bars_processed"],
                "entries": entries,
            }, f)
        print(f"  broker-state-hash: wrote {len(entries)} entries to {bsh_path}")
    if args.dump_book is not None:
        # strat is None only under --runner docker, which sys.exit's above
        # when --dump-book is set. A .so predating the exports still runs
        # (the accessors are hasattr-guarded) but has no book to read --
        # warn rather than write an empty, misleading file.
        if strat.IntentRowV1 is None:
            print("  dump-book: WARNING -- strategy.so predates "
                  "strategy_pending_order_layout (rebuild the engine); "
                  f"skipping {args.dump_book}", file=sys.stderr)
        else:
            book = report.get("pending_orders", [])
            args.dump_book.parent.mkdir(parents=True, exist_ok=True)
            with args.dump_book.open("w", encoding="utf-8") as f:
                json.dump({
                    "strategy": str(strategy_dir),
                    "ohlcv": str(ohlcv_path),
                    "struct_version": PENDING_ORDER_STRUCT_VERSION,
                    # The runtime's own field table, so a consumer can tell
                    # which fields this engine build emitted.
                    "layout": [
                        {"name": n, "type": t, "offset": o, "size": z}
                        for n, t, o, z in strat.pending_order_layout
                    ],
                    "count": len(book),
                    "orders": book,
                    # ABI v4 task 8: the position scalars the derived
                    # values were resolved against (None on a .so that
                    # predates strategy_position_avg_price & co.).
                    "position": report.get("position"),
                }, f)
            print(f"  dump-book: wrote {len(book)} resting order(s) to {args.dump_book}")
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
            runtime_kwargs = dict(run_kwargs)
            if report.get("aux_security_source_feed_sha256"):
                runtime_kwargs["aux_security_source_feed_sha256"] = \
                    report["aux_security_source_feed_sha256"]
            if report.get("native_security_feeds"):
                runtime_kwargs["native_security_feeds"] = \
                    report["native_security_feeds"]
            runtime = build_runtime_provenance(
                runtime_kwargs, trade_start_ms)
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
        f"{'' if raw_trade_count == len(trades_to_write) else f' ({raw_trade_count} raw)'}"
        f"{f' ({range_end_rows} range-end)' if range_end_rows else ''}, "
        f"net_profit={report['net_profit']:.2f}, "
        f"bars={report['input_bars_processed']}, "
        f"{elapsed:.2f}s -> {out_path.name}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
