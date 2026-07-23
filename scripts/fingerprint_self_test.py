#!/usr/bin/env python3
"""fingerprint_self_test.py — pins the backtest-fingerprint logic.

The fingerprint helpers are duplicated (byte-identical) in
docker/run_json.py and scripts/run_strategy.py because scripts/ is
.dockerignore'd, so a shared runtime module cannot be COPY'd into the
image. This test loads BOTH copies, runs one fixture generated.cpp
through each, and asserts (a) the parser/provenance/fingerprint behave
as designed and (b) the two copies produce identical output — so they
cannot silently drift.

Exit 0 iff every check passes.
"""
from __future__ import annotations

import ast
import base64
import enum
import hashlib
import importlib.util
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RUN_JSON = REPO / "docker" / "run_json.py"
RUN_STRATEGY = REPO / "scripts" / "run_strategy.py"

# Deterministic numeric vectors: (python_value, expected_canonical_text).
# Expected forms match ECMAScript NumberToString (RFC 8785 numeric form).
CANONICAL_NUMBER_VECTORS = [
    (12345.0, "12345"),
    (0.0, "0"),
    (-0.0, "0"),
    (1.5, "1.5"),
    (-1.5, "-1.5"),
    (0.1, "0.1"),
    (1e-7, "1e-7"),
    (1e-6, "0.000001"),
    (1e20, "100000000000000000000"),
    (1e21, "1e+21"),
    (-12345.0, "-12345"),
    (-1e-7, "-1e-7"),
    (-1e20, "-100000000000000000000"),
    (1, "1"),  # int path
    (0, "0"),
    # Product safe-integer edges (Number.MAX/MIN_SAFE_INTEGER = ±(2**53-1)).
    (9007199254740991, "9007199254740991"),
    (-9007199254740991, "-9007199254740991"),
]

# String forms match RFC 8785 / JCS (raw Unicode, not ensure_ascii escapes).
# Control chars / quotes / backslash stay escaped.
CANONICAL_STRING_VECTORS = [
    ("", '""'),
    ("ascii", '"ascii"'),
    ("中文键", '"中文键"'),
    ("中文值", '"中文值"'),
    ("😀", '"😀"'),
    ("a😀b", '"a😀b"'),
    ("\u2028", '"\u2028"'),  # LINE SEPARATOR — raw, not \u2028 escape
    ("\u2029", '"\u2029"'),  # PARAGRAPH SEPARATOR
    ("\u2028\u2029", '"\u2028\u2029"'),
    ('say "hi"\\ok', '"say \\"hi\\"\\\\ok"'),
    ("\n\t\r\b\f", '"\\n\\t\\r\\b\\f"'),
    ("\x00\x1f", '"\\u0000\\u001f"'),
]

# Exact integers outside the product safe-integer domain — rejected by policy
# (unique lossless integer identity for generic ECMAScript consumers). This is
# intentional domain rejection, not a claim that every magnitude must round
# under binary64 (see float(2**53) vectors below, which remain legal).
UNSAFE_INTEGERS = [
    9007199254740992,   # exact int 2**53 — outside product int domain
    9007199254740993,   # not uniquely representable as binary64 integer
    9223372036854775807,
    -9007199254740992,
    -9007199254740993,
    -9223372036854775808,
]


class _WeirdStrInt(int):
    """int subclass whose dunder hooks deliberately lie about the value."""

    def __index__(self) -> int:
        return 99

    def __int__(self) -> int:
        return 99

    def __lt__(self, other) -> bool:
        return False

    def __gt__(self, other) -> bool:
        return False

    def __le__(self, other) -> bool:
        return False

    def __ge__(self, other) -> bool:
        return False

    def __eq__(self, other) -> bool:
        return False

    def __str__(self) -> str:
        return "not-a-json-number"

    def __repr__(self) -> str:
        return "not-a-json-number"

    def __format__(self, format_spec: str) -> str:
        return "99"


class _WeirdFloat(float):
    """float subclass whose dunder hooks deliberately lie about the value."""

    def __float__(self) -> float:
        return -88.0

    def __abs__(self):
        return 88.0

    def __lt__(self, other) -> bool:
        return True

    def __eq__(self, other) -> bool:
        return False

    def __repr__(self) -> str:
        return "-88"

    def __format__(self, format_spec: str) -> str:
        return "-88"


class _WeirdStr(str):
    """str subclass whose dunder hooks deliberately lie about the value.

    Without base ``str.__str__`` normalization, ``__iter__`` can hide
    unpaired surrogates from the rejection scan and ``encode`` can corrupt
    JCS UTF-16 object-key order (e.g. flip ``"10"`` / ``"2"``).
    """

    def __str__(self) -> str:
        return "hostile"

    def __iter__(self):
        # Hide underlying content (including unpaired surrogates).
        return iter("")

    def encode(self, encoding: str = "utf-8", errors: str = "strict") -> bytes:
        # Flip UTF-16 code-unit order of keys "10" and "2" if consulted.
        underlying = str.__str__(self)
        if encoding == "utf-16-be":
            if underlying == "10":
                return "9".encode("utf-16-be")
            if underlying == "2":
                return "1".encode("utf-16-be")
            return ("z" * max(1, len(underlying))).encode("utf-16-be")
        return "hostile".encode(encoding, errors)


class _TaggedStr(str):
    """Adversarial str subclass: tag/identity hash/eq allow coexistence.

    Two instances can share the same underlying text (so base
    ``str.__str__`` normalizes both to one plain key) while tag-based
    ``__eq__``/``__hash__`` let them both live in one Python dict. A
    naive writer would emit duplicate JSON names; the fingerprint
    helpers must fail closed with ValueError.
    """

    def __new__(cls, text: str, tag: object):
        obj = str.__new__(cls, text)
        obj.tag = tag
        return obj

    def __eq__(self, other):
        if isinstance(other, _TaggedStr):
            return (self.tag == other.tag
                    and str.__str__(self) == str.__str__(other))
        return NotImplemented

    def __hash__(self):
        return hash((_TaggedStr, str.__str__(self), self.tag))

    def __str__(self) -> str:
        return "hostile"


class _SampleIntEnum(enum.IntEnum):
    """IntEnum-style int subclass regression (Py3.9 str was Enum.NAME)."""

    ALPHA = 7
    EDGE = 9007199254740991

