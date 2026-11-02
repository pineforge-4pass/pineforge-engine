"""Test helpers for one-pass report assembly: immutable 6b33c47f oracle and real file instrumentation.

Nothing in this module is imported by the adapter. The two source fixtures are
whole, verbatim copies so their assembly/loader closures cannot resolve a future
product helper by accident. Only the tests execute them, after checking hashes.
"""
from contextlib import contextmanager, ExitStack, nullcontext
import builtins
import ctypes
from dataclasses import dataclass
import hashlib
import io
import os
from pathlib import Path
import sys
import types

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import test_run_json_sink as ordinary
from run_json_trade_scan_test import _sink, _point, _trade, _event, _trailer_start


FROZEN_FILES = (
    ("run_json", "639435843b16fe9b6804b2165e9e16c585518b0c6ee66cba8b90147033394360"),
    ("pf_sink", "5ef5b865650031765d953f01c5c36200793994f067252832b8f89e58cf6e6fcb"),
)


def _frozen_path(name):
    return ROOT / "tests" / "fixtures" / f"{name}_frozen_6b33c47f_rjone.py"


def _assert_frozen_hashes():
    for name, expected in FROZEN_FILES:
        path = _frozen_path(name)
        source = path.read_bytes()
        assert hashlib.sha256(source).hexdigest() == expected, (
            f"ONE_PASS_FROZEN_SOURCE_CHANGED: {path}")


# This check is deliberately at test-module import time. A changed oracle
# cannot silently become the expected answer for the rows below.
_assert_frozen_hashes()


def load_frozen_copy(prefix):
    """Load a separate pinned closure for an oracle or a test-only mutant."""
    _assert_frozen_hashes()

    def load(name):
        path = _frozen_path(name)
        module = types.ModuleType(f"{prefix}_{name}")
        # Preserve resource resolution, as the existing L9A frozen fixture does.
        module.__file__ = str(ROOT / "docker" / (name + ".py"))
        sys.modules[module.__name__] = module
        exec(compile(path.read_bytes(), str(path), "exec"), module.__dict__)
        return module

    modules = {"pf_sink": load("pf_sink")}
    previous = sys.modules.get("pf_sink")
    sys.modules["pf_sink"] = modules["pf_sink"]
    try:
        modules["run_json"] = load("run_json")
    finally:
        if previous is None:
            sys.modules.pop("pf_sink", None)
        else:
            sys.modules["pf_sink"] = previous
    # The global imported by the frozen run_json source must remain frozen even
    # after the temporary sys.modules binding is restored.
    modules["run_json"].pf_sink = modules["pf_sink"]
    modules["run_json"]._rjone_pf_sink = modules["pf_sink"]
    return modules["run_json"]


@pytest.fixture(scope="module")
def frozen():
    return load_frozen_copy("rjone_frozen")


@contextmanager
def reference_imports(runner):
    """Keep the frozen reader independent even when product pf_sink changes."""
    if runner is run_json:
        yield
        return
    previous = sys.modules["pf_sink"]
    previous_global = runner.pf_sink
    sys.modules["pf_sink"] = runner._rjone_pf_sink
    runner.pf_sink = runner._rjone_pf_sink
    try:
        yield
    finally:
        sys.modules["pf_sink"] = previous
        runner.pf_sink = previous_global


def reference_sink(runner, sink):
    """Borrow the same native sink without reinstalling it on the native state."""
    result = object.__new__(runner._ReportSink)
    result.directory, result.path = sink.directory, sink.path
    result.named, result.run_id = sink.named, sink.run_id
    result.header, result.counts, result.sha256 = {}, {}, ""
    result._identity = None
    result.summary = result.summary_failure = result.diagnostics = None
    result.keep, result.closed = True, False
    if runner is run_json:
        result._fused_pass = True
        result._spool_resources = ExitStack()
        result._equity_spool = result._trades_spool = None
        result._spool_failure = None
        result._spool_open_failure = False
        result._equity_spool_failed = result._trades_spool_failed = False
        result._fused_outputs = None
    return result


