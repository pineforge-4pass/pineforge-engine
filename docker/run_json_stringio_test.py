"""Resident report text of the sink-off jailed route: allocation, publication and inherited fd coverage.

Written before the production change; execution belongs on the Linux box.
The private preparer is the 3ea5a0d6 implementation. The allocation ceiling
is 2.5 times the document, derived from the two mechanisms (see the comment
at the ceiling): the old StringIO peaks at 5 times the document by
construction, the new buffer at about 1.5 times as measured. The old
StringIO realizes its 4 B/char buffer at the first read, so the measurement
includes the real publication path.
"""
from contextlib import ExitStack, redirect_stdout
import io
import json
from pathlib import Path
import sys
import tracemalloc
from types import SimpleNamespace
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import test_run_json_jailed_profile as profiles
import test_run_json_sink_fd as fd_sinks


_MIB = 1024 * 1024


def _old_prepare_report_json(document, resources, args, sink):
    # Frozen from 3ea5a0d6: change the buffer only in production, never here.
    if args.profile == "jailed" or getattr(sink, "fd_mode", False):
        buffer = resources.enter_context(io.StringIO())
        run_json._stream_json(document, buffer)
        buffer.write("\n")
        buffer.seek(0)
        return buffer
    return run_json._prepare_stream_report(
        document, resources, sink.directory if sink is not None else args.spill_dir,
        sink_errors=sink is not None)


class _Stdout:
    """Capture actual UTF-8 bytes; report writes return character counts."""

    def __init__(self, fault=None):
        self.raw = io.BytesIO()
        self.text = io.TextIOWrapper(self.raw, encoding="utf-8", newline="\n")
        self.fault = fault
        self.writes = []
        self.flushes = 0

    def write(self, chunk):
        self.writes.append(len(chunk))
        if len(self.writes) == 2:
            if self.fault == "oserror":
                raise OSError("injected stdout failure")
            if self.fault == "short":
                return self.text.write(chunk[:-1])
        return self.text.write(chunk)

    def flush(self):
        self.flushes += 1
        self.text.flush()

    def getvalue(self):
        self.text.flush()
        return self.raw.getvalue()


class _CountingStdout:
    """Consume the full chunked read-back without retaining another report."""

    def __init__(self):
        self.size = self.calls = self.largest = self.flushes = 0

    def write(self, chunk):
        assert chunk.isascii()
        self.size += len(chunk)
        self.calls += 1
        self.largest = max(self.largest, len(chunk))
        return len(chunk)

    def flush(self):
        self.flushes += 1


def _invoke(source, *, reference=False, pnls=fd_sinks.PNLS, extra=(),
            inject=None, stdout=None):
    # Reuse the neighbouring rows' native-call double, feed, fixed clock,
    # scratch admission and file prohibition; assembly and publication are real.
    library = profiles.ProfileFdLibrary()
    library.pnls = pnls
    output = stdout if stdout is not None else _Stdout()
    builder = run_json.build_report_dict

    def build(*args, **kwargs):
        document = builder(*args, **kwargs)
        inject(document)
        return document

    with ExitStack() as patches:
        if reference:
            patches.enter_context(mock.patch.object(
                run_json, "_prepare_report_json", _old_prepare_report_json))
        if inject is not None:
            patches.enter_context(mock.patch.object(run_json, "build_report_dict", build))
        result = fd_sinks.invoke(
            source, library, extra, no_files=True, stdout=output)
    return result, library, output


def _allocation_document(shape):
    # ASCII only (json.dumps default). "one-string": one 8 MiB string, which
    # the serializer writes in 4096-character windows. "records": a header and
    # 200,000 small equity records written one by one, the real many-small-
    # writes pattern of a report body (about 9 MiB of text).
    if shape == "one-string":
        return {"payload": "x" * (8 * _MIB)}
    return {"schema": "stringio-allocation", "equity": [
        {"t": 1_700_000_000_000 + 60_000 * index, "e": 100000.0 + 0.25 * index, "op": 0.0}
        for index in range(200_000)]}


