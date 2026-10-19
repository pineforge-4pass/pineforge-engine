"""CR-150 L9A: full JSON from a verified temporary v1 sink.

The red commit is intentionally executable against the merged L2/L6b-1 base:
the first test fails because main never installs the existing sink ABI. Native
rows compare the frozen producer with the changed producer on the SAME real
module, and forbid passing report arrays to the legacy builder. No builds or
tests were run on the executor Mac; CTest supplies the lab-built modules.
"""
from __future__ import annotations

import contextlib
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import types

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json
import test_run_json_bin as bars


FROZEN_SOURCE = ROOT / "tests" / "fixtures" / "run_json_frozen_pre_l9a.py"
FROZEN_SOURCE_SHA256 = "743a1cb79e7d2aa24389ac58dcb2a0fc157e1fd4739addaf8cfbe2604b98dd65"
TRADE_FIELDS = (
    "n", "side", "entry_time", "exit_time", "entry_price", "exit_price", "qty",
    "pnl", "pnl_pct", "max_runup", "max_drawdown", "commission", "entry_bar_index",
    "exit_bar_index", "open_at_end", "entry_incarnation", "close_cause", "entry_id",
    "exit_id", "exit_comment",
)
TRADE_FLOATS = ("entry_price", "exit_price", "qty", "pnl", "pnl_pct", "max_runup",
                "max_drawdown", "commission")


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode() + b"\n"


def descriptor(tag, fields, types_, units):
    return {"type": tag, "fields": fields, "types": types_, "units": units}


def header(outputs=False):
    records = [
        descriptor("E", ["time_ms", "equity", "open_profit"],
                   ["i64", "f64?", "f64?"], ["unix_ms", "account_ccy", "account_ccy"]),
        descriptor("T", list(TRADE_FIELDS),
                   ["i64", "str", "i64", "i64"] + ["f64?"] * 8
                   + ["i64", "i64", "bool", "u64", "i64", "str", "str", "str"],
                   ["", "", "unix_ms", "unix_ms", "quote_ccy", "quote_ccy", "qty",
                    "account_ccy", "percent", "account_ccy", "account_ccy", "account_ccy",
                    "script_bar", "script_bar", "", "", "", "", "", ""]),
    ]
    if outputs:
        records += [
            descriptor("O", ["row", "open_ms", "close_ms", "v0", "v1"],
                       ["i64", "i64?", "i64?", "f64?", "f64?"], ["", "unix_ms", "unix_ms", "", ""]),
            descriptor("V", ["sequence", "output_index", "bar_index", "bar_open_ms",
                             "bar_close_ms", "ordinal_in_bar", "phase", "confirmed", "value",
                             "message_hash64", "message"],
                       ["u64", "i64", "i64", "i64?", "i64?", "i64", "i64", "i64", "f64?", "hex64", "str?"],
                       ["", "", "", "unix_ms", "unix_ms", "", "", "", "", "", ""]),
            descriptor("K", ["c0"], ["f64?"], [""]),
        ]
    return {"schema": "pf-report-sink", "version": 1, "run_id": "", "engine": "test", "records": records}


def write_sink(path, rows, *, outputs=False, fault=None):
    head = header(outputs)
    counts = {entry["type"]: 0 for entry in head["records"]}
    digest = hashlib.sha256()
    with open(path, "wb") as file:
        line = encoded(head)
        digest.update(line)
        file.write(line)
        for row in rows:
            counts[row[0]] += 1
            line = encoded(row)
            digest.update(line)
            file.write(line)
        trailer = {"trailer": "pf-report-sink", "version": 1, "counts": counts, "sha256": digest.hexdigest()}
        if fault == "hash":
            trailer["sha256"] = "0" * 64
        if fault != "truncated":
            file.write(encoded(trailer))
    return counts, digest.hexdigest()


class Receipt(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("version", ctypes.c_uint32)] + [
        ("count_" + tag, ctypes.c_int64) for tag in "etovkxh"
    ] + [("sha256_hex", ctypes.c_char * 65), ("reserved", ctypes.c_uint8 * 7),
         ("complete", ctypes.c_uint32), ("trades_retained", ctypes.c_uint32)]


