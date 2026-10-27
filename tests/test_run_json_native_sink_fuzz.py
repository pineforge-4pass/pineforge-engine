"""CR-397 differential fuzz contract; execute only on the lab.

The reference is the pinned pf_sink.SinkReader, not native validation, the
finalizer, or the converter. Seeds and mutation families are fixed before the
first run. Missing native modules/symbols and unexpected fallback fail the row.
"""
from __future__ import annotations

import copy
from dataclasses import dataclass, fields, is_dataclass, replace
import hashlib
import io
import json
import math
from pathlib import Path
import random
import struct

import pytest

from test_run_json_native_sink_json import (
    configured_sink,
    native_library,
    raw_call,
    wide_header,
)
# The existing helper installs this worktree's docker/ import path.
import pf_sink
import run_json


SEEDS = (
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
)
TAGS = "ETOVKXH"
I53_MAX = (1 << 53) - 1
LINE_CAP = 1024 * 1024
REPORT_NET_PROFIT = 1234.5
FLOAT_EDGES = (
    -0.0, 0.0, 5e-324, -5e-324, 2.2250738585072014e-308,
    1.7976931348623157e308, -1.7976931348623157e308,
    1e-5, 0.0001, 1e15, 1e16, 0.1 + 0.2,
)
TEXT_PIECES = ("", "ascii", '"', "\\/", "\n\r\t\b\f", "\0",
               "漢", "𝄞", "😀", "e\u0301", "\u2028")


def compact(value, *, ascii_only=True):
    return json.dumps(value, ensure_ascii=ascii_only, separators=(",", ":"),
                      allow_nan=False).encode("utf-8")


@dataclass(frozen=True)
class Row:
    tag: str
    tokens: tuple[bytes, ...]

    def wire(self):
        return b"[" + b",".join(self.tokens) + b"]\n"

    def field(self, index, token):
        tokens = list(self.tokens)
        tokens[index] = token
        return replace(self, tokens=tuple(tokens))


@dataclass(frozen=True)
class Case:
    seed: int
    header: dict
    rows: tuple[Row, ...]

    def packed(self, rows=None, *, header=None):
        """Handwritten sink-v1 writer: descriptor-order counts and prefix SHA."""
        header = self.header if header is None else header
        rows = self.rows if rows is None else rows
        counts = {entry["type"]: 0 for entry in header["records"]}
        for row in rows:
            if row.tag in counts:
                counts[row.tag] += 1
        prefix = compact(header) + b"\n" + b"".join(row.wire() for row in rows)
        trailer = {"trailer": "pf-report-sink", "version": 1, "counts": counts,
                   "sha256": hashlib.sha256(prefix).hexdigest()}
        return prefix + compact(trailer) + b"\n"


def text(rng):
    return "".join(rng.choice(TEXT_PIECES) for _ in range(rng.randrange(9)))


def integer(rng):
    return rng.choice((-I53_MAX, I53_MAX, -1, 0, 1,
                       rng.randint(-I53_MAX, I53_MAX)))


def finite(rng):
    if rng.randrange(3) == 0:
        return rng.choice(FLOAT_EDGES)
    # Filter only the finite wire domain, never a native acceptance verdict.
    while True:
        value = struct.unpack("!d", rng.getrandbits(64).to_bytes(8, "big"))[0]
        if math.isfinite(value):
            return value


def nullable(rng, make):
    return None if rng.randrange(3) == 0 else make(rng)