def spool_windows(directory):
    """Inspect the actual equity and trade spools of an empty verified sink."""
    library = ordinary.SinkLibrary()
    sink = run_json._ReportSink(library, 1, directory=str(directory))
    try:
        library.counts, library.sha = ordinary.write_sink(sink.path, [])
        sink.verify(library, 1, run_json.ReportC())
        return {"E": sink._equity_spool.file.window,
                "T": sink._trades_spool.file.window}
    finally:
        sink.close()


def clone_report(runner, report):
    assert ctypes.sizeof(report) == ctypes.sizeof(runner.ReportC)
    return runner.ReportC.from_buffer_copy(
        ctypes.string_at(ctypes.addressof(report), ctypes.sizeof(report)))


def failure_envelope(runner, error):
    kind = type(error)
    reader_module = getattr(runner, "_rjone_pf_sink", pf_sink)
    module = {runner.__name__: "run_json",
              reader_module.__name__: "pf_sink"}.get(kind.__module__, kind.__module__)
    code, args = getattr(error, "code", None), getattr(error, "code_args", None)
    return (module, kind.__qualname__, str(error), code, args, error.args,
            getattr(error, "exit_status", 1),
            runner.failure_line(str(error), code, args).encode("ascii"))


@dataclass
class Outcome:
    report: bytes = b""
    failure: tuple | None = None
    error: Exception | None = None


def report_args(report, sink, *, window_anchor=False):
    return (report, Path("fixed.csv"), 2000, ordinary.bars.T0,
            ordinary.bars.T0 + 86400000, 0.125, {"quoted": '汉"\\'},
            {"initial_capital": 10000.}, {"input_tf": "1"}, sink)


@contextmanager
def _consume_before_column_close(runner, consume, document):
    """Observe the real prepared artifact before its source spools close."""
    assert consume is not None
    prepare = runner._prepare_stream_report
    calls = 0

    def prepare_and_consume(source, *args, **kwargs):
        nonlocal calls
        artifact = prepare(source, *args, **kwargs)
        assert source is document
        assert calls == 0
        calls += 1
        consume(artifact, document)
        return artifact

    with pytest.MonkeyPatch.context() as patch:
        patch.setattr(runner, "_prepare_stream_report", prepare_and_consume)
        yield
    assert calls == 1


def assemble(runner, sink, library, report, *, outputs=False, window_anchor=False,
             before_prepare=None, after_verify=None, configure=None, consume=None,
             consume_before_close=False):
    """Run real assembly; opt-in column consumers observe the pre-close window."""
    outcome = Outcome()
    target = sink if runner is run_json else reference_sink(runner, sink)
    native = clone_report(runner, report)
    try:
        setup = configure(runner) if configure is not None else nullcontext()
        with setup, reference_imports(runner), ExitStack() as resources:
            target.verify(library, 1, native)
            if after_verify is not None:
                after_verify(target, native)
            block = (runner._sink_outputs_block(
                library, 1, target, resources, summary_only=target.named)
                if outputs else None)
            document = runner._build_sink_report(
                *report_args(native, target), window_anchor=window_anchor)
            if block is not None:
                document["outputs"] = block
            if before_prepare is not None:
                before_prepare(document)
            # _prepare_report_json closes fused columns before returning.
            # Opt in to observing its real artifact just before that close;
            # ordinary consumers still run after all preparation checks.
            observation = (_consume_before_column_close(runner, consume, document)
                           if consume_before_close else nullcontext())
            with observation:
                artifact = runner._prepare_report_json(
                    document, resources,
                    types.SimpleNamespace(profile="unjailed", spill_dir=target.directory),
                    target)
            if consume is None:
                chunks = []
                while chunk := artifact.read(65536):
                    chunks.append(chunk.encode("ascii") if isinstance(chunk, str) else chunk)
                outcome.report = b"".join(chunks)
            elif not consume_before_close:
                consume(artifact, document)
    except Exception as error:
        outcome.error = error
        outcome.failure = failure_envelope(runner, error)
    return outcome


