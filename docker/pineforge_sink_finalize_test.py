"""CX-SINK-B4: real pipe transport, strict refusal, atomic cleanup and RSS.

CTest's native row supplies the existing L6B1 strategy module: the round
trip then uses its real path sink through run_json's existing ctypes owner.
The ordinary row reuses tests/test_run_json_sink.py's v1 fixture writer.
No sink fixture or hash is recorded here. The RSS probe runs this file in
a fresh stdlib-only process, before pytest/run_json are imported, so
RUSAGE_CHILDREN describes just the finalizer rather than pytest's children
or a large inherited pytest heap. It generates and hashes >=64 MiB on the
fly and requires peak RSS <20 MiB (less than one third of the input).
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys


DOCKER = Path(__file__).resolve().parent
TOOL = DOCKER / "pineforge_sink_finalize.py"
LAUNCHER = DOCKER / "pineforge-sink-finalize"
TAGS = "ETOVKXH"
RECEIPT_KEYS = ("complete", *(f"count_{tag.lower()}" for tag in TAGS),
                "sink_bytes", "sha256", "finalizer_version")
RSS_LIMIT_BYTES = 20 * 1024 * 1024
STREAM_BYTES = 64 * 1024 * 1024


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8") + b"\n"


def write_all(fd, data):
    remaining = memoryview(data)
    while remaining:
        count = os.write(fd, remaining)
        if count <= 0:
            raise OSError("test writer made no progress")
        remaining = remaining[count:]


def rss_probe(directory, header_path):
    """One child only; do not import pytest, run_json or the test fixtures."""
    directory = Path(directory)
    out = directory / "large.sink"
    head = json.loads(Path(header_path).read_bytes())
    counts = {item["type"]: 0 for item in head["records"]}
    logical, wire = hashlib.sha256(), hashlib.sha256()
    total = 0
    in_read, in_write = os.pipe()
    process = subprocess.Popen(
        [sys.executable, "-B", "-S", str(TOOL), "--in-fd", str(in_read), "--out", str(out)],
        pass_fds=(in_read,), stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=directory)
    os.close(in_read)

    def send(line, *, trailer=False):
        nonlocal total
        total += len(line)
        wire.update(line)
        if not trailer:
            logical.update(line)
        write_all(in_write, line)

    try:
        send(encoded(head))
        message = "s" * (16 * 1024)
        while total < STREAM_BYTES:
            number = counts["V"] + 1
            send(encoded(["V", number, 0, number - 1, None, None,
                          0, 1, 1, 1.0, "0123456789abcdef", message]))
            counts["V"] += 1
        send(encoded(["K", 50.0]))
        counts["K"] += 1
        send(encoded({"trailer": "pf-report-sink", "version": 1,
                      "counts": counts, "sha256": logical.hexdigest()}), trailer=True)
    finally:
        os.close(in_write)
    stdout, stderr = process.communicate(timeout=90)
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    # Darwin reports bytes; Linux and the lab's BSD-style interface use KiB.
    peak_bytes = int(peak if sys.platform == "darwin" else peak * 1024)
    print(json.dumps({"returncode": process.returncode, "stderr": stderr.decode(),
                      "receipt": json.loads(stdout), "peak_bytes": peak_bytes,
                      "expected_counts": counts, "expected_bytes": total,
                      "expected_sha256": logical.hexdigest(),
                      "expected_wire_sha256": wire.hexdigest()}, separators=(",", ":")))
    return process.returncode


if __name__ == "__main__" and len(sys.argv) == 4 and sys.argv[1] == "--rss-probe":
    raise SystemExit(rss_probe(sys.argv[2], sys.argv[3]))


import ctypes
import errno
import socket
import stat
import threading
import time

import pytest

sys.path.insert(0, str(DOCKER))
sys.path.insert(0, str(DOCKER.parent / "tests"))

import pf_sink
import pineforge_sink_finalize as finalizer
import test_run_json_sink as fixtures


def trade(number=1):
    return ["T", number, "long", 1700000040000, 1700000100000,
            100.0, 101.0, 2.0, 1.0, 0.5, 2.0, -1.0, 0.25,
            0, 1, False, number, 0, 'L"\n漢', "", ""]


def sample_rows():
    return [["E", 1700000040000, 10000.0, -0.0],
            ["E", 1700000100000, 10001.0, None],
            trade(),
            ["O", 0, 1700000040000, 1700000100000, -0.0, 4294967295.0],
            ["O", 1, None, None, None, 1.0],
            ["V", 1, 1, 0, 1700000040000, 1700000100000,
             0, 1, 1, None, "0123456789abcdef", 'quote"\n漢'],
            ["K", 50.0]]


def fixture_stream(tmp_path, rows=None, *, outputs=True):
    source = tmp_path / "source.sink"
    fixtures.write_sink(source, sample_rows() if rows is None else rows, outputs=outputs)
    return source.read_bytes()


def refold(lines):
    """Independent test encoder; recompute counts and logical hash after mutation."""
    head = json.loads(lines[0])
    counts = {item["type"]: 0 for item in head["records"]}
    logical = hashlib.sha256()
    for line in lines:
        logical.update(line)
    for line in lines[1:]:
        counts[json.loads(line)[0]] += 1
    return b"".join(lines) + encoded({"trailer": "pf-report-sink", "version": 1,
                                    "counts": counts, "sha256": logical.hexdigest()})


def reference(path):
    with pf_sink.SinkReader(path) as reader:
        for _ in reader:
            pass
        assert reader.complete
        return reader.header, reader.counts, reader.sha256


def native_stream(tmp_path, module):
    """The same real module and temporary path owner as the L9A Python tests."""
    runner = fixtures.run_json
    lib = runner.load_strategy(Path(module))
    state = lib.strategy_create(b"MOCK")
    assert state
    report, sink = runner.ReportC(), None
    try:
        sink = runner._TemporaryReportSink(lib, state, directory=tmp_path, run_id='finalize"\n漢')
        bars = (runner.BarC * 8)(*(
            runner.BarC(100.0 + i * .25, 100.5 + i * .25, 99.5 + i * .25,
                        100.125 + i * .25, 1.0, 1700000040000 + i * 60000)
            for i in range(8)))
        lib.run_backtest_full(state, bars, len(bars), b"1", b"1", 0, 4, 0,
                              ctypes.byref(report))
        sink.verify(lib, state, report)
        assert sink.counts["E"] == len(bars) and sink.counts["T"] > 0
        return Path(sink.path).read_bytes()
    finally:
        runner._free_sink_run(lib, state, report, sink)


def invoke(tmp_path, payload, *, extra=(), receipt_pipe=False, unix_socket=False,
           tiny=False, out=None, explicit_spool=True, launcher=False):
    out = tmp_path / "out.sink" if out is None else out
    spool = tmp_path / "spool"
    spool.mkdir(exist_ok=True)
    sockets = socket.socketpair() if unix_socket else None
    in_read, in_write = ((sockets[0].fileno(), sockets[1].fileno())
                         if sockets else os.pipe())
    receipt_read, receipt_write = os.pipe() if receipt_pipe else (None, None)
    inherited = [in_read]
    command = [sys.executable, "-B", "-S", str(LAUNCHER if launcher else TOOL),
               "--in-fd", str(in_read), "--out", str(out)]
    if explicit_spool:
        command += ["--spool-dir", str(spool)]
    if receipt_pipe:
        inherited.append(receipt_write)
        command += ["--receipt-fd", str(receipt_write)]
    command += list(extra)
    process = subprocess.Popen(command, pass_fds=inherited, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=tmp_path)
    if sockets:
        sockets[0].close()
    else:
        os.close(in_read)
    if receipt_pipe:
        os.close(receipt_write)
    writer_errors = []

    def feed():
        try:
            step = 3 if tiny else 64 * 1024
            for start in range(0, len(payload), step):
                write_all(in_write, payload[start:start + step])
                if tiny:
                    time.sleep(.0001)
        except BrokenPipeError:
            pass  # refusal can close the read end before all input is sent
        except BaseException as error:
            writer_errors.append(error)
        finally:
            if sockets:
                sockets[1].close()
            else:
                os.close(in_write)

    writer = threading.Thread(target=feed, daemon=True)
    writer.start()
    try:
        stdout, stderr = process.communicate(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        writer.join(timeout=5)
    assert not writer.is_alive(), "input writer did not finish"
    assert not writer_errors, writer_errors
    if receipt_pipe:
        with os.fdopen(receipt_read, "rb") as file:
            receipt_text = file.read()
        assert stdout == b""
    else:
        receipt_text = stdout
    result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    receipt = json.loads(receipt_text) if receipt_text else None
    if receipt is not None:
        assert receipt_text == encoded(receipt)
        assert tuple(receipt) == RECEIPT_KEYS
    return result, receipt, out, spool


def assert_failure(result, receipt, out, spool, code):
    assert result.returncode == code, result.stderr.decode()
    assert receipt["complete"] is False and receipt["sha256"] == ""
    assert result.stderr
    assert not os.path.lexists(out)
    assert list(spool.iterdir()) == []


def assert_success(result, receipt, out, spool, payload):
    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    assert out.read_bytes() == payload
    assert stat.S_IMODE(out.stat().st_mode) == 0o600
    _, counts, digest = reference(out)
    assert receipt["complete"] is True
    assert receipt["sink_bytes"] == len(payload)
    assert receipt["sha256"] == digest
    assert receipt["finalizer_version"] == "1.0.0"
    assert {tag: receipt["count_" + tag.lower()] for tag in TAGS} == {
        tag: counts.get(tag, 0) for tag in TAGS}
    assert digest != hashlib.sha256(payload).hexdigest()  # trailer is excluded
    assert list(spool.iterdir()) == []


def test_pipe_round_trip_from_path_writer(tmp_path):
    module = os.environ.get("PINEFORGE_FINALIZE_STRATEGY_SO")
    payload = native_stream(tmp_path, module) if module else fixture_stream(tmp_path)
    expected_id = json.loads(payload.splitlines()[0])["run_id"]
    result, receipt, out, spool = invoke(tmp_path, payload, extra=("--run-id", expected_id))
    assert_success(result, receipt, out, spool, payload)


@pytest.mark.parametrize("tiny,unix_socket,receipt_pipe,launcher", [
    (True, False, False, False), (True, True, True, False),
    (False, False, True, True),
])
def test_chunking_socket_receipt_fd_and_launcher(tmp_path, tiny, unix_socket, receipt_pipe, launcher):
    payload = fixture_stream(tmp_path)
    result, receipt, out, spool = invoke(
        tmp_path, payload, tiny=tiny, unix_socket=unix_socket,
        receipt_pipe=receipt_pipe, launcher=launcher, explicit_spool=False)
    assert_success(result, receipt, out, spool, payload)
    assert list(tmp_path.glob(".pineforge-sink-*.spool")) == []


@pytest.mark.parametrize("prefix", ["empty", "header", "records",
                                  "mid-header", "mid-record", "mid-trailer"])
def test_eof_without_trailer_removes_output_and_spool(tmp_path, prefix):
    payload = fixture_stream(tmp_path)
    lines = payload.splitlines(keepends=True)
    if prefix == "empty":
        truncated = b""
    elif prefix == "header":
        truncated = lines[0]
    elif prefix == "mid-header":
        truncated = lines[0][:len(lines[0]) // 2]
    elif prefix == "mid-record":
        truncated = lines[0] + lines[1][:len(lines[1]) // 2]
    elif prefix == "mid-trailer":
        truncated = b"".join(lines[:-1]) + lines[-1][:len(lines[-1]) // 2]
    else:
        truncated = b"".join(lines[:-1])
    out = tmp_path / "out.sink"
    out.write_bytes(b"old completed run must not survive")
    result, receipt, out, spool = invoke(tmp_path, truncated)
    assert_failure(result, receipt, out, spool, 6)
    assert receipt["sink_bytes"] == len(truncated)
    if prefix in ("records", "mid-trailer"):
        _, counts, _ = reference(tmp_path / "source.sink")
        assert {tag: receipt["count_" + tag.lower()] for tag in TAGS} == {
            tag: counts.get(tag, 0) for tag in TAGS}
    else:
        assert all(receipt["count_" + tag.lower()] == 0 for tag in TAGS)


@pytest.mark.parametrize("fault,code", [
    ("bad-json", 3), ("wrong-type", 3), ("whitespace", 3), ("bom", 3),
    ("duplicate-key", 3), ("header-order", 3), ("swapped", 3),
    ("missing-lf", 6), ("hash", 4), ("count", 4), ("trailer-shape", 4),
    ("extra", 4), ("second-trailer", 4), ("wrong-run-id", 3), ("oversize", 3),
])
def test_corruption_never_publishes(tmp_path, fault, code):
    payload = fixture_stream(tmp_path)
    lines = payload.splitlines(keepends=True)
    extra = ()
    if fault == "bad-json":
        lines[1] = b'["E",broken]\n'
    elif fault == "wrong-type":
        lines[1] = encoded(["E", True, 10000.0, 0.0])
    elif fault == "whitespace":
        lines[1] = b" " + lines[1]
    elif fault == "bom":
        lines[0] = b"\xef\xbb\xbf" + lines[0]
    elif fault == "duplicate-key":
        lines[0] = lines[0].replace(b'{"schema":', b'{"version":1,"schema":', 1)
    elif fault == "header-order":
        head = json.loads(lines[0])
        head["records"][0], head["records"][1] = head["records"][1], head["records"][0]
        lines[0] = encoded(head)
    elif fault == "swapped":
        lines[1], lines[2] = lines[2], lines[1]
    elif fault == "missing-lf":
        lines[-1] = lines[-1][:-1]
    elif fault in ("hash", "count", "trailer-shape"):
        trailer = json.loads(lines[-1])
        if fault == "hash":
            trailer["sha256"] = ("0" if trailer["sha256"][0] != "0" else "1") + trailer["sha256"][1:]
        elif fault == "count":
            trailer["counts"]["E"] += 1
        else:
            trailer["extra"] = True
        lines[-1] = encoded(trailer)
    elif fault == "extra":
        lines.append(encoded(["E", 1700000160000, 10002.0, 0.0]))
    elif fault == "second-trailer":
        lines.append(lines[-1])
    elif fault == "wrong-run-id":
        extra = ("--run-id", "different")
    elif fault == "oversize":
        lines[1] = b'"' + b"x" * pf_sink.MAX_LINE_BYTES + b'"\n'
    result, receipt, out, spool = invoke(tmp_path, b"".join(lines), extra=extra)
    assert_failure(result, receipt, out, spool, code)


@pytest.mark.parametrize("kind", ["E", "T", "O", "K"])
def test_record_order_is_checked_even_with_a_refolded_valid_receipt(tmp_path, kind):
    rows = sample_rows()
    if kind == "E":
        rows[0], rows[1] = rows[1], rows[0]
    elif kind == "T":
        rows[2][1] = 2
    elif kind == "O":
        rows[3], rows[4] = rows[4], rows[3]
    else:
        rows.append(["K", 51.0])
    payload = fixture_stream(tmp_path, rows)
    # SinkReader accepts these wire values and the independent counts/hash;
    # the finalizer must additionally enforce the producer's row order.
    reference(tmp_path / "source.sink")
    result, receipt, out, spool = invoke(tmp_path, payload)
    assert_failure(result, receipt, out, spool, 3)


def test_all_seven_counts_and_legal_interleaving(tmp_path):
    head = fixtures.header(outputs=True)
    for tag, columns in (("X", pf_sink._X), ("H", pf_sink._H)):
        head["records"].append(fixtures.descriptor(tag, *(
            [column[index] for column in columns] for index in range(3))))
    rows = sample_rows()
    rows = [rows[0], rows[2], rows[3], rows[5], rows[1], rows[4], rows[6],
            ["X", 1700000100000, 1, "trace", -0.0], ["H", "abcdef0123456789"]]
    payload = refold([encoded(head), *(encoded(row) for row in rows)])
    result, receipt, out, spool = invoke(tmp_path, payload)
    assert_success(result, receipt, out, spool, payload)
    assert receipt["count_x"] == receipt["count_h"] == 1


def test_exact_line_limit_and_utf8_run_id(tmp_path):
    head = fixtures.header(outputs=True)
    head["run_id"] = 'run"\n漢'
    event = ["V", 1, 0, 0, None, None, 0, 1, 1, None, "0123456789abcdef", ""]
    event[-1] = "x" * (pf_sink.MAX_LINE_BYTES - len(encoded(event)))
    line = encoded(event)
    assert len(line) == pf_sink.MAX_LINE_BYTES
    payload = refold([encoded(head), line])
    result, receipt, out, spool = invoke(tmp_path, payload, extra=("--run-id", head["run_id"]))
    assert_success(result, receipt, out, spool, payload)


@pytest.mark.parametrize("cap_kind", ["zero", "short", "exact"])
def test_size_cap_and_exact_fit(tmp_path, cap_kind):
    payload = fixture_stream(tmp_path)
    cap = 0 if cap_kind == "zero" else len(payload) - 1 if cap_kind == "short" else len(payload)
    result, receipt, out, spool = invoke(tmp_path, payload, extra=("--max-bytes", str(cap)))
    if cap_kind == "exact":
        assert_success(result, receipt, out, spool, payload)
    else:
        assert_failure(result, receipt, out, spool, 5)
        assert receipt["sink_bytes"] == cap + 1


def test_unwritable_output_path(tmp_path):
    payload = fixture_stream(tmp_path)
    parent = tmp_path / "not-a-directory"
    parent.write_bytes(b"file")
    result, receipt, out, spool = invoke(tmp_path, payload, out=parent / "out.sink")
    # ENOTDIR is deterministic even when the lab runs tests as uid 0.
    assert_failure(result, receipt, out, spool, 5)


def test_invalid_receipt_fd_removes_an_already_verified_output(tmp_path):
    payload = fixture_stream(tmp_path)
    result, receipt, out, spool = invoke(tmp_path, payload, extra=("--receipt-fd", "999999"))
    assert result.returncode == 5 and result.stdout == b"" and receipt is None
    assert b"receipt write failed" in result.stderr
    assert not out.exists() and list(spool.iterdir()) == []


def test_unreadable_input_fd_is_io_failure(tmp_path):
    payload = fixture_stream(tmp_path)
    result, receipt, out, spool = invoke(tmp_path, payload, extra=("--in-fd", "999999"))
    assert_failure(result, receipt, out, spool, 5)
    assert receipt["sink_bytes"] == 0


def test_stderr_can_be_reserved_for_a_failure_receipt(tmp_path):
    payload = fixture_stream(tmp_path)
    result, receipt, out, spool = invoke(
        tmp_path, payload, extra=("--run-id", "wrong", "--receipt-fd", "2"))
    assert result.returncode == 3 and result.stdout == b"" and receipt is None
    document = json.loads(result.stderr)
    assert tuple(document) == RECEIPT_KEYS and document["complete"] is False
    assert result.stderr == encoded(document)
    assert not out.exists() and list(spool.iterdir()) == []


@pytest.mark.parametrize("arguments", [
    [], ["--in-fd", "-1", "--out", "out"], ["--in-fd", "3"],
    ["--in-fd", "3", "--out", "out", "--max-bytes", "-1"],
    ["--in-fd", "3", "--out", "out", "--receipt-fd", "3"],
    ["--in-fd", "3", "--out", "out", "--unknown"],
    ["--in-fd", "2147483648", "--out", "out"],
])
def test_usage_is_exit_two_with_no_receipt(tmp_path, arguments):
    result = subprocess.run([sys.executable, "-B", "-S", str(TOOL), *arguments],
                            stdin=subprocess.DEVNULL, capture_output=True, cwd=tmp_path, timeout=10)
    assert result.returncode == 2 and result.stdout == b""
    assert b"usage:" in result.stderr
    assert list(tmp_path.iterdir()) == []


def test_help_contains_the_contract(tmp_path):
    result = subprocess.run([sys.executable, "-B", "-S", str(TOOL), "--help"],
                            stdin=subprocess.DEVNULL, capture_output=True, cwd=tmp_path, timeout=10)
    assert result.returncode == 0 and result.stderr == b""
    text = result.stdout.decode()
    for option in ("--in-fd", "--out", "--run-id", "--spool-dir", "--max-bytes", "--receipt-fd"):
        assert option in text
    for code in (0, 2, 3, 4, 5, 6, 7):
        assert f"  {code}  " in text


def call_in_process(tmp_path, payload, monkeypatch, *, fault=None):
    source = tmp_path / "input.sink"
    source.write_bytes(payload)
    receipt_path = tmp_path / "receipt.json"
    spool = tmp_path / "spool"
    spool.mkdir()
    out = tmp_path / "out.sink"
    if fault == "fsync":
        def fail_sync(fd):
            raise OSError(errno.ENOSPC, "injected spool fsync failure")
        monkeypatch.setattr(finalizer.os, "fsync", fail_sync)
    elif fault in ("rename", "cross-device"):
        def fail_rename(src, dst):
            raise OSError(errno.EXDEV if fault == "cross-device" else errno.EACCES,
                          "injected rename failure")
        monkeypatch.setattr(finalizer.os, "replace", fail_rename)
    elif fault == "internal":
        def fail_verify(*args):
            raise RuntimeError("injected internal failure")
        monkeypatch.setattr(finalizer, "_verify", fail_verify)
    elif fault == "reader-io":
        def fail_open(*args, **kwargs):
            raise OSError(errno.EIO, "injected strict-reader open failure")
        monkeypatch.setattr(pf_sink, "open", fail_open, raising=False)
    elif fault == "receipt-internal":
        def fail_document(self, complete):
            raise RuntimeError("injected receipt construction failure")
        monkeypatch.setattr(finalizer._Receipt, "document", fail_document)
    with source.open("rb") as input_file, receipt_path.open("w+b") as receipt_file:
        code = finalizer.main(["--in-fd", str(input_file.fileno()), "--out", str(out),
                               "--spool-dir", str(spool), "--receipt-fd", str(receipt_file.fileno())])
        receipt_file.seek(0)
        receipt_text = receipt_file.read()
        receipt = json.loads(receipt_text) if receipt_text else None
    return code, receipt, out, spool


@pytest.mark.parametrize("fault,expected", [
    ("fsync", 5), ("rename", 5), ("cross-device", 5), ("reader-io", 5), ("internal", 7),
])
def test_output_io_and_internal_errors_clean_up(tmp_path, monkeypatch, fault, expected):
    payload = fixture_stream(tmp_path)
    code, receipt, out, spool = call_in_process(tmp_path, payload, monkeypatch, fault=fault)
    assert code == expected and receipt["complete"] is False and receipt["sha256"] == ""
    assert not out.exists() and list(spool.iterdir()) == []
    assert receipt["sink_bytes"] == len(payload)


def test_receipt_internal_error_removes_published_output(tmp_path, monkeypatch):
    payload = fixture_stream(tmp_path)
    code, receipt, out, spool = call_in_process(
        tmp_path, payload, monkeypatch, fault="receipt-internal")
    assert code == 7 and receipt is None
    assert not out.exists() and list(spool.iterdir()) == []


def test_fsync_then_verify_then_publish_and_short_writes(tmp_path, monkeypatch):
    payload = fixture_stream(tmp_path)
    events = []
    real_sync, real_verify = finalizer.os.fsync, finalizer._verify
    real_replace, real_write = finalizer.os.replace, finalizer.os.write

    def sync(fd):
        events.append("fsync")
        return real_sync(fd)

    def verify(path, run_id, receipt):
        assert not (tmp_path / "out.sink").exists()
        real_verify(path, run_id, receipt)
        events.append("verified")

    def publish(source, destination):
        assert events == ["fsync", "verified"]
        assert Path(source).parent == tmp_path / "spool"
        assert stat.S_IMODE(Path(source).stat().st_mode) == 0o600
        assert Path(source).read_bytes() == payload
        real_replace(source, destination)
        events.append("published")

    def short_write(fd, data):
        return real_write(fd, data[:7])

    monkeypatch.setattr(finalizer.os, "fsync", sync)
    monkeypatch.setattr(finalizer, "_verify", verify)
    monkeypatch.setattr(finalizer.os, "replace", publish)
    monkeypatch.setattr(finalizer.os, "write", short_write)
    code, receipt, out, spool = call_in_process(tmp_path, payload, monkeypatch)
    assert code == 0 and receipt["complete"] is True
    assert events == ["fsync", "verified", "published"]
    assert out.read_bytes() == payload and list(spool.iterdir()) == []


def test_64_mib_stream_has_flat_peak_rss(tmp_path):
    header = tmp_path / "header.json"
    header.write_bytes(encoded(fixtures.header(outputs=True)))
    directory = tmp_path / "rss"
    directory.mkdir()
    result = subprocess.run(
        [sys.executable, "-B", "-S", str(Path(__file__).resolve()),
         "--rss-probe", str(directory), str(header)],
        stdin=subprocess.DEVNULL, capture_output=True, cwd=tmp_path, timeout=120)
    assert result.returncode == 0, result.stderr.decode()
    measured = json.loads(result.stdout)
    assert measured["returncode"] == 0 and measured["stderr"] == "", measured
    assert 0 < measured["peak_bytes"] < RSS_LIMIT_BYTES, measured
    assert measured["expected_bytes"] >= STREAM_BYTES
    receipt = measured["receipt"]
    assert tuple(receipt) == RECEIPT_KEYS and receipt["complete"] is True
    assert receipt["sink_bytes"] == measured["expected_bytes"]
    assert receipt["sha256"] == measured["expected_sha256"]
    assert {tag: receipt["count_" + tag.lower()] for tag in TAGS} == {
        tag: measured["expected_counts"].get(tag, 0) for tag in TAGS}
    out = directory / "large.sink"
    wire = hashlib.sha256()
    with out.open("rb") as file:
        while chunk := file.read(64 * 1024):
            wire.update(chunk)
    assert out.stat().st_size == measured["expected_bytes"]
    assert wire.hexdigest() == measured["expected_wire_sha256"]
    assert list(directory.glob(".pineforge-sink-*.spool")) == []