def row_for(tag, index, header, rng):
    if tag == "E":
        values = [integer(rng), finite(rng), finite(rng)]
    elif tag == "T":
        floats = [finite(rng) for _ in range(8)]
        # Keep valid-baseline cumulative PnL finite. Other T/E columns cover
        # arbitrary finite binary64, and mutation 25 also covers fold overflow.
        floats[3] = rng.choice((-0.0, 0.0, 0.1, 0.2, -0.3, 1e16, 1.0,
                                -1e16, rng.uniform(-1e6, 1e6)))
        values = [index, rng.choice(("long", "short")), integer(rng), integer(rng),
                  *floats, integer(rng), integer(rng), bool(rng.getrandbits(1)),
                  rng.getrandbits(53), integer(rng), text(rng), text(rng), text(rng)]
    elif tag == "O":
        columns = len(next(d for d in header["records"] if d["type"] == "O")["fields"]) - 3
        values = [integer(rng), nullable(rng, integer), nullable(rng, integer),
                  *(nullable(rng, finite) for _ in range(columns))]
    elif tag == "V":
        values = [rng.getrandbits(53), integer(rng), integer(rng),
                  nullable(rng, integer), nullable(rng, integer),
                  integer(rng), integer(rng), integer(rng), nullable(rng, finite),
                  f"{rng.getrandbits(64):016x}", nullable(rng, text)]
    elif tag == "K":
        columns = len(next(d for d in header["records"] if d["type"] == "K")["fields"])
        values = [nullable(rng, finite) for _ in range(columns)]
    elif tag == "X":
        values = [integer(rng), integer(rng), text(rng), nullable(rng, finite)]
    else:
        assert tag == "H"
        values = [f"{rng.getrandbits(64):016x}"]
    ascii_only = bool(rng.getrandbits(1))
    tokens = tuple(compact(value, ascii_only=ascii_only) for value in [tag, *values])
    if rng.randrange(4) == 0:
        tokens = (f'"\\u{ord(tag):04x}"'.encode("ascii"), *tokens[1:])
    return Row(tag, tokens)


def make_case(seed, *, edge_counts=None):
    rng = random.Random(seed)
    header = wide_header(rng.randint(0, 32))
    selected = {"E", "T"}
    if rng.getrandbits(1):
        selected.update("OVK")
    for tag in "XH":
        if rng.getrandbits(1):
            selected.add(tag)
    header["records"] = [d for d in header["records"] if d["type"] in selected]
    header["run_id"] = f"fuzz-{seed}-" + text(rng)
    header["engine"] = text(rng)
    counts = {tag: rng.randint(0, 2000) for tag in TAGS if tag in selected}
    if edge_counts is not None:
        counts = {tag: edge_counts.get(tag, 0) for tag in counts}
    order = [tag for tag, count in counts.items() for _ in range(count)]
    rng.shuffle(order)
    seen = dict.fromkeys(counts, 0)
    rows = []
    for tag in order:
        seen[tag] += 1
        rows.append(row_for(tag, seen[tag], header, rng))
    return Case(seed, header, tuple(rows))


@dataclass(frozen=True)
class Failure:
    exception_type: type
    code: str
    code_args: bytes
    message: str
    exit_status: int


def failure(error):
    return Failure(type(error), error.code, compact(error.code_args),
                   str(error), error.exit_status)


def capture(call):
    try:
        return call()
    except run_json.RunFailure as error:
        return failure(error)


@dataclass(frozen=True)
class Source:
    header: bytes
    counts: tuple[int, ...]
    digest: bytes
    equity: bytes
    trades: bytes
    fold: bytes


