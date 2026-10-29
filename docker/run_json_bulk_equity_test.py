"""Bounded equity batches through fused assembly and the frozen strict reader.

Counters delegate to real proofs, parsers, encoders and artifact writes.
The standalone scan also retains its exact completed-prefix contract.
"""
import errno
from functools import lru_cache
import hashlib
from pathlib import Path
import re
import sys
import tracemalloc

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import run_json_fused_test_support as support
import test_run_json_sink as ordinary
import test_run_json_stream_fastpath as oracle
from run_json_trade_scan_test import _event, _point, _sink, _trade, _trailer_start


I53 = (1 << 53) - 1
POINTS = 2000


@lru_cache(maxsize=1)
def _reference_runner():
    return support.load_frozen_copy("bulk_equity_reference")


def _document(sink, report, runner=run_json):
    return runner._build_sink_report(
        report, Path("fixed.csv"), POINTS, ordinary.bars.T0,
        ordinary.bars.T0 + 86400000, 0.125, {}, {}, {}, sink)


def _endpoints(sink):
    diagnostics = sink.diagnostics
    if diagnostics is None:
        return None
    return (diagnostics.count_e, ordinary.encoded(diagnostics.first),
            ordinary.encoded(diagnostics.last), diagnostics.trade_at_start)


def _result(outcome, counts, endpoints):
    envelope = outcome.failure
    failure = ((envelope[:2], *envelope[2:5]) if envelope is not None else None)
    return outcome.report, failure, counts, endpoints, outcome


def _run(sink, library, report, monkeypatch, use_fused, *,
         destination=None, bulk=True, after_verify=None):
    """Publish a real prepared artifact; the reference disables bulk in frozen code."""
    runner = run_json if use_fused else _reference_runner()
    parser = pf_sink if use_fused else runner._rjone_pf_sink
    target = support.reference_sink(runner, sink) if use_fused else sink
    destination = oracle._Destination() if destination is None else destination
    counts = {"verify": 0, "copy": 0, "decode_e": 0, "dump_e": 0,
              "strict_e": 0, "bulk_e": 0, "encoded_e": 0, "batch_sizes": []}
    phase = "verify"
    verified_target = None
    loads, dumps = runner.json.loads, runner.json.dumps
    parse, decode = parser.RecordParser.parse, parser.RecordParser.parse_verified
    prove = runner._bulk_equity

    def counted_loads(*args, **kwargs):
        counts[phase] += 1
        return loads(*args, **kwargs)

    def counted_dumps(value, *args, **kwargs):
        if type(value) is dict and tuple(value) == ("time_ms", "equity", "open_profit"):
            counts["dump_e"] += 1
        return dumps(value, *args, **kwargs)

    def counted_decode(parser, line):
        counts["decode_e"] += 1
        return decode(parser, line)

    def counted_parse(parser, *args, **kwargs):
        record = parse(parser, *args, **kwargs)
        if record.tag == "E":
            counts["strict_e"] += 1
        return record

    def counted_proof(raw):
        batch = prove(raw)
        if batch is not None:
            counts["bulk_e"] += batch.count
            counts["batch_sizes"].append(batch.count)
        return batch

    def verified(verified_sink, native):
        nonlocal phase, verified_target
        verified_target = verified_sink
        phase = "copy"
        if after_verify is not None:
            after_verify(verified_sink, native)

    def publish(artifact, document):
        while chunk := artifact.read(64 * 1024):
            destination.write(chunk.decode("ascii") if isinstance(chunk, bytes) else chunk)

    with monkeypatch.context() as patch:
        patch.setattr(runner, "_BULK_E_ENABLED", use_fused and bulk)
        patch.setattr(runner.json, "loads", counted_loads)
        patch.setattr(runner.json, "dumps", counted_dumps)
        patch.setattr(parser.RecordParser, "parse", counted_parse)
        patch.setattr(parser.RecordParser, "parse_verified", counted_decode)
        patch.setattr(runner, "_bulk_equity", counted_proof)
        if use_fused:
            append = runner._JSONColumn.append_ascii

            def counted_append(column, data, *, row_count=1):
                result = append(column, data, row_count=row_count)
                counts["encoded_e"] += data.count(b'{"time_ms":')
                return result

            patch.setattr(runner._JSONColumn, "append_ascii", counted_append)
        try:
            outcome = support.assemble(
                runner, target, library, report, after_verify=verified, consume=publish)
        finally:
            if use_fused:
                target.close()
    outcome.report = destination.bytes()
    endpoints = _endpoints(verified_target) if verified_target is not None else None
    return _result(outcome, counts, endpoints)