class SinkLibrary(bars.Library):
    """Only native calls are replaced; report assembly, reader and writer stay real."""

    def __init__(self, *, pnls=(10., -7., -0.), outputs=False, fault=None, bad_field=None):
        self.pnls, self.outputs, self.fault, self.bad_field = pnls, outputs, fault, bad_field
        self.installed, self.freed, self.queried = [], [], 0
        self.path = None
        self.backing = []
        super().__init__()
        self.pf_report_sink_version = self.export("pf_report_sink_version", lambda: 1)
        self.strategy_set_report_sink_v1 = self.export("strategy_set_report_sink_v1", self.install)
        self.strategy_report_sink_receipt_v1 = self.export("strategy_report_sink_receipt_v1", self.receipt)
        self.strategy_free = self.export("strategy_free", self.free)

    def install(self, state, pointer):
        sink = pointer._obj
        assert sink.struct_size == 8 + 3 * ctypes.sizeof(ctypes.c_void_p)
        assert sink.version == 1 and sink.run_id == b""
        self.path = os.fsdecode(sink.path)
        assert Path(self.path).stat().st_size == 0  # a fresh file, even on repeats
        self.installed.append(self.path)
        return 0

    def run(self, state, feed, count, *args):
        super().run(state, feed, count, *args)
        report = args[-1]._obj
        trades = (run_json.TradeC * len(self.pnls))()
        rows = []
        for i, pnl in enumerate(self.pnls):
            trade = trades[i]
            trade.entry_time, trade.exit_time = bars.T0, bars.T0 + 60000
            trade.entry_price, trade.exit_price, trade.qty = 100., 101., 2.
            trade.pnl, trade.pnl_pct, trade.max_runup, trade.max_drawdown = pnl, 5., 12., -8.
            trade.commission, trade.is_long, trade.open_at_end = .5, int(i % 2 == 0), int(i == 2)
            trade.entry_bar_index, trade.exit_bar_index = i, i + 1
            row = ["T", i + 1, "long" if trade.is_long else "short", trade.entry_time, trade.exit_time,
                   trade.entry_price, trade.exit_price, trade.qty, trade.pnl, trade.pnl_pct,
                   trade.max_runup, trade.max_drawdown, trade.commission, i, i + 1,
                   bool(trade.open_at_end), 0, 0, "L\"\n漢", "", ""]
            if self.bad_field and i == len(self.pnls) - 1:
                row[1 + TRADE_FIELDS.index(self.bad_field)] = None
            rows.append(row)
        curve = (run_json.EquityPointC * count)()
        for i in range(count):
            curve[i] = run_json.EquityPointC(bars.T0 + i * 60000, 10000. + i, -0.)
            rows.insert(i, ["E", curve[i].time_ms, curve[i].equity, curve[i].open_profit])
        if self.outputs:
            rows.extend([["O", 0, bars.T0, bars.T0 + 60000, -0., 4294967295.],
                         ["O", 1, None, None, None, None],
                         ["V", 1, 1, 0, bars.T0, bars.T0 + 60000, 0, 1, 1, None,
                          "0123456789abcdef", "quote\"\n漢"], ["K", 50.]])
        report.total_trades = len(trades)
        report.net_profit = sum(self.pnls)
        report.input_bars_processed = report.script_bars_processed = count
        report.metrics.all.profit_factor = float("nan")
        if self.path is None:
            report.trades, report.trades_len = trades, len(trades)
            report.equity_curve, report.equity_curve_len = curve, count
        else:
            self.counts, self.sha = write_sink(self.path, rows, outputs=self.outputs, fault=self.fault)
        self.backing = [trades, curve]

    def receipt(self, state, pointer):
        self.queried += 1
        out = pointer._obj
        assert ctypes.sizeof(out) == 144 and out.struct_size == 144 and out.version == 1
        for tag in "ETOVKXH":
            setattr(out, "count_" + tag.lower(), self.counts.get(tag, 0))
        out.sha256_hex = ("f" * 64 if self.fault == "receipt" else self.sha).encode()
        out.complete = 0 if self.fault == "incomplete" else 1
        return 0

    def free(self, state):
        if self.path is not None:
            assert Path(self.path).exists()  # handle cleanup precedes file cleanup
            self.freed.append(self.path)
            self.path = None