def reference_fold(records):
    """Independent sequential fold of the Python reader's converted values."""
    n = wins = losses = count_e = count_t = 0
    best = worst = cumulative = peak = drawdown = 0.0
    first = last = None
    trade_at_start = False
    trade_error = equity_error = 0
    for record in records:
        values = record.values
        if record.tag == "E":
            point = record.as_dict()
            if not count_e:
                first = point
            last = point
            count_e += 1
            if not equity_error:
                for index in (2, 3):
                    if values[index - 1] is None:
                        equity_error = index
                        break
        elif record.tag == "T":
            count_t += 1
            trade_at_start |= values[12] <= 0
            if trade_error:
                continue
            if values[0] != n + 1:
                trade_error = 1
                continue
            for index in range(5, 13):
                if values[index - 1] is None:
                    trade_error = index
                    break
            if trade_error:
                continue
            pnl = values[7]
            if not n or pnl > best:
                best = pnl
            if not n or pnl < worst:
                worst = pnl
            n += 1
            wins += int(pnl > 0)
            losses += int(pnl < 0)
            cumulative += pnl
            if cumulative > peak:
                peak = cumulative
            current = cumulative - peak
            if not math.isfinite(cumulative) or not math.isfinite(current):
                trade_error = 21
                continue
            if current < drawdown:
                drawdown = current
    # Fold values after a T failure are explicitly unusable in the ABI.
    return compact({
        "count_e": count_e, "count_t": count_t, "first": first, "last": last,
        "trade_at_start": bool(trade_at_start),
        "result_tag": "T" if trade_error else "E" if equity_error else "",
        "result_field": trade_error or equity_error,
        "summary": None if trade_error else [wins, losses, best, worst, drawdown],
    })


def oracle(path):
    """Acceptance/refusal and expected bytes come only from the strict reader."""
    try:
        with pf_sink.SinkReader(path) as reader:
            records = list(reader)
            assert reader.complete
            counts, digest = reader.counts, reader.sha256
    except pf_sink.SinkFormatError as error:
        # This is the public strict-route wrapping, with its original text.
        raise run_json.RunFailure("the engine's report sink is invalid: " + str(error),
                                  "engine_invariant") from error
    with path.open("rb") as file:
        header = file.readline()
    equity = b",".join(compact(record.as_dict()) for record in records if record.tag == "E")
    trades = b",".join(compact(dict(zip(record.fields[:16], record.values[:16])))
                       for record in records if record.tag == "T")
    return Source(header, tuple(counts.get(tag, 0) for tag in TAGS), digest.encode("ascii"),
                  equity, trades, reference_fold(records))


def reference_summary(source):
    folded = json.loads(source.fold)
    assert not folded["result_tag"]
    n = folded["count_t"]
    wins, losses, best, worst, drawdown = folded["summary"]
    return {
        "total_trades": n, "wins": wins, "losses": losses,
        "win_rate_pct": round(wins / n * 100.0, 4) if n else 0.0,
        "net_pnl": REPORT_NET_PROFIT, "avg_trade": REPORT_NET_PROFIT / n if n else 0.0,
        "best_trade": best, "worst_trade": worst, "max_drawdown": drawdown,
        "bars_processed": source.counts[0],
    }


def native_fold(result):
    diagnostics = result.diagnostics
    assert result.summary.complete == diagnostics.complete == 1
    assert result.summary.count_t == result.counts[1]
    assert diagnostics.count_e == result.counts[0]
    tag = chr(result.result_tag) if result.result_tag else ""
    field = int(result.result_field)
    assert result.summary.nonfinite_t_field == (field if tag == "T" and 5 <= field <= 12 else 0)

    def point(value):
        return {"time_ms": int(value.time_ms),
                "equity": None if math.isnan(value.equity) else float(value.equity),
                "open_profit": None if math.isnan(value.open_profit) else float(value.open_profit)}

    summary = result.summary
    return compact({
        "count_e": int(result.counts[0]), "count_t": int(result.counts[1]),
        "first": point(diagnostics.first) if diagnostics.count_e else None,
        "last": point(diagnostics.last) if diagnostics.count_e else None,
        "trade_at_start": bool(diagnostics.trade_at_start),
        "result_tag": tag, "result_field": field,
        "summary": None if tag == "T" else [
            int(summary.wins), int(summary.losses), float(summary.best_trade),
            float(summary.worst_trade), float(summary.max_drawdown)],
    })