def _scan(sink, monkeypatch, use_bulk, *, destination=None):
    """Exercise the surviving raw E scan, including bytes completed before refusal."""
    runner = run_json if use_bulk else _reference_runner()
    target = support.reference_sink(runner, sink)
    target.header, target.counts = sink.header, dict(sink.counts)
    target.sha256, target._identity = sink.sha256, sink._identity
    destination = oracle._Destination() if destination is None else destination
    outcome = support.Outcome()
    with monkeypatch.context() as patch, support.reference_imports(runner):
        patch.setattr(runner, "_BULK_E_ENABLED", use_bulk)
        try:
            runner._stream_json(runner._JSONEquityArray(target), destination)
        except Exception as error:
            outcome.error = error
            outcome.failure = support.failure_envelope(runner, error)
        finally:
            target.close()
    outcome.report = destination.bytes()
    return _result(outcome, {}, None)


def _assert_result(actual, strict):
    assert actual[:2] == strict[:2]
    assert actual[3] == strict[3]
    support.assert_same(actual[4], strict[4])


def _assert_scan(actual, strict):
    assert actual[:2] == strict[:2]
    assert actual[4].failure == strict[4].failure


def _assert_bulk(actual, strict, *, verify=True, copy=True):
    _assert_result(actual, strict)
    points = strict[3][0]
    if verify:
        assert actual[2]["verify"] < strict[2]["verify"] - POINTS // 2
        assert actual[2]["bulk_e"] == points
    if copy:
        assert actual[2]["decode_e"] == actual[2]["dump_e"] == 0
        assert actual[2]["encoded_e"] == points
        assert strict[2]["decode_e"] >= points
        assert strict[2]["dump_e"] >= points


def _control(tmp_path, monkeypatch):
    with _sink(tmp_path, [_point(i) for i in range(POINTS)], verify=False) as args:
        strict = _run(*args, monkeypatch, False)
        actual = _run(*args, monkeypatch, True)
        assert strict[1] is actual[1] is None
        _assert_bulk(actual, strict)
        assert actual[2]["verify"] == 2
        assert actual[2]["copy"] == 0
        assert actual[2]["bulk_e"] == actual[2]["encoded_e"] == POINTS


@pytest.mark.parametrize("kind", ["ordinary", "negative", "exponents", "integer-edges", "mixed"])
def test_report_bytes_and_no_per_point_python_json(tmp_path, monkeypatch, kind):
    rows = [_point(i) for i in range(POINTS)]
    if kind == "negative":
        rows[3:6] = [["E", -3, -12.5, -0.0], ["E", -2, -0.0, 0.0],
                     ["E", -1, 5e-324, -5e-324]]
    elif kind == "exponents":
        values = (1e16, 1e-7, 1.25e100, 1.7976931348623157e308)
        rows[3:3] = [["E", i, value, -value] for i, value in enumerate(values)]
    elif kind == "integer-edges":
        rows[3:3] = [["E", -I53, 1.0, -0.0], ["E", I53, 2.0, 0.0]]
    elif kind == "mixed":
        rows[1:1] = [_trade(1, entry_bar_index=0), _event("quote\"\\漢"), ["K", 50.]]
        rows[1500:1500] = [_trade(2, pnl=-7.0), _event("tail")]
    with _sink(tmp_path, rows, verify=False) as args:
        strict = _run(*args, monkeypatch, False)
        actual = _run(*args, monkeypatch, True)
        assert actual[1] is strict[1] is None
        _assert_bulk(actual, strict)