@pytest.mark.parametrize("shape", ["one-string", "records"])
@pytest.mark.parametrize("profile,fd_mode", [
    ("jailed", False), ("unjailed", True),
], ids=["jailed-no-sink", "fd-summary-buffer"])
def test_write_seek_peak_is_one_encoded_document(profile, fd_mode, shape):
    # Allocate the synthetic input before tracing. Only encoded output storage
    # and the serializer's bounded temporaries belong to the measured peak.
    document = _allocation_document(shape)
    size = len(json.dumps(document, separators=(",", ":"), allow_nan=False)) + 1
    assert size >= 8 * _MIB
    # Ceiling = 2.5 x the encoded document, derived from the two mechanisms,
    # not from an allocator constant. Old: StringIO keeps the written text
    # (1 B per ASCII character) and realizes a 4 B/char buffer at its first
    # read, so its peak is at least 5 x the document by construction. New:
    # the 1 B/char byte buffer plus a bounded read-back working set, measured
    # at 1.50-1.51 x the document on a Linux box with Python 3.12.3 (two
    # shapes, both profiles). 2.5 x lies 2.0 x below the old peak and 1.65 x
    # above the new one, and does not depend on the interpreter's allocator.
    ceiling = size * 5 // 2
    args = SimpleNamespace(profile=profile, spill_dir=None)
    sink = SimpleNamespace(fd_mode=True) if fd_mode else None
    stdout = _CountingStdout()
    with ExitStack() as resources:
        tracemalloc.start()
        try:
            text = run_json._prepare_report_json(document, resources, args, sink)
            after_write = tracemalloc.get_traced_memory()[1]  # write and seek(0)
            # The real 1 MiB read/write/close path is inside the measurement:
            # the old StringIO realizes its 4 B/char buffer at the first read.
            with redirect_stdout(stdout):
                assert run_json._write_report(text) == 0
            peak = tracemalloc.get_traced_memory()[1]  # write, seek(0), reads, close
        finally:
            tracemalloc.stop()
        assert text.closed
    # Diagnostic, not an assertion (shown with -s or -rP): where the peak jumps.
    print(f"PEAKS shape={shape} profile={profile} size={size} "
          f"after_write_seek={after_write} after_read={peak} ceiling={ceiling}")
    assert stdout.size == size and stdout.calls > 1
    assert stdout.largest == _MIB and stdout.flushes == 1
    assert peak <= ceiling, (peak, ceiling, size)  # RED on 3ea5a0d6


def _publication_peak(document, profile, fd_mode):
    # Peak traced bytes of prepare + the real _write_report, and the bytes the
    # consumer received. The document is allocated by the caller before tracing.
    args = SimpleNamespace(profile=profile, spill_dir=None)
    sink = SimpleNamespace(fd_mode=True) if fd_mode else None
    stdout = _CountingStdout()
    with ExitStack() as resources:
        tracemalloc.start()
        try:
            text = run_json._prepare_report_json(document, resources, args, sink)
            with redirect_stdout(stdout):
                assert run_json._write_report(text) == 0
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        assert text.closed
    return peak, stdout.size


@pytest.mark.parametrize("profile,fd_mode", [
    ("jailed", False), ("unjailed", True),
], ids=["jailed-no-sink", "fd-summary-buffer"])
def test_peak_growth_is_about_one_byte_per_added_byte(profile, fd_mode):
    # The slope between two document sizes (8 MiB and 16 MiB) separates the
    # per-byte cost from any fixed read-back term, which cancels. Old StringIO:
    # 1 B/char written text + a 4 B/char realized buffer = 5 per added byte by
    # construction. New: a 1 B/char byte buffer that CPython grows by at most
    # 12.5% over need, so about 1.0-1.125 per added byte. The bound 2.5 lies
    # between the two mechanisms (2.0 x below the old slope), the same
    # derivation as the ceiling of the row above.
    low_document = {"payload": "x" * (8 * _MIB)}
    high_document = {"payload": "x" * (16 * _MIB)}
    low, low_size = _publication_peak(low_document, profile, fd_mode)
    high, high_size = _publication_peak(high_document, profile, fd_mode)
    assert high_size > low_size
    slope = (high - low) / (high_size - low_size)
    print(f"SLOPE profile={profile} low={low} high={high} "
          f"added_bytes={high_size - low_size} slope={slope:.3f}")
    assert slope <= 2.5, (low, high, high_size - low_size, slope)  # RED on 3ea5a0d6