def native_source(function, path, directory):
    with (directory / "equity.fragment").open("w+b", buffering=0) as equity, (
            directory / "trades.fragment").open("w+b", buffering=0) as trades:
        result, header, offending = raw_call(
            function, path, output=equity, trades_output=trades)
        # Generated shapes stay inside the documented native subset. Neither
        # fallback nor an internal/resource/I/O status counts as matching a
        # malformed source. Header refusals also reach the actual C export here.
        assert result.status in (0, 2), (
            "unexpected native status/fallback", result.status, bytes(result.error.message))
        if result.status == 2:
            assert result.complete == 0
            run_json._raise_native_sink_json(result, header, offending)
            raise AssertionError("native refusal translation returned")
        assert result.complete == 1
        e_bytes, t_bytes = equity.read(), trades.read()
        assert len(e_bytes) == result.equity_json_bytes
        assert len(t_bytes) == result.trades_json_bytes
        assert hashlib.sha256(e_bytes).hexdigest().encode() == bytes(result.equity_json_sha256_hex)
        assert hashlib.sha256(t_bytes).hexdigest().encode() == bytes(result.trades_json_sha256_hex)
        return Source(header, tuple(result.counts), bytes(result.sha256_hex),
                      e_bytes, t_bytes, native_fold(result))


class OracleReceipt:
    """A wire-valid sink's receipt is supplied by the Python oracle alone."""
    def __init__(self, source):
        self.source, self.queries = source, 0

    def strategy_report_sink_receipt_v1(self, state, pointer):
        self.queries += 1
        assert isinstance(self.source, Source), "invalid source reached receipt admission"
        receipt = pointer._obj
        for tag, count in zip(TAGS, self.source.counts):
            setattr(receipt, "count_" + tag.lower(), count)
        receipt.sha256_hex, receipt.complete = self.source.digest, 1
        return 0


@dataclass(frozen=True)
class Host:
    outcome: bytes | Failure
    stage: str
    receipt_queries: int


def host_route(function, path, source, case):
    sink = configured_sink(function, path)
    sink.named, sink.keep, sink.run_id = False, True, case.header["run_id"]
    receipt = OracleReceipt(source)
    report = run_json.ReportC()
    if isinstance(source, Source):
        report.total_trades = source.counts[1]
        report.input_bars_processed = report.script_bars_processed = source.counts[0]
    # Deliberately independent of the sink's PnL fold.
    report.net_profit = REPORT_NET_PROFIT
    stage = "verify"
    try:
        sink.verify(receipt, None, report)
        stage = "assemble"
        document = run_json._build_sink_report(
            report, Path("fuzz.csv"), int(report.input_bars_processed),
            0, 60000, 0.0, {}, {}, {}, sink)
        stage = "serialize"
        buffer = io.StringIO()
        run_json._stream_json(document, buffer)
        outcome = buffer.getvalue().encode("ascii")
        stage = "done"
    except run_json.RunFailure as error:
        outcome = failure(error)
    finally:
        sink.close()
    return Host(outcome, stage, receipt.queries)


def assert_same(expected, actual, context):
    """The same equality hook is used by every row and the negative control."""
    if type(actual) is not type(expected):
        raise AssertionError(f"{context}: type {type(actual)!r}, expected {type(expected)!r}")
    if is_dataclass(expected):
        for field in fields(expected):
            assert_same(getattr(expected, field.name), getattr(actual, field.name),
                        context + "." + field.name)
    elif isinstance(expected, bytes):
        if actual != expected:
            offset = next((i for i, pair in enumerate(zip(expected, actual))
                           if pair[0] != pair[1]), min(len(expected), len(actual)))
            raise AssertionError(
                f"{context}: first differing byte {offset}; "
                f"strict length={len(expected)}, native length={len(actual)}; "
                f"strict={expected[offset:offset + 48]!r}, native={actual[offset:offset + 48]!r}")
    elif actual != expected:
        raise AssertionError(f"{context}: strict={expected!r}, native={actual!r}")