@pytest.mark.parametrize("offset", range(97))
def test_every_small_window_edge_preserves_bytes(tmp_path, monkeypatch, offset):
    # The message changes the next E line's starting offset through every
    # residue, using the same encoder as the fixture writer.
    rows = [_event("p" * offset), *(_point(i) for i in range(POINTS))]
    monkeypatch.setattr(run_json, "_BULK_E_WINDOW", 97, raising=False)
    with _sink(tmp_path, rows, verify=False) as args:
        strict = _run(*args, monkeypatch, False)
        actual = _run(*args, monkeypatch, True)
        assert actual[1] is strict[1] is None
        _assert_bulk(actual, strict)


@pytest.mark.parametrize("count", [0, 1, 511, 512, 513, 1024, 1025])
def test_empty_and_batch_boundary_write_calls(tmp_path, monkeypatch, count):
    with _sink(tmp_path, [_point(i) for i in range(count)]) as args:
        old_scan, new_scan = oracle._Destination(), oracle._Destination()
        strict_scan = _scan(args[0], monkeypatch, False, destination=old_scan)
        actual_scan = _scan(args[0], monkeypatch, True, destination=new_scan)
        assert strict_scan[1] is actual_scan[1] is None
        _assert_scan(actual_scan, strict_scan)
        assert new_scan.calls == old_scan.calls
        old_destination, new_destination = oracle._Destination(), oracle._Destination()
        strict = _run(*args, monkeypatch, False, destination=old_destination)
        actual = _run(*args, monkeypatch, True, destination=new_destination)
        assert strict[1] is actual[1] is None
        _assert_result(actual, strict)
        assert new_destination.calls == old_destination.calls
        assert all(len(chunk) <= 64 * 1024 for chunk in new_destination.calls)
        assert actual[2]["bulk_e"] == actual[2]["encoded_e"] == count
        assert all(0 < size <= _reference_runner()._BULK_E_BATCH
                   for size in actual[2]["batch_sizes"])
        assert actual[2]["decode_e"] == actual[2]["dump_e"] == 0
        assert strict[2]["decode_e"] >= count
        assert strict[2]["dump_e"] >= count
    _control(tmp_path, monkeypatch)


def test_named_summary_keeps_bytes_and_only_bulk_verifies(tmp_path, monkeypatch):
    with _sink(tmp_path, [_point(i) for i in range(POINTS)], verify=False) as args:
        args[0].named = True
        try:
            strict = _run(*args, monkeypatch, False)
            actual = _run(*args, monkeypatch, True)
            assert strict[1] is actual[1] is None
            _assert_bulk(actual, strict, copy=False)
            assert actual[2]["copy"] == strict[2]["copy"] == 0
        finally:
            # Restore temporary-file cleanup after the named projection.
            args[0].named = False


def _rewrite(sink, library, edit, *, trailer_edit=None):
    path = Path(sink.path)
    raw = path.read_bytes()
    cut = _trailer_start(raw)
    body, trailer = raw[:cut], ordinary.json.loads(raw[cut:])
    body = edit(body)
    library.sha = hashlib.sha256(body).hexdigest()
    trailer["sha256"] = library.sha
    if trailer_edit is not None:
        trailer_edit(trailer)
    path.write_bytes(body + ordinary.encoded(trailer))