def test_one_read_back_chunk_working_set_is_reported():
    # Diagnostic: the traced peak of ONE text.read(1 MiB) on a prepared buffer,
    # printed, not asserted (the only assertion is that the read returns the
    # requested 1 MiB of text). Measured on a Linux box with Python 3.12.3: one
    # read peaks at 2.0 MiB; the roughly 4 MiB that the whole read-back loop
    # adds above the write peak is therefore not explained by one read, and the
    # remainder (overlap between chunks in the loop) was not measured.
    document = {"payload": "x" * (8 * _MIB)}
    args = SimpleNamespace(profile="jailed", spill_dir=None)
    with ExitStack() as resources:
        text = run_json._prepare_report_json(document, resources, args, None)
        tracemalloc.start()
        try:
            chunk = text.read(_MIB)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        assert len(chunk) == _MIB
        print(f"READ-CHUNK one_read_peak={peak} chunk_chars={len(chunk)}")
        text.close()


@pytest.mark.parametrize("pnls", [(), fd_sinks.PNLS], ids=["zero-trade", "trading"])
def test_jailed_run_stdout_bytes_match_stringio(tmp_path, pnls):
    source = fd_sinks.bars.csv_file(tmp_path)
    extra = ["--profile", "jailed"]
    before, _, _ = _invoke(source, reference=True, pnls=pnls, extra=extra)
    after, library, stdout = _invoke(source, pnls=pnls, extra=extra)
    assert before == after
    assert after[0] == 0 and after[2] == ""
    document = json.loads(after[1])
    assert len(document["trades"]) == len(pnls)
    assert len(document["equity_curve"]) == 5
    assert library.installed == [] and library.arena_admissions
    assert stdout.flushes == 1
    fd_sinks.full.assert_run_profile(document, "jailed", admitted=True)


def test_non_ascii_input_stdout_bytes_match_stringio(tmp_path):
    source = fd_sinks.bars.csv_file(tmp_path)
    value = 'café 漢字 \U0001f680 \u2028 quote"\r\n'
    extra = ["--profile", "jailed", "--inputs", json.dumps({"label": value})]
    before, _, _ = _invoke(source, reference=True, extra=extra)
    after, _, _ = _invoke(source, extra=extra)
    assert before == after
    assert after[0] == 0 and after[2] == ""
    assert json.loads(after[1])["applied_inputs"]["label"] == value
    # The normal JSON writer escapes Unicode, including the fingerprint's
    # provenance; the fingerprint's canonical raw Unicode is base64 encoded.
    assert after[1].isascii()


def test_raw_unicode_text_wrapper_matches_stringio():
    # Future/direct text producers must not acquire an ASCII encoding failure.
    # A JSONColumn also copies already-encoded text straight to its destination.
    value = "café 漢字 \U0001f680\r\n"
    column = object.__new__(run_json._JSONColumn)
    column.file = io.StringIO(json.dumps(value, ensure_ascii=False) + "\r\n")
    document = {"column": column}
    outputs = []
    args = SimpleNamespace(profile="jailed", spill_dir=None)
    for prepare in (_old_prepare_report_json, run_json._prepare_report_json):
        stdout = _Stdout()
        with ExitStack() as resources, redirect_stdout(stdout):
            text = prepare(document, resources, args, None)
            assert run_json._write_report(text) == 0
            assert text.closed
        outputs.append(stdout.getvalue())
    assert outputs[0] == outputs[1]
    assert not outputs[1].isascii() and b"\r\n" in outputs[1]
    assert json.loads(outputs[1]) == {"column": [value]}
    column.file.close()


def test_mid_document_serialization_publishes_no_report_bytes():
    failures = []
    args = SimpleNamespace(profile="jailed", spill_dir=None)
    for prepare in (_old_prepare_report_json, run_json._prepare_report_json):
        visited = []

        def records():
            for index in range(1024):
                visited.append(index)
                yield {"index": index}
            yield {"bad": float("inf")}

        stdout = _Stdout()
        with ExitStack() as resources, redirect_stdout(stdout):
            with pytest.raises(ValueError) as failure:
                text = prepare(
                    {"rows": run_json._JSONRecordArray(records, 1025)},
                    resources, args, None)
                run_json._write_report(text)
        failures.append((type(failure.value), str(failure.value)))
        assert len(visited) == 1024  # a real prefix was serialized privately
        assert stdout.getvalue() == b"" and stdout.writes == []
    assert failures[0] == failures[1]