def assert_same(actual, expected):
    assert actual.failure == expected.failure
    assert actual.report == expected.report
    if expected.failure is None:
        assert actual.report.endswith(b"\n") and not actual.report.endswith(b"\n\n")
    else:
        assert actual.report == expected.report == b""


def set_report(report, rows):
    trades = [row for row in rows if row[0] == "T"]
    report.total_trades = report.metrics.all.num_trades = len(trades)
    report.input_bars_processed = report.script_bars_processed = sum(
        row[0] == "E" for row in rows)
    report.net_profit = sum(row[8] for row in trades if row[8] is not None)
    report.metrics.equity.cagr, report.metrics.equity.calmar = 25., 2.


class SinkIO:
    """Count actual raw-file and libc pread returns at one boundary apiece.

    Buffered reads enter through RawIOBase; uncached reads bypass that object
    through libc, so neither path is counted a second time at the logical read.
    Logical EOF is recorded separately: the window reader can return EOF without
    issuing a further pread once its known file size has been consumed.
    """
    def __init__(self, path):
        self.path = os.path.abspath(os.fspath(path))
        self.opens, self.files, self.reads, self.seeks, self.eofs = [], [], [], [], []
        self.digests = []
        self.fstats = []

    def matches(self, path):
        return isinstance(path, (str, bytes, os.PathLike)) and (
            os.path.abspath(os.fsdecode(path)) == self.path)

    @contextmanager
    def install(self, monkeypatch, *, runner=run_json, after_read=None, transform=None):
        audit = self
        original_open, original_io_open = builtins.open, io.open
        original_init = runner._UncachedSinkFile.__init__
        original_read = runner._UncachedSinkFile.read
        original_hash, original_fstat = hashlib.sha256, os.fstat

        def recorded(fd, offset, data, route):
            # transform changes returned bytes, not the on-disk file.
            if transform is not None:
                data = transform(fd, offset, data)
            audit.reads.append((fd, offset, bytes(data), route))
            if after_read is not None:
                after_read(fd, offset, bytes(data))
            return data

        class Raw(io.RawIOBase):
            def __init__(self, file):
                super().__init__()
                self.file = file
            def fileno(self):
                return self.file.fileno()
            def tell(self):
                return self.file.tell()
            def readable(self):
                return True
            def seekable(self):
                return True
            def seek(self, offset, whence=0):
                audit.seeks.append((self.fileno(), offset, whence))
                return self.file.seek(offset, whence)
            def readinto(self, buffer):
                position = self.tell()
                data = recorded(self.fileno(), position, self.file.read(len(buffer)), "raw")
                buffer[:len(data)] = data
                if not data:
                    audit.eofs.append(self.fileno())
                return len(data)
            def close(self):
                if not self.closed:
                    self.file.close()
                super().close()

        def open_file(path, mode="r", buffering=-1, *args, **kwargs):
            if (not audit.matches(path) or "r" not in mode or "b" not in mode
                    or "+" in mode):
                return original_open(path, mode, buffering, *args, **kwargs)
            file = Raw(original_open(path, mode, buffering=0, *args, **kwargs))
            audit.opens.append(file.fileno())
            audit.files.append(file)
            # Keep the header read at the same raw boundary as the later
            # direct windows. A BufferedReader would report its read-ahead
            # bytes before _UncachedSinkFile resumes at the logical header
            # end, making the audit count buffered lookahead twice.
            return file

        class Libc:
            def __init__(self, inner, fd):
                self.inner, self.fd = inner, fd
            def __getattr__(self, name):
                return getattr(self.inner, name)
            def pread(self, fd, address, count, offset):
                result = self.inner.pread(fd, address, count, offset)
                if result >= 0 and fd == self.fd:
                    data = recorded(fd, offset, ctypes.string_at(address, result), "pread")
                    assert len(data) == result, "read transforms must preserve length"
                    if result:
                        ctypes.memmove(address, data, result)
                return result

        def init(file, source):
            original_init(file, source)
            if audit.matches(getattr(source, "name", audit.path)):
                # The raw wrapper's fileno, not its Python identity, joins
                # initial buffered-header I/O to the later direct windows.
                if file.fileno() in audit.opens:
                    file.libc = Libc(file.libc, file.fileno())

        def read(file, count=-1):
            data = original_read(file, count)
            if count and not data and file.fileno() in audit.opens:
                audit.eofs.append(file.fileno())
            return data

        class Digest:
            def __init__(self, data=b"", **kwargs):
                self.inner = original_hash(data, **kwargs)
                self.parts = [bytes(data)] if data else []
                audit.digests.append(self)
            def update(self, data):
                self.parts.append(bytes(data))
                return self.inner.update(data)
            def __getattr__(self, name):
                return getattr(self.inner, name)

        def fstat(fd):
            value = original_fstat(fd)
            if fd in audit.opens:
                audit.fstats.append((fd, value.st_ino, value.st_size, value.st_mtime_ns))
            return value

        with monkeypatch.context() as patch:
            patch.setattr(builtins, "open", open_file)
            patch.setattr(io, "open", lambda *a, **kw: open_file(*a, **kw)
                          if a and audit.matches(a[0]) else original_io_open(*a, **kw))
            patch.setattr(runner._UncachedSinkFile, "__init__", init)
            patch.setattr(runner._UncachedSinkFile, "read", read)
            patch.setattr(hashlib, "sha256", Digest)
            patch.setattr(os, "fstat", fstat)
            yield self

    def assert_one_pass(self, raw):
        assert len(self.opens) == 1, (
            f"ONE_PASS_SINK_OPEN_BUDGET: expected 1, observed {len(self.opens)}")
        assert not self.seeks, self.seeks
        position = 0
        for fd, offset, data, route in self.reads:
            assert fd == self.opens[0]
            assert offset == position, (route, offset, position)
            assert data == raw[position:position + len(data)]
            position += len(data)
        assert position == len(raw), "header and trailer must both enter the single read"
        assert self.opens[0] in self.eofs, "the real reader must consume EOF"
        body = raw[:_trailer_start(raw)]
        header = raw[:raw.index(b"\n") + 1]
        hashes = [b"".join(digest.parts) for digest in self.digests]
        # main() may also hash an outputs manifest or provenance. Sink hashes
        # start with the exact sink header; require one complete feed of it.
        sink_hashes = [value for value in hashes if value.startswith(header)]
        assert sink_hashes == [body], (
            "SHA must consume header + records once, never the trailer")
        assert self.files and all(file.closed for file in self.files)