BAD_LINES = {
    "crlf": b'["E",0,1.0,0.0]\r\n',
    "bom": b'\xef\xbb\xbf["E",0,1.0,0.0]\n',
    "blank": b"\n",
    "space": b'["E",0, 1.0,0.0]\n',
    "negative-zero": b'["E",-0,1.0,0.0]\n',
    "leading-zero": b'["E",00,1.0,0.0]\n',
    "uppercase": b'["E",0,1E5,0.0]\n',
    "integer-float": b'["E",0,1,0.0]\n',
    "repr-decimal": b'["E",0,0.10,0.0]\n',
    "repr-exponent": b'["E",0,1.0e0,0.0]\n',
    "large-int": ordinary.encoded(["E", I53 + 1, 1.0, 0.0]),
    "large-negative-int": ordinary.encoded(["E", -I53 - 1, 1.0, 0.0]),
    "float-time": b'["E",9007199254740992.0,1.0,0.0]\n',
    "nan": b'["E",0,NaN,0.0]\n',
    "overflow": b'["E",0,1e999,0.0]\n',
    "null-equity": ordinary.encoded(["E", 0, None, 0.0]),
    "null-open-profit": ordinary.encoded(["E", 0, 1.0, None]),
    "both-null": ordinary.encoded(["E", 0, None, None]),
    "arity": ordinary.encoded(["E", 0, 1.0]),
    "tag": ordinary.encoded(["EE", 0, 1.0, 0.0]),
    "escaped-tag": b'["\\u0045",0,1.0,0.0]\n',
    "invalid-utf8": b'["E",0,"\xff",0.0]\n',
}


@pytest.mark.parametrize("fault", tuple(BAD_LINES) + (
    "digest", "truncated", "duplicate-trailer", "after-trailer", "count",
    "long-record", "header-bom", "receipt", "incomplete", "run-id", "trade-order",
    "receipt-count", "report-trade-count",
))
def test_strict_refusal_type_text_code_and_priority(tmp_path, monkeypatch, fault):
    rows = [_point(i) for i in range(POINTS)]
    target = ordinary.encoded(rows[-1])
    with _sink(tmp_path, rows, verify=False) as (sink, library, report):
        path = Path(sink.path)
        if fault in BAD_LINES:
            _rewrite(sink, library, lambda body: body.replace(target, BAD_LINES[fault]))
        elif fault == "long-record":
            line = b'["E",0,' + b"1" * pf_sink.MAX_LINE_BYTES + b",0.0]\n"
            _rewrite(sink, library, lambda body: body.replace(target, line))
        elif fault in ("digest", "count"):
            def corrupt(trailer):
                if fault == "digest":
                    digest = trailer["sha256"]
                    trailer["sha256"] = ("0" if digest[0] != "0" else "1") + digest[1:]
                else:
                    trailer["counts"]["E"] += 1
            _rewrite(sink, library, lambda body: body, trailer_edit=corrupt)
        elif fault in ("truncated", "duplicate-trailer", "after-trailer", "header-bom"):
            raw = path.read_bytes()
            changed = {
                "truncated": raw[:-1],
                "duplicate-trailer": raw + raw[_trailer_start(raw):],
                "after-trailer": raw + b"x\n",
                "header-bom": b"\xef\xbb\xbf" + raw,
            }[fault]
            path.write_bytes(changed)
        elif fault in ("receipt", "incomplete"):
            library.fault = fault
        elif fault == "receipt-count":
            library.counts = dict(library.counts, E=library.counts["E"] + 1)
        elif fault == "report-trade-count":
            report.total_trades += 1
        elif fault == "run-id":
            sink.run_id = "different"
        else:
            library.counts, library.sha = ordinary.write_sink(
                path, [*rows, _trade(2)], outputs=True)
            report.total_trades = 1
        strict = _run(sink, library, report, monkeypatch, False)
        actual = _run(sink, library, report, monkeypatch, True)
        _assert_result(actual, strict)
        if fault not in ("negative-zero", "escaped-tag"):
            assert strict[1] is not None, fault
        else:
            assert strict[1] is None
        if fault in ("null-equity", "both-null", "null-open-profit"):
            field = ("equity_curve.open_profit" if fault == "null-open-profit"
                     else "equity_curve.equity")
            assert strict[1][2:] == (
                "report_result_invalid", {"field": field, "reason": "nonfinite"})
    # Header failures cannot themselves reach E. Pin the intended performance
    # change in every row as well as preserving the corrupted input's result.
    _control(tmp_path, monkeypatch)


