"""Bounded equity batches against the unchanged strict path and real writers.

Every differential test also measures the real canonical path: on the older
implementation that assertion fails because E still calls loads/dumps per row.
Counters always delegate; they do not replace parsing or serialization.
"""
from contextlib import ExitStack
import errno
import hashlib
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import test_run_json_sink as ordinary
import test_run_json_stream_fastpath as oracle
from run_json_trade_scan_test import _event, _point, _sink, _trade, _trailer_start


I53 = (1 << 53) - 1
POINTS = 2000


def _document(sink, report):
    return run_json._build_sink_report(
        report, Path("fixed.csv"), POINTS, ordinary.bars.T0,
        ordinary.bars.T0 + 86400000, 0.125, {}, {}, {}, sink)


def _run(sink, library, report, monkeypatch, enabled, *, copy_only=False,
         destination=None, equity_only=False):
    destination = oracle._Destination() if destination is None else destination
    counts = {"verify": 0, "copy": 0, "decode_e": 0, "dump_e": 0}
    phase = "verify"
    loads, dumps = run_json.json.loads, run_json.json.dumps
    decode = pf_sink.RecordParser.parse_verified

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

    failure = None
    with monkeypatch.context() as patch:
        # raising=False makes the performance assertion, rather than a missing
        # switch, the substantive failure on the older implementation.
        patch.setattr(run_json, "_BULK_E_ENABLED", enabled, raising=False)
        patch.setattr(run_json.json, "loads", counted_loads)
        patch.setattr(run_json.json, "dumps", counted_dumps)
        patch.setattr(pf_sink.RecordParser, "parse_verified", counted_decode)
        try:
            if not copy_only:
                sink.verify(library, 1, report)
            phase = "copy"
            document = _document(sink, report)
            run_json._stream_json(document["equity_curve"] if equity_only else document, destination)
        except Exception as error:
            failure = (type(error), str(error), getattr(error, "code", None),
                       getattr(error, "code_args", None))
    diagnostics = sink.diagnostics
    endpoints = (diagnostics.count_e, ordinary.encoded(diagnostics.first),
                 ordinary.encoded(diagnostics.last),
                 diagnostics.trade_at_start) if diagnostics is not None else None
    return destination.bytes(), failure, counts, endpoints


def _assert_bulk(actual, strict, *, verify=True, copy=True):
    assert actual[:2] == strict[:2]
    assert actual[3] == strict[3]
    if verify:
        assert actual[2]["verify"] < strict[2]["verify"] - POINTS // 2
    if copy:
        assert actual[2]["decode_e"] == actual[2]["dump_e"] == 0
        assert strict[2]["decode_e"] >= POINTS
        assert strict[2]["dump_e"] >= POINTS


def _control(tmp_path, monkeypatch):
    with _sink(tmp_path, [_point(i) for i in range(POINTS)], verify=False) as args:
        strict = _run(*args, monkeypatch, False)
        actual = _run(*args, monkeypatch, True)
        assert strict[1] is actual[1] is None
        _assert_bulk(actual, strict)
        assert actual[2]["verify"] == 2
        # Empty T and nonempty E each parse their own raw header.
        assert actual[2]["copy"] == 2


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
    with _sink(tmp_path, [_point(i) for i in range(count)], verify=False) as args:
        old_destination, new_destination = oracle._Destination(), oracle._Destination()
        strict = _run(*args, monkeypatch, False, destination=old_destination)
        actual = _run(*args, monkeypatch, True, destination=new_destination)
        assert strict[1] is actual[1] is None
        assert actual[:2] == strict[:2]
        assert new_destination.calls == old_destination.calls
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
        assert actual[:2] == strict[:2]
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
def test_copy_guards_and_completed_prefix_match_strict(tmp_path, monkeypatch, fault):
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
        strict = _run(sink, library, report, monkeypatch, False,
                      copy_only=True, equity_only=True)
        actual = _run(sink, library, report, monkeypatch, True,
                      copy_only=True, equity_only=True)
        assert actual[:2] == strict[:2]
        assert strict[1] is not None and strict[1][2] == "engine_invariant"
        if fault == "format-and-digest":
            assert "invalid UTF-8 or JSON" in strict[1][1]
        elif fault == "count":
            assert strict[1][1].endswith("E scan disagrees with its verified count")
        else:
            assert strict[1][1].endswith(f"{fault} differs")
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
        assert actual[:2] == strict[:2]
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
    original = run_json._UncachedSinkFile.read

    def fail(file, count=-1):
        if file.position >= file.window:
            raise OSError(errno.EIO, "injected sink read failure")
        return original(file, count)

    with _sink(tmp_path, [_point(i) for i in range(POINTS)], verify=False) as args:
        sink, library, report = args
        if invalid_first:
            target = ordinary.encoded(_point(0))
            _rewrite(sink, library, lambda body: body.replace(
                target, BAD_LINES["repr-decimal"], 1))
        with monkeypatch.context() as patch:
            patch.setattr(run_json._UncachedSinkFile, "read", fail)
            strict = _run(sink, library, report, patch, False)
            actual = _run(sink, library, report, patch, True)
        assert actual[:2] == strict[:2]
        assert strict[1] is not None and strict[1][2] == "engine_invariant"
        if invalid_first:
            assert "f64 column is not the Python repr" in strict[1][1]
        else:
            assert "injected sink read failure" in strict[1][1]
    _control(tmp_path, monkeypatch)


def test_bulk_count_overflow_replays_the_strict_failure(tmp_path, monkeypatch):
    original = run_json._open_uncached_sink

    def open_sink(path, *, raw=False):
        reader = original(path, raw=raw)
        if not raw:
            reader._counts["E"] = I53 - 1
        return reader

    with _sink(tmp_path, [_point(0), _point(1)], verify=False) as args:
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_open_uncached_sink", open_sink)
            strict = _run(*args, patch, False)
            actual = _run(*args, patch, True)
        assert actual[:2] == strict[:2]
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
        assert actual[:2] == strict[:2]
        assert proofs and all(later not in raw for raw in proofs)
        assert actual[2]["decode_e"] >= POINTS // 2


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
        actual = _run(*args, monkeypatch, True)
        assert actual[1] is None
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
        with monkeypatch.context() as patch:
            patch.setattr(hashlib, "sha256", Digest)
            sink.verify(library, 1, report)
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
    original = run_json._BoundedJSONFile.write_ascii
    attempted = []

    def fail(file, data):
        if b'"time_ms":' in data:
            attempted.append(data)
            raise OSError(errno.ENOSPC, "injected report artifact full")
        return original(file, data)

    with _sink(tmp_path, [_point(i) for i in range(POINTS)]) as (sink, library, report):
        monkeypatch.setattr(run_json._BoundedJSONFile, "write_ascii", fail)
        with ExitStack() as resources:
            with pytest.raises(run_json.RunFailure) as failure:
                run_json._prepare_stream_report(
                    _document(sink, report), resources, str(tmp_path))
        assert failure.value.code == "report_sink_failed"
        assert failure.value.code_args == {"reason": "scratch_io"}
        assert str(failure.value) == "report sink write failed"
        assert len(attempted) == 1