def compare_case(case, payload, function, directory, label):
    path = directory / "input.sink"
    path.write_bytes(payload)
    context = f"seed={case.seed} mutation={label} sha256={hashlib.sha256(payload).hexdigest()}"
    expected = capture(lambda: oracle(path))
    actual = capture(lambda: native_source(function, path, directory))
    assert_same(expected, actual, context + " raw")

    calls = []

    def observed(request, result):
        status = function(request, result)
        calls.append((request._obj.operation, result._obj.status,
                      result._obj.complete, request._obj.guard_tag))
        return status

    strict = host_route(None, path, expected, case)
    native = host_route(observed, path, expected, case)
    assert_same(strict, native, context + " host")
    if isinstance(expected, Source):
        if not json.loads(expected.fold)["result_tag"]:
            assert isinstance(strict.outcome, bytes), context + ": valid report was refused"
        else:
            assert isinstance(strict.outcome, Failure) and strict.stage == "assemble", context
        if isinstance(native.outcome, bytes):
            assert calls == [(1, 0, 1, 0), (2, 0, 1, ord("T")), (2, 0, 1, ord("E"))], context
            document = json.loads(native.outcome)
            assert_same(compact(reference_summary(expected)), compact(document["summary"]),
                        context + " independent report summary")
            assert compact(document["trades"]) == b"[" + expected.trades + b"]", context
            assert compact(document["equity_curve"]) == b"[" + expected.equity + b"]", context
        else:
            assert calls == [(1, 0, 1, 0)], context
        assert strict.receipt_queries == native.receipt_queries == 1, context
    else:
        assert_same(expected, strict.outcome, context + " oracle/strict refusal")
        assert strict.stage == native.stage == "verify", context
        assert strict.receipt_queries == native.receipt_queries == 0, context
    assert path.read_bytes() == payload, context + ": route changed the source"
    return expected, actual


# Every seed exercises every family once; only positions/fields/variants are
# randomized. These are rule citations, never precomputed refusal expectations.
MUTATIONS = (
    ("truncate", "pf_sink.py:138-141,332-339"),
    ("flip-number-byte", "pf_sink.py:115-134,161-167,220-223,350-352"),
    ("change-tag", "pf_sink.py:247-259"),
    ("drop-line", "pf_sink.py:337-352,373-378"),
    ("duplicate-line", "pf_sink.py:347-352,373-378"),
    ("trailer-count", "pf_sink.py:346-349"),
    ("trailer-sha256", "pf_sink.py:350-352"),
    ("append-after-trailer", "pf_sink.py:353-354"),
    ("insert-nul", "pf_sink.py:147-167"),
    ("overlong-line", "pf_sink.py:138-141,335"),
    ("lone-surrogate", "pf_sink.py:95-98,228-229"),
    ("integer-negative-zero", "pf_sink.py:115-123,216-219"),
    ("f64-negative-zero-integer", "pf_sink.py:220-223"),
    ("nan", "pf_sink.py:126-134,161-167"),
    ("integer-past-i53", "pf_sink.py:115-123"),
    ("wrong-arity", "pf_sink.py:255-256"),
    ("wrong-type", "pf_sink.py:210-233,257"),
    ("noncanonical-f64", "pf_sink.py:220-223"),
    ("header-descriptor", "pf_sink.py:90-103,170-207"),
    ("blank-bom-whitespace", "pf_sink.py:142-160"),
    ("duplicate-json-key", "pf_sink.py:106-112"),
    ("token-before-field", "pf_sink.py:161-163,257"),
    ("count-before-digest-eof", "pf_sink.py:347-354"),
    ("source-before-deferred-result", "pf_sink.py:369-381; run_json.py:_ReportSink.verify"),
    ("trade-before-equity-result", "pf_sink.py:211-213; run_json.py:_sink_summary,_build_sink_report"),
)


def numeric_fields(row):
    return [index for index, token in enumerate(row.tokens)
            if token and token[:1] in b"-0123456789"]