@pytest.mark.parametrize("fault", ["identity", "digest", "count", "format-and-digest"])
def test_sink_scan_guards_and_fused_refusals_match_frozen(tmp_path, monkeypatch, fault):
    with _sink(tmp_path, [_point(i) for i in range(POINTS)]) as (sink, library, report):
        path = Path(sink.path)
        raw = path.read_bytes()
        if fault == "identity":
            path.write_bytes(raw + b"x")
        elif fault == "count":
            sink.counts["E"] += 1
        else:
            row = _point(POINTS - 1)
            old = ordinary.encoded(row)
            token = ordinary.encoded(row[2]).rstrip(b"\n")
            if fault == "digest":
                row[2] += 1000.0
                changed = ordinary.encoded(row)
            else:
                changed = old.replace(token, token[:-1] + b"x")
                sink.counts["E"] += 1
            assert len(changed) == len(old) and changed != old
            path.write_bytes(raw.replace(old, changed))
        strict = _scan(sink, monkeypatch, False)
        actual = _scan(sink, monkeypatch, True)
        _assert_scan(actual, strict)
        assert strict[1] is not None and strict[1][2] == "engine_invariant"
        if fault == "format-and-digest":
            assert "invalid UTF-8 or JSON" in strict[1][1]
        elif fault == "count":
            assert strict[1][1].endswith("E scan disagrees with its verified count")
        else:
            assert strict[1][1].endswith(f"{fault} differs")
        # The fused read proves the trailer itself before exposing any prefix.
        if fault == "count":
            _rewrite(sink, library, lambda body: body,
                     trailer_edit=lambda trailer: trailer["counts"].update(E=POINTS + 1))
        strict = _run(sink, library, report, monkeypatch, False)
        actual = _run(sink, library, report, monkeypatch, True)
        _assert_result(actual, strict)
        assert strict[1] is not None and strict[1][2] == "engine_invariant"
        assert actual[0] == strict[0] == b""
        if fault == "format-and-digest":
            assert "invalid UTF-8 or JSON" in strict[1][1]
        elif fault == "count":
            assert "trailer count mismatch for E" in strict[1][1]
        elif fault == "digest":
            assert "trailer SHA-256 mismatch" in strict[1][1]
        else:
            assert "bytes after the trailer" in strict[1][1]
    _control(tmp_path, monkeypatch)


