"""Deterministic differential fuzz of the bulk equity path of docker/run_json.py.

The bulk path accepts a strict subset of canonical E lines at C speed and hands
every other byte to the unchanged strict reader. Its safety claim is: it never
accepts what the strict reader rejects, never writes other bytes, and a refusal
keeps its type, text, typed code and priority. run_json_bulk_equity_test.py pins
35 named corruptions; this file attacks the same claim with generated inputs.

Every generator is a fixed-seed random.Random over integers (no clock, no
entropy, no hash randomisation), so the id of a failing row names a case that
reproduces on any host. A failure message prints family, seed, variant, window,
point count and the exact mutation list. The strict side is always the existing
report entry (run_json_bulk_equity_test._run: verify, build, stream) with
run_json._BULK_E_ENABLED switched off; the bulk side is the same call on the
same file with it switched on and a small _BULK_E_WINDOW, so line cuts land at
every offset. Two independent anchors keep both sides from being wrong alike:
the generator's own rows (expected equity_curve bytes and endpoints) and, for
the single-mutation rows, the exact pf_sink refusal text each mutation must
produce.

A second family builds one valid sink of about 76 KiB (read-only, shared by its
rows) and puts a single fault on, beside and far from the real 64 KiB direct-I/O
page boundary of the sink reader. Every line index is read off the file's bytes;
each fault must be refused by the strict reader with its named check at exactly
that line (or be accepted, for integer -0), at the production window and at 97.

The controls at the end break the bulk acceptance on purpose and require the
same comparison to fail (the repr proof, the integer -0 grammar, the time range
test, the reader's E count and its line number), and require the engagement
check (json.loads count) to fail for a bulk path that never accepts anything.

No row is skipped or disabled. Run on the lab's disk-backed workspace like the
other run_json rows; docker/run_json_*_test.py is found by preflight.
"""
import hashlib
import inspect
import json
import math
from pathlib import Path
import random
import re
import struct
import sys
from typing import NamedTuple

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import test_run_json_sink as ordinary
import test_run_json_stream_fastpath as oracle
from run_json_bulk_equity_test import I53, _run
from run_json_trade_scan_test import _event, _point, _sink, _trade, _trailer_start


START = 1_700_000_000_000
SEEDS = tuple(range(40))
SEED_IDS = [f"seed={seed}" for seed in SEEDS]
FAMILIES = ("fresh", "stale")
SINGLE_SEEDS = (0, 1)
CHAIN_VARIANTS = 2
TAMPER_VARIANTS = 3
CONTROL_SEEDS = tuple(range(6))
_SALT = {"valid": 1, "single": 2, "chain": 3, "tamper": 4, "long": 5, "control": 6,
         "page": 7, "pick": 8}


def _rng(family, seed, variant=0):
    """A fixed-seed generator: integers only, so no clock, entropy or hash seed."""
    return random.Random((_SALT[family] << 48) | (seed << 16) | variant)


# ---------------------------------------------------------------- valid sinks

_EXTREMES = (
    1.7976931348623157e308, -1.7976931348623157e308, 2.2250738585072014e-308,
    -2.2250738585072014e-308, 5e-324, -5e-324, 1e-300, -1e-300, 1e300, 123456789.125,
    1e16, -1e16, 1e22, 1e23, 1e-05, 0.0001, 0.1 + 0.2, 9007199254740991.0,
    9007199254740992.0,
)
_TIME_EDGES = (0, 1, -1, 2 ** 31, -2 ** 31, 2 ** 32 - 1, 2 ** 40, I53, -I53, I53 - 1,
               1 - I53, 10 ** 15, -10 ** 15)
_TEXTS = ("", "plain", 'quote"\\漢', '\n["E",0,1.0,0.0]\n', '["E",1,2.0,3.0]', "\\u0045",
          '{"trailer":"pf-report-sink"}', "x" * 97, "\u2028\u2029 tab\t", "\U0001f680")


def _double_from_bits(bits):
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def _float(rng):
    """A finite double in one of many repr shapes (fixed, exponent, subnormal, -0.0)."""
    shape = rng.randrange(11)
    if shape == 0:
        return round(rng.uniform(-2e5, 2e5), rng.randint(0, 8))
    if shape == 1:
        return float(rng.randint(-10 ** 9, 10 ** 9))
    if shape == 2:
        return -0.0
    if shape == 3:
        return 0.0
    if shape == 4:
        return rng.choice((-1.0, 1.0)) * 10.0 ** rng.randint(-30, 30)
    if shape == 5:
        return rng.uniform(-1.0, 1.0) * 10.0 ** rng.randint(-12, 12)
    if shape == 6:
        bits = rng.getrandbits(52) or 1  # exponent field 0: a subnormal
        return rng.choice((-1.0, 1.0)) * _double_from_bits(bits)
    if shape == 7:
        return rng.choice(_EXTREMES)
    if shape == 8:
        big = rng.choice((2 ** 53 - 1, 2 ** 53, 2 ** 53 + 2, 10 ** 15, 10 ** 16 - 1,
                          10 ** 16, 10 ** 17))
        return float(big) * rng.choice((-1.0, 1.0))
    if shape == 9:
        while True:
            value = _double_from_bits(rng.getrandbits(64))
            if math.isfinite(value):
                return value
    return rng.uniform(-1e6, 1e6)


def _random_time(rng):
    pick = rng.randrange(3)
    if pick == 0:
        return rng.randint(-I53, I53)
    if pick == 1:
        return rng.choice(_TIME_EDGES)
    return rng.randint(-10 ** 6, 10 ** 6)


