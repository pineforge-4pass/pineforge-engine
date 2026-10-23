"""P2 run_json regressions; execute on the lab box, never on the executor Mac."""
from contextlib import ExitStack, contextmanager, redirect_stderr, redirect_stdout
from pathlib import Path
import argparse
import errno
import fcntl
import io
import json
import os
import socket
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import run_json
import test_run_json_sink as ordinary


def test_bounded_artifact_syncs_at_most_once(tmp_path, monkeypatch):
    """E-04: completed writes reuse one window; publication is the sync point."""
    truncates, syncs = [], []
    real_truncate = os.ftruncate
    real_sync = getattr(os, "fdatasync", os.fsync)
    payload = "0123456789abcdef" * 4096
    tail = "final partial block"
    with tempfile.TemporaryFile(mode="w+b", buffering=0, dir=tmp_path) as file:
        fd = file.fileno()

        def truncate(descriptor, size):
            if descriptor == fd:
                truncates.append(size)
            return real_truncate(descriptor, size)

        def sync(descriptor):
            if descriptor == fd:
                syncs.append(descriptor)
            return real_sync(descriptor)

        monkeypatch.setattr(os, "ftruncate", truncate)
        monkeypatch.setattr(os, "fdatasync", sync, raising=False)
        artifact = run_json._BoundedJSONFile(file, require_disk=False)
        try:
            for _ in range(384):
                artifact.write(payload)
            artifact.write(tail)
            artifact.flush()
            artifact.flush()
            artifact.seek(0)
            for _ in range(384):
                assert artifact.read(len(payload)) == payload
            assert artifact.read(4096) == tail
            assert artifact.read(1) == ""
            assert os.fstat(fd).st_size == 384 * len(payload) + len(tail)
        finally:
            artifact.close()
    # One changed artifact has one explicit durability boundary, including
    # its exact final length. Rewinds and cleanup must not synchronize again.
    assert len(truncates) == 1, truncates
    assert len(syncs) == 1, syncs


def test_sink_report_reads_endpoints_once(tmp_path, monkeypatch):
    """E-05: summary, validation, and at most one endpoint pass suffice."""
    path = tmp_path / "endpoints.sink"
    first = ordinary.bars.T0
    last = first + 86_400_000
    rows = [
        ["E", first, 10000.0, -0.0],
        ["E", first + 43_200_000, 10001.0, 0.0],
        ["E", last, 10002.0, 0.0],
    ]
    counts, digest = ordinary.write_sink(path, rows)
    sink = run_json._ReportSink.__new__(run_json._ReportSink)
    sink.path, sink.header = str(path), ordinary.header()
    sink.counts, sink.sha256, sink.named = counts, digest, False
    report = run_json.ReportC()
    report.input_bars_processed = report.script_bars_processed = 3
    report.metrics.equity.cagr = 1.0
    report.metrics.equity.calmar = 2.0
    opened = []
    open_sink = run_json._open_uncached_sink

    def counted_open(source):
        opened.append(source)
        return open_sink(source)

    monkeypatch.setattr(run_json, "_open_uncached_sink", counted_open)
    document = run_json._build_sink_report(
        report, Path("fixture.csv"), 3, first, last, 0.0, {}, {}, {}, sink,
        window_anchor=True)
    # One pass folds T, one validates E. The endpoint budget is at most one
    # more pass, independent of curve length or diagnostic field count.
    assert opened and all(source == str(path) for source in opened)
    assert len(opened) <= 3, opened
    assert "annualization" not in document["diagnostics"]
    assert list(document["equity_curve"]) == [
        {"time_ms": row[1], "equity": row[2], "open_profit": row[3]}
        for row in rows
    ]