def outputs_reader():
    manifest = {"series": [{"slot": 1, "output": "p", "encoding": "rgba-u32"},
                           {"slot": 0, "output": "p"}],
                "outputs": [{"index": 0, "id": "p", "kind": "plot"},
                            {"index": 1, "id": "a", "kind": "alert", "freq": "once"},
                            {"index": 2, "id": "h", "kind": "hline", "price": {"constant": 0}}],
                "constants": [{"index": 0}]}
    event = types.SimpleNamespace(sequence=1, output_index=1, bar_index=0,
                                  bar_open_ms=bars.T0, bar_close_ms=bars.T0 + 60000,
                                  ordinal_in_bar=0, phase=1, value=float("nan"), message="quote\"\n漢")
    return types.SimpleNamespace(manifest=lambda: encoded(manifest).rstrip(b"\n"),
                                 bar_times=lambda: ([bars.T0, -(2**63)], [bars.T0 + 60000, -(2**63)]),
                                 series_count=lambda: 2, series=lambda slot: (
                                     [-0., float("nan")] if slot == 0 else [4294967295., float("nan")]),
                                 constants=lambda: [50.], events=lambda: iter([event]))


def _frozen_source_bytes():
    source = FROZEN_SOURCE.read_bytes()
    assert hashlib.sha256(source).hexdigest() == FROZEN_SOURCE_SHA256, (
        "frozen pre-L9A producer fixture changed")
    return source


@pytest.fixture(scope="module")
def frozen():
    source = _frozen_source_bytes()
    module = types.ModuleType("cr150_l9a_frozen")
    module.__file__ = str(ROOT / "docker/run_json.py")
    exec(compile(source, str(FROZEN_SOURCE), "exec"), module.__dict__)
    # Both producers read the same unchanged C ABI allocations. In particular
    # _stats_dict's isinstance check must recognize the shared equity struct.
    module.ReportC, module.EquityStatsC = run_json.ReportC, run_json.EquityStatsC
    return module


def invoke(tmp_path, monkeypatch, runner=run_json, *, library=None, extra=(), forbid_output_arrays=False):
    source = bars.csv_file(tmp_path)
    library = library or SinkLibrary()
    reader = outputs_reader()
    with monkeypatch.context() as patch:
        patch.setattr(runner, "load_strategy", lambda path: library)
        patch.setattr(runner.time, "time", lambda: 10.)
        patch.setattr(runner, "require_outputs", lambda lib: None)
        patch.setattr(runner, "enable_outputs", lambda lib, state: None)
        def read_outputs(lib, state):
            assert not forbid_output_arrays, "streamed outputs must not call the retained-array readers"
            return reader
        patch.setattr(runner, "make_outputs_reader", read_outputs)
        patch.setattr(runner, "_outputs_manifest", lambda lib, state: reader.manifest())
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = runner.main(["--so", "unused.so", "--ohlcv", str(source), *extra])
    return status, out.getvalue(), err.getvalue(), library


@pytest.mark.parametrize("outputs", [False, True])
@pytest.mark.parametrize("pnls", [(10., -7., -0.), (), (-0., 0.), (1e16, 1., -1e16, -2.)])
def test_full_json_installs_sink_and_matches_frozen(tmp_path, monkeypatch, frozen, outputs, pnls):
    extra = ["--outputs"] if outputs else []
    before = invoke(tmp_path, monkeypatch, frozen, library=SinkLibrary(outputs=outputs, pnls=pnls), extra=extra)
    after = invoke(tmp_path, monkeypatch, library=SinkLibrary(outputs=outputs, pnls=pnls),
                   extra=extra, forbid_output_arrays=outputs)
    assert after[3].installed, "L9A missing: main never installs the available report-sink ABI"
    assert after[3].queried == 1
    assert (before[0], before[2], after[0], after[2]) == (0, "", 0, "")
    assert after[1] == before[1]  # fixed clock: not even wall-clock masks are needed
    assert len(after[3].freed) == 1
    assert not Path(after[3].installed[0]).exists()


@pytest.mark.parametrize("fault", ["truncated", "hash", "receipt", "incomplete"])
def test_incomplete_or_mismatched_receipt_never_emits_success(tmp_path, monkeypatch, fault):
    status, text, _, library = invoke(tmp_path, monkeypatch, library=SinkLibrary(fault=fault))
    assert library.installed, "the missing sink installation is the red reason"
    document = json.loads(text)
    assert status == 1 and document["code"] == "engine_invariant"
    assert "summary" not in document and len(text.splitlines()) == 1
    assert len(library.freed) == 1 and not Path(library.installed[0]).exists()