def _filler(size):
    return ("pq\"\\" * (size // 4 + 1))[:size]


class _Spec(NamedTuple):
    rows: list
    points: list
    others: int
    trades: int
    trade_at_start: bool


def _spec(rng, count=None, large_event=False):
    """Random E rows with T/V/K/O rows interleaved; trades are numbered in file order."""
    count = rng.randint(20, 300) if count is None else count
    mode = rng.choice(("regular", "regular", "random", "mixed"))
    step = rng.choice((1, 60_000, 86_400_000))
    rows = []
    for index in range(count):
        regular = mode == "regular" or (mode == "mixed" and rng.random() < 0.5)
        stamp = START + index * step if regular else _random_time(rng)
        rows.append(["E", stamp, _float(rng), _float(rng)])
    points = list(rows)
    extras = ["T", "V"] + [rng.choice(("T", "T", "V", "K", "O"))
                           for _ in range(rng.randint(0, 6))]
    if large_event:
        extras.append("big")
    for tag in extras:
        rows.insert(rng.randint(0, len(rows)), [tag])
    number, at_start = 0, False
    for position, row in enumerate(rows):
        if len(row) != 1:
            continue
        tag = row[0]
        if tag == "T":
            number += 1
            bar = rng.choice((0, 1, 1, 2, 5))
            at_start = at_start or bar <= 0
            rows[position] = _trade(number, pnl=round(rng.uniform(-500.0, 500.0), 2),
                                    entry_bar_index=bar, exit_comment=rng.choice(_TEXTS))
        elif tag == "V":
            rows[position] = _event(rng.choice(_TEXTS))
        elif tag == "K":
            rows[position] = ["K", 50.0]
        elif tag == "O":
            rows[position] = ["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1, -0.0, 4294967295.0]
        else:
            rows[position] = _event(_filler(rng.randint(30_000, 70_000)))
    return _Spec(rows, points, len(rows) - len(points), number, at_start)


def _window(rng):
    return rng.randint(97, 700) if rng.random() < 0.8 else 64 * 1024


def _original(tmp_path, spec):
    """The bytes of the valid sink, written by the fixture's own writer."""
    path = tmp_path / "original.sink"
    ordinary.write_sink(path, spec.rows, outputs=True)
    return path.read_bytes()


def _curve(points):
    return [{"time_ms": row[1], "equity": row[2], "open_profit": row[3]} for row in points]


def _expected_curve(points):
    text = json.dumps(_curve(points), separators=(",", ":"), allow_nan=False)
    return b'"equity_curve":' + text.encode("ascii")


def _expected_endpoints(spec):
    curve = _curve(spec.points)
    return (len(curve), ordinary.encoded(curve[0]), ordinary.encoded(curve[-1]),
            spec.trade_at_start)


# ---------------------------------------------------------------- comparison

def _first_difference(left, right):
    for index, (a, b) in enumerate(zip(left, right)):
        if a != b:
            return index
    return min(len(left), len(right))


def _assert_same(strict, actual, context):
    """The one comparison every family and every control uses.

    Both ran the same file through the same entry; the only difference is the
    bulk switch. Same refusal (type, text, code, code_args), same bytes written
    before it, same endpoints."""
    assert actual[1] == strict[1], (
        f"{context}: the refusal differs\n  strict: {strict[1]!r}\n  bulk:   {actual[1]!r}")
    assert actual[0] == strict[0], (
        f"{context}: the report bytes differ at offset "
        f"{_first_difference(actual[0], strict[0])} ({len(actual[0])} vs {len(strict[0])} bytes)")
    assert actual[3] == strict[3], (
        f"{context}: the endpoints differ\n  strict: {strict[3]!r}\n  bulk:   {actual[3]!r}")


def _assert_oracle(spec, outcome, context):
    """The generator's own rows, not the strict path, say what a valid report holds."""
    assert outcome[1] is None, f"{context}: a generated valid sink was refused: {outcome[1]!r}"
    assert _expected_curve(spec.points) in outcome[0], (
        f"{context}: the equity_curve array is not the generated points")
    assert outcome[3] == _expected_endpoints(spec), f"{context}: wrong endpoints"


def _assert_engaged(spec, strict, actual, context):
    """json.loads stays O(1) in the E count: the bulk path really accepted its lines.

    Without this a bulk path that always falls back would make every
    comparison trivially equal."""
    points, bulk, base = len(spec.points), actual[2], strict[2]
    assert bulk["verify"] <= 2 + spec.others, (
        f"{context}: verify decoded {bulk['verify']} documents for {points} points "
        f"(header, trailer and {spec.others} other records allow {2 + spec.others})")
    assert bulk["copy"] <= 2 + spec.trades, (
        f"{context}: the copy decoded {bulk['copy']} documents for {points} points "
        f"(two headers and {spec.trades} trades allow {2 + spec.trades})")
    assert bulk["decode_e"] == bulk["dump_e"] == 0, f"{context}: per-point decode or dump ran"
    assert base["verify"] >= points and base["copy"] >= points, (
        f"{context}: the strict path no longer decodes per point, so the counter is blind")
    assert base["decode_e"] >= points and base["dump_e"] >= points, context


# ------------------------------------------------------------ expectations

F_JSON = "invalid UTF-8 or JSON"
F_WS = "whitespace or control byte outside a JSON string"
F_ABSENT = "record tag is absent from the header"
F_COUNT = "record field count differs from its descriptor"
F_TAGTYPE = "record must be a nonempty array with a string tag"
F_BOM = "UTF-8 BOM is forbidden"
F_BLANK = "blank line"
F_LF = "line does not end with LF"
F_LONG = "line exceeds 1 MiB including LF"
F_MISSING = "missing header or trailer"
F_BOUND = "integer exceeds the exact JSON integer bound"
F_I64 = "invalid i64 column"
F_F64 = "f64 column is not the Python repr of its double"
F_FINITE = "non-finite JSON number"
F_CONST = "non-JSON numeric constant"
F_NULL = "null in a non-optional column"
F_TRAILER_COUNT = "trailer count mismatch for"
F_DIGEST = "trailer SHA-256 mismatch"
F_AFTER = "bytes after the trailer"

ACCEPT = ("accept",)
EITHER = ("either",)
_LATER = "later"


def REFUSE(*fragments):
    return ("refuse",) + fragments


def LATER(field):
    return (_LATER, field)


def _check_expectation(expect, outcome, stale, context):
    """Pin which strict check fires. A stale digest turns every valid body into F_DIGEST."""
    failure = outcome[1]
    if stale and expect[0] in ("accept", _LATER):
        expect = REFUSE(F_DIGEST)
    elif stale and expect[0] == "either":
        expect = REFUSE()
    if expect[0] == "accept":
        assert failure is None, f"{context}: the strict path refused a valid sink: {failure!r}"
    elif expect[0] == _LATER:
        assert failure is not None and failure[2:] == (
            "report_result_invalid", {"field": expect[1], "reason": "nonfinite"}), (
            f"{context}: expected the deferred nonfinite refusal, got {failure!r}")
    elif expect[0] == "refuse":
        assert failure is not None, f"{context}: the strict path accepted a corrupted sink"
        assert len(expect) == 1 or any(part in failure[1] for part in expect[1:]), (
            f"{context}: refused for another reason: {failure[1]!r}; expected {expect[1:]!r}")


# ---------------------------------------------------------------- mutations

_E_PREFIX = b'["E",'
_E_SHAPE = re.compile(rb'\["E",([^,\[\]"]*),([^,\[\]"]*),([^,\[\]"]*)\]')
_TAGGED = re.compile(rb'\["([ETOVK])",')
_E_LINE_AT = re.compile(rb'\n(\["E",[^\n]*)(?=\n)')
_NUMBER_BYTES = b"0123456789.-+eE"
_FLIP_POOL = b"0123456789.-+eE xX,\t\r\x00\xff:[]{}\"\\"
_HOSTILE = b"\"'\\{}[],:;#xX\x00\x01\x07\x08\x0b\x0c\x0e\x1b\x1f\x7f\x80\xbf\xc3\xe9\xff"
_OTHER_TAGS = ("T", "E", "O", "V", "K", "X", "H", "e", "EE", "")
_EXTRA_FIELDS = (b"0", b"1.0", b"null", b"true", b'"x"', b"[]")
_HOSTILE_TAGS = (b'"\\u0065"', b'"E\\u0020"', b'"E\\n"', b'"E\\\\"', b'"\\"E"',
                 b'"\\u0045\\u0045"', b'"\\ud83d\\ude80"', b'"\\ud800"', b'"E\\u0000"')
TOKENS = ("-0", "00", "01", "1E5", "1.0e0", "0.10", "1e400", "-1e400",
          "9007199254740992", "9007199254740993", "null", "NaN", "Infinity", "1.5", "1")
_TOKEN_SLOTS = {"1.5": (0,), "1": (1, 2)}
# (token, is the integer slot) -> what the strict reader does with it. Traced by
# hand against pf_sink.py: _parse, _int_token, _float_token, _constant, _field.
_TOKEN_EXPECT = {
    ("-0", True): ACCEPT, ("-0", False): REFUSE(F_F64),
    ("00", True): REFUSE(F_JSON), ("00", False): REFUSE(F_JSON),
    ("01", True): REFUSE(F_JSON), ("01", False): REFUSE(F_JSON),
    ("1E5", True): REFUSE(F_I64), ("1E5", False): REFUSE(F_F64),
    ("1.0e0", True): REFUSE(F_I64), ("1.0e0", False): REFUSE(F_F64),
    ("0.10", True): REFUSE(F_I64), ("0.10", False): REFUSE(F_F64),
    ("1e400", True): REFUSE(F_FINITE), ("1e400", False): REFUSE(F_FINITE),
    ("-1e400", True): REFUSE(F_FINITE), ("-1e400", False): REFUSE(F_FINITE),
    ("9007199254740992", True): REFUSE(F_BOUND), ("9007199254740992", False): REFUSE(F_BOUND),
    ("9007199254740993", True): REFUSE(F_BOUND), ("9007199254740993", False): REFUSE(F_BOUND),
    ("null", True): REFUSE(F_NULL), ("null", False): _LATER,
    ("NaN", True): REFUSE(F_CONST), ("NaN", False): REFUSE(F_CONST),
    ("Infinity", True): REFUSE(F_CONST), ("Infinity", False): REFUSE(F_CONST),
    ("1.5", True): REFUSE(F_I64),
    ("1", False): REFUSE(F_F64),
}


class _Image:
    """A sink file taken apart into editable lines; render() puts it back together."""

    def __init__(self, raw):
        cut = _trailer_start(raw)
        self.lines = raw[:cut].split(b"\n")[:-1]  # header first, no terminators
        self.trailer = json.loads(raw[cut:])
        self.trailer_edits, self.file_edits = [], []

    def render(self, fresh):
        """The mutated file and, when fresh, the receipt digest of its new body."""
        body = b"".join(line + b"\n" for line in self.lines)
        trailer = dict(self.trailer, counts=dict(self.trailer["counts"]))
        receipt = None
        if fresh:
            receipt = trailer["sha256"] = hashlib.sha256(body).hexdigest()
        for edit in self.trailer_edits:
            edit(trailer)
        raw = body + ordinary.encoded(trailer)
        for edit in self.file_edits:
            raw = edit(raw)
        return raw, receipt


KINDS = {}


def _kind(name):
    def register(function):
        KINDS[name] = function
        return function
    return register


def _e_lines(image):
    return [i for i in range(1, len(image.lines)) if image.lines[i].startswith(_E_PREFIX)]


def _canonical_lines(image):
    return [i for i in range(1, len(image.lines)) if _E_SHAPE.fullmatch(image.lines[i])]


def _replace_kind(token):
    def apply(rng, image, slot=None):
        indices = _canonical_lines(image)
        if not indices:
            return None
        index = rng.choice(indices)
        if slot is None:
            slot = rng.choice(_TOKEN_SLOTS.get(token, (0, 1, 2)))
        line = image.lines[index]
        start, end = _E_SHAPE.fullmatch(line).span(slot + 1)
        image.lines[index] = line[:start] + token.encode("ascii") + line[end:]
        expect = _TOKEN_EXPECT[(token, slot == 0)]
        if expect == _LATER:
            expect = LATER("equity_curve.equity" if slot == 1 else "equity_curve.open_profit")
        return f"line {index} slot {slot} -> {token}", expect
    return apply


for _token in TOKENS:
    KINDS["replace:" + _token] = _replace_kind(_token)


def _respellings(text):
    """Other spellings of the double that repr() prints as `text`; never `text` itself."""
    sign = "-" if text.startswith("-") else ""
    body = text[len(sign):]
    mantissa, _, exponent = body.partition("e")
    spelled = []
    if exponent:
        marker = exponent[0] if exponent[0] in "+-" else ""
        digits = exponent[len(marker):]
        spelled += [
            f"{sign}{mantissa}E{exponent}",
            f"{sign}{mantissa}{'0' if '.' in mantissa else '.0'}e{exponent}",
            f"{sign}{mantissa}e{marker}0{digits}",
        ]
        if marker == "+":
            spelled.append(f"{sign}{mantissa}e{digits}")
        if len(digits) > 1 and digits.startswith("0"):
            spelled.append(f"{sign}{mantissa}e{marker}{digits.lstrip('0')}")
    else:
        spelled += [f"{sign}{body}0", f"{sign}{body}e0", f"{sign}{body}E0", f"{sign}{body}e+0"]
    return [item for item in spelled if item != text and float(item) == float(text)]


@_kind("respell-float")
def _respell_float(rng, image):
    indices = _canonical_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    slot = rng.choice((1, 2))
    line = image.lines[index]
    start, end = _E_SHAPE.fullmatch(line).span(slot + 1)
    token = line[start:end].decode("ascii")
    try:
        value = float(token)
    except ValueError:
        return None
    if not math.isfinite(value):
        return None
    options = [item for item in _respellings(repr(value)) if item != token]
    if not options:
        return None
    spelling = rng.choice(options)
    image.lines[index] = line[:start] + spelling.encode("ascii") + line[end:]
    return f"line {index} slot {slot}: {token} -> {spelling}", REFUSE(F_F64)


@_kind("flip-number-byte")
def _flip_number_byte(rng, image):
    indices = _e_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    line = image.lines[index]
    spots = [i for i in range(5, len(line) - 1) if line[i] in _NUMBER_BYTES]
    if not spots:
        return None
    spot = rng.choice(spots)
    new = line[spot]
    while new == line[spot]:
        new = rng.choice(_FLIP_POOL)
    image.lines[index] = line[:spot] + bytes((new,)) + line[spot + 1:]
    return f"line {index} byte {spot}: {line[spot:spot + 1]!r} -> {bytes((new,))!r}", EITHER


@_kind("insert-byte")
def _insert_byte(rng, image):
    indices = _e_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    line = image.lines[index]
    offset = rng.randint(0, len(line))
    byte = bytes((rng.randrange(256),))
    image.lines[index] = line[:offset] + byte + line[offset:]
    return f"line {index} offset {offset}: insert {byte!r}", EITHER


@_kind("delete-byte")
def _delete_byte(rng, image):
    indices = _e_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    line = image.lines[index]
    offset = rng.randrange(len(line))
    image.lines[index] = line[:offset] + line[offset + 1:]
    return f"line {index} offset {offset}: delete {line[offset:offset + 1]!r}", EITHER


def _insert_into_e_line(rng, image, choices):
    indices = _e_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    line = image.lines[index]
    offset = rng.randint(0, len(line))
    piece = choices[rng.randrange(len(choices))]
    image.lines[index] = line[:offset] + piece + line[offset:]
    return f"line {index} offset {offset}: insert {piece!r}"


@_kind("insert-hostile")
def _insert_hostile(rng, image):
    # None of these bytes can sit anywhere in an E line and leave a valid record.
    note = _insert_into_e_line(rng, image, [bytes((b,)) for b in _HOSTILE])
    return None if note is None else (
        note, REFUSE(F_JSON, F_WS, F_ABSENT, F_COUNT, F_TAGTYPE))


@_kind("insert-whitespace")
def _insert_whitespace(rng, image):
    note = _insert_into_e_line(rng, image, [b" ", b"\t", b"\r"])
    return None if note is None else (note, REFUSE(F_WS, F_JSON, F_ABSENT))


@_kind("insert-bom")
def _insert_bom(rng, image):
    note = _insert_into_e_line(rng, image, [b"\xef\xbb\xbf"])
    return None if note is None else (note, REFUSE(F_BOM, F_JSON, F_ABSENT))


@_kind("blank-line")
def _blank_line(rng, image):
    index = rng.randint(1, len(image.lines))
    image.lines.insert(index, b"")
    return f"blank line before record line {index}", REFUSE(F_BLANK)


@_kind("tag-replace")
def _tag_replace(rng, image):
    indices = [i for i in range(1, len(image.lines)) if _TAGGED.match(image.lines[i])]
    if not indices:
        return None
    index = rng.choice(indices)
    line = image.lines[index]
    old = _TAGGED.match(line).group(1).decode("ascii")
    new = rng.choice([tag for tag in _OTHER_TAGS if tag != old])
    image.lines[index] = b'["' + new.encode("ascii") + b'",' + line[5:]
    expect = REFUSE(F_COUNT) if new in ("E", "T", "O", "V", "K") else REFUSE(F_ABSENT)
    return f"line {index}: tag {old} -> {new!r}", expect


@_kind("field-delete")
def _field_delete(rng, image):
    indices = _canonical_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    fields = [b'"E"', *_E_SHAPE.fullmatch(image.lines[index]).groups()]
    victim = rng.randrange(4)
    del fields[victim]
    image.lines[index] = b"[" + b",".join(fields) + b"]"
    return f"line {index}: field {victim} removed", REFUSE(F_TAGTYPE if victim == 0 else F_COUNT)


@_kind("field-add")
def _field_add(rng, image):
    indices = _canonical_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    fields = [b'"E"', *_E_SHAPE.fullmatch(image.lines[index]).groups()]
    extra = rng.choice(_EXTRA_FIELDS)
    position = rng.randrange(5)
    fields.insert(position, extra)
    image.lines[index] = b"[" + b",".join(fields) + b"]"
    if position == 0:
        expect = REFUSE(F_ABSENT) if extra == b'"x"' else REFUSE(F_TAGTYPE)
    else:
        expect = REFUSE(F_COUNT)
    return f"line {index}: {extra!r} added at field {position}", expect


@_kind("merge-lines")
def _merge_lines(rng, image):
    indices = [i for i in _e_lines(image) if i + 1 < len(image.lines)]
    if not indices:
        return None
    index = rng.choice(indices)
    image.lines[index:index + 2] = [image.lines[index] + image.lines[index + 1]]
    return f"lines {index} and {index + 1} joined", REFUSE(F_JSON)


@_kind("swap-e-lines")
def _swap_e_lines(rng, image):
    indices = _e_lines(image)
    if len(indices) < 2:
        return None
    for _ in range(20):
        a, b = rng.choice(indices), rng.choice(indices)
        if a != b and image.lines[a] != image.lines[b]:
            image.lines[a], image.lines[b] = image.lines[b], image.lines[a]
            return f"lines {a} and {b} swapped", ACCEPT
    return None


@_kind("duplicate-e-line")
def _duplicate_e_line(rng, image):
    indices = _e_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    where = rng.randint(1, len(image.lines))
    image.lines.insert(where, image.lines[index])
    return f"line {index} copied before line {where}", REFUSE(F_TRAILER_COUNT)


@_kind("delete-e-line")
def _delete_e_line(rng, image):
    indices = _e_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    del image.lines[index]
    return f"line {index} removed", REFUSE(F_TRAILER_COUNT)


@_kind("tag-escape")
def _tag_escape(rng, image):
    indices = _e_lines(image)
    if not indices:
        return None
    chosen = sorted({rng.choice(indices) for _ in range(rng.randint(1, 3))})
    for index in chosen:
        image.lines[index] = b'["\\u0045"' + image.lines[index][4:]
    return f"lines {chosen} spell the tag \\u0045", ACCEPT


@_kind("tag-escape-hostile")
def _tag_escape_hostile(rng, image):
    indices = _e_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    tag = rng.choice(_HOSTILE_TAGS)
    image.lines[index] = image.lines[index][:1] + tag + image.lines[index][4:]
    return f"line {index}: tag spelled {tag!r}", REFUSE(F_ABSENT, F_JSON)


@_kind("tag-escape-split")
def _tag_escape_split(rng, image):
    indices = _e_lines(image)
    if not indices:
        return None
    index = rng.choice(indices)
    head = b'["\\u0045"'
    line = head + image.lines[index][4:]
    cut = rng.randint(2, len(head))
    image.lines[index] = line[:cut] + b"\n" + line[cut:]
    return f"line {index}: escaped tag split after byte {cut}", REFUSE(F_JSON)


def _damage(rng, line):
    """One flipped, inserted or deleted byte that is sure to change the line."""
    operation = rng.choice(("flip", "insert", "delete"))
    if operation == "insert":
        offset = rng.randint(0, len(line))
        byte = bytes((rng.randrange(256),))
        return line[:offset] + byte + line[offset:], f"insert {byte!r} at {offset}"
    offset = rng.randrange(len(line))
    if operation == "delete":
        return line[:offset] + line[offset + 1:], f"delete byte {offset}"
    new = line[offset]
    while new == line[offset]:
        new = rng.randrange(256)
    return line[:offset] + bytes((new,)) + line[offset + 1:], f"set byte {offset} to {new:#04x}"


@_kind("header-byte")
def _header_byte(rng, image):
    image.lines[0], note = _damage(rng, image.lines[0])
    return f"header: {note}", EITHER


@_kind("other-line-byte")
def _other_line_byte(rng, image):
    indices = [i for i in range(1, len(image.lines))
               if image.lines[i] and not image.lines[i].startswith(_E_PREFIX)]
    if not indices:
        return None
    index = rng.choice(indices)
    image.lines[index], note = _damage(rng, image.lines[index])
    return f"line {index}: {note}", EITHER


@_kind("trailer-count")
def _trailer_count(rng, image):
    tag = rng.choice(sorted(image.trailer["counts"]))
    delta = rng.choice((-1, 1))

    def edit(trailer):
        trailer["counts"][tag] += delta

    image.trailer_edits.append(edit)
    return f"trailer count {tag} {delta:+d}", REFUSE(F_TRAILER_COUNT)


@_kind("trailer-digest-digit")
def _trailer_digest_digit(rng, image):
    position = rng.randrange(64)

    def edit(trailer):
        digest = trailer["sha256"]
        replacement = "0" if digest[position] != "0" else "1"
        trailer["sha256"] = digest[:position] + replacement + digest[position + 1:]

    image.trailer_edits.append(edit)
    return f"trailer digest digit {position}", REFUSE(F_DIGEST)


@_kind("truncate")
def _truncate(rng, image):
    fraction = rng.random()

    def edit(raw):
        return raw[:min(int(fraction * len(raw)), len(raw) - 1)]

    image.file_edits.append(edit)
    return f"file cut at fraction {fraction:.6f}", REFUSE(F_LF, F_MISSING)


@_kind("append-after-trailer")
def _append_after_trailer(rng, image):
    choice = rng.randrange(5)
    if choice == 0:
        extra = b"x"
    elif choice == 1:
        extra = b"\n"
    elif choice == 2:
        extra = b'["E",0,1.0,0.0]\n'
    elif choice == 3:
        extra = ordinary.encoded(image.trailer)
    else:
        extra = bytes(rng.randrange(256) for _ in range(rng.randint(1, 40)))
    image.file_edits.append(lambda raw: raw + extra)
    return f"{len(extra)} bytes after the trailer", REFUSE(F_AFTER)


NAMES = tuple(KINDS)
SINGLE_PARAMS = [(name, seed) for name in NAMES for seed in SINGLE_SEEDS]
SINGLE_IDS = [f"{name}-seed={seed}" for name, seed in SINGLE_PARAMS]


class _Mutated(NamedTuple):
    raw: bytes
    receipt: object
    notes: tuple
    expect: tuple
    changed: bool


class _Case(NamedTuple):
    spec: _Spec
    raw: bytes
    mutated: _Mutated
    window: int


def _finish(image, raw, fresh, notes, expect):
    mutated, receipt = image.render(fresh)
    return _Mutated(mutated, receipt, tuple(notes), expect, mutated != raw)


def _mutate_plan(raw, rng, plan, fresh):
    image = _Image(raw)
    notes, expect = [], EITHER
    for name, forced in plan:
        result = KINDS[name](rng, image, **forced)
        assert result is not None, f"{name} does not apply to this sink"
        notes.append(f"{name}: {result[0]}")
        expect = result[1]
    return _finish(image, raw, fresh, notes, expect)


def _mutate_random(raw, rng, fresh):
    image = _Image(raw)
    notes, expect = [], EITHER
    wanted = rng.randint(1, 3)
    for _ in range(60):
        if len(notes) == wanted:
            break
        name = rng.choice(NAMES)
        result = KINDS[name](rng, image)
        if result is not None:
            notes.append(f"{name}: {result[0]}")
            expect = result[1]
    return _finish(image, raw, fresh, notes, expect)


def _chain_case(tmp_path, family, seed, variant):
    rng = _rng("chain", seed, variant)
    spec = _spec(rng)
    raw = _original(tmp_path, spec)
    window = _window(rng)
    return _Case(spec, raw, _mutate_random(raw, rng, family == "fresh"), window)


def _context(label, case):
    return (f"{label} window={case.window} points={len(case.spec.points)} "
            f"mutations={list(case.mutated.notes)}")


def _execute(tmp_path, monkeypatch, case, patches=()):
    """Strict then bulk, on the very same mutated file, through the existing report entry."""
    with _sink(tmp_path, case.spec.rows, verify=False) as (sink, library, report):
        Path(sink.path).write_bytes(case.mutated.raw)
        if case.mutated.receipt is not None:
            library.sha = case.mutated.receipt  # as _rewrite does: the engine "recomputed"
        strict = _run(sink, library, report, monkeypatch, False)
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_BULK_E_WINDOW", case.window, raising=True)
            for target, name, value in patches:
                patch.setattr(target, name, value, raising=True)
            actual = _run(sink, library, report, patch, True)
    return strict, actual


# --------------------------------------------------------------------- tests

@pytest.mark.parametrize("seed", SEEDS, ids=SEED_IDS)
def test_generated_valid_sinks_are_byte_identical_and_really_bulk(tmp_path, monkeypatch, seed):
    rng = _rng("valid", seed)
    spec = _spec(rng, count=rng.randint(513, 1100) if seed % 8 == 0 else None,
                 large_event=seed % 8 == 4)
    windows = (rng.randint(97, 160), rng.randint(161, 700), rng.randint(701, 5000), 64 * 1024)
    with _sink(tmp_path, spec.rows, verify=False) as (sink, library, report):
        reference = oracle._Destination()
        strict = _run(sink, library, report, monkeypatch, False, destination=reference)
        _assert_oracle(spec, strict, f"valid seed={seed} strict")
        for window in windows:
            context = (f"valid seed={seed} window={window} points={len(spec.points)} "
                       f"others={spec.others}")
            destination = oracle._Destination()
            with monkeypatch.context() as patch:
                patch.setattr(run_json, "_BULK_E_WINDOW", window, raising=True)
                actual = _run(sink, library, report, patch, True, destination=destination)
            _assert_same(strict, actual, context)
            assert destination.calls == reference.calls, f"{context}: write boundaries differ"
            _assert_oracle(spec, actual, context)
            _assert_engaged(spec, strict, actual, context)


@pytest.mark.parametrize("seed", SEEDS, ids=SEED_IDS)
def test_an_untouched_image_renders_the_original_bytes(tmp_path, seed):
    """The harness itself: split, join, re-encode and re-digest must be lossless.

    Otherwise every "fresh" row would corrupt the digest on its own."""
    raw = _original(tmp_path, _spec(_rng("single", 1000 + seed)))
    image = _Image(raw)
    stale, receipt = image.render(False)
    fresh, digest = image.render(True)
    assert stale == raw and receipt is None
    assert fresh == raw and digest == json.loads(raw[_trailer_start(raw):])["sha256"]


@pytest.mark.parametrize("family", FAMILIES)
@pytest.mark.parametrize("name,seed", SINGLE_PARAMS, ids=SINGLE_IDS)
def test_each_mutation_kind_is_refused_or_accepted_like_the_strict_path(
        tmp_path, monkeypatch, family, name, seed):
    rng = _rng("single", NAMES.index(name) * 8 + seed)
    spec = _spec(rng, count=rng.randint(20, 120))
    raw = _original(tmp_path, spec)
    window = _window(rng)
    mutated = _mutate_plan(raw, rng, [(name, {})], family == "fresh")
    case = _Case(spec, raw, mutated, window)
    context = _context(f"single {family} {name} seed={seed}", case)
    assert mutated.changed, context
    strict, actual = _execute(tmp_path, monkeypatch, case)
    _assert_same(strict, actual, context)
    _check_expectation(mutated.expect, strict, family == "stale", context)
    if name == "swap-e-lines" and family == "fresh":
        # Reordered canonical lines are still a canonical sink: no fallback may occur.
        _assert_engaged(spec, strict, actual, context)


@pytest.mark.parametrize("family", FAMILIES)
@pytest.mark.parametrize("seed", SEEDS, ids=SEED_IDS)
def test_random_mutation_chains_end_like_the_strict_path(tmp_path, monkeypatch, family, seed):
    for variant in range(CHAIN_VARIANTS):
        case = _chain_case(tmp_path, family, seed, variant)
        context = _context(f"chain {family} seed={seed} variant={variant}", case)
        strict, actual = _execute(tmp_path, monkeypatch, case)
        _assert_same(strict, actual, context)
        if family == "stale" and case.mutated.changed:
            # A stale digest guarantees a refusal: either an earlier format error
            # or the trailer SHA-256, whichever the sequential reader meets first.
            assert strict[1] is not None, f"{context}: the strict path accepted a stale digest"


@pytest.mark.parametrize("family", FAMILIES)
def test_chain_generator_is_not_tame_for_the_reference_reader(tmp_path, family):
    """Independent of run_json: pf_sink.SinkReader refuses most generated chains."""
    refused = changed = 0
    for seed in SEEDS:
        for variant in range(CHAIN_VARIANTS):
            case = _chain_case(tmp_path, family, seed, variant)
            if not case.mutated.changed:
                continue
            changed += 1
            path = tmp_path / "mutated.sink"
            path.write_bytes(case.mutated.raw)
            try:
                with pf_sink.SinkReader(path) as reader:
                    for _ in reader:
                        pass
            except pf_sink.SinkFormatError:
                refused += 1
    assert changed >= len(SEEDS)
    if family == "stale":
        assert refused == changed
    else:
        assert refused >= 0.7 * changed, (refused, changed)


_TAMPER_KINDS = ("byte", "digit", "swap", "join", "split", "trailer", "header")
_TAMPER_POOL = _FLIP_POOL + b"\n{"


def _other_byte(rng, old):
    new = old
    while new == old:
        new = rng.choice(_TAMPER_POOL)
    return new


def _tamper(rng, raw):
    """Same-length damage to a verified sink: (bytes, first changed offset, note)."""
    cut = _trailer_start(raw)
    head = raw.index(b"\n") + 1
    spans = [(m.start(1), m.end(1)) for m in _E_LINE_AT.finditer(raw, 0, cut)]
    data = bytearray(raw)
    kind = rng.choice(_TAMPER_KINDS)
    if kind == "swap":
        by_length = {}
        for start, end in spans:
            by_length.setdefault(end - start, []).append((start, end))
        pairs = []
        for length in sorted(by_length):
            group = by_length[length]
            for a in range(len(group) - 1):
                (s1, e1), (s2, e2) = group[a], group[a + 1]
                if raw[s1:e1] != raw[s2:e2]:
                    pairs.append((group[a], group[a + 1]))
        if pairs:
            (s1, e1), (s2, e2) = rng.choice(pairs)
            data[s1:e1], data[s2:e2] = raw[s2:e2], raw[s1:e1]
            return bytes(data), s1, f"E lines at {s1} and {s2} swapped"
        kind = "byte"
    if kind == "digit":
        spots = []
        for start, end in spans:
            spots.extend(i for i in range(start + 5, end - 1) if 48 <= raw[i] <= 57)
        if spots:
            offset = rng.choice(spots)
            data[offset] = 48 + (raw[offset] - 48 + rng.randint(1, 9)) % 10
            return bytes(data), offset, f"digit at {offset} changed"
        kind = "byte"
    if kind == "join":
        offset = rng.choice([i for i in range(head - 1, cut) if raw[i] == 10])
        data[offset] = 0x20
        return bytes(data), offset, f"LF at {offset} replaced by a space"
    if kind == "split":
        commas = []
        for start, end in spans:
            commas.extend(i for i in range(start, end) if raw[i] == 0x2C)
        offset = rng.choice(commas)
        data[offset] = 0x0A
        return bytes(data), offset, f"comma at {offset} replaced by LF"
    if kind == "trailer":
        offset = rng.randrange(cut, len(raw))
    elif kind == "header":
        offset = rng.randrange(0, head - 1)
    else:
        offset = rng.randrange(head, cut)
    data[offset] = _other_byte(rng, raw[offset])
    return bytes(data), offset, f"{kind}: byte {offset} set to {data[offset]:#04x}"


@pytest.mark.parametrize("seed", SEEDS, ids=SEED_IDS)
def test_tampering_after_verification_ends_like_the_strict_path(tmp_path, monkeypatch, seed):
    """The copy pass sees different bytes than verify did, with size and inode kept."""
    rng = _rng("tamper", seed)
    spec = _spec(rng)
    with _sink(tmp_path, spec.rows) as (sink, library, report):
        path = Path(sink.path)
        raw = path.read_bytes()
        cut = _trailer_start(raw)
        for variant in range(TAMPER_VARIANTS):
            tampered, offset, note = _tamper(_rng("tamper", seed, variant + 1), raw)
            window = _window(_rng("tamper", seed, 100 + variant))
            context = (f"tamper seed={seed} variant={variant} window={window} "
                       f"points={len(spec.points)}: {note}")
            assert len(tampered) == len(raw) and tampered != raw, context
            path.write_bytes(tampered)
            strict = _run(sink, library, report, monkeypatch, False,
                          copy_only=True, equity_only=True)
            with monkeypatch.context() as patch:
                patch.setattr(run_json, "_BULK_E_WINDOW", window, raising=True)
                actual = _run(sink, library, report, patch, True,
                              copy_only=True, equity_only=True)
            _assert_same(strict, actual, context)
            if offset < cut:
                # The digest covers every body byte, so the strict pass must fail,
                # at a format error or at the end-of-scan comparison.
                assert strict[1] is not None, f"{context}: the strict path accepted a tampered body"
    # (the sink and its temporary file are closed by _sink)


@pytest.mark.parametrize("where", ("first", "middle", "last"))
@pytest.mark.parametrize("delta", (-1, 0, 1, 2), ids=lambda delta: f"limit{delta:+d}")
def test_a_line_around_one_mebibyte_is_refused_like_the_strict_path(
        tmp_path, monkeypatch, delta, where):
    # A line of MAX-1 or MAX bytes is within the limit and fails on its integer;
    # MAX+1 is the exact-limit refusal; MAX+2 is cut by readline and lacks its LF.
    fragments = {-1: F_BOUND, 0: F_BOUND, 1: F_LONG, 2: F_LF}
    rng = _rng("long", delta + 8, ("first", "middle", "last").index(where))
    spec = _spec(rng, count=40)
    raw = _original(tmp_path, spec)
    window = _window(rng)
    image = _Image(raw)
    indices = _e_lines(image)
    index = {"first": indices[0], "middle": indices[len(indices) // 2],
             "last": indices[-1]}[where]
    line = image.lines[index]
    filler = pf_sink.MAX_LINE_BYTES + delta - len(line) - 1
    assert filler > 0
    image.lines[index] = line[:5] + b"1" * filler + line[5:]
    mutated = _finish(image, raw, True, [f"line {index} grown to {pf_sink.MAX_LINE_BYTES + delta} bytes"],
                      REFUSE(fragments[delta]))
    case = _Case(spec, raw, mutated, window)
    context = _context(f"long {where} limit{delta:+d}", case)
    strict, actual = _execute(tmp_path, monkeypatch, case)
    _assert_same(strict, actual, context)
    _check_expectation(mutated.expect, strict, False, context)


def test_the_catalogue_names_every_listed_corruption():
    required = ["replace:" + token for token in TOKENS] + [
        "flip-number-byte", "insert-byte", "delete-byte", "swap-e-lines", "duplicate-e-line",
        "trailer-count", "trailer-digest-digit", "truncate", "append-after-trailer",
        "insert-whitespace", "insert-bom", "blank-line", "tag-replace", "field-delete",
        "field-add", "tag-escape-split", "tag-escape",
    ]
    missing = [name for name in required if name not in KINDS]
    assert not missing, missing


# ------------------------------------------- faults at the direct-I/O page boundary

# The sink reader pulls the file through 64 KiB direct-I/O pages (run_json's
# _BoundedJSONFile.window) aligned at file offsets 0, 65536, ... and the bulk window
# carries the unfinished tail of a line from one page into the next. One valid sink
# of about 76 KiB holds exactly one such boundary; every row below damages one line
# of a copy of it. No line index is a constant of this file: _page_positions reads
# them all off the bytes.
PAGE = 64 * 1024
PAGE_POINTS = 1350
PAGE_WINDOWS = (64 * 1024, 97)
PAGE_FAULTS = ("equity-0.10", "profit-0.10", "time-minus-zero", "space", "point-deleted",
               "tag-X")
# Inside the bulk grammar: only the repr proof refuses these two, and it refuses a whole batch.
PAGE_PROOF_FAULTS = ("equity-0.10", "profit-0.10")
PAGE_LABELS = (
    ("boundary", "before", "after", "first-1", "first-2", "first-3", "last-3", "last-2",
     "last-1", "after-non-e") + tuple(f"random-{rank}" for rank in range(12)))


def _page_cases():
    """The boundary line and its two neighbours take every fault; each other position one."""
    cases = []
    for rank, label in enumerate(PAGE_LABELS):
        if rank < 3:
            cases.extend((label, fault) for fault in PAGE_FAULTS)
        else:
            cases.append((label, PAGE_FAULTS[rank % len(PAGE_FAULTS)]))
    return cases


PAGE_CASES = _page_cases()
PAGE_IDS = [f"{label}-{fault}" for label, fault in PAGE_CASES]


def _paged_spec(pad):
    """1350 E points of full-width doubles, five other records and `pad` bytes of event text.

    Full-width reprs make an E line about 57 bytes, so the sink is about 76 KiB and the
    64 KiB boundary falls near E line 1100, between the other records at E lines 880
    and 1240. `pad` moves every later byte so the fixture can step the boundary off a
    line start."""
    rng = _rng("page", 0)
    points = [["E", START + index * 60_000, rng.uniform(1000.0, 99999.0),
               rng.uniform(-999.0, 999.0)] for index in range(PAGE_POINTS)]
    after = {120: _trade(1, entry_bar_index=1, pnl=12.5), 360: _event("p" * pad),
             640: ["K", 50.0],
             880: ["O", 0, ordinary.bars.T0, ordinary.bars.T0 + 1, -0.0, 4294967295.0],
             1240: _trade(2, entry_bar_index=2, pnl=-3.25)}
    rows = []
    for index, point in enumerate(points):
        rows.append(point)
        if index in after:
            rows.append(after[index])
    return _Spec(rows, points, len(after), 2, False)


def _line_starts(lines):
    starts, offset = [], 0
    for line in lines:
        starts.append(offset)
        offset += len(line) + 1
    return starts


def _boundary_line(lines, starts):
    """The E line holding both the last byte of page 0 and the first byte of page 1."""
    for index in range(1, len(lines) - 1):
        if starts[index] < PAGE < starts[index] + len(lines[index]) + 1:
            around = (lines[index - 1], lines[index], lines[index + 1])
            return index if all(line.startswith(_E_PREFIX) for line in around) else None
    return None


def _page_positions(lines, boundary):
    """Label -> index into `lines` for every fault position, and all E line indices."""
    e_lines = [i for i in range(1, len(lines)) if lines[i].startswith(_E_PREFIX)]
    named = {"boundary": boundary, "before": boundary - 1, "after": boundary + 1,
             "first-1": e_lines[0], "first-2": e_lines[1], "first-3": e_lines[2],
             "last-3": e_lines[-3], "last-2": e_lines[-2], "last-1": e_lines[-1]}
    # An E line that resumes a run after a non-E record which itself followed an E line.
    named["after-non-e"] = next(
        i for i in range(2, len(lines))
        if lines[i].startswith(_E_PREFIX) and not lines[i - 1].startswith(_E_PREFIX)
        and lines[i - 2].startswith(_E_PREFIX))
    rng = _rng("pick", 0)
    pool = [i for i in e_lines if i not in named.values()]
    chosen = []
    while len(chosen) < 12:
        pick = pool[rng.randrange(len(pool))]
        if pick not in chosen:
            chosen.append(pick)
    named.update((f"random-{rank}", index) for rank, index in enumerate(chosen))
    return named, e_lines


class _Paged(NamedTuple):
    spec: _Spec
    raw: bytes
    lines: list      # header first, no terminators (the _Image split)
    starts: list     # file offset of each line
    boundary: int    # index of the E line that straddles offset PAGE
    positions: dict  # label -> line index
    e_lines: list    # line index of every E line, in file order


@pytest.fixture(scope="module")
def paged(tmp_path_factory):
    """The one valid sink every page row reads and never writes."""
    directory = tmp_path_factory.mktemp("paged-sink")
    for pad in range(64):
        spec = _paged_spec(pad)
        raw = _original(directory, spec)
        lines = _Image(raw).lines
        starts = _line_starts(lines)
        boundary = _boundary_line(lines, starts)
        if boundary is not None:
            positions, e_lines = _page_positions(lines, boundary)
            return _Paged(spec, raw, lines, starts, boundary, positions, e_lines)
    raise AssertionError("no event padding put the 64 KiB boundary inside an E line")


def _apply_page_fault(fault, line, page_offset):
    """The damaged line, what the strict reader must do with it, and a note.

    `page_offset` is where the page boundary falls inside this line, or None. Every
    expectation is traced against pf_sink: _parse, _int_token, _field, RecordParser."""
    shape = _E_SHAPE.fullmatch(line)
    assert shape is not None, line
    if fault in ("equity-0.10", "profit-0.10"):
        start, end = shape.span(2 if fault.startswith("equity") else 3)
        # 0.10 is a double but not its repr: the grammar takes it, only the proof refuses.
        return (line[:start] + b"0.10" + line[end:], REFUSE(F_F64),
                f"{line[start:end]!r} -> 0.10")
    if fault == "time-minus-zero":
        start, end = shape.span(1)
        # An integer -0 is a valid i64 for the strict reader; the bulk grammar refuses it.
        return line[:start] + b"-0" + line[end:], ACCEPT, f"{line[start:end]!r} -> -0"
    if fault == "space":
        # On the boundary line the space becomes the first byte of the second page.
        at = len(line) - 1 if page_offset is None else min(max(page_offset, 5), len(line) - 1)
        return line[:at] + b" " + line[at:], REFUSE(F_WS), f"space inserted at byte {at}"
    if fault == "point-deleted":
        start, end = shape.span(2)
        token = line[start:end]
        point = token.index(b".")
        digits = token[:point] + token[point + 1:]
        # Without its point the equity is a JSON integer: past 2**53 - 1 the integer
        # bound refuses first, otherwise the f64 column refuses an integer.
        expect = REFUSE(F_BOUND if int(digits) > I53 else F_F64)
        return (line[:start + point] + line[start + point + 1:], expect,
                f"{token!r} lost its point")
    assert fault == "tag-X", fault
    return b'["X"' + line[4:], REFUSE(F_ABSENT), "tag E -> X"


def _execute_windows(tmp_path, monkeypatch, case, windows):
    """Strict once, then the bulk path at each window, all on the very same mutated file."""
    outcomes = {}
    with _sink(tmp_path, case.spec.rows, verify=False) as (sink, library, report):
        Path(sink.path).write_bytes(case.mutated.raw)
        if case.mutated.receipt is not None:
            library.sha = case.mutated.receipt
        strict = _run(sink, library, report, monkeypatch, False)
        for window in windows:
            with monkeypatch.context() as patch:
                patch.setattr(run_json, "_BULK_E_WINDOW", window, raising=True)
                outcomes[window] = _run(sink, library, report, patch, True)
    return strict, outcomes


def _assert_prefix_in_bulk(strict, actual, accepted, context):
    """About one json.loads fewer per canonical E line that precedes the fault."""
    saved = strict[2]["verify"] - actual[2]["verify"]
    assert saved >= accepted // 2, (
        f"{context}: verify decoded {actual[2]['verify']} documents against "
        f"{strict[2]['verify']} for the strict reader, which read {accepted} E lines the "
        f"bulk side should have accepted without a decode")


def test_the_page_sink_has_the_geometry_the_fault_rows_rely_on(tmp_path, paged):
    # Attacks the premise of every page row: that the fixture puts the 64 KiB boundary
    # strictly inside a canonical E line with E neighbours and other records on both
    # sides, and that production reads in the 64 KiB pages assumed here. It cannot pass
    # vacuously: each claim is read off the sink's bytes, and pf_sink's own reader
    # (independent of run_json) must accept those bytes and count what was built.
    assert PAGE == run_json._UncachedSinkFile.window == run_json._BULK_E_WINDOW
    assert 70 * 1024 <= len(paged.raw) <= 90 * 1024, len(paged.raw)
    lines, starts, index = paged.lines, paged.starts, paged.boundary
    end = starts[index] + len(lines[index]) + 1
    assert paged.raw[starts[index]:end] == lines[index] + b"\n"
    assert starts[index] < PAGE < end and _E_SHAPE.fullmatch(lines[index]), index
    assert all(lines[i].startswith(_E_PREFIX) for i in (index - 1, index + 1))
    others = [i for i in range(1, len(lines)) if not lines[i].startswith(_E_PREFIX)]
    assert len(others) == paged.spec.others and others[0] < index < others[-1], others
    assert set(paged.positions) == set(PAGE_LABELS)
    assert len(set(paged.positions.values())) == len(PAGE_LABELS), paged.positions
    for label, position in paged.positions.items():
        assert _E_SHAPE.fullmatch(lines[position]), (label, lines[position])
    path = tmp_path / "page.sink"
    path.write_bytes(paged.raw)
    with pf_sink.SinkReader(path) as reader:
        records = list(reader)
        assert reader.complete
        counts = reader.counts
    assert counts == {"E": PAGE_POINTS, "T": 2, "O": 1, "V": 1, "K": 1}, counts
    assert len(records) == PAGE_POINTS + paged.spec.others


@pytest.mark.parametrize("window", PAGE_WINDOWS, ids=("production-window", "window-97"))
def test_a_valid_sink_with_an_equity_line_across_the_page_boundary_is_bulk(
        tmp_path, monkeypatch, paged, window):
    # Attacks the hand-over of a canonical E line across the page boundary. The clean
    # sink must be accepted in bulk (json.loads stays O(1) in the E count, which a
    # per-line fallback would break) and a spy on _BulkEquityFile._fill must have seen
    # the carry: a refill entered at file position PAGE holding exactly the bytes of the
    # boundary line that lie before PAGE. Without that hit the fault rows below would
    # not exercise the carry at all.
    spec, refills = paged.spec, []
    fill = run_json._BulkEquityFile._fill

    def spy(self):
        refills.append((self.file.position, len(self.data) - self.cursor))
        return fill(self)

    context = f"page valid window={window}"
    with _sink(tmp_path, spec.rows, verify=False) as (sink, library, report):
        assert Path(sink.path).read_bytes() == paged.raw, context
        reference = oracle._Destination()
        strict = _run(sink, library, report, monkeypatch, False, destination=reference)
        _assert_oracle(spec, strict, context + " strict")
        destination = oracle._Destination()
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_BULK_E_WINDOW", window, raising=True)
            patch.setattr(run_json._BulkEquityFile, "_fill", spy, raising=True)
            actual = _run(sink, library, report, patch, True, destination=destination)
    _assert_same(strict, actual, context)
    assert destination.calls == reference.calls, f"{context}: write boundaries differ"
    _assert_oracle(spec, actual, context)
    _assert_engaged(spec, strict, actual, context)
    carried = PAGE - paged.starts[paged.boundary]
    assert (PAGE, carried) in refills, (
        f"{context}: no refill at offset {PAGE} carried the {carried} bytes of the boundary "
        f"line; the last refills were {refills[-3:]}")


@pytest.mark.parametrize("label,fault", PAGE_CASES, ids=PAGE_IDS)
def test_a_fault_around_the_direct_io_page_boundary_ends_like_the_strict_path(
        tmp_path, monkeypatch, paged, label, fault):
    # Attacks the hand-over from the bulk path to the strict reader when one line is
    # damaged on, beside or far from the 64 KiB page boundary, at the production window
    # and at 97. It cannot pass vacuously: the file differs from the valid sink in that
    # one line only, the strict reader must refuse at exactly that line with the named
    # check (or accept, for -0, and write the changed point), and the bulk side must have
    # skipped the per-line decode of the E lines before the fault, which a bulk path that
    # always falls back cannot do (not counted where the repr proof rejects the whole
    # batch of up to _BULK_E_BATCH lines that holds the fault, so that it is replayed
    # strictly).
    index = paged.positions[label]
    image = _Image(paged.raw)
    page_offset = PAGE - paged.starts[index] if index == paged.boundary else None
    image.lines[index], expect, note = _apply_page_fault(fault, image.lines[index], page_offset)
    mutated = _finish(image, paged.raw, True, [f"line {index}: {note}"], expect)
    context = f"page {label}-{fault} line={index} {note}"
    changed = [i for i in range(len(paged.lines)) if image.lines[i] != paged.lines[i]]
    assert changed == [index] and mutated.changed, context
    case = _Case(paged.spec, paged.raw, mutated, PAGE_WINDOWS[0])
    strict, outcomes = _execute_windows(tmp_path, monkeypatch, case, PAGE_WINDOWS)
    _check_expectation(expect, strict, False, context)
    accepted = paged.e_lines.index(index)  # canonical E lines before the fault
    if expect[0] == "refuse":
        assert f"line {index + 1}: " in strict[1][1], (
            f"{context}: the strict reader refused elsewhere: {strict[1][1]!r}")
    else:
        points = [list(point) for point in paged.spec.points]
        points[accepted][1] = 0
        assert _expected_curve(points) in strict[0], f"{context}: the changed point is not written"
    for window, actual in outcomes.items():
        where = f"{context} window={window}"
        _assert_same(strict, actual, where)
        if accepted >= 20 and (fault not in PAGE_PROOF_FAULTS or window < PAGE):
            _assert_prefix_in_bulk(strict, actual, accepted, where)


# ------------------------------------------------------------------ controls

def _bulk_equity_without_repr_proof(raw):
    """run_json._bulk_equity (docker/run_json.py) minus its repr-identity proof."""
    if not raw or run_json._BULK_E_RUN.fullmatch(raw) is None:
        return None
    times, equities, profits = zip(*run_json._BULK_E_LINE.findall(raw))
    timestamps = list(map(int, times))
    if min(timestamps) < -I53 or max(timestamps) > I53:
        return None
    numbers = list(map(float, map(bytes.decode, equities + profits)))
    if not all(map(math.isfinite, numbers)):
        return None
    count = len(timestamps)
    first = dict(time_ms=timestamps[0], equity=numbers[0], open_profit=numbers[count])
    last = dict(time_ms=timestamps[-1], equity=numbers[count - 1], open_profit=numbers[-1])
    return run_json._BulkEquity(raw, count, first, last)


def _detections(tmp_path, monkeypatch, plan_for, patches):
    """How many control seeds the shared comparison flags once `patches` break the bulk path.

    The honest bulk path must agree with the strict one on every control seed,
    so a detection is owed to the patch alone."""
    detected = 0
    for seed in CONTROL_SEEDS:
        rng = _rng("control", seed)
        spec = _spec(rng, count=rng.randint(20, 80))
        raw = _original(tmp_path, spec)
        window = _window(rng)
        mutated = _mutate_plan(raw, rng, plan_for(seed), True)
        case = _Case(spec, raw, mutated, window)
        context = _context(f"control seed={seed}", case)
        strict, honest = _execute(tmp_path, monkeypatch, case)
        _assert_same(strict, honest, context + " (unpatched)")
        _, broken = _execute(tmp_path, monkeypatch, case, patches)
        try:
            _assert_same(strict, broken, context + " (patched)")
        except AssertionError:
            detected += 1
    return detected


def test_control_a_bulk_that_skips_the_repr_identity_proof_is_detected(tmp_path, monkeypatch):
    canonical = b'["E",1,1.5,2.5]\n["E",2,0.25,-0.0]\n'
    spelled = b'["E",1,0.10,2.5]\n'
    assert vars(_bulk_equity_without_repr_proof(canonical)) == vars(run_json._bulk_equity(canonical))
    assert run_json._bulk_equity(spelled) is None
    assert _bulk_equity_without_repr_proof(spelled) is not None
    detected = _detections(
        tmp_path, monkeypatch,
        lambda seed: [("replace:0.10" if seed % 2 == 0 else "replace:1.0e0",
                       {"slot": 1 + (seed // 2) % 2})],
        # docker/run_json.py: _bulk_equity, the accept-only proof of one E window.
        [(run_json, "_bulk_equity", _bulk_equity_without_repr_proof)])
    assert detected >= 1, "the comparison cannot see a bulk path that accepts non-repr floats"


def test_control_b_grammar_that_accepts_integer_negative_zero_is_detected(tmp_path, monkeypatch):
    original = rb"(0|-?[1-9][0-9]{0,15})"
    broken_line = re.compile(run_json._BULK_E_LINE.pattern.replace(
        original, rb"(-?0|-?[1-9][0-9]{0,15})", 1))
    bound = rb"){1,%d}" % run_json._BULK_E_BATCH
    broken_run = re.compile(rb"(?:" + broken_line.pattern + bound)
    assert run_json._BULK_E_RUN.pattern == rb"(?:" + run_json._BULK_E_LINE.pattern + bound
    assert broken_line.pattern != run_json._BULK_E_LINE.pattern
    assert run_json._BULK_E_LINE.fullmatch(b'["E",-0,1.0,0.0]\n') is None
    assert broken_line.fullmatch(b'["E",-0,1.0,0.0]\n') is not None
    detected = _detections(
        tmp_path, monkeypatch, lambda seed: [("replace:-0", {"slot": 0})],
        # docker/run_json.py: _BULK_E_LINE and _BULK_E_RUN, the canonical E grammar.
        [(run_json, "_BULK_E_LINE", broken_line), (run_json, "_BULK_E_RUN", broken_run)])
    assert detected >= 1, "the comparison cannot see a grammar that accepts integer -0"


@pytest.mark.parametrize("never", ("switch-off", "proof-never-accepts"))
def test_control_c_a_bulk_path_that_never_engages_fails_the_engagement_check(
        tmp_path, monkeypatch, never):
    rng = _rng("control", 50)
    spec = _spec(rng, count=120)
    with _sink(tmp_path, spec.rows, verify=False) as (sink, library, report):
        strict = _run(sink, library, report, monkeypatch, False)
        if never == "switch-off":
            silent = _run(sink, library, report, monkeypatch, False)
        else:
            with monkeypatch.context() as patch:
                patch.setattr(run_json, "_bulk_equity", lambda raw: None, raising=True)
                silent = _run(sink, library, report, patch, True)
    # Exactly the false success to prevent: identical bytes and refusals ...
    _assert_same(strict, silent, f"control {never}")
    _assert_oracle(spec, silent, f"control {never}")
    # ... which only the json.loads count can tell from a real bulk run.
    with pytest.raises(AssertionError):
        _assert_engaged(spec, strict, silent, f"control {never}")


def _without(function, *statements):
    """`function` recompiled from its own source minus the consecutive lines `statements`.

    A mutant made of the production code itself cannot drift from run_json.py: the
    removal must match exactly once or the test fails. Its globals stay run_json's own,
    so every name another patch replaces is looked up live, as in the original."""
    lines = inspect.getsource(function).splitlines(keepends=True)
    stripped = [line.strip() for line in lines]
    wanted = list(statements)
    hits = [index for index in range(len(lines) - len(wanted) + 1)
            if stripped[index:index + len(wanted)] == wanted]
    assert len(hits) == 1, f"{function.__name__} does not hold {statements!r} exactly once"
    del lines[hits[0]:hits[0] + len(wanted)]
    scope = {}
    exec(compile("".join(lines), f"<{function.__name__} without {statements[0]}>", "exec"),
         function.__globals__, scope)
    return scope[function.__name__]


def test_control_d_a_bulk_proof_without_the_time_range_test_is_detected(tmp_path, monkeypatch):
    # Attacks the +-(2**53 - 1) test of run_json._bulk_equity: the E grammar takes up to
    # 16 digits, so only that test keeps a time of 2**53 or more out of an accepted batch.
    # The mutant is the real function minus those two lines. It cannot pass vacuously: in
    # each anchor sink one time is out of range, the strict reader refuses it by name
    # (F_BOUND), the unpatched bulk side must agree, and the mutant must end with no
    # refusal at all; the generated sinks then repeat that with a random line and window.
    mutant = _without(
        run_json._bulk_equity,
        "if min(timestamps) < -((1 << 53) - 1) or max(timestamps) > (1 << 53) - 1:",
        "return None")
    canonical = (b'["E",1,1.5,2.5]\n["E",-9007199254740991,0.25,-0.0]\n'
                 b'["E",9007199254740991,1.0,2.0]\n')
    assert vars(mutant(canonical)) == vars(run_json._bulk_equity(canonical))
    for line in (b'["E",9007199254740992,1.5,2.5]\n', b'["E",9007199254740993,1.5,2.5]\n',
                 b'["E",-9007199254740992,1.5,2.5]\n'):
        assert run_json._bulk_equity(line) is None
        assert mutant(line) is not None
    for bad in (I53 + 1, I53 + 2, -I53 - 1):
        rows = [_point(index) for index in range(6)]
        rows[3] = ["E", bad, 1.5, 2.5]
        for window in (97, 64 * 1024):
            context = f"control D time {bad} window={window}"
            with _sink(tmp_path, rows, verify=False) as (sink, library, report):
                strict = _run(sink, library, report, monkeypatch, False)
                with monkeypatch.context() as patch:
                    patch.setattr(run_json, "_BULK_E_WINDOW", window, raising=True)
                    honest = _run(sink, library, report, patch, True)
                    patch.setattr(run_json, "_bulk_equity", mutant, raising=True)
                    broken = _run(sink, library, report, patch, True)
            assert strict[1] is not None and F_BOUND in strict[1][1], f"{context}: {strict[1]!r}"
            _assert_same(strict, honest, context + " (unpatched)")
            assert broken[1] is None, f"{context}: the mutant still refused: {broken[1]!r}"
            with pytest.raises(AssertionError):
                _assert_same(strict, broken, context + " (patched)")
    detected = _detections(
        tmp_path, monkeypatch,
        lambda seed: [("replace:9007199254740992" if seed % 2 == 0
                       else "replace:9007199254740993", {"slot": 0})],
        # docker/run_json.py: _bulk_equity, the accept-only proof of one E window.
        [(run_json, "_bulk_equity", mutant)])
    assert detected >= 1, "the comparison cannot see a bulk proof that accepts times past 2**53 - 1"


@pytest.mark.parametrize("skipped", ("count", "line-number"))
def test_control_e_a_bulk_reader_that_skips_its_own_bookkeeping_is_detected(
        tmp_path, monkeypatch, skipped):
    # Attacks the two statements with which run_json._bulk_equity_records advances the
    # strict reader past an accepted batch: its E count (compared with the trailer) and
    # its line number (printed in every later refusal). Both sit inline in that generator,
    # so no smaller named function can be patched; the mutant is the generator recompiled
    # from its own source minus that one line. It cannot pass vacuously: each anchor is a
    # hand-countable 40-point sink (header is line 1, E lines 2-41, trailer line 42) on
    # which the strict reader ends at a known text, the unpatched bulk side must agree, and
    # the mutant must end somewhere else; the generated sinks then repeat that.
    statement = {"count": 'reader._counts["E"] += batch.count',
                 "line-number": "reader._line_number += batch.count"}[skipped]
    mutant = _without(run_json._bulk_equity_records, statement)
    assert mutant is not run_json._bulk_equity_records
    rows = [_point(index) for index in range(40)]
    spec = _Spec(rows, rows, 0, 0, False)
    raw = _original(tmp_path, spec)
    if skipped == "count":
        # A valid sink: only the skipped count can make the trailer disagree.
        mutated, receipt, expect, notes = raw, None, ACCEPT, ()
    else:
        # A trailer whose E count is one too high: strict and honest bulk both refuse it at
        # the trailer line, whose number only an unskipped line counter can print.
        image = _Image(raw)
        image.trailer_edits.append(
            lambda trailer: trailer["counts"].__setitem__("E", trailer["counts"]["E"] + 1))
        mutated, receipt = image.render(True)
        expect, notes = REFUSE(F_TRAILER_COUNT), ("trailer E count +1",)
    trailer_refusal = "line 42: trailer count mismatch for E"
    patches = [(run_json, "_bulk_equity_records", mutant)]
    for window in (97, 64 * 1024):
        case = _Case(spec, raw, _Mutated(mutated, receipt, notes, expect, mutated != raw), window)
        context = _context(f"control E {skipped}", case)
        strict, honest = _execute(tmp_path, monkeypatch, case)
        _, broken = _execute(tmp_path, monkeypatch, case, patches)
        _assert_same(strict, honest, context + " (unpatched)")
        _check_expectation(expect, strict, False, context)
        assert broken[1] is not None, f"{context}: the mutant ended without a refusal"
        assert "trailer count mismatch for E" in broken[1][1], f"{context}: {broken[1]!r}"
        if skipped == "count":
            assert broken[1][1].endswith(trailer_refusal), f"{context}: {broken[1]!r}"
        else:
            assert strict[1][1].endswith(trailer_refusal), f"{context}: {strict[1]!r}"
            assert "line 42:" not in broken[1][1], f"{context}: {broken[1]!r}"
        with pytest.raises(AssertionError):
            _assert_same(strict, broken, context + " (patched)")
    detected = _detections(
        tmp_path, monkeypatch,
        (lambda seed: []) if skipped == "count" else (lambda seed: [("trailer-count", {})]),
        # docker/run_json.py: _bulk_equity_records, the generator that drives the strict reader.
        patches)
    assert detected >= 1, f"the comparison cannot see a bulk reader that skips its {skipped}"