def target(rows, case, rng, tags="ET", *, numeric=False, nonempty=False):
    candidates = [index for index, row in enumerate(rows)
                  if row.tag in tags and (not numeric or numeric_fields(row))
                  and (not nonempty or len(row.tokens) > 1)]
    if candidates:
        return rng.choice(candidates)
    # Empty-tag/empty-sink inputs still receive a real mutation, never a skip.
    # E/T are always declared and have both scalar and numeric fields.
    tag = rng.choice("".join(tag for tag in "ET" if tag in tags))
    index = 1 + sum(row.tag == tag for row in rows)
    rows.append(row_for(tag, index, case.header, rng))
    return len(rows) - 1


def edit_trailer(payload, edit):
    prefix, line = payload.rsplit(b"\n", 2)[:2]
    trailer = json.loads(line)
    edit(trailer)
    return prefix + b"\n" + compact(trailer) + b"\n"


def mutate(case, name, mutation_index):
    # Separate stream for every mutation: additional draws within one family
    # cannot retune the valid corpus or another family's random choices.
    rng = random.Random((case.seed << 16) | mutation_index)
    rows = list(case.rows)
    original = case.packed()
    if name == "truncate":
        return original[:rng.randrange(len(original))]
    if name == "drop-line":
        lines = original.splitlines(keepends=True)
        if rng.getrandbits(1) and rows:
            rows.pop(rng.randrange(len(rows)))
            return case.packed(rows)
        lines.pop(rng.randrange(len(lines)))
        return b"".join(lines)
    if name == "duplicate-line":
        if rng.getrandbits(1) and rows:
            index = rng.randrange(len(rows))
            rows.insert(index, rows[index])
            return case.packed(rows)
        lines = original.splitlines(keepends=True)
        index = rng.randrange(len(lines))
        lines.insert(index, lines[index])
        return b"".join(lines)
    if name in ("trailer-count", "count-before-digest-eof"):
        def change(trailer):
            tag = rng.choice(tuple(trailer["counts"]))
            trailer["counts"][tag] += rng.randint(1, 2000)
            if name == "count-before-digest-eof":
                trailer["sha256"] = "g" * 64
        payload = edit_trailer(original, change)
        return payload + (b"after\n" if name == "count-before-digest-eof" else b"")
    if name == "trailer-sha256":
        def change(trailer):
            digest = bytearray(trailer["sha256"].encode("ascii"))
            index = rng.randrange(64)
            digest[index] = ord("0") if digest[index] != ord("0") else ord("1")
            trailer["sha256"] = digest.decode("ascii")
        return edit_trailer(original, change)
    if name == "append-after-trailer":
        return original + rng.choice((b"\n", b"\0", b"x", b'["E",0,0.0,0.0]\n'))
    if name == "insert-nul":
        index = rng.randrange(len(original) + 1)
        return original[:index] + b"\0" + original[index:]
    if name == "header-descriptor":
        header = copy.deepcopy(case.header)
        variant = rng.randrange(6)
        if variant == 0:
            header = dict(reversed(tuple(header.items())))
        elif variant == 1:
            header["version"] = rng.choice((True, 1.0, 2))
        elif variant == 2:
            header["records"][0]["units"][rng.randrange(3)] = "wrong"
        elif variant == 3:
            header["records"].reverse()
        elif variant == 4:
            header["run_id"] = "\udfff"
        else:
            header["records"] = [d for d in header["records"] if d["type"] != "T"]
        return case.packed(header=header)
    if name == "duplicate-json-key":
        return original.replace(b'"version":1,', b'"version":1,"version":1,', 1)
    if name == "source-before-deferred-result":
        # Earlier legal-wire result errors must lose to a later source error.
        equity = row_for("E", 1, case.header, rng).field(rng.choice((2, 3)), b"null")
        trade = row_for("T", 1, case.header, rng).field(rng.randrange(5, 13), b"null")
        bad = row_for("E", 2, case.header, rng).field(2, b"1e999")
        return case.packed([equity, trade, *rows, bad])
    if name == "trade-before-equity-result":
        equity = row_for("E", 1, case.header, rng).field(rng.choice((2, 3)), b"null")
        first = row_for("T", 1, case.header, rng)
        second = row_for("T", 2, case.header, rng)
        variant = rng.randrange(4)
        if variant == 0:
            first = first.field(rng.randrange(5, 13), b"null")
        elif variant == 1:
            first = first.field(1, b"2").field(rng.randrange(5, 13), b"null")
        elif variant == 2:
            first = first.field(8, b"1.7976931348623157e+308")
            second = second.field(8, b"1.7976931348623157e+308")
        else:
            first = first.field(12, b"null")
            second = second.field(1, b"5")
        return case.packed([equity, first, second, *rows])

    tags = (TAGS if name in ("flip-number-byte", "change-tag", "wrong-arity", "wrong-type")
            else "T" if name in ("overlong-line", "lone-surrogate") else "ET")
    index = target(rows, case, rng, tags, numeric=name == "flip-number-byte",
                   nonempty=name == "wrong-type")
    row = rows[index]
    if name == "flip-number-byte":
        field = rng.choice(numeric_fields(row))
        token = bytearray(row.tokens[field])
        digit = rng.choice([i for i, byte in enumerate(token) if 48 <= byte <= 57])
        token[digit] ^= 1 << rng.randrange(8)
        rows[index] = row.field(field, bytes(token))
    elif name == "change-tag":
        tag = rng.choice(tuple(tag for tag in TAGS + "Zet" if tag != row.tag))
        rows[index] = replace(row.field(0, compact(tag)), tag=tag)
    elif name == "overlong-line":
        # MAX+1 with LF, or larger so the reader's bounded probe lacks LF.
        row = row.field(20, b'""')
        extra = rng.choice((1, rng.randint(2, 128)))
        length = LINE_CAP + extra - len(row.wire())
        rows[index] = row.field(20, b'"' + b"x" * length + b'"')
    elif name == "lone-surrogate":
        rows[index] = row.field(rng.choice((18, 19, 20)),
                                rng.choice((b'"\\ud800"', b'"\\udfff"')))
    elif name == "integer-negative-zero":
        index = target(rows, case, rng, "E")
        rows[index] = rows[index].field(1, b"-0")
        return case.packed(rows)  # A guaranteed wire-valid mutation/control.
    elif name == "f64-negative-zero-integer":
        rows[index] = row.field(2 if row.tag == "E" else rng.randrange(5, 13), b"-0")
    elif name == "nan":
        rows[index] = row.field(2 if row.tag == "E" else rng.randrange(5, 13),
                                rng.choice((b"nan", b"NaN", b"Infinity", b"-Infinity")))
    elif name == "integer-past-i53":
        rows[index] = row.field(1 if row.tag == "E" else rng.choice((3, 4, 13, 14, 16)),
                                compact(rng.choice((-1, 1)) * (I53_MAX + rng.randint(1, 1000))))
    elif name == "wrong-arity":
        tokens = list(row.tokens)
        if len(tokens) > 1 and rng.getrandbits(1):
            tokens.pop(rng.randrange(1, len(tokens)))
        else:
            tokens.insert(rng.randrange(1, len(tokens) + 1), b"null")
        rows[index] = replace(row, tokens=tuple(tokens))
    elif name == "wrong-type":
        field = rng.randrange(1, len(row.tokens))
        value = json.loads(row.tokens[field])
        token = b"true" if type(value) is int else b"1" if type(value) in (float, bool, str) else b"{}"
        rows[index] = row.field(field, token)
    elif name == "noncanonical-f64":
        rows[index] = row.field(2 if row.tag == "E" else rng.randrange(5, 13),
                                rng.choice((b"1.00", b"1e0", b"1E+16", b"1e-999")))
    elif name == "blank-bom-whitespace":
        lines = case.packed(rows).splitlines(keepends=True)
        line_index = rng.randrange(len(lines))
        variant = rng.randrange(3)
        if variant == 0:
            lines[line_index] = b"\n"
        elif variant == 1:
            lines[line_index] = b"\xef\xbb\xbf" + lines[line_index]
        else:
            lines[line_index] = b" " + lines[line_index]
        return b"".join(lines)
    elif name == "token-before-field":
        index = target(rows, case, rng, "E")
        rows[index] = rows[index].field(1, b"true").field(
            rng.choice((2, 3)), rng.choice((b"9007199254740992", b"1e999", b"NaN")))
    else:
        raise AssertionError("unknown mutation: " + name)
    return case.packed(rows)