# Unpaired UTF-16 surrogates — invalid I-JSON / UTF-8; fail closed.
UNPAIRED_SURROGATES = [
    "\ud800",
    "\udfff",
    "a\ud800b",
    "\ud800\ud800",
]

# Compact representative set from RFC 8785 Appendix B (ES6 Number samples):
# (big-endian IEEE-754 binary64 hex, expected JCS / NumberToString text).
RFC8785_APPENDIX_B_BINARY64 = [
    ("0000000000000000", "0"),                         # +0
    ("8000000000000000", "0"),                         # -0
    ("0000000000000001", "5e-324"),                    # min subnormal
    ("000FFFFFFFFFFFFF", "2.225073858507201e-308"),    # max subnormal
    ("0010000000000000", "2.2250738585072014e-308"),   # min normal
    ("7FEFFFFFFFFFFFFF", "1.7976931348623157e+308"),   # max finite
    ("4340000000000000", "9007199254740992"),          # 2**53
    ("4430000000000000", "295147905179352830000"),
    ("44B52D02C7E14AF5", "9.999999999999997e+22"),
    ("44B52D02C7E14AF6", "1e+23"),
    ("44B52D02C7E14AF7", "1.0000000000000001e+23"),
    ("444B1AE4D6E2EF4E", "999999999999999700000"),
    ("444B1AE4D6E2EF4F", "999999999999999900000"),
    ("444B1AE4D6E2EF50", "1e+21"),
]

# Float sources whose canonical tokens look like large integers — plain
# json.loads yields Python int outside the product int domain; IEEE-754 parse
# is required for re-canonicalization tests. Exact-int forms of these
# magnitudes remain rejected (product integer domain), but the binary64
# values themselves are deliberately legal.
IEEE754_TOKEN_ROUNDTRIP_VECTORS = [
    (1e20, "100000000000000000000"),
    (9007199254740992.0, "9007199254740992"),  # float 2**53 legal; int 2**53 not
]