@pytest.mark.parametrize("window_anchor", [False, True])
def test_annualization_reads_each_endpoint_once(window_anchor):
    """E-05: one-day diagnostics must reuse both decoded endpoint records."""
    points = [
        {"time_ms": 0, "equity": 10000.0, "open_profit": -0.0},
        {"time_ms": 86_400_000, "equity": 10002.0, "open_profit": 0.0},
    ]
    accesses = []

    class Curve:
        def __len__(self):
            return len(points)

        def __getitem__(self, index):
            accesses.append(index)
            return points[index]

    reasons = run_json._annualization_diagnostics(
        Curve(), {"equity": {"cagr": 1.0, "calmar": 2.0}},
        window_anchor=window_anchor)
    assert reasons == {}
    assert accesses == [0, -1], accesses


@contextmanager
def _sink_descriptor(tmp_path, kind):
    """Keep test-owned backing alive; duplicate away from reserved/runtime fds."""
    with ExitStack() as resources:
        if kind in ("closed", "regular"):
            source = resources.enter_context(
                tempfile.TemporaryFile(mode="w+b", dir=tmp_path)).fileno()
        elif kind == "tcp":
            source = resources.enter_context(
                socket.socket(socket.AF_INET, socket.SOCK_STREAM)).fileno()
        elif kind in ("unix-stream", "unix-datagram"):
            type_ = socket.SOCK_STREAM if kind == "unix-stream" else socket.SOCK_DGRAM
            reader, writer = socket.socketpair(socket.AF_UNIX, type_)
            resources.enter_context(reader)
            source = resources.enter_context(writer).fileno()
        else:
            assert kind == "fifo"
            reader, source = os.pipe()
            resources.callback(os.close, reader)
            resources.callback(os.close, source)
        descriptor = fcntl.fcntl(source, fcntl.F_DUPFD, 64)
        if kind == "closed":
            os.close(descriptor)
        try:
            yield descriptor
        finally:
            if kind != "closed":
                try:
                    os.close(descriptor)
                except OSError as error:
                    assert error.errno == errno.EBADF


def _sink_args(descriptor):
    return argparse.Namespace(
        profile="unjailed", report_sink_fd=descriptor, report_sink=None,
        spill_dir=None, bench=False, validate_window_only=False,
        run_phase_fd=None, report_sink_format=1, run_id="")


@pytest.mark.parametrize("kind", ["closed", "regular", "tcp", "unix-datagram"])
def test_report_sink_fd_must_be_open_stream_at_startup(tmp_path, monkeypatch, kind):
    """E-08: refuse before library loading and retain invalid-fd ownership."""
    source = ordinary.bars.csv_file(tmp_path)
    loaded, closed = [], []
    real_close = os.close

    def load(path):
        loaded.append(path)
        raise AssertionError("invalid sink descriptor reached load_strategy")

    def close(descriptor):
        closed.append(descriptor)
        return real_close(descriptor)

    with _sink_descriptor(tmp_path, kind) as descriptor:
        stdout, stderr = io.StringIO(), io.StringIO()
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "load_strategy", load)
            patch.setattr(os, "close", close)
            with redirect_stdout(stdout), redirect_stderr(stderr):
                status = run_json.main([
                    "--so", "unused.so", "--ohlcv", str(source),
                    "--report-sink-fd", str(descriptor)])
        assert loaded == [], loaded
        assert descriptor not in closed, closed
        assert status == 2 and stderr.getvalue() == "", stdout.getvalue()
        failure = json.loads(stdout.getvalue())
        assert failure["code"] == "run_request_invalid"
        assert failure["args"] == {"option": "arguments"}
        if kind != "closed":
            os.fstat(descriptor)


@pytest.mark.parametrize("kind", ["closed", "regular", "tcp", "unix-datagram"])
def test_inherited_sink_owner_never_closes_an_unvalidated_fd(tmp_path, monkeypatch, kind):
    """E-08: startup-abort and direct-owner paths must validate before claiming."""
    closed, real_close = [], os.close

    def close(descriptor):
        closed.append(descriptor)
        return real_close(descriptor)

    with _sink_descriptor(tmp_path, kind) as descriptor:
        with monkeypatch.context() as patch:
            patch.setattr(os, "close", close)
            with pytest.raises(run_json.RunFailure) as refused:
                owner = run_json._InheritedSinkFd(descriptor)
                owner.close()
        assert refused.value.code == "run_request_invalid"
        assert descriptor not in closed, closed
        if kind != "closed":
            os.fstat(descriptor)