@pytest.fixture(scope="module", params=SEEDS, ids=lambda seed: f"seed-{seed:02d}")
def generated_case(request):
    return make_case(request.param)


def test_differential_valid_sink(generated_case, native_library, tmp_path):
    expected, _ = compare_case(generated_case, generated_case.packed(),
                               native_library[2], tmp_path, "valid")
    assert isinstance(expected, Source), "the handwritten writer generated an invalid sink"


@pytest.mark.parametrize("mutation_index,name,citation",
                         [(i, name, citation) for i, (name, citation) in enumerate(MUTATIONS)],
                         ids=[name for name, _ in MUTATIONS])
def test_differential_corruption(generated_case, native_library, tmp_path,
                                mutation_index, name, citation):
    payload = mutate(generated_case, name, mutation_index)
    assert payload != generated_case.packed(), (generated_case.seed, name, "mutation was a no-op")
    compare_case(generated_case, payload, native_library[2], tmp_path, name + " " + citation)


@pytest.mark.parametrize("equities,trades", ((0, 0), (0, 2000), (2000, 0), (2000, 2000)))
def test_differential_count_boundaries(native_library, tmp_path, equities, trades):
    case = make_case(SEEDS[0], edge_counts={"E": equities, "T": trades})
    expected, _ = compare_case(case, case.packed(), native_library[2], tmp_path, "count-boundary")
    assert isinstance(expected, Source)
    assert expected.counts[:2] == (equities, trades)


