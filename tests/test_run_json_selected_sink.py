"""Selected full/v1 bytes from the sink versus the current array producer.

Only native calls and the planner are replaced in the unit rows. The sink
reader, report builders, fingerprint checks, phase writer and serialization
stay real. Native rows consume supervisor-built modules without building or
materializing feeds. No expected report is copied from observed output.
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
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import request_feed_inventory as inventory
import run_json
import run_phase_transport as phases
import selected_window_plan as planner
import selected_window_report as swr
import test_run_json_selected_window as selected
import test_run_json_sink as sinks
import test_run_json_sink_summary as summaries

# Reuse the existing immutable producer and its SHA-256 guard.
frozen = sinks.frozen
arena_stats = sinks.arena_stats


class SelectedSinkLibrary(selected.FakeLib):
    """The same ReportC values, exposed through arrays or a real v1 sink."""

    def __init__(self, plan, *, pnls=(10., -7., -0.), outputs=False, fault=None,
                 bad_field=None, arena_stats=None, **kwargs):
        super().__init__(plan, **kwargs)
        sinks.configure_arena_stats(self, arena_stats)
        self.pnls, self.outputs, self.fault, self.bad_field = pnls, outputs, fault, bad_field
        self.path, self.installed, self.queried, self.closed = None, [], 0, []
        self.incarnation_reads = []
        self.pf_report_sink_version = lambda: 1
        self.strategy_set_report_sink_v1 = self.install
        self.strategy_report_sink_receipt_v1 = self.sink_receipt
        self.strategy_closed_trade_entry_incarnation = self.incarnation
        free = self.strategy_free
        report_free = self.report_free

        def release_report(ref):
            if self.path is not None:
                assert Path(self.path).exists()
            report_free(ref)

        def release_state(state):
            if self.path is not None:
                assert Path(self.path).exists()
                self.closed.append(self.path)
            free(state)

        self.report_free, self.strategy_free = release_report, release_state

    def install(self, state, ref):
        descriptor = ref._obj
        assert descriptor.struct_size == ctypes.sizeof(run_json.ReportSinkC)
        assert descriptor.version == 1 and descriptor.run_id == b""
        self.path = os.fsdecode(descriptor.path)
        assert Path(self.path).stat().st_size == 0
        self.installed.append(self.path)
        self.calls.append(("set_report_sink",))
        return 0

    def incarnation(self, state, index):
        assert self.path is None, "selected sink assembly must not read trade history"
        self.incarnation_reads.append(index)
        return index + 41

    def _run(self, *args):
        super()._run(*args)
        report = args[-1]._obj
        self._trades = (run_json.TradeC * len(self.pnls))()
        records = [["E", point.time_ms, point.equity, point.open_profit] for point in self._curve]
        if self.fault == "curve-count":
            records.pop()
        elif self.fault == "anchor-time":
            records[0][1] += 1
        elif self.fault == "anchor-profit":
            records[0][3] = 1.
        elif self.fault == "anchor-equity":
            records[0][2] += 1.
        elif self.fault == "anchor-null":
            records[0][2] = None
        for index, pnl in enumerate(self.pnls):
            trade = self._trades[index]
            trade.entry_time, trade.exit_time = selected.T, selected.T + selected.MINUTE
            trade.entry_price, trade.exit_price, trade.qty = 100., 101., 2.
            trade.pnl, trade.pnl_pct = pnl, 5.
            trade.max_runup, trade.max_drawdown, trade.commission = 12., -8., .5
            trade.is_long, trade.open_at_end = int(index % 2 == 0), int(index == len(self.pnls) - 1)
            trade.entry_bar_index, trade.exit_bar_index = index, index + 1
            row = ["T", index + 1, "long" if trade.is_long else "short",
                   trade.entry_time, trade.exit_time, trade.entry_price, trade.exit_price,
                   trade.qty, trade.pnl, trade.pnl_pct, trade.max_runup, trade.max_drawdown,
                   trade.commission, trade.entry_bar_index, trade.exit_bar_index,
                   bool(trade.open_at_end), index + 41, 0, "L\"\n漢", "", ""]
            if self.bad_field and index == len(self.pnls) - 1:
                row[1 + sinks.TRADE_FIELDS.index(self.bad_field)] = None
            records.append(row)
        if self.outputs:
            records += [
                ["O", 0, sinks.bars.T0, sinks.bars.T0 + 60000, -0., 4294967295.],
                ["O", 1, None, None, None, None],
                ["V", 1, 1, 0, sinks.bars.T0, sinks.bars.T0 + 60000, 0, 1, 1,
                 None, "0123456789abcdef", "quote\"\n漢"],
                ["K", 50.],
            ]
        report.total_trades = len(self.pnls)
        report.net_profit = sum(self.pnls)
        # Every metrics field travels unchanged through the report ABI.
        for group in ("all", "longs", "shorts", "equity"):
            block = getattr(report.metrics, group)
            for index, (name, kind) in enumerate(block._fields_):
                setattr(block, name, (index + 1) / 7 if kind is ctypes.c_double else index)
        report.metrics.all.profit_factor = float("nan")
        report.metrics.equity.open_pl = -0.
        if self.path is None or self.fault == "retained-arrays":
            report.trades, report.trades_len = self._trades, len(self.pnls)
        if self.path is not None:
            self.counts, self.sha = sinks.write_sink(
                self.path, records, outputs=self.outputs, fault=self.fault)
            if self.fault != "retained-arrays":
                report.trades = ctypes.POINTER(run_json.TradeC)()
                report.trades_len = 0
                report.equity_curve = ctypes.POINTER(run_json.EquityPointC)()
                report.equity_curve_len = 0

    def sink_receipt(self, state, ref):
        self.queried += 1
        result = ref._obj
        assert result.struct_size == ctypes.sizeof(run_json.ReportSinkReceiptC)
        assert result.version == 1
        for tag in "ETOVKXH":
            setattr(result, "count_" + tag.lower(), self.counts.get(tag, 0))
        result.sha256_hex = ("f" * 64 if self.fault == "receipt" else self.sha).encode()
        result.complete = 0 if self.fault == "incomplete" else 1
        return 0


class NamedSinkLibrary(SelectedSinkLibrary):
    """The selected fixture behind a caller-named sink: the descriptor the harness hands the
    engine is recorded, and the sink header carries the run id it names, as the engine writes it."""

    def __init__(self, plan, **kwargs):
        super().__init__(plan, **kwargs)
        self.run_id, self.descriptors = "", []

    def install(self, state, ref):
        descriptor = ref._obj
        assert descriptor.struct_size == ctypes.sizeof(run_json.ReportSinkC)
        assert descriptor.version == 1
        self.path = os.fsdecode(descriptor.path)
        self.run_id = descriptor.run_id.decode("utf-8")
        self.descriptors.append((self.path, self.run_id, os.fsdecode(descriptor.scratch_dir)))
        self.installed.append(self.path)
        self.calls.append(("set_report_sink",))
        return 0

    def _run(self, *args):
        header = sinks.header
        with mock.patch.object(
                sinks, "header", lambda outputs=False: {**header(outputs), "run_id": self.run_id}):
            super()._run(*args)


def invoke(tmp_path, monkeypatch, *, runner=run_json, library=None, plan=None,
           outputs=False, normalize=None, extra=()):
    plan = selected.full_plan() if plan is None else plan
    library = SelectedSinkLibrary(plan, outputs=outputs) if library is None else library
    source = tmp_path / "chart.csv"
    selected.write_ohlcv(source, selected.csv_rows(plan["supplied_input_bars"]))
    artifact, feed_inventory = selected.write_inventory(tmp_path, [])
    writers = []
    writer_type = phases.RunPhaseWriter
    output_reader = sinks.outputs_reader()

    def writer(fd):
        result = writer_type(fd)
        writers.append(result)
        return result

    def array_outputs(lib, state):
        assert library.path is None, "selected sink outputs must not read retained arrays"
        return output_reader

    with monkeypatch.context() as patch:
        patch.setattr(runner, "load_strategy", lambda path: library)
        if runner is not run_json:
            patch.setattr(runner, "build_report_dict", sinks.frozen_builder_with_arena_stats(
                runner.build_report_dict, library.arena_stats))
        patch.setattr(runner.time, "time", lambda: 10.)
        patch.setattr(planner, "SelectedPrimaryPlanner", lambda path: types.SimpleNamespace(
            plan=lambda *args, **kwargs: dict(plan)))
        patch.setattr(phases, "RunPhaseWriter", writer)
        patch.setattr(runner, "normalize_release_provenance",
                      normalize or (lambda provenance, cpp, receipt, checked: provenance))
        patch.setattr(runner, "require_outputs", lambda lib: None)
        patch.setattr(runner, "enable_outputs", lambda lib, state: None)
        patch.setattr(runner, "make_outputs_reader", array_outputs)
        patch.setattr(runner, "_outputs_manifest", lambda lib, state: output_reader.manifest())
        stdout, stderr = io.StringIO(), io.StringIO()
        argv = selected.selected_argv(
            artifact, source, feed_inventory, extra=[*(["--outputs"] if outputs else []), *extra])
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = runner.main(argv)
    return status, stdout.getvalue(), stderr.getvalue(), library, writers


def masked_bytes(text):
    document = json.loads(text)
    assert text == json.dumps(document, separators=(",", ":"), allow_nan=False) + "\n"
    # Exactly the selected-mode wall-clock masks in DESIGN 4.2.
    document["elapsed_seconds"] = 0.
    document["diagnostics"].pop("phase_timing")
    return json.dumps(document, separators=(",", ":"), allow_nan=False)


@pytest.mark.parametrize("outputs", [False, True])
@pytest.mark.parametrize("pnls", [(10., -7., -0.), (), (-0., 0.), (1e16, 1., -1e16, -2.)])
def test_selected_sink_matches_frozen_array_bytes(
        tmp_path, monkeypatch, outputs, pnls, arena_stats):
    before = invoke(tmp_path, monkeypatch, outputs=outputs,
                    library=sinks.without_report_sink(SelectedSinkLibrary(
                        selected.full_plan(), pnls=pnls, outputs=outputs, arena_stats=arena_stats)))
    after = invoke(tmp_path, monkeypatch, outputs=outputs,
                   library=SelectedSinkLibrary(
                       selected.full_plan(), pnls=pnls, outputs=outputs, arena_stats=arena_stats))
    assert after[3].installed, "L9S missing: selected mode never installs the sink"
    assert (before[0], before[2], after[0], after[2]) == (0, "", 0, ""), (before, after)
    assert not before[3].installed
    sinks.assert_run_profile(json.loads(before[1]))
    sinks.assert_run_profile(json.loads(after[1]))
    assert masked_bytes(after[1]) == masked_bytes(before[1])
    assert after[3].queried == 1 and not after[3].incarnation_reads
    assert after[3].closed == after[3].installed
    assert all(not Path(path).exists() for path in after[3].installed)
    assert after[4][0].phase == "completed"
    report = json.loads(after[1])
    assert report["report_shape"] == swr.report_shape_full(
        after[3].counts["E"], after[3].counts["T"])
    assert report["trades"] == json.loads(before[1])["trades"]


def test_selected_empty_window_is_the_array_anchor_only_report(
        tmp_path, monkeypatch, arena_stats):
    plan = selected.empty_window_plan()
    before = invoke(tmp_path, monkeypatch, plan=plan,
                    library=sinks.without_report_sink(
                        SelectedSinkLibrary(plan, pnls=(), arena_stats=arena_stats)))
    after = invoke(tmp_path, monkeypatch, plan=plan,
                   library=SelectedSinkLibrary(plan, pnls=(), arena_stats=arena_stats))
    assert after[3].installed
    assert (before[0], after[0]) == (0, 0), (before[1], after[1])
    assert not before[3].installed
    assert masked_bytes(after[1]) == masked_bytes(before[1])
    assert len(json.loads(after[1])["equity_curve"]) == plan["window_script_bars"] + 1


def test_selected_unresolved_capital_keeps_the_same_fingerprint(
        tmp_path, monkeypatch, arena_stats):
    normalize = selected.unresolved_capital_normalizer()
    before = invoke(tmp_path, monkeypatch, normalize=normalize,
                    library=sinks.without_report_sink(
                        SelectedSinkLibrary(selected.full_plan(), arena_stats=arena_stats)))
    after = invoke(tmp_path, monkeypatch, normalize=normalize,
                   library=SelectedSinkLibrary(selected.full_plan(), arena_stats=arena_stats))
    assert after[3].installed and before[0] == after[0] == 0, (before[1], after[1])
    assert not before[3].installed
    assert masked_bytes(after[1]) == masked_bytes(before[1])


NAMED_RUN_ID = 'l9s "named"\n漢'


@pytest.mark.parametrize("outputs", [False, True])
def test_selected_named_sink_yields_the_summary_projection(
        tmp_path, monkeypatch, outputs, arena_stats):
    """A selected request that names a sink is no longer run_request_invalid. Its report is the
    current array producer's selected report projected to DESIGN 4.3: trades become report_sink,
    the curve is dropped, outputs keep their six O(1) members, report_window and report_shape
    stay. The reference reader checks the file the engine was told to write."""
    path, spill = tmp_path / "selected report 漢.ndjson", tmp_path / "spill"
    spill.mkdir()
    plan = selected.full_plan()
    before = invoke(tmp_path, monkeypatch, plan=plan, outputs=outputs,
                    library=sinks.without_report_sink(
                        SelectedSinkLibrary(plan, outputs=outputs, arena_stats=arena_stats)))
    library, directories = NamedSinkLibrary(plan, outputs=outputs, arena_stats=arena_stats), []
    temporary = run_json.tempfile.TemporaryFile

    def track(*args, **kwargs):
        directories.append(kwargs.get("dir"))
        return temporary(*args, **kwargs)

    monkeypatch.setattr(run_json.tempfile, "TemporaryFile", track)
    after = invoke(tmp_path, monkeypatch, plan=plan, outputs=outputs, library=library,
                   extra=["--report-sink", str(path), "--run-id", NAMED_RUN_ID,
                          "--spill-dir", str(spill)])
    assert (before[0], before[2], after[0], after[2]) == (0, "", 0, ""), (before[1], after[1])
    assert not before[3].installed
    reference = summaries.read_reference(path)
    expected = summaries.project_summary(json.loads(masked_bytes(before[1])), reference)
    assert masked_bytes(after[1]) == json.dumps(expected, separators=(",", ":"), allow_nan=False)
    document, array = json.loads(after[1]), json.loads(before[1])
    assert tuple(document["report_sink"]) == summaries.REFERENCE_KEYS
    assert not {"trades", "equity_curve"} & set(document)
    assert {"report_window", "report_shape"} <= set(document)
    if outputs:
        assert tuple(document["outputs"]) == summaries.OUTPUT_KEYS
    # The sink is the one the fixture wrote, with the array report's counts and the request's id.
    assert reference["run_id"] == NAMED_RUN_ID
    assert reference["counts"] == library.counts and reference["sha256"] == library.sha
    assert reference["counts"]["E"] == len(array["equity_curve"])
    assert reference["counts"]["T"] == len(array["trades"])
    assert library.descriptors == [(str(path), NAMED_RUN_ID, str(spill))]
    assert library.queried == 1 and library.closed == [str(path)] and path.is_file()
    assert not library.incarnation_reads and after[4][0].phase == "completed"
    # One prepared document in the spill directory: no unlinked sink, no output columns.
    assert directories == [str(spill)]


@pytest.mark.parametrize("bulk", [False, True], ids=["strict", "bulk"])
def test_selected_validation_reads_only_the_first_equity_record(tmp_path, monkeypatch, bulk):
    monkeypatch.setattr(run_json, "_BULK_E_ENABLED", bulk)
    new_sink, equity_passes = run_json._new_report_sink, []

    def counting_sink(*args, **kwargs):
        sink = new_sink(*args, **kwargs)
        original = sink.records

        def records(tag=None):
            yielded = 0
            with contextlib.closing(original(tag)) as source:
                try:
                    for row in source:
                        yielded += 1
                        yield row
                finally:
                    if tag == "E":
                        equity_passes.append(yielded)

        sink.records = records
        return sink

    monkeypatch.setattr(run_json, "_new_report_sink", counting_sink)
    status, text, stderr, library, _ = invoke(tmp_path, monkeypatch)
    assert library.installed and status == 0 and stderr == "", text
    # Verification now folds the finite-result scan and both annualization
    # endpoint reads into its strict-reader pass, bypassing sink.records().
    # records("E") sees only the one-point anchor check and the writer's
    # complete streaming pass, in that order. The bulk path of a concrete path
    # sink writes the equity array from the raw sink bytes (verified and
    # copied window by window) and never calls sink.records("E") for it, so
    # only the anchor check remains observable there; the strict row keeps the
    # original two passes.
    count = library.counts["E"]
    if bulk:
        assert equity_passes == [1]
    else:
        assert equity_passes == [1, count]
        assert equity_passes[-1] == count


@pytest.mark.parametrize("fault", [
    "truncated", "hash", "receipt", "incomplete", "curve-count",
    "anchor-time", "anchor-profit", "anchor-equity", "anchor-null", "retained-arrays",
])
def test_selected_sink_faults_never_publish_a_report(tmp_path, monkeypatch, fault):
    library = SelectedSinkLibrary(selected.full_plan(), fault=fault)
    status, text, stderr, _, writers = invoke(tmp_path, monkeypatch, library=library)
    assert library.installed, "the selected sink must be exercised"
    document = json.loads(text)
    assert status == 1 and stderr == ""
    # Main's finite-result guard precedes selected anchor consistency checks.
    # A nonfinite native E is null on this wire, with the same typed R as
    # the array route's malformed-anchor test in test_run_json_selected_window.
    expected_code = "report_result_invalid" if fault == "anchor-null" else "engine_invariant"
    assert document["code"] == expected_code and "summary" not in document
    if fault == "anchor-null":
        assert document["args"] == {"field": "equity_curve.equity", "reason": "nonfinite"}
    assert len(text.splitlines()) == 1 and writers[0].phase == "result_assembly"
    assert library.closed == library.installed
    assert not Path(library.installed[0]).exists()


@pytest.mark.parametrize("field", sinks.TRADE_FLOATS)
def test_selected_sink_null_trade_keeps_the_existing_failure(tmp_path, monkeypatch, field):
    status, text, stderr, library, writers = invoke(
        tmp_path, monkeypatch, library=SelectedSinkLibrary(selected.full_plan(), bad_field=field))
    assert library.installed and status == 1 and stderr == ""
    document = json.loads(text)
    assert document["code"] == "report_result_invalid"
    assert document["args"] == {"field": "trades." + field, "reason": "nonfinite"}
    assert writers[0].phase == "result_assembly"


def test_selected_serialization_failure_drops_the_prepared_report(tmp_path, monkeypatch):
    original = run_json._stream_json
    files, temporary = [], run_json.tempfile.TemporaryFile

    def track(*args, **kwargs):
        file = temporary(*args, **kwargs)
        files.append(file)
        return file

    def fail(value, destination):
        if isinstance(value, run_json._JSONRecordArray):
            destination.write("[partial")
            raise OSError(28, "injected selected report disk failure")
        return original(value, destination)

    monkeypatch.setattr(run_json.tempfile, "TemporaryFile", track)
    monkeypatch.setattr(run_json, "_stream_json", fail)
    status, text, stderr, library, writers = invoke(tmp_path, monkeypatch)
    assert library.installed and status == 1 and stderr == ""
    assert json.loads(text)["code"] == "report_sink_failed"
    assert len(text.splitlines()) == 1 and "[partial" not in text
    assert writers[0].phase == "serialization"
    assert files and all(file.closed for file in files)


def test_selected_sink_setup_failure_frees_the_state(tmp_path, monkeypatch):
    def fail(*args, **kwargs):
        raise OSError(28, "injected selected sink open failure")

    monkeypatch.setattr(run_json.tempfile, "mkstemp", fail)
    status, text, stderr, library, writers = invoke(tmp_path, monkeypatch)
    assert status == 1 and stderr == ""
    assert json.loads(text)["code"] == "report_sink_failed"
    assert library.calls[-1][0] == "strategy_free"
    assert writers[0].phase == "execution"


def test_selected_configuration_failure_closes_the_installed_sink(tmp_path, monkeypatch):
    library = SelectedSinkLibrary(selected.full_plan())
    library.strategy_set_selected_window_v1 = lambda state, ref: -2 if ref else 0
    status, text, stderr, _, writers = invoke(tmp_path, monkeypatch, library=library)
    assert status == 1 and stderr == "" and library.installed
    assert json.loads(text)["code"] == "window_mode_unsupported"
    assert library.closed == library.installed
    assert not Path(library.installed[0]).exists()
    assert writers[0].phase == "execution"


def test_selected_output_close_failure_precedes_stdout(tmp_path, monkeypatch):
    temporary, files = run_json.tempfile.TemporaryFile, []

    class CloseFailure:
        def __init__(self, file):
            self.file = file

        def __getattr__(self, name):
            return getattr(self.file, name)

        def close(self):
            if not self.file.closed:
                self.file.close()
                raise OSError(28, "injected selected output close failure")

    def track(*args, **kwargs):
        file = temporary(*args, **kwargs)
        if kwargs["mode"] == "w+b":
            assert kwargs["buffering"] == 0
            file = CloseFailure(file)
        files.append(file)
        return file

    monkeypatch.setattr(run_json.tempfile, "TemporaryFile", track)
    status, text, stderr, library, writers = invoke(tmp_path, monkeypatch, outputs=True)
    assert library.installed and status == 1 and stderr == ""
    assert json.loads(text)["code"] == "report_sink_failed"
    assert len(text.splitlines()) == 1 and writers[0].phase == "serialization"
    assert files and all(file.closed for file in files)


def test_selected_old_module_keeps_the_array_report(tmp_path, monkeypatch, arena_stats):
    def library():
        result = SelectedSinkLibrary(selected.full_plan(), arena_stats=arena_stats)
        for name in ("pf_report_sink_version", "strategy_set_report_sink_v1",
                     "strategy_report_sink_receipt_v1"):
            delattr(result, name)
        return result

    before = invoke(tmp_path, monkeypatch,
                    library=SelectedSinkLibrary(selected.full_plan(), arena_stats=arena_stats))
    after = invoke(tmp_path, monkeypatch, library=library())
    assert before[0] == after[0] == 0, (before[1], after[1])
    assert before[3].installed
    assert not after[3].installed
    assert masked_bytes(before[1]) == masked_bytes(after[1])
    for name in sinks.ARENA_DIAGNOSTICS:
        assert (name in json.loads(after[1])["diagnostics"]) == (arena_stats is not None)


NATIVE_DRIVER = r'''
import importlib.util, json, os, sys, tracemalloc
sys.path.insert(0, sys.argv[1])
spec = importlib.util.spec_from_file_location("l9s_producer", sys.argv[2])
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
''' + sinks.NATIVE_FILE_SCRATCH_FIXTURE + r'''
if os.environ.get("PINEFORGE_L9S_PLANNER_SO"):
    runner.trusted_planner_path = lambda: os.environ["PINEFORGE_L9S_PLANNER_SO"]
if sys.argv[3] != "array":
    original = runner.build_report_dict
    def scalars_only(report, *args, **kwargs):
        assert not report.trades and report.trades_len == 0, "legacy trade array was read"
        assert not report.equity_curve and report.equity_curve_len == 0, "legacy curve array was read"
        return original(report, *args, **kwargs)
    runner.build_report_dict = scalars_only
if sys.argv[3] == "memory":
    def memory_failure(text, code=None, code_args=None):
        sys.stderr.write(runner.failure_line(text, code, code_args))
    runner.write_failure = memory_failure
    load = runner.load_strategy
    def measured_load(path):
        lib = load(path)
        run = lib.run_backtest_full
        def measured_run(*args):
            tracemalloc.start()
            return run(*args)
        lib.run_backtest_full = measured_run
        return lib
    runner.load_strategy = measured_load
    publish = runner._write_report
    peak = None
    def measured_publish(document, phases=None):
        global peak
        peak = tracemalloc.get_traced_memory()[1]
        tracemalloc.stop()
        return publish(document, phases)
    runner._write_report = measured_publish
status = runner.main(sys.argv[4:])
if sys.argv[3] == "memory" and status == 0:
    assert peak is not None, "the selected report was not prepared"
    print(json.dumps({"python_peak_bytes": peak}), file=sys.stderr)
raise SystemExit(status)
'''


def native_argv(tmp_path, module, *, start=0, count=97, script_tf="1", empty=False):
    source = tmp_path / "chart.csv"
    with source.open("w", encoding="utf-8", newline="\n") as chart:
        chart.write(sinks.bars.HEADER)
        for index in range(count):
            chart.write(f"{sinks.bars.T0 + index * 60000},{100 + index},{102 + index},"
                        f"{99 + index},{101 + index},{index}\n")
    bound = sinks.bars.T0 + start * 60000
    end = sinks.bars.T0 + (count + 10) * 60000
    # All chosen cases use minute boundaries; align the horizon for aggregation.
    period = int(script_tf) * 60000
    end = ((end + period - 1) // period) * period
    feed_inventory = tmp_path / "native-inventory.json"
    feed_inventory.write_text(json.dumps({
        "schema": inventory.SCHEMA, "source_sha256": "0" * 64,
        "artifact_sha256": hashlib.sha256(Path(module).read_bytes()).hexdigest(),
        "primary_chart_timeframe": script_tf, "entries": [],
    }))
    argv = ["--ohlcv", str(source), "--input-tf", "1", "--script-tf", script_tf,
            "--report-policy", swr.POLICY, "--window-start-ms", str(bound),
            "--window-end-ms", str(end), "--preroll-bars", str(min(start, 3)),
            "--fed-start-ms", str(sinks.bars.T0),
            "--request-feed-inventory", str(feed_inventory),
            "--overrides", '{"initial_capital":"10000"}']
    if empty:
        argv += ["--inputs", '{"Mode":"empty"}']
    return argv


def native_compare(tmp_path, module, argv):
    results = []
    for mode in ("array", "stream"):
        parent, child = socket.socketpair()
        try:
            result = subprocess.run(
                [sys.executable, "-B", "-c", NATIVE_DRIVER, str(ROOT / "docker"),
                 str(ROOT / "docker/run_json.py"), mode, "--so", str(module), *map(str, argv),
                 "--run-phase-fd", str(child.fileno())],
                cwd=ROOT, capture_output=True, text=True, pass_fds=(child.fileno(),))
            child.close()
            with parent.makefile("r") as records:
                actual_phases = [json.loads(line)["phase"] for line in records]
        finally:
            child.close()
            parent.close()
        assert result.returncode == 0 and result.stderr == "", (
            mode, result.returncode, result.stdout, result.stderr)
        assert actual_phases == ["preflight", "execution", "result_assembly",
                                 "serialization", "completed"]
        results.append(masked_bytes(result.stdout))
    assert results[0] == results[1]
    return json.loads(results[1])


@pytest.mark.parametrize("start,count,script_tf,empty", [
    (0, 5, "1", False),        # range-end open-position rewrite
    (3, 97, "1", False),      # pre-roll plus a populated window
    (0, 99, "3", False),      # aggregated selected observations
    (4, 4, "1", True),        # M = 0, anchor only; REVISION F13(2)
])
def test_native_selected_frozen_bytes(tmp_path, start, count, script_tf, empty):
    module = os.environ.get("PINEFORGE_L9S_STRATEGY_SO")
    if not module:
        pytest.skip("SKIP lab module not supplied: PINEFORGE_L9S_STRATEGY_SO; CTest wires it")
    assert Path(module).is_file(), module
    report = native_compare(tmp_path, module, native_argv(
        tmp_path, module, start=start, count=count, script_tf=script_tf, empty=empty))
    if empty:
        assert report["report_window"]["counts"]["window_script_bars"] == 0
    else:
        assert report["trades"] and any(trade["open_at_end"] for trade in report["trades"])


def test_native_selected_corpus_frozen_bytes(tmp_path):
    """Ready selected corpus cases: [{so, argv}], prepared once by CORE-TOP."""
    manifest = os.environ.get("PINEFORGE_L9S_CORPUS_CASES")
    if not manifest:
        pytest.skip("SKIP selected corpus proof requires PINEFORGE_L9S_CORPUS_CASES")
    cases = json.loads(Path(manifest).read_text())
    assert cases, "the selected byte proof must contain real cases"
    for case in cases:
        argv = case["argv"]
        assert "--report-policy" in argv and "selected-window/v1" in argv, case
        assert "--report-sink" not in argv, "the array full-report comparison has no named sink"
        assert Path(case["so"]).is_file(), case
        native_compare(tmp_path, case["so"], argv)