def _helper_block(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    start = text.index("# >>> fingerprint helpers")
    end = text.index("# <<< fingerprint helpers") + len("# <<< fingerprint helpers")
    return text[start:end]


def _ieee754_json_loads(text: str):
    """Decode JSON with a binary64 number model (RFC 8785 / ES Number).

    Default json.loads promotes integral tokens to unbounded Python int, which
    is the wrong model for JCS re-canonicalization tests (e.g. 1e20).
    """
    return json.loads(text, parse_int=float)


def _f64_from_be_hex(hex64: str) -> float:
    return struct.unpack(">d", bytes.fromhex(hex64))[0]

# Fixture: a ctor that sets a SUBSET of strategy() fields (process_orders_on_close
# and close_entries_rule are intentionally absent — they default in the engine
# base class), a set_strategy_override with a decoy `initial_capital_ =` line
# (must NOT be parsed), and a mix of input types incl. a duplicate title.
FIXTURE_CPP = '''
struct GeneratedStrategy : Strategy {
    explicit GeneratedStrategy() : _ta_ema_1(5), _ta_ema_2(13) {
        initial_capital_ = 50000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 3.0;
        pyramiding_ = 2;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.04;
        slippage_ = 1;
    }
    void set_strategy_override(const std::string& key, const std::string& value) {
        if (key == "initial_capital") { initial_capital_ = std::stod(value); return; }
    }
    void on_bar(const Bar& bar) override {
        if (!_inputs_initialized_) {
            i_fast = get_input_int("Fast EMA", 5);
            i_slow = get_input_int("Slow EMA", 13);
            thr = get_input_double("ADX trend threshold", 25);
            mode = get_input_string("Mode", std::string("fast"));
            src = get_input_source("Source", close);
            _inputs_initialized_ = true;
        }
        i_fast = get_input_int("Fast EMA", 5);
    }
};
'''


def _load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    passed = failed = 0

    def check(name: str, cond: bool) -> None:
        nonlocal passed, failed
        if cond:
            passed += 1
            print(f"  OK   {name}")
        else:
            failed += 1
            print(f"  FAIL {name}")

    rj = _load(RUN_JSON, "pf_run_json")
    rs = _load(RUN_STRATEGY, "pf_run_strategy")

    for label, m in (("run_json", rj), ("run_strategy", rs)):
        # --- strategy() defaults from the ctor only -----------------------
        strat = m.parse_strategy_params(FIXTURE_CPP)
        check(f"{label}: initial_capital parsed", strat.get("initial_capital") == 50000.0)
        check(f"{label}: enum default_qty_type mapped",
              strat.get("default_qty_type") == "percent_of_equity")
        check(f"{label}: commission_value parsed", strat.get("commission_value") == 0.04)
        check(f"{label}: pyramiding parsed", strat.get("pyramiding") == 2)
        check(f"{label}: slippage parsed", strat.get("slippage") == 1)
        # decoy line in set_strategy_override must NOT pollute the parse
        check(f"{label}: override-fn line not parsed (still 50000.0)",
              strat.get("initial_capital") == 50000.0)
        # ctor does NOT set these two; parse_strategy_params must omit them
        check(f"{label}: ctor omits process_orders_on_close",
              "process_orders_on_close" not in strat)

        # --- effective strategy: seed fills the base-class-defaulted fields
        eff_no_ovr = m.effective_strategy(FIXTURE_CPP, {})
        check(f"{label}: seed fills process_orders_on_close=False (no override)",
              eff_no_ovr.get("process_orders_on_close") is False)
        check(f"{label}: seed fills close_entries_rule=FIFO (no override)",
              eff_no_ovr.get("close_entries_rule") == "FIFO")
        check(f"{label}: effective keeps ctor initial_capital",
              eff_no_ovr.get("initial_capital") == 50000.0)

        eff_ovr = m.effective_strategy(FIXTURE_CPP,
                                       {"pyramiding": "5", "process_orders_on_close": "true"})
        check(f"{label}: override wins (pyramiding=5)", eff_ovr.get("pyramiding") == "5")
        check(f"{label}: override wins (process_orders_on_close=true)",
              eff_ovr.get("process_orders_on_close") == "true")

        # --- inputs ------------------------------------------------------
        inp = m.parse_inputs(FIXTURE_CPP)
        check(f"{label}: 5 distinct inputs (dedup Fast EMA)", len(inp) == 5)
        check(f"{label}: int default typed", inp["Fast EMA"] == {"type": "int", "default": 5})
        check(f"{label}: double default typed",
              inp["ADX trend threshold"]["default"] == 25 and inp["ADX trend threshold"]["type"] == "double")
        check(f"{label}: string default unquoted", inp["Mode"]["default"] == "fast")
        check(f"{label}: source default kept", inp["Source"]["default"] == "close")

        eff_in = m.effective_inputs(FIXTURE_CPP, {"Fast EMA": "8"})
        check(f"{label}: input override -> value", eff_in["Fast EMA"]["value"] == "8")
        check(f"{label}: non-overridden input -> default value", eff_in["Slow EMA"]["value"] == 13)
        check(f"{label}: effective_inputs keeps default+type",
              eff_in["Fast EMA"] == {"type": "int", "default": 5, "value": "8"})

        # --- fingerprint round-trip + determinism ------------------------
        prov = {
            "engine": {"version_string": "0.10.2", "major": 0, "minor": 10, "patch": 2,
                       "commit_sha": "f3fc3a3"},
            "codegen": {"version": "0.6.4", "generated_cpp_sha256": "deadbeef",
                        "transpiled_from_pine": True},
            "strategy": eff_no_ovr,
            "inputs": eff_in,
            "applied": {"inputs": {"Fast EMA": "8"}, "overrides": {}},
            "runtime": {"input_tf": "", "bar_magnifier": False},
        }
        fp = m.build_fingerprint(prov)
        decoded = json.loads(base64.b64decode(fp["token"]))
        check(f"{label}: token decodes to provenance", decoded == prov)
        check(f"{label}: digest prefixed sha256:", fp["digest"].startswith("sha256:"))
        canonical = m._canonical_fingerprint_json(prov)
        check(f"{label}: digest matches canonical sha256",
              fp["digest"] == "sha256:" + hashlib.sha256(canonical.encode()).hexdigest())
        fp2 = m.build_fingerprint(prov)
        check(f"{label}: deterministic token", fp["token"] == fp2["token"])

        # --- canonical number / nest vectors (language-independent) ------
        for value, expected in CANONICAL_NUMBER_VECTORS:
            got = m._canonical_fingerprint_json(value)
            check(f"{label}: number vector {value!r} -> {expected!r}",
                  got == expected)

        # RFC 8785 Appendix B binary64 samples via exact bit patterns.
        for hex64, expected in RFC8785_APPENDIX_B_BINARY64:
            value = _f64_from_be_hex(hex64)
            got = m._canonical_fingerprint_json(value)
            check(f"{label}: RFC8785 App.B {hex64} -> {expected!r}",
                  got == expected)

        # Canonical-token round trips that require IEEE-754 number parsing:
        # token bytes are authoritative for digest; re-canonicalization uses
        # parse_int=float. Source Python ints outside the safe range still fail.
        for value, expected in IEEE754_TOKEN_ROUNDTRIP_VECTORS:
            canon = m._canonical_fingerprint_json(value)
            check(f"{label}: ieee token form {value!r} -> {expected!r}",
                  canon == expected)
            fp_n = m.build_fingerprint({"n": value})
            token_bytes = base64.b64decode(fp_n["token"])
            token_text = token_bytes.decode("utf-8")
            check(f"{label}: ieee digest from token bytes {value!r}",
                  fp_n["digest"]
                  == "sha256:" + hashlib.sha256(token_bytes).hexdigest())
            # Plain json.loads yields a Python int outside the safe range.
            plain = json.loads(token_text)["n"]
            check(f"{label}: plain loads of {expected!r} is int (not float)",
                  isinstance(plain, int) and not isinstance(plain, bool))
            plain_rejected = False
            try:
                m._canonical_fingerprint_json(plain)
            except ValueError:
                plain_rejected = True
            check(f"{label}: plain-loaded int of {expected!r} rejected",
                  plain_rejected)
            # Verifier reconstruction: binary64 number model + re-encode.
            rebuilt = m._canonical_fingerprint_json(
                _ieee754_json_loads(token_text))
            check(f"{label}: ieee re-canon round-trip {value!r}",
                  rebuilt == token_text)
            rebuilt_fp = m.build_fingerprint(_ieee754_json_loads(token_text))
            check(f"{label}: ieee re-canon digest matches {value!r}",
                  rebuilt_fp["digest"] == fp_n["digest"]
                  and rebuilt_fp["token"] == fp_n["token"])

        for value, expected in CANONICAL_STRING_VECTORS:
            got = m._canonical_fingerprint_json(value)
            check(f"{label}: string vector {value!r} -> {expected!r}",
                  got == expected)
            # Raw Unicode must not be ensure_ascii-escaped.
            if any(ord(ch) >= 0x80 for ch in value):
                check(f"{label}: string {value!r} is raw Unicode (no \\u)",
                      "\\u" not in got)

        # Chinese object + value: JCS string form (raw Unicode).
        chinese = {"中文键": "中文值"}
        chinese_canon = m._canonical_fingerprint_json(chinese)
        check(f"{label}: Chinese object is raw Unicode JSON",
              chinese_canon == '{"中文键":"中文值"}')

        # U+10000 sorts *before* U+E000 under UTF-16 code units (JCS/JS),
        # but *after* under Unicode code-point order (Python sorted()).
        key_order = {"\U00010000": 1, "\uE000": 2}
        key_order_canon = m._canonical_fingerprint_json(key_order)
        check(f"{label}: UTF-16 key order U+10000 before U+E000",
              key_order_canon == '{"\U00010000":1,"\uE000":2}')
        # Sanity: code-point sort would reverse these two keys.
        check(f"{label}: UTF-16 order differs from code-point order",
              sorted(["\U00010000", "\uE000"])
              == ["\uE000", "\U00010000"])

        # Array-index-looking keys + "__proto__" must all be retained in
        # lexical UTF-16 order. A naive JS object-rebuild + JSON.stringify
        # reorders integer-index keys and drops assignment to __proto__.
        special_keys = {
            "2": 2,
            "10": 10,
            "__proto__": "own",
        }
        special_keys_canon = m._canonical_fingerprint_json(special_keys)
        check(f"{label}: special keys retained in lexical UTF-16 order",
              special_keys_canon == '{"10":10,"2":2,"__proto__":"own"}')
        check(f"{label}: special keys all present (no __proto__ drop)",
              '"10":10' in special_keys_canon
              and '"2":2' in special_keys_canon
              and '"__proto__":"own"' in special_keys_canon
              and special_keys_canon.index('"10"')
              < special_keys_canon.index('"2"')
              < special_keys_canon.index('"__proto__"'))
        special_nested = {
            "z": [{"2": 1, "10": 2, "__proto__": 3}],
            "2": 0,
            "10": -1,
            "__proto__": {"nested": True},
        }
        special_nested_canon = m._canonical_fingerprint_json(special_nested)
        check(f"{label}: nested special keys lexical UTF-16 + array recurse",
              special_nested_canon == (
                  '{"10":-1,"2":0,"__proto__":{"nested":true},'
                  '"z":[{"10":2,"2":1,"__proto__":3}]}'))

        nested = {
            "z": [0.0, -0.0, 12345.0, {"inner": 1e-6}],
            "a": {"initial_capital": 12345.0, "qty": 1.5},
            "emoji": "😀",
            "sep": "\u2028\u2029",
            "中文键": "中文值",
            "\U00010000": "supplementary",
            "\uE000": "pua",
        }
        nested_canon = m._canonical_fingerprint_json(nested)
        # UTF-16 key order: a, emoji, sep, z, 中文键, U+10000, U+E000
        check(f"{label}: nested keys UTF-16-sorted + numbers/strings canonical",
              nested_canon == (
                  '{"a":{"initial_capital":12345,"qty":1.5},'
                  '"emoji":"😀",'
                  '"sep":"\u2028\u2029",'
                  '"z":[0,0,12345,{"inner":0.000001}],'
                  '"中文键":"中文值",'
                  '"\U00010000":"supplementary",'
                  '"\uE000":"pua"}'))

        nonfinite_rejected = True
        for bad in (float("nan"), float("inf"), float("-inf")):
            try:
                m._canonical_fingerprint_json({"x": bad})
                nonfinite_rejected = False
            except ValueError:
                pass
        check(f"{label}: non-finite numbers rejected", nonfinite_rejected)

        unsafe_int_rejected = True
        for bad in UNSAFE_INTEGERS:
            try:
                m._canonical_fingerprint_json(bad)
                unsafe_int_rejected = False
            except ValueError:
                pass
            try:
                m._canonical_fingerprint_json({"n": bad})
                unsafe_int_rejected = False
            except ValueError:
                pass
        check(f"{label}: out-of-domain exact integers rejected",
              unsafe_int_rejected)

        # Product policy: exact int 2**53 is outside the integer domain, while
        # isolated binary64 float(2**53) remains legal NumberToString output.
        # This is intentional accepted-type policy, not "every such value
        # must round."
        check(f"{label}: float 2**53 is legal binary64 token",
              m._canonical_fingerprint_json(9007199254740992.0)
              == "9007199254740992")
        int_2_53_rejected = False
        try:
            m._canonical_fingerprint_json(9007199254740992)
        except ValueError:
            int_2_53_rejected = True
        check(f"{label}: exact int 2**53 rejected (product int domain)",
              int_2_53_rejected)
        # Out-of-domain provenance yields no fingerprint under callers:
        # build_fingerprint raises before emitting token/digest.
        no_fp = False
        try:
            m.build_fingerprint({"n": 9007199254740992})
        except ValueError:
            no_fp = True
        check(f"{label}: out-of-domain provenance yields no fingerprint",
              no_fp)

        # Int subclasses must emit underlying decimal digits even when
        # __index__/__int__/comparisons/__str__/__repr__/__format__ are
        # hostile (custom int) or Enum-style (IntEnum on Python 3.9; str
        # was Enum.NAME). Domain checks use the normalized plain int.
        check(f"{label}: custom int subclass ignores hostile int hooks",
              m._canonical_fingerprint_json(_WeirdStrInt(42)) == "42"
              and m._canonical_fingerprint_json(_WeirdStrInt(0)) == "0"
              and m._canonical_fingerprint_json(_WeirdStrInt(-7)) == "-7"
              and m._canonical_fingerprint_json(_WeirdStrInt(3)) == "3")
        check(f"{label}: IntEnum emits decimal digits not Enum.NAME",
              m._canonical_fingerprint_json(_SampleIntEnum.ALPHA) == "7"
              and m._canonical_fingerprint_json(_SampleIntEnum.EDGE)
              == "9007199254740991")
        weird_oob_rejected = False
        try:
            m._canonical_fingerprint_json(_WeirdStrInt(9007199254740992))
        except ValueError:
            weird_oob_rejected = True
        check(f"{label}: custom int subclass still enforces safe-int domain",
              weird_oob_rejected)

        # Float subclasses: normalize via float.__float__ so hostile
        # __float__/__abs__/comparisons/repr cannot rewrite NumberToString.
        check(f"{label}: custom float subclass ignores hostile float hooks",
              m._canonical_fingerprint_json(_WeirdFloat(1.5)) == "1.5"
              and m._canonical_fingerprint_json(_WeirdFloat(-0.0)) == "0"
              and m._canonical_fingerprint_json(_WeirdFloat(0.0)) == "0")
        weird_float_nonfinite_rejected = True
        for bad in (float("nan"), float("inf"), float("-inf")):
            try:
                m._canonical_fingerprint_json(_WeirdFloat(bad))
                weird_float_nonfinite_rejected = False
            except ValueError:
                pass
        check(f"{label}: custom float subclass still rejects nonfinite",
              weird_float_nonfinite_rejected)

        # Str subclasses: normalize via str.__str__ so hostile
        # __str__/__iter__/encode cannot rewrite emission, key order, or
        # hide unpaired surrogates from the rejection scan.
        check(f"{label}: custom str subclass ignores hostile str hooks",
              m._canonical_fingerprint_json(_WeirdStr("good")) == '"good"'
              and m._canonical_fingerprint_json({
                  _WeirdStr("10"): 1,
                  _WeirdStr("2"): 2,
              }) == '{"10":1,"2":2}')
        weird_str_surrogate_rejected = False
        try:
            m._canonical_fingerprint_json(_WeirdStr("\ud800"))
        except ValueError:
            weird_str_surrogate_rejected = True
        try:
            m._canonical_fingerprint_json({_WeirdStr("\ud800"): "x"})
            weird_str_surrogate_rejected = False
        except ValueError:
            pass
        check(f"{label}: custom str subclass still rejects unpaired surrogates",
              weird_str_surrogate_rejected)

        # Distinct str-subclass keys can override __eq__/__hash__ so both
        # coexist in a Python dict while str.__str__ yields the same plain
        # text. Emitting both would produce duplicate JSON names (JS drops
        # one); helpers must fail closed on the collision.
        tagged_a = _TaggedStr("same", "a")
        tagged_b = _TaggedStr("same", "b")
        tagged_dup = {tagged_a: 1, tagged_b: 2}
        check(f"{label}: tagged str keys coexist pre-normalization",
              len(tagged_dup) == 2
              and str.__str__(tagged_a) == str.__str__(tagged_b) == "same")
        tagged_dup_rejected = False
        try:
            m._canonical_fingerprint_json(tagged_dup)
        except ValueError:
            tagged_dup_rejected = True
        check(f"{label}: duplicate normalized object keys rejected",
              tagged_dup_rejected)

        # Booleans must remain distinct from the int path (bool subclasses int).
        check(f"{label}: bool true/false stay boolean tokens",
              m._canonical_fingerprint_json(True) == "true"
              and m._canonical_fingerprint_json(False) == "false"
              and m._canonical_fingerprint_json([True, False, 0, 1])
              == "[true,false,0,1]")

        surrogate_rejected = True
        for bad in UNPAIRED_SURROGATES:
            try:
                m._canonical_fingerprint_json(bad)
                surrogate_rejected = False
            except ValueError:
                pass
            try:
                m._canonical_fingerprint_json({bad: "x"})
                surrogate_rejected = False
            except ValueError:
                pass
            try:
                m._canonical_fingerprint_json({"k": bad})
                surrogate_rejected = False
            except ValueError:
                pass
        check(f"{label}: unpaired surrogates rejected", surrogate_rejected)

        # Live bug: integral float must not produce a trailing ".0" that a
        # JS NumberToString path would drop, breaking token/digest verification.
        live = {"strategy": {"initial_capital": 12345.0}}
        live_fp = m.build_fingerprint(live)
        live_token_json = base64.b64decode(live_fp["token"]).decode("utf-8")
        check(f"{label}: live integral-float token has no trailing .0",
              '"initial_capital":12345' in live_token_json
              and "12345.0" not in live_token_json)
        # Safe integral tokens re-parse under either model; prefer binary64
        # for verifier reconstruction consistency with large-number vectors.
        recomputed = m.build_fingerprint(_ieee754_json_loads(live_token_json))
        check(f"{label}: digest stable after token JSON re-parse",
              recomputed["digest"] == live_fp["digest"]
              and recomputed["token"] == live_fp["token"])
        # Authoritative path: hash decoded token bytes (no re-serialization).
        live_token_bytes = live_token_json.encode("utf-8")
        check(f"{label}: digest equals sha256 of token bytes",
              live_fp["digest"]
              == "sha256:" + hashlib.sha256(live_token_bytes).hexdigest())

        # Unicode provenance survives token base64 + UTF-8 round-trip.
        uni_live = {"中文键": "中文值", "emoji": "😀\u2028"}
        uni_fp = m.build_fingerprint(uni_live)
        uni_token_json = base64.b64decode(uni_fp["token"]).decode("utf-8")
        check(f"{label}: Unicode token is raw UTF-8 (no ensure_ascii)",
              "中文键" in uni_token_json and "\\u" not in uni_token_json)
        uni_recomputed = m.build_fingerprint(_ieee754_json_loads(uni_token_json))
        check(f"{label}: Unicode digest stable after token re-parse",
              uni_recomputed["digest"] == uni_fp["digest"]
              and uni_recomputed["token"] == uni_fp["token"])

    # --- the effective execution gate must participate in the fingerprint
    runtime_kwargs = {
        "input_tf": "15",
        "script_tf": "",
        "bar_magnifier": False,
        "magnifier_samples": 4,
        "magnifier_distribution": "ENDPOINTS",
        "chart_timezone": "UTC",
    }
    gate_ms = 1712345678901
    open_runtime = rs.build_runtime_provenance(runtime_kwargs, None)
    gated_runtime = rs.build_runtime_provenance(runtime_kwargs, gate_ms)
    same_gate_runtime = rs.build_runtime_provenance(runtime_kwargs, str(gate_ms))
    check("run_strategy: open execution records trade_start_ms=null",
          "trade_start_ms" in open_runtime and open_runtime["trade_start_ms"] is None)
    check("run_strategy: gated execution records trade_start_ms as int",
          gated_runtime["trade_start_ms"] == gate_ms
          and isinstance(gated_runtime["trade_start_ms"], int))
    check("run_strategy: equivalent gate values normalize identically",
          same_gate_runtime == gated_runtime)
    check("run_strategy: runtime helper is pure",
          "trade_start_ms" not in runtime_kwargs)

    open_fp = rs.build_fingerprint({"runtime": open_runtime})
    gated_fp = rs.build_fingerprint({"runtime": gated_runtime})
    same_gate_fp = rs.build_fingerprint({"runtime": same_gate_runtime})
    check("run_strategy: gated/open digests differ",
          gated_fp["digest"] != open_fp["digest"])
    check("run_strategy: same effective gate has deterministic digest",
          gated_fp["digest"] == same_gate_fp["digest"])

    # Timestamped FX provenance is pinned to the exact effective C-ABI values,
    # not only to a provider-file hash, point count, or endpoints.  Changing an
    # interior rate must therefore change both the value digest and the final
    # run fingerprint even when every summary field is otherwise identical.
    fx_timestamps = [1_000, 2_000, 3_000]
    fx_kwargs_a = dict(runtime_kwargs,
                       account_currency_fx_series=(fx_timestamps,
                                                    [1.0, 1.001, 1.002]),
                       account_currency_fx_source_sha256="a" * 64)
    fx_kwargs_b = dict(runtime_kwargs,
                       account_currency_fx_series=(fx_timestamps,
                                                    [1.0, 1.0015, 1.002]),
                       account_currency_fx_source_sha256="a" * 64)
    fx_runtime_a = rs.build_runtime_provenance(fx_kwargs_a, gate_ms)
    fx_runtime_b = rs.build_runtime_provenance(fx_kwargs_b, gate_ms)
    fx_meta_a = fx_runtime_a["account_currency_fx_series"]
    expected_fx_hasher = hashlib.sha256()
    expected_fx_hasher.update(
        b"pineforge:account-currency-fx:i64-f64-le:v1\0")
    fx_record = struct.Struct("<qd")
    for point in zip(fx_timestamps, [1.0, 1.001, 1.002]):
        expected_fx_hasher.update(fx_record.pack(*point))
    check("run_strategy: FX provenance names canonical packed-value contract",
          fx_meta_a["canonicalization"]
          == "pf-account-currency-fx-i64-f64-le-v1")
    check("run_strategy: FX effective-value digest is independently reproducible",
          fx_meta_a["effective_values_sha256"]
          == expected_fx_hasher.hexdigest())
    check("run_strategy: FX raw provider-file hash remains provenance",
          fx_meta_a["source_file_sha256"] == "a" * 64)
    check("run_strategy: FX series records its scalar pre-first fallback",
          fx_meta_a["fallback_account_per_quote"] == 1.0
          and fx_runtime_a["account_currency_fx_scalar"] == 1.0)
    check("run_strategy: changed interior FX rate changes effective digest",
          fx_meta_a["effective_values_sha256"]
          != fx_runtime_b["account_currency_fx_series"]["effective_values_sha256"])
    check("run_strategy: changed interior FX rate changes run fingerprint",
          rs.build_fingerprint({"runtime": fx_runtime_a})["digest"]
          != rs.build_fingerprint({"runtime": fx_runtime_b})["digest"])

    fx_kwargs_fallback = dict(
        fx_kwargs_a, syminfo_metadata={"account_currency_fx": 1.25})
    fx_runtime_fallback = rs.build_runtime_provenance(
        fx_kwargs_fallback, gate_ms)
    check("run_strategy: changed pre-first FX fallback changes provenance",
          fx_runtime_fallback["account_currency_fx_scalar"] == 1.25
          and fx_runtime_fallback["account_currency_fx_series"]
                  ["fallback_account_per_quote"] == 1.25
          and rs.build_fingerprint({"runtime": fx_runtime_fallback})["digest"]
              != rs.build_fingerprint({"runtime": fx_runtime_a})["digest"])

    # Provider adapter contract: a UTC daily close becomes effective at the
    # next day's 00:00Z boundary, key order is normalized, and duplicate dates
    # fail closed before json.loads can silently overwrite either value.
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        valid_fx = td_path / "fx.json"
        valid_fx.write_text(
            '{"2026-01-02": 1.002, "2026-01-01": 1.001}',
            encoding="utf-8")
        (loaded_timestamps, loaded_rates), loaded_source_sha = \
            rs._load_account_currency_fx_daily_closes(valid_fx)
        check("run_strategy: daily FX closes sort and shift to D+1 00:00Z",
              loaded_timestamps == [1767312000000, 1767398400000]
              and loaded_rates == [1.001, 1.002])
        check("run_strategy: daily FX loader records raw file SHA-256",
              loaded_source_sha
              == hashlib.sha256(valid_fx.read_bytes()).hexdigest())

        duplicate_fx = td_path / "fx-duplicate.json"
        duplicate_fx.write_text(
            '{"2026-01-01": 1.0, "2026-01-01": 1.1}',
            encoding="utf-8")
        duplicate_rejected = False
        try:
            rs._load_account_currency_fx_daily_closes(duplicate_fx)
        except ValueError:
            duplicate_rejected = True
        check("run_strategy: duplicate daily FX date fails closed",
              duplicate_rejected)

    source_tree = ast.parse(RUN_STRATEGY.read_text(encoding="utf-8"))
    main_fn = next((node for node in source_tree.body
                    if isinstance(node, ast.FunctionDef) and node.name == "main"), None)
    main_uses_helper = main_fn is not None and any(
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "build_runtime_provenance"
        for node in ast.walk(main_fn)
    )
    check("run_strategy: main fingerprint path uses runtime helper",
          main_uses_helper)

    # --- canonical source-feed identity ---------------------------------
    # Seven test classes pin the feed contract independently of any strategy
    # binary. Rows use the exact BarC field order: O,H,L,C,V,timestamp.
    rows = [
        (100.0, 101.0, 99.0, 100.5, 10.0, 1_000),
        (100.5, 103.0, 100.0, 102.0, 20.0, 2_000),
        (102.0, 104.0, 101.0, 103.5, 30.0, 3_000),
        (103.5, 105.0, 102.5, 104.0, 40.0, 4_000),
    ]

    def expected_feed_sha(feed_rows) -> str:
        h = hashlib.sha256()
        h.update(b"pineforge:ohlcv:barc-le:v1\0")
        record = struct.Struct("<5dq")
        for row in feed_rows:
            h.update(record.pack(*row))
        return h.hexdigest()

    def loader_fails(loader, path: Path) -> bool:
        try:
            loader(path)
        except (OSError, KeyError, ValueError, IndexError):
            return True
        return False

    def provenance_rejects(module, digest) -> bool:
        try:
            module.build_provenance({}, None, False, {}, {}, {},
                                    source_feed_sha256=digest)
        except ValueError:
            return True
        return False

    with tempfile.TemporaryDirectory(prefix="pf-fingerprint-feed-") as td:
        tmp = Path(td)
        feed_a = tmp / "a.csv"
        feed_b = tmp / "b.csv"
        feed_changed = tmp / "changed.csv"
        feed_reordered = tmp / "reordered.csv"
        malformed = tmp / "malformed.csv"

        # A: canonical bytes are domain-prefixed <5dq> records.
        feed_a.write_text(
            "timestamp,open,high,low,close,volume\n" + "".join(
                f"{ts},{o:g},{h:g},{lo:g},{c:g},{v:g}\n"
                for o, h, lo, c, v, ts in rows),
            encoding="utf-8")
        expected = expected_feed_sha(rows)
        rs_bars, rs_n, rs_sha = rs._load_bars(feed_a)
        check("feed A: local hash pins domain-prefixed <5dq> contract",
              rs_n == len(rows) and rs_sha == expected
              and len(rs_bars) == len(rows))

        # B: Docker and local loaders must agree on one source tape.
        rj_bars, rj_n, rj_sha = rj.load_bars(feed_a)
        check("feed B: local/docker loaders agree",
              rj_n == rs_n and len(rj_bars) == len(rows)
              and rj_sha == rs_sha)

        # C: path, header order, newline style and numeric spelling are not
        # semantic; the parsed values stay identical.
        feed_b.write_bytes((
            "open,high,low,close,volume,timestamp\r\n" + "".join(
                f"{o:.1f},{h:.1f},{lo:.1f},{c:.1f},{v:.1f},{ts}\r\n"
                for o, h, lo, c, v, ts in rows)
        ).encode("utf-8"))
        _, _, rs_sha_b = rs._load_bars(feed_b)
        _, _, rj_sha_b = rj.load_bars(feed_b)
        check("feed C: equivalent parsed values ignore path/text formatting",
              rs_sha_b == expected and rj_sha_b == expected)

        # D: content and original row order are both identity-bearing, even
        # when count plus first/last timestamps are unchanged.
        changed_rows = list(rows)
        changed_rows[1] = (*changed_rows[1][:3], 102.25,
                           *changed_rows[1][4:])
        feed_changed.write_text(
            "timestamp,open,high,low,close,volume\n" + "".join(
                f"{ts},{o},{h},{lo},{c},{v}\n"
                for o, h, lo, c, v, ts in changed_rows),
            encoding="utf-8")
        reordered_rows = [rows[0], rows[2], rows[1], rows[3]]
        feed_reordered.write_text(
            "timestamp,open,high,low,close,volume\n" + "".join(
                f"{ts},{o},{h},{lo},{c},{v}\n"
                for o, h, lo, c, v, ts in reordered_rows),
            encoding="utf-8")
        _, _, changed_sha = rs._load_bars(feed_changed)
        _, _, reordered_sha = rs._load_bars(feed_reordered)
        check("feed D: middle value and row-order changes alter identity",
              changed_sha != expected and reordered_sha != expected
              and changed_sha != reordered_sha)

        # E: source identity is computed before local start/end slicing.
        _, filtered_n, filtered_sha = rs._load_bars(
            feed_a, ohlcv_start_ms=3_000, ohlcv_end_ms=4_000)
        check("feed E: validation slicing preserves source identity",
              filtered_n == 2 and filtered_sha == expected)

        # F: feed identity is mandatory provenance and changes the digest.
        prov_rs = rs.build_provenance(
            {}, None, False, {}, {}, {}, source_feed_sha256=expected)
        prov_rj = rj.build_provenance(
            {}, None, False, {}, {}, {}, source_feed_sha256=expected)
        changed_prov = rs.build_provenance(
            {}, None, False, {}, {}, {}, source_feed_sha256=changed_sha)
        check("feed F: provenance records feed and digest discriminates tapes",
              prov_rs == prov_rj
              and prov_rs["feed"] == {
                  "canonicalization": "pf-ohlcv-barc-le-v1",
                  "source_values_sha256": expected,
              }
              and rs.build_fingerprint(prov_rs)["digest"]
              != rs.build_fingerprint(changed_prov)["digest"])
        check("feed F: null/invalid identities fail closed",
              provenance_rejects(rs, None)
              and provenance_rejects(rj, None)
              and provenance_rejects(rs, expected.upper()))

        # G: absent/malformed sources fail in both loaders before provenance.
        malformed.write_text(
            "timestamp,open,high,low,close\n1000,1,2,0,1.5\n",
            encoding="utf-8")
        missing = tmp / "missing.csv"
        check("feed G: missing/malformed feeds produce no identity",
              loader_fails(rs._load_bars, missing)
              and loader_fails(rj.load_bars, missing)
              and loader_fails(rs._load_bars, malformed)
              and loader_fails(rj.load_bars, malformed))

    # Pin production call-site plumbing so a future refactor cannot compute a
    # digest and then silently omit it from one harness's provenance.
    docker_tree = ast.parse(RUN_JSON.read_text(encoding="utf-8"))
    callsite_has_feed = lambda tree: any(
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "build_provenance"
        and any(kw.arg == "source_feed_sha256" for kw in node.keywords)
        for node in ast.walk(tree))
    check("feed call sites: local and docker require source identity",
          callsite_has_feed(source_tree) and callsite_has_feed(docker_tree))

    # --- the two copies must agree byte-for-byte on the fixture ----------
    check("copies agree: parse_strategy_params",
          rj.parse_strategy_params(FIXTURE_CPP) == rs.parse_strategy_params(FIXTURE_CPP))
    check("copies agree: parse_inputs",
          rj.parse_inputs(FIXTURE_CPP) == rs.parse_inputs(FIXTURE_CPP))
    check("copies agree: effective_strategy",
          rj.effective_strategy(FIXTURE_CPP, {"pyramiding": "5"})
          == rs.effective_strategy(FIXTURE_CPP, {"pyramiding": "5"}))
    check("copies agree: effective_inputs",
          rj.effective_inputs(FIXTURE_CPP, {"Fast EMA": "8"})
          == rs.effective_inputs(FIXTURE_CPP, {"Fast EMA": "8"}))
    check("copies agree: helper source is byte-identical",
          _helper_block(RUN_JSON) == _helper_block(RUN_STRATEGY))

    # --- JS RFC 8785/JCS direct-writer boundary (Node optional) ----------
    # Deterministic core vectors above are always mandatory. Node is optional:
    # when present, this bridge checks a JS direct JCS writer (not plain
    # JSON.stringify) against the Python encoder; when absent, print a skip
    # note and do NOT count the bridge as a pass (pure-Python CI still
    # enforces all core vectors). Token bytes remain authoritative.
    node = shutil.which("node")
    bridge_payload = {
        "strategy": {
            "initial_capital": 12345.0,
            "commission_value": 0.0,
            "default_qty_value": 1.5,
            "safe_int": 9007199254740991,
        },
        "nested": {"arr": [0.0, -0.0, 1e-6, 1e20, 1e21, -1e-7]},
        "flags": {"on": True, "off": False, "empty": None},
        # Array-index keys + __proto__: object-rebuild+stringify is wrong.
        "10": 10,
        "2": 2,
        "__proto__": "own",
        "indexish": {"2": 20, "10": 10, "__proto__": {"x": 1}},
        "arr_special": [{"10": 1, "2": 2, "__proto__": 3}],
        # Unicode / JCS coverage beyond ASCII+numbers:
        "中文键": "中文值",
        "emoji": "😀",
        "sep": "\u2028\u2029",
        "ctrl": "\n\t\x00",
        "\U00010000": "supplementary",
        "\uE000": "pua",
    }
    py_canon = rs._canonical_fingerprint_json(bridge_payload)
    py_digest = "sha256:" + hashlib.sha256(py_canon.encode("utf-8")).hexdigest()
    # Pin expected UTF-16 key order on the bridge payload itself (always).
    check("bridge payload: UTF-16 key order (U+10000 before U+E000)",
          '"\U00010000":"supplementary","\uE000":"pua"' in py_canon
          and py_canon.index('"\U00010000"') < py_canon.index('"\uE000"'))
    check("bridge payload: Chinese/emoji/U+2028 raw (no ensure_ascii)",
          "中文键" in py_canon and "😀" in py_canon
          and "\u2028" in py_canon and "\\u4e2d" not in py_canon
          and "\\ud83d" not in py_canon and "\\u2028" not in py_canon)
    # Lexical UTF-16: "10" < "2" < "__proto__" — all retained as own keys.
    check("bridge payload: special keys 10/2/__proto__ lexical order",
          '"10":10' in py_canon
          and '"2":2' in py_canon
          and '"__proto__":"own"' in py_canon
          and py_canon.index('"10":10') < py_canon.index('"2":2')
          < py_canon.index('"__proto__":"own"')
          and '"indexish":{"10":10,"2":20,"__proto__":{"x":1}}' in py_canon
          and '"arr_special":[{"10":1,"2":2,"__proto__":3}]' in py_canon)
    if node is None:
        # Not a check() pass — optional bridge simply not exercised.
        print("  SKIP JS bridge: node not available "
              "(optional; core deterministic vectors remain mandatory)")
    else:
        # Emit non-canonical Python JSON that a Worker would re-canonicalize
        # with a JCS direct writer (not plain JSON.stringify). ensure_ascii
        # mangles non-ASCII; sort_keys uses code-point order so the JS path
        # must re-sort U+10000 vs U+E000 into UTF-16 order.
        py_legacy = json.dumps(bridge_payload, sort_keys=True,
                               separators=(",", ":"), ensure_ascii=True)
        check("bridge: legacy Python JSON uses ensure_ascii + code-point sort",
              "\\u4e2d" in py_legacy
              and py_legacy.index("\\ue000") < py_legacy.index("\\ud800\\udc00"))
        special_payload = {
            "10": 10,
            "2": 2,
            "__proto__": "own",
            "nested": [{"10": 1, "2": 2, "__proto__": 3}],
        }
        py_special = rs._canonical_fingerprint_json(special_payload)
        script = r"""
const fs = require('fs');
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
// RFC 8785/JCS direct canonical writer over JS-parsed values.
// Do NOT rebuild a plain object (array-index key reorder; __proto__ drop).
function canonical(v) {
  if (Array.isArray(v)) {
    let s = '[';
    for (let i = 0; i < v.length; i++) {
      if (i) s += ',';
      s += canonical(v[i]);
    }
    return s + ']';
  }
  if (v !== null && typeof v === 'object') {
    const keys = Object.keys(v).sort();
    let s = '{';
    for (let i = 0; i < keys.length; i++) {
      const k = keys[i];
      if (i) s += ',';
      s += JSON.stringify(k) + ':' + canonical(v[k]);
    }
    return s + '}';
  }
  return JSON.stringify(v);
}
const fromLegacy = canonical(JSON.parse(input.legacy));
const fromCanon = canonical(JSON.parse(input.canonical));
const specialKeys = canonical(JSON.parse(input.special));
// Direct Unicode/key vectors independent of the payload shape.
const direct = {
  chinese: canonical({"中文键": "中文值"}),
  emoji: canonical("😀"),
  seps: canonical("\u2028\u2029"),
  keyOrder: canonical(
    Object.fromEntries(
      ["\u{10000}", "\uE000"].sort().map((k, i) => [k, i + 1])
    )
  ),
};
process.stdout.write(JSON.stringify({fromLegacy, fromCanon, specialKeys, direct}));
"""
        proc = subprocess.run(
            [node, "-e", script],
            input=json.dumps({
                "legacy": py_legacy,
                "canonical": py_canon,
                "special": json.dumps(
                    special_payload, separators=(",", ":"),
                    ensure_ascii=False),
            }, ensure_ascii=False),
            text=True, capture_output=True, check=False)
        if proc.returncode != 0:
            check("JS bridge: node execution", False)
            print(proc.stderr)
        else:
            out = json.loads(proc.stdout)
            check("JS bridge: direct JCS writer matches Python canonical",
                  out["fromLegacy"] == py_canon
                  and out["fromCanon"] == py_canon)
            # Digest recomputed from the JS-emitted text must match.
            js_digest = "sha256:" + hashlib.sha256(
                out["fromLegacy"].encode("utf-8")).hexdigest()
            check("JS bridge: digest stable across JS boundary",
                  js_digest == py_digest
                  and rs.build_fingerprint(bridge_payload)["digest"]
                  == py_digest)
            check("JS bridge: special keys 10/2/__proto__ match Python",
                  out["specialKeys"] == py_special
                  and out["specialKeys"]
                  == ('{"10":10,"2":2,"__proto__":"own",'
                      '"nested":[{"10":1,"2":2,"__proto__":3}]}')
                  and '"__proto__"' in out["specialKeys"]
                  and out["specialKeys"].index('"10"')
                  < out["specialKeys"].index('"2"')
                  < out["specialKeys"].index('"__proto__"'))
            check("JS bridge: direct Chinese object matches",
                  out["direct"]["chinese"]
                  == rs._canonical_fingerprint_json({"中文键": "中文值"}))
            check("JS bridge: direct emoji matches",
                  out["direct"]["emoji"]
                  == rs._canonical_fingerprint_json("😀"))
            check("JS bridge: direct U+2028/U+2029 matches",
                  out["direct"]["seps"]
                  == rs._canonical_fingerprint_json("\u2028\u2029"))
            check("JS bridge: direct UTF-16 key order matches",
                  out["direct"]["keyOrder"]
                  == rs._canonical_fingerprint_json(
                      {"\U00010000": 1, "\uE000": 2}))
            # Fail-closed companions (out-of-domain / invalid I-JSON).
            check("JS bridge companion: NaN rejected in Python contract",
                  all(_raises_value_error(rs, x)
                      for x in (math.nan, math.inf, -math.inf)))
            check("JS bridge companion: out-of-domain ints rejected",
                  all(_raises_value_error(rs, x) for x in UNSAFE_INTEGERS))
            check("JS bridge companion: unpaired surrogates rejected",
                  all(_raises_value_error(rs, x) for x in UNPAIRED_SURROGATES))

    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


def _raises_value_error(module, value) -> bool:
    try:
        module._canonical_fingerprint_json(value)
    except ValueError:
        return True
    return False


if __name__ == "__main__":
    sys.exit(main())