@contextmanager
def artifact_fault(monkeypatch, runner, fault):
    """Fault real artifact I/O; retain every real owner for cleanup assertions."""
    files, calls = [], []
    original_temporary = runner.tempfile.TemporaryFile
    original_write = runner._BoundedJSONFile.write_ascii
    original_flush = runner._BoundedJSONFile.flush
    original_close = runner._BoundedJSONFile.close
    fired = []

    def fail(operation):
        calls.append(operation)
        if operation == fault and not fired:
            fired.append(operation)
            raise OSError(28, "injected artifact ENOSPC")

    def temporary(*args, **kwargs):
        fail("open")
        file = original_temporary(*args, **kwargs)
        files.append(file)
        return file

    def write(file, data):
        fail("write")
        return original_write(file, data)

    def flush(file):
        fail("flush")
        return original_flush(file)

    def close(file):
        # Close the real fd before simulating its close error.
        original_close(file)
        fail("close")

    with monkeypatch.context() as patch:
        patch.setattr(runner.tempfile, "TemporaryFile", temporary)
        patch.setattr(runner._BoundedJSONFile, "write_ascii", write)
        patch.setattr(runner._BoundedJSONFile, "flush", flush)
        patch.setattr(runner._BoundedJSONFile, "close", close)
        yield files, calls, fired