@pytest.mark.parametrize("kind", ["fifo", "unix-stream"])
def test_report_sink_fd_accepts_fifo_and_unix_stream(tmp_path, monkeypatch, kind):
    """E-08: admission borrows a valid endpoint; its owner closes it once."""
    with _sink_descriptor(tmp_path, kind) as descriptor:
        run_json._validate_sink_options(_sink_args(descriptor))
        assert os.write(descriptor, b"valid stream") == len(b"valid stream")
        closed, real_close = [], os.close

        def close(number):
            if number == descriptor:
                closed.append(number)
            return real_close(number)

        with monkeypatch.context() as patch:
            patch.setattr(os, "close", close)
            owner = run_json._InheritedSinkFd(descriptor)
            owner.close()
            owner.close()
        assert closed == [descriptor]
        with pytest.raises(OSError) as absent:
            os.fstat(descriptor)
        assert absent.value.errno == errno.EBADF


@pytest.mark.parametrize("document", [
    {2: 2, None: 3, 1.5: 4, True: 5, False: 6, -7: -0.0, "text": "漢\n\""},
    {-0.0: {"nested": {None: [True, 1.5, "value"]}}},
    {1: 2, None: 3, 1.5: 4, True: 5},
])
def test_stream_json_matches_json_dump_for_nonstring_keys(document):
    """E-14: JSON object keys use json.dump's coercion and quoting rules."""
    expected, actual = io.StringIO(), io.StringIO()
    json.dump(document, expected, separators=(",", ":"), allow_nan=False)
    run_json._stream_json(document, actual)
    assert actual.getvalue() == expected.getvalue()
    assert json.loads(actual.getvalue()) == json.loads(expected.getvalue())


@pytest.mark.parametrize("key", [(1, 2), frozenset({1})])
def test_stream_json_refuses_keys_json_dump_refuses(key):
    document = {key: "value"}
    with pytest.raises(TypeError):
        json.dump(document, io.StringIO(), separators=(",", ":"), allow_nan=False)
    with pytest.raises(TypeError):
        run_json._stream_json(document, io.StringIO())


@pytest.mark.parametrize("named_sink", [False, True])
def test_bench_states_install_no_sink(tmp_path, monkeypatch, named_sink):
    """E-15: only the untimed body run installs the caller/default path sink."""
    extra = ["--report-sink", str(tmp_path / "body.sink")] if named_sink else []

    class BenchLibrary(ordinary.SinkLibrary):
        def install(self, state, pointer):
            # The native setter creates/truncates a named path itself. The
            # shared fixture expects that file to be empty on setter entry.
            Path(os.fsdecode(pointer._obj.path)).write_bytes(b"")
            return super().install(state, pointer)

    reference = ordinary.invoke(
        tmp_path, monkeypatch, library=BenchLibrary(), extra=extra)
    assert reference[0] == 0 and reference[2] == "", reference[1]
    library = BenchLibrary()
    observed = []
    run = library.run_backtest_full

    def timed_run(*args):
        observed.append(library.path)
        return run(*args)

    library.run_backtest_full = timed_run
    ticks = iter((100, 125, 200, 225, 300, 325))
    monkeypatch.setattr(run_json.time, "perf_counter_ns", lambda: next(ticks))
    status, text, stderr, library = ordinary.invoke(
        tmp_path, monkeypatch, library=library,
        extra=[*extra, "--bench", "--warmup", "2", "--repeats", "3"])
    assert status == 0 and stderr == "", text
    assert len(observed) == 6
    assert observed[:5] == [None] * 5, observed
    assert len(library.installed) == len(library.freed) == 1
    assert observed[-1] == library.installed[0]
    document = json.loads(text)
    assert document["diagnostics"]["timing"]["samples_ns"] == [25, 25, 25]
    document["diagnostics"].pop("timing")
    document["diagnostics"].pop("throughput")
    assert json.dumps(document, separators=(",", ":"), allow_nan=False) + "\n" == reference[1]