def test_serialization_failure_keeps_main_failure_line_and_status(tmp_path):
    source = fd_sinks.bars.csv_file(tmp_path)

    def poison(document):
        document["late_serialization_failure"] = float("inf")

    extra = ["--profile", "jailed"]
    before, _, _ = _invoke(source, reference=True, extra=extra, inject=poison)
    after, _, stdout = _invoke(source, extra=extra, inject=poison)
    assert before == after
    assert after[0] == 1 and after[2] == ""
    assert len(after[1].splitlines()) == 1 and len(stdout.writes) == 1
    failure = json.loads(after[1])
    assert failure["code"] == "report_result_invalid"
    assert failure["args"] == {"field": "report.document", "reason": "nonfinite"}
    assert failure["error"] == "the report document holds a non-finite number"


@pytest.mark.parametrize("fault", ["oserror", "short"])
def test_mid_write_failure_is_stderr_only_with_status_one(tmp_path, fault):
    source = fd_sinks.bars.csv_file(tmp_path)
    payload = "x" * (2 * _MIB)
    prepared = []
    prepare = run_json._prepare_report_json

    def watch(*args):
        text = prepare(*args)
        prepared.append(text)
        return text

    def enlarge(document):
        document["chunk_probe"] = payload

    extra = ["--profile", "jailed"]
    before, _, _ = _invoke(
        source, reference=True, extra=extra, inject=enlarge, stdout=_Stdout(fault))
    with mock.patch.object(run_json, "_prepare_report_json", watch):
        after, _, stdout = _invoke(
            source, extra=extra, inject=enlarge, stdout=_Stdout(fault))
    assert before == after
    assert after[0] == 1 and prepared[0].closed
    assert stdout.writes == [_MIB, _MIB] and stdout.flushes == 0
    reason = "injected stdout failure" if fault == "oserror" else "short report write"
    assert after[2] == f"run_json: the report could not be written: {reason}\n"
    assert len(after[1]) == (_MIB if fault == "oserror" else 2 * _MIB - 1)
    assert after[1].startswith(b'{"engine":"pineforge",')
    assert b'"error":' not in after[1]  # no failure JSON after the report prefix


@pytest.mark.parametrize("profile", ["jailed", "unjailed"])
def test_fd_run_builds_only_bounded_summary_and_preserves_bytes(tmp_path, profile):
    count = 32768
    source = fd_sinks.bars.csv_file(tmp_path, count=count)
    prepare = run_json._prepare_report_json
    prepared = []

    def watch(document, resources, args, sink):
        assert sink.fd_mode
        assert "trades" not in document and "equity_curve" not in document
        text = prepare(document, resources, args, sink)
        text.seek(0, io.SEEK_END)
        size = text.tell()  # UTF-8 byte position; no full read-back allocation
        text.seek(0)
        prepared.append((text, size))
        return text

    results, wires = [], []
    for reference in (True, False):
        with fd_sinks.Stream() as stream, ExitStack() as patches:
            if not reference:
                patches.enter_context(mock.patch.object(
                    run_json, "_prepare_report_json", watch))
            result, library, stdout = _invoke(
                source, reference=reference,
                extra=["--profile", profile, "--report-sink-fd", str(stream.writer)])
        results.append(result)
        wires.append(stream.data)
        assert result[0] == 0 and result[2] == ""
        assert library.installed == library.freed == [stream.writer]
        assert stdout.flushes == 1
    assert results[0] == results[1] and wires[0] == wires[1]
    assert len(prepared) == 1 and prepared[0][0].closed
    assert prepared[0][1] == len(results[1][1]) <= 32 * 1024
    document = json.loads(results[1][1])
    assert document["summary"]["bars_processed"] == count
    assert "trades" not in document and "equity_curve" not in document
    fd_sinks.full.assert_run_profile(document, profile, admitted=profile == "jailed")
    with fd_sinks.pf_sink.SinkReader(stream=io.BytesIO(wires[1])) as reader:
        equity_count = sum(record.tag == "E" for record in reader)
        assert reader.complete
    assert equity_count == count and len(wires[1]) > _MIB