@pytest.mark.parametrize("competing", ["format", "receipt", "trade", "summary", "metrics"])
def test_deferred_equity_failure_keeps_competing_priority(tmp_path, monkeypatch, competing):
    rows = [_point(i) for i in range(POINTS)]
    rows[-1][2] = rows[-1][3] = None
    if competing == "trade":
        rows.insert(POINTS // 2, _trade(1, pnl=None))
    with _sink(tmp_path, rows, verify=False) as (sink, library, report):
        if competing == "format":
            _rewrite(sink, library, lambda body: body,
                     trailer_edit=lambda trailer: trailer["counts"].update(E=POINTS + 1))
        elif competing == "receipt":
            library.fault = "incomplete"
        elif competing == "summary":
            report.net_profit = float("inf")
        elif competing == "metrics":
            report.metrics.all.profit_factor = float("inf")
        strict = _run(sink, library, report, monkeypatch, False)
        actual = _run(sink, library, report, monkeypatch, True)
        _assert_result(actual, strict)
        assert strict[1] is not None
        if competing in ("format", "receipt"):
            assert strict[1][2] == "engine_invariant"
        else:
            field = {"trade": "trades.pnl", "summary": "summary.net_pnl",
                     "metrics": "metrics.all.profit_factor"}[competing]
            assert strict[1][2:] == (
                "report_result_invalid", {"field": field, "reason": "nonfinite"})
    _control(tmp_path, monkeypatch)


@pytest.mark.parametrize("invalid_first", [False, True], ids=["read-error", "format-first"])
def test_verify_read_error_keeps_line_and_format_priority(tmp_path, monkeypatch, invalid_first):
    def failing_read(original):
        def fail(file, count=-1):
            if file.position >= file.window:
                raise OSError(errno.EIO, "injected sink read failure")
            return original(file, count)
        return fail

    with _sink(tmp_path, [_point(i) for i in range(POINTS)], verify=False) as args:
        sink, library, report = args
        if invalid_first:
            target = ordinary.encoded(_point(0))
            _rewrite(sink, library, lambda body: body.replace(
                target, BAD_LINES["repr-decimal"], 1))
        with monkeypatch.context() as patch:
            for runner in (_reference_runner(), run_json):
                patch.setattr(runner._UncachedSinkFile, "read",
                              failing_read(runner._UncachedSinkFile.read))
            strict = _run(sink, library, report, patch, False)
            actual = _run(sink, library, report, patch, True)
        _assert_result(actual, strict)
        assert strict[1] is not None and strict[1][2] == "engine_invariant"
        if invalid_first:
            assert "f64 column is not the Python repr" in strict[1][1]
        else:
            assert "injected sink read failure" in strict[1][1]
    _control(tmp_path, monkeypatch)


def test_bulk_count_overflow_replays_the_strict_failure(tmp_path, monkeypatch):
    def overflowing_reader(original):
        def open_sink(path, *, raw=False):
            reader = original(path, raw=raw)
            if not raw:
                reader._counts["E"] = I53 - 1
            return reader
        return open_sink

    with _sink(tmp_path, [_point(0), _point(1)], verify=False) as args:
        with monkeypatch.context() as patch:
            for runner in (_reference_runner(), run_json):
                patch.setattr(runner, "_open_uncached_sink",
                              overflowing_reader(runner._open_uncached_sink))
            strict = _run(*args, patch, False)
            actual = _run(*args, patch, True)
        _assert_result(actual, strict)
        assert strict[1] is not None
        assert strict[1][1].endswith("line 3: record count exceeds the exact JSON integer bound")
    _control(tmp_path, monkeypatch)


def test_first_noncanonical_e_disables_later_bulk_proofs(tmp_path, monkeypatch):
    _control(tmp_path, monkeypatch)
    rows = [_point(i) for i in range(POINTS)]
    target = ordinary.encoded(rows[POINTS // 2])
    later = ordinary.encoded(rows[-1])
    with _sink(tmp_path, rows, verify=False) as (sink, library, report):
        _rewrite(sink, library, lambda body: body.replace(
            target, BAD_LINES["negative-zero"], 1))
        strict = _run(sink, library, report, monkeypatch, False)
        original, proofs = run_json._bulk_equity, []

        def prove(raw):
            proofs.append(raw)
            return original(raw)

        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_bulk_equity", prove)
            actual = _run(sink, library, report, patch, True)
        assert strict[1] is actual[1] is None
        _assert_result(actual, strict)
        assert proofs and all(later not in raw for raw in proofs)
        assert actual[2]["strict_e"] >= POINTS // 2
        assert actual[2]["dump_e"] >= POINTS // 2


def test_new_scan_buffer_is_window_bounded(tmp_path, monkeypatch):
    _control(tmp_path, monkeypatch)
    original = run_json._BulkEquityFile._fill
    sizes = []

    def fill(file):
        result = original(file)
        sizes.append(len(file.data))
        assert len(file.data) <= run_json._BULK_E_WINDOW
        return result

    monkeypatch.setattr(run_json._BulkEquityFile, "_fill", fill)
    rows = [_point(i) for i in range(POINTS)]
    rows[1500:1500] = [_event("x" * (2 * run_json._BULK_E_WINDOW))]
    with _sink(tmp_path, rows, verify=False) as args:
        strict = _run(*args, monkeypatch, False)
        actual = _run(*args, monkeypatch, True)
        assert actual[1] is None
        _assert_result(actual, strict)
    assert sizes and max(sizes) <= run_json._BULK_E_WINDOW


def test_verify_hashes_body_in_windows_including_other_tags(tmp_path, monkeypatch):
    rows = [_point(i) for i in range(POINTS)]
    rows[100:100] = [_trade(1), _event("event"), ["K", 50.]]
    original, states = hashlib.sha256, []

    class Digest:
        def __init__(self, data=b""):
            self.inner, self.parts = original(data), [bytes(data)] if data else []
            states.append(self)

        def update(self, data):
            self.parts.append(bytes(data))
            self.inner.update(data)

        def hexdigest(self):
            return self.inner.hexdigest()

    with _sink(tmp_path, rows, verify=False) as (sink, library, report):
        raw = Path(sink.path).read_bytes()
        strict = _run(sink, library, report, monkeypatch, False)
        with monkeypatch.context() as patch:
            patch.setattr(hashlib, "sha256", Digest)
            actual = _run(sink, library, report, patch, True)
        _assert_result(actual, strict)
        assert len(states) == 1
        parts = states[0].parts
        assert b"".join(parts) == raw[:_trailer_start(raw)]
        window = getattr(run_json, "_BULK_E_WINDOW", 64 * 1024)
        assert parts[0] == raw[:raw.index(b"\n") + 1]
        assert len(parts) <= 2 + len(raw) // window
        assert all(len(part) == window for part in parts[1:-1])
        assert all(len(part) <= window for part in parts[1:])
    _control(tmp_path, monkeypatch)


def test_encoded_artifact_write_error_retains_code(tmp_path, monkeypatch):
    _control(tmp_path, monkeypatch)
    outcomes = []
    with _sink(tmp_path, [_point(i) for i in range(POINTS)], verify=False) as args:
        for use_fused, runner in ((False, _reference_runner()), (True, run_json)):
            original = runner._BoundedJSONFile.write_ascii
            attempted = []

            def fail(file, data):
                if b'"time_ms":' in data:
                    attempted.append(data)
                    raise OSError(errno.ENOSPC, "injected report artifact full")
                return original(file, data)

            with monkeypatch.context() as patch:
                patch.setattr(runner._BoundedJSONFile, "write_ascii", fail)
                outcome = _run(*args, patch, use_fused)
            assert outcome[1] is not None
            assert outcome[1][1:] == (
                "report sink write failed", "report_sink_failed", {"reason": "scratch_io"})
            assert outcome[0] == b""
            assert len(attempted) == 1
            outcomes.append(outcome)
    _assert_result(outcomes[1], outcomes[0])


def test_a_run_is_consumed_in_batches_of_at_most_the_batch_size():
    line = b'["E",1700000000000,10000.0,0.0]\n'
    batch = run_json._BULK_E_BATCH
    data = line * (3 * batch + 5)
    sizes, cursor = [], 0
    while (match := run_json._BULK_E_RUN.match(data, cursor)) is not None:
        sizes.append((match.end() - cursor) // len(line))
        cursor = match.end()
    assert sizes == [batch, batch, batch, 5] and cursor == len(data)
    # The proof takes exactly the runs the regex hands out; a longer one is no
    # match, so no caller can pass it a whole window.
    accepted = run_json._bulk_equity(data[:batch * len(line)])
    assert accepted is not None and accepted.count == batch
    assert run_json._bulk_equity(data[:(batch + 1) * len(line)]) is None


MEMORY_COUNTS = (1_000, 20_000)
MEMORY_LIMIT = 1024 * 1024
MEMORY_SPREAD = 256 * 1024


def _traced_peak(action):
    tracemalloc.start()
    try:
        action()
        return tracemalloc.get_traced_memory()[1]
    finally:
        tracemalloc.stop()


class _DigestDestination:
    def __init__(self):
        self.digest, self.size = hashlib.sha256(), 0

    def write(self, text):
        data = text.encode("ascii")
        self.digest.update(data)
        self.size += len(data)
        return len(text)


def _pass_peaks(tmp_path, count, monkeypatch):
    """Trace fused verification and encoded readback without retaining their bytes.

    The frozen reader supplies the expected byte count and SHA outside tracing.
    Both stages must complete, even though readback no longer parses E records.
    """
    rows = [_point(i) for i in range(count)]
    with _sink(tmp_path, rows, verify=False) as (sink, library, report):
        reference = _reference_runner()
        expected, actual = _DigestDestination(), _DigestDestination()
        native = support.clone_report(reference, report)
        target = support.reference_sink(reference, sink)
        with monkeypatch.context() as patch, support.reference_imports(reference):
            patch.setattr(reference, "_BULK_E_ENABLED", False)
            target.verify(library, 1, native)
            document = _document(target, native, reference)
            reference._stream_json(document["equity_curve"], expected)
        verify = _traced_peak(lambda: sink.verify(library, 1, report))
        document = _document(sink, report)
        copy = _traced_peak(lambda: run_json._stream_json(
            document["equity_curve"], actual))
        assert actual.size == expected.size > 0
        assert actual.digest.hexdigest() == expected.digest.hexdigest()
    return {"verify": verify, "copy": copy}


def _memory_problems(tmp_path, monkeypatch):
    """Check the same memory bounds for the fused read and encoded readback.

    Without a bound on the lines one regex call matches, the engine's
    backtracking stack holds about 2 KiB for every line of the run: the
    1,000-point sink is one run of 1,000 lines (~2 MB), the 20,000-point sink
    has runs of a full window, ~2,000 lines (~4 MB). Both are over 1 MiB and
    they differ by ~2 MB. With 128 lines per call every run costs a few
    hundred KB, whatever the file.
    """
    original, accepted = run_json._bulk_equity, []

    def counted(raw):
        batch = original(raw)
        if batch is not None:
            accepted.append(batch.count)
        return batch

    problems, peaks = [], {}
    with monkeypatch.context() as patch:
        patch.setattr(run_json, "_bulk_equity", counted)
        _pass_peaks(tmp_path, 200, patch)  # exclude first-use imports and caches
        for count in MEMORY_COUNTS:
            accepted.clear()
            peaks[count] = _pass_peaks(tmp_path, count, patch)
            # Each point is proved once; readback's bytes match the frozen curve.
            if sum(accepted) != count:
                problems.append(f"engaged: {sum(accepted)} of {count} lines were "
                                f"accepted in bulk for {count} points")
    for name in ("verify", "copy"):
        by_count = {count: peaks[count][name] for count in MEMORY_COUNTS}
        low, high = min(by_count.values()), max(by_count.values())
        if high >= MEMORY_LIMIT:
            problems.append(f"limit: the {name} pass reaches {high} traced bytes, "
                            f"not below {MEMORY_LIMIT} ({by_count})")
        if high - low > MEMORY_SPREAD:
            problems.append(f"spread: the {name} pass differs by {high - low} traced bytes "
                            f"across the point counts, more than {MEMORY_SPREAD} ({by_count})")
    return problems


def test_fused_bulk_and_artifact_readback_use_flat_python_memory(tmp_path, monkeypatch):
    problems = _memory_problems(tmp_path, monkeypatch)
    assert not problems, "\n".join(problems)


def test_unbounded_regex_breaks_fused_memory_limits(tmp_path, monkeypatch):
    # The batch size raised to the whole window: one regex call may span every
    # line of a window again, as before the bound. Each of the two bounds must
    # see it, or the check could not fail on that bound.
    whole_window = re.compile(
        rb"(?:" + run_json._BULK_E_LINE.pattern + (rb"){1,%d}" % run_json._BULK_E_WINDOW))
    assert whole_window.pattern != run_json._BULK_E_RUN.pattern
    monkeypatch.setattr(run_json, "_BULK_E_RUN", whole_window)
    problems = _memory_problems(tmp_path, monkeypatch)
    assert any(problem.startswith("limit:") for problem in problems), problems
    assert any(problem.startswith("spread:") for problem in problems), problems
    assert not any(problem.startswith("engaged:") for problem in problems), problems