@pytest.mark.parametrize("field", TRADE_FLOATS)
def test_null_trade_double_has_the_existing_nonfinite_failure(tmp_path, monkeypatch, field):
    status, text, stderr, _ = invoke(tmp_path, monkeypatch, library=SinkLibrary(bad_field=field))
    assert status == 1 and stderr == ""
    document = json.loads(text)
    assert document["code"] == "report_result_invalid"
    assert document["args"] == {"field": "trades." + field, "reason": "nonfinite"}


def test_bench_uses_a_fresh_sink_for_every_state(tmp_path, monkeypatch):
    status, text, stderr, library = invoke(tmp_path, monkeypatch,
        extra=["--bench", "--warmup", "2", "--repeats", "3"])
    assert status == 0 and stderr == "", text
    assert len(library.installed) == len(library.freed) == 6
    assert json.loads(text)["summary"]["total_trades"] == 3


def test_sink_ctypes_layout_and_runtime_binding(tmp_path, monkeypatch):
    library = SinkLibrary()
    monkeypatch.setattr(ctypes, "CDLL", lambda path: library)
    run_json.load_strategy(tmp_path / "strategy.so")
    assert library.pf_report_sink_version.restype is ctypes.c_uint32
    assert library.strategy_set_report_sink_v1.argtypes == [
        ctypes.c_void_p, ctypes.POINTER(run_json.ReportSinkC)]
    assert library.strategy_report_sink_receipt_v1.argtypes == [
        ctypes.c_void_p, ctypes.POINTER(run_json.ReportSinkReceiptC)]
    assert ctypes.sizeof(run_json.ReportSinkReceiptC) == ctypes.sizeof(Receipt) == 144
    for name, _ in Receipt._fields_:
        assert getattr(run_json.ReportSinkReceiptC, name).offset == getattr(Receipt, name).offset


def test_serialization_fault_discards_all_prepared_bytes_and_closes_files(tmp_path, monkeypatch):
    files, real_temporary = [], run_json.tempfile.TemporaryFile
    def temporary(*args, **kwargs):
        file = real_temporary(*args, **kwargs)
        files.append(file)
        assert os.fstat(file.fileno()).st_nlink == 0
        return file
    monkeypatch.setattr(run_json.tempfile, "TemporaryFile", temporary)
    original = run_json._stream_json
    def fail_after_trades(value, destination):
        if isinstance(value, run_json._JSONRecordArray):
            destination.write("[partial")
            raise OSError(28, "injected full report disk")
        return original(value, destination)
    monkeypatch.setattr(run_json, "_stream_json", fail_after_trades)
    status, text, _, library = invoke(tmp_path, monkeypatch)
    assert status == 1 and library.installed
    assert json.loads(text)["code"] == "report_sink_failed"
    assert len(text.splitlines()) == 1 and "[partial" not in text
    assert files and all(file.closed for file in files)


def test_sink_open_failure_has_the_existing_sink_failure_code(tmp_path, monkeypatch):
    def refuse(*args, **kwargs):
        raise OSError(28, "injected full sink disk")
    monkeypatch.setattr(run_json.tempfile, "TemporaryFile", refuse)
    status, text, stderr, library = invoke(tmp_path, monkeypatch)
    assert status == 1 and stderr == ""
    assert json.loads(text)["code"] == "report_sink_failed"
    assert library.calls[-1] == "strategy_free"


@pytest.mark.parametrize("fault", ["flush-and-close", "column-close"])
def test_persistent_temporary_io_error_keeps_the_disk_code(tmp_path, monkeypatch, fault):
    files, real_temporary = [], run_json.tempfile.TemporaryFile
    class File:
        def __init__(self, file):
            self.file = file
        def __getattr__(self, name):
            return getattr(self.file, name)
        def flush(self):
            if fault == "flush-and-close":
                raise OSError(28, "injected persistent disk failure")
            return self.file.flush()
        def close(self):
            if not self.file.closed:
                self.file.close()
                raise OSError(28, "injected persistent close failure")
    def temporary(*args, **kwargs):
        file = real_temporary(*args, **kwargs)
        if kwargs["mode"] == "w+":
            file = File(file)
        files.append(file)
        return file
    monkeypatch.setattr(run_json.tempfile, "TemporaryFile", temporary)
    outputs = fault == "column-close"
    status, text, stderr, _ = invoke(tmp_path, monkeypatch,
        library=SinkLibrary(outputs=outputs), extra=["--outputs"] if outputs else [])
    assert status == 1 and stderr == ""
    assert json.loads(text)["code"] == "report_sink_failed"
    assert len(text.splitlines()) == 1
    assert files and all(file.closed for file in files)