def test_comparison_control_rejects_mutated_expected(native_library, tmp_path):
    case = make_case(SEEDS[0], edge_counts={"E": 3, "T": 3})
    expected, actual = compare_case(case, case.packed(), native_library[2], tmp_path, "control")
    assert isinstance(expected, Source) and isinstance(actual, Source)
    for field in ("equity", "trades", "fold", "digest"):
        wrong = replace(expected, **{field: getattr(expected, field) + b"!"})
        with pytest.raises(AssertionError):
            assert_same(wrong, actual, "deliberately wrong " + field)
    with pytest.raises(AssertionError):
        assert_same(replace(expected, counts=(expected.counts[0] + 1, *expected.counts[1:])),
                    actual, "deliberately wrong count")
    bad_path = tmp_path / "bad-control.sink"
    bad_path.write_bytes(mutate(case, "trailer-count", 5))
    refused = capture(lambda: oracle(bad_path))
    assert isinstance(refused, Failure)
    native_refused = capture(lambda: native_source(native_library[2], bad_path, tmp_path))
    assert_same(refused, native_refused, "control refusal")
    for wrong in (replace(refused, message=refused.message + "!"),
                  replace(refused, code=refused.code + "!"),
                  replace(refused, code_args=b'{"wrong":true}'),
                  replace(refused, exception_type=ValueError),
                  replace(refused, exit_status=refused.exit_status + 1)):
        with pytest.raises(AssertionError):
            assert_same(wrong, native_refused, "deliberately wrong refusal")
    strict_host = host_route(None, bad_path, refused, case)
    native_host = host_route(native_library[2], bad_path, refused, case)
    assert_same(strict_host, native_host, "control host refusal")
    with pytest.raises(AssertionError):
        assert_same(replace(strict_host, receipt_queries=strict_host.receipt_queries + 1), native_host,
                    "deliberately wrong refusal priority")