def test_prepared_report_is_copied_in_bounded_chunks(tmp_path):
    expected = "x" * (2 * 1024 * 1024 + 17)
    chunks = []
    class Stdout:
        def write(self, text):
            assert 0 < len(text) <= 1024 * 1024
            chunks.append(text)
            return len(text)
        def flush(self):
            pass
    with contextlib.ExitStack() as resources:
        file = run_json._prepare_stream_report({"value": expected}, resources, str(tmp_path))
        with contextlib.redirect_stdout(Stdout()):
            assert run_json._write_report(file) == 0
    assert len(chunks) == 3
    assert "".join(chunks) == json.dumps({"value": expected}, separators=(",", ":")) + "\n"


def test_stdout_failure_never_appends_a_failure_document(tmp_path, monkeypatch):
    class BrokenStdout:
        def __init__(self):
            self.calls = 0
        def write(self, text):
            self.calls += 1
            raise BrokenPipeError("injected closed pipe")
        def flush(self):
            pytest.fail("a failed write must not flush or publish completion")
    stdout, stderr = BrokenStdout(), io.StringIO()
    # This is an in-memory stdout without a file descriptor; do not redirect
    # the test runner's process-wide fd when exercising the partial-write rule.
    monkeypatch.setattr(run_json, "_discard_stdout", lambda: None)
    with contextlib.ExitStack() as resources:
        file = run_json._prepare_stream_report({"value": 1}, resources, str(tmp_path))
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = run_json._write_report(file)
    assert status == 1 and stdout.calls == 1
    assert "report could not be written" in stderr.getvalue()


def test_l6b1_output_compatibility_reads_bounded_windows(tmp_path, monkeypatch):
    reader = outputs_reader()
    def no_array(*args):
        pytest.fail("the compatibility path must not materialize a whole output array")
    reader.bar_times = reader.series = no_array
    monkeypatch.setattr(run_json, "make_outputs_reader", lambda lib, state: reader)
    n, requests = 8195, []
    def times(state, offset, opens, closes, count, copied):
        assert count <= 4096 and offset + count <= n
        requests.append(("times", offset, count))
        for i in range(count):
            opens[i], closes[i] = bars.T0 + offset + i, bars.T0 + offset + i + 1
        copied._obj.value = count
        return 0
    def series(state, slot, offset, values, count, copied):
        assert count <= 4096 and offset + count <= n
        requests.append((slot, offset, count))
        for i in range(count):
            values[i] = float(offset + i) if slot == 0 else 4294967295.
        copied._obj.value = count
        return 0
    library = types.SimpleNamespace(
        strategy_outputs_series_count=lambda state: 2,
        strategy_outputs_bars_len=lambda state: n,
        strategy_outputs_bar_times_copy=times, strategy_outputs_series_copy=series)
    with contextlib.ExitStack() as resources:
        document = run_json._legacy_streamed_outputs_block(library, 7)
        file = run_json._prepare_stream_report(document, resources, str(tmp_path))
        result = json.load(file)
    assert len(result["bars"]["open_ms"]) == len(result["bars"]["close_ms"]) == n
    assert result["bars"]["open_ms"][-1] == bars.T0 + n - 1
    assert result["series"][0]["values"][4096] == 4096.
    assert result["series"][0]["values"][-1] == float(n - 1)
    assert result["series"][1]["values"][-1] == 4294967295
    assert type(result["series"][1]["values"][-1]) is int
    assert len(requests) == 12  # 3 windows each for opens, closes and both slots


def mask_bytes(text, *, binary=False):
    document = json.loads(text)
    # Assert the wire bytes are already compact canonical json.dump output.
    assert text == json.dumps(document, separators=(",", ":"), allow_nan=False) + "\n"
    document["elapsed_seconds"] = 0.
    document.get("diagnostics", {}).pop("timing", None)
    document.get("diagnostics", {}).pop("throughput", None)
    if binary:
        document["input"]["ohlcv"] = "<same chart values>"
    return json.dumps(document, separators=(",", ":"), allow_nan=False)


NATIVE_DRIVER = r'''
import importlib.util, pathlib, sys
sys.path.insert(0, sys.argv[1])
spec = importlib.util.spec_from_file_location("l9a_producer", sys.argv[2])
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
if sys.argv[3] == "stream":
    original = runner.build_report_dict
    def scalars_only(report, *args, **kwargs):
        assert not report.trades and report.trades_len == 0, "legacy trade array was read"
        assert not report.equity_curve and report.equity_curve_len == 0, "legacy equity array was read"
        return original(report, *args, **kwargs)
    runner.build_report_dict = scalars_only
raise SystemExit(runner.main(sys.argv[4:]))
'''


def native_compare(tmp_path, module, argv, *, binary=False, phase_fd=False):
    frozen_path = tmp_path / "frozen_run_json.py"
    frozen_path.write_bytes(_frozen_source_bytes())
    results = []
    for mode, producer in (("frozen", frozen_path), ("stream", ROOT / "docker/run_json.py")):
        parent, child = socket.socketpair()
        try:
            phase_options = ["--run-phase-fd", str(child.fileno())] if phase_fd else []
            result = subprocess.run([sys.executable, "-B", "-c", NATIVE_DRIVER, str(ROOT / "docker"),
                                     str(producer), mode, "--so", str(module), *map(str, argv), *phase_options],
                                    cwd=ROOT, capture_output=True, text=True,
                                    pass_fds=(child.fileno(),) if phase_fd else ())
            child.close()
            if phase_fd:
                with parent.makefile("r") as records:
                    phases = [json.loads(line)["phase"] for line in records]
                assert phases == ["preflight", "execution", "result_assembly", "serialization", "completed"]
        finally:
            child.close()
            parent.close()
        assert result.returncode == 0, (mode, result.stdout, result.stderr)
        assert result.stderr == ""
        results.append(mask_bytes(result.stdout, binary=binary))
    assert results[0] == results[1]
    return results[1]


@pytest.mark.parametrize("fixture,outputs", [("PINEFORGE_L9A_STRATEGY_SO", False),
                                            ("PINEFORGE_L9A_OUTPUTS_SO", True)])
@pytest.mark.parametrize("binary", [False, True])
@pytest.mark.parametrize("bench", [False, True])
@pytest.mark.parametrize("phase_fd", [False, True])
def test_native_frozen_producer_bytes(tmp_path, fixture, outputs, binary, bench, phase_fd):
    module = os.environ.get(fixture)
    if not module:
        pytest.skip("SKIP lab module not supplied: " + fixture + "; CTest wires the native targets")
    assert Path(module).is_file(), module
    source = bars.csv_file(tmp_path, count=97)
    argv = ["--ohlcv", source, "--input-tf", "1", "--script-tf", "1"]
    if binary:
        import pf_bars
        target = tmp_path / "chart.bin"
        pf_bars.convert_csv(source, target, symbol="LAKE", tf="1")
        argv[:2] = ["--ohlcv-bin", target]
    if outputs:
        argv += ["--outputs"]
    if bench:
        argv += ["--bench", "--warmup", "1", "--repeats", "2"]
    native_compare(tmp_path, module, argv, phase_fd=phase_fd)


def test_lab_corpus_subset_frozen_bytes(tmp_path):
    """CORE-TOP supplies exact probe argv after preparing derived feeds ONCE.

    Manifest: [{"so": "/lab/probe.so", "argv": ["--ohlcv", "...", ...]}, ...].
    This consumes ready modules/feeds and never builds or derives concurrently.
    """
    manifest = os.environ.get("PINEFORGE_L9A_CORPUS_CASES")
    if not manifest:
        pytest.skip("SKIP corpus byte proof requires PINEFORGE_L9A_CORPUS_CASES from CORE-TOP")
    cases = json.loads(Path(manifest).read_text())
    assert cases, "the corpus proof must name real probes"
    for case in cases:
        assert Path(case["so"]).is_file(), case
        native_compare(tmp_path, case["so"], case["argv"])
