"""Named-sink summaries and native temporary-sink corpus projections.

Only native calls are replaced in the Python rows. The real reader, report
assembly and JSON writer run unchanged. The corpus row uses real native
calls throughout, with a sink-off FULL oracle and an observed run_json.main.
Native rows consume lab-built modules and are built and run by the verification
host, not by this file.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import pf_sink
import run_json
import test_run_json_bin as bars
import test_run_json_sink as full
from test_run_json_sink import arena_stats, frozen  # noqa: F401 (shared pytest fixtures)


OUTPUT_KEYS = ("schema_version", "message_format", "manifest_sha256",
               "manifest", "constants", "hlines")
REFERENCE_KEYS = ("path", "schema", "version", "run_id", "counts", "sha256")

# Own-peak sink-route excess (no recorded outputs): at most 1.86 MB measured
# with 64 KiB windows, 2.5 MiB with the host swing, plus the two sink-only
# large windows (equity, trade spool; the final report window is on both
# routes) and the 64 KiB reader page; no bar or trade term. A bound, not a fit.
SINK_RSS_ALLOWANCE_BYTES = (5 * 1024 * 1024 // 2
                            + 2 * getattr(run_json, "_ARTIFACT_LARGE_WINDOW", 0) + 64 * 1024)


class SummaryLibrary(full.SinkLibrary):
    """Reuse L9A's independent trade/equity fixture and add named-path lifetime."""

    def __init__(self, *, all_tags=False, **kwargs):
        super().__init__(**kwargs)
        self.all_tags = all_tags
        self.descriptors = []
        self.runs = []

    def install(self, state, pointer):
        descriptor = pointer._obj
        assert descriptor.struct_size == ctypes.sizeof(run_json.ReportSinkC)
        assert descriptor.version == 1
        self.path = os.fsdecode(descriptor.path)
        self.path_expected_at_free = Path(self.path).exists()
        self.run_id = descriptor.run_id.decode("utf-8")
        self.descriptors.append((self.path, self.run_id, os.fsdecode(descriptor.scratch_dir)))
        self.installed.append(self.path)
        return 0

    def run(self, *args):
        with mock.patch.object(full, "write_sink", self.write_sink):
            super().run(*args)
        if self.path is not None:
            self.runs.append((self.path, Path(self.path).read_bytes()))

    def write_sink(self, path, rows, *, outputs=False, fault=None):
        head = full.header(outputs)
        head["run_id"] = "wrong request" if fault == "run-id" else self.run_id
        rows = list(rows)
        if self.all_tags:
            head["records"] += [
                full.descriptor("X", ["timestamp", "bar_index", "name", "value"],
                                ["i64", "i64", "str", "f64?"], ["unix_ms", "", "", ""]),
                full.descriptor("H", ["hash"], ["hex64"], [""]),
            ]
            rows += [["X", bars.T0, 0, "trace\"\n漢", -0.],
                     ["H", "0123456789abcdef"], ["H", "fedcba9876543210"]]
        counts = {entry["type"]: 0 for entry in head["records"]}
        digest = hashlib.sha256()
        with open(path, "wb") as file:
            self.path_expected_at_free = True
            line = full.encoded(head)
            file.write(line)
            digest.update(line)
            for row in rows:
                counts[row[0]] += 1
                line = full.encoded(row)
                file.write(line)
                digest.update(line)
            trailer = {"trailer": "pf-report-sink", "version": 1,
                       "counts": dict(counts), "sha256": digest.hexdigest()}
            if fault == "counts":
                trailer["counts"]["E"] += 1
            if fault == "hash":
                trailer["sha256"] = "0" * 64
            if fault != "truncated":
                file.write(full.encoded(trailer))
            if fault == "trailing":
                file.write(b"\n")
        return counts, digest.hexdigest()

    def receipt(self, state, pointer):
        status = super().receipt(state, pointer)
        if self.fault == "receipt-count":
            pointer._obj.count_e += 1
        return status

    def free(self, state):
        if self.path is not None and not self.path_expected_at_free:
            # A named path absent at install stays absent if opening it fails.
            assert not Path(self.path).exists()
            self.freed.append(self.path)
            self.path = None
            return
        super().free(state)  # Existing or opened paths must remain until strategy_free.


def read_reference(path):
    """Use the public reader, then independently check its digest over raw lines."""
    path = Path(path)
    with pf_sink.SinkReader(path) as reader:
        observed = {entry["type"]: 0 for entry in reader.header["records"]}
        for record in reader:
            observed[record.tag] += 1
        assert reader.complete
        assert observed == reader.counts == reader.trailer["counts"]
        reference = {"path": str(path), "schema": reader.header["schema"],
                     "version": reader.header["version"], "run_id": reader.header["run_id"],
                     "counts": observed, "sha256": reader.sha256}
    digest, previous = hashlib.sha256(), None
    with path.open("rb") as file:
        for line in file:
            if previous is not None:
                digest.update(previous)
            previous = line
    trailer = json.loads(previous)
    assert reference["sha256"] == trailer["sha256"] == digest.hexdigest()
    return reference


def project_summary(document, reference):
    """Independent section-4.3 projection of the full producer's document."""
    expected = {}
    for key, value in document.items():
        if key == "trades":
            expected["report_sink"] = reference
        elif key == "equity_curve":
            continue
        elif key == "outputs":
            expected[key] = {name: value[name] for name in OUTPUT_KEYS}
        else:
            expected[key] = value
    return expected


@pytest.mark.parametrize("outputs", [False, True])
@pytest.mark.parametrize("pnls", [(10., -7., -0.), (), (-0., 0.), (1e16, 1., -1e16, -2.)])
def test_named_summary_matches_frozen_projection(
        tmp_path, monkeypatch, outputs, pnls, arena_stats):
    """Fail-before: the base parser refuses --report-sink and emits no summary."""
    path = tmp_path / "report 漢.ndjson"
    spill = tmp_path / "spill"
    spill.mkdir()
    extra = ["--outputs"] if outputs else []
    before = full.invoke(tmp_path, monkeypatch,
                         library=full.without_report_sink(full.SinkLibrary(
                             outputs=outputs, pnls=pnls, arena_stats=arena_stats)), extra=extra)
    library = SummaryLibrary(
        outputs=outputs, pnls=pnls, all_tags=outputs, arena_stats=arena_stats)
    after = full.invoke(tmp_path, monkeypatch, library=library,
                        extra=[*extra, "--report-sink", str(path), "--run-id", "run\"\n漢",
                               "--spill-dir", str(spill)],
                        forbid_output_arrays=outputs)
    assert before[0] == after[0] == 0 and before[2] == after[2] == "", after[1]
    assert not before[3].installed
    full.assert_run_profile(json.loads(before[1]))
    full.assert_run_profile(json.loads(after[1]))
    reference = read_reference(path)
    expected = project_summary(json.loads(before[1]), reference)
    assert after[1] == json.dumps(expected, separators=(",", ":"), allow_nan=False) + "\n"
    document = json.loads(after[1])
    assert tuple(document["report_sink"]) == REFERENCE_KEYS
    assert reference["run_id"] == "run\"\n漢"
    assert reference["counts"]["T"] == len(json.loads(before[1])["trades"])
    assert reference["counts"]["E"] == len(json.loads(before[1])["equity_curve"])
    assert library.queried == 1 and library.freed == [str(path)] and path.is_file()
    assert library.descriptors == [(str(path), reference["run_id"], str(spill))]
    if outputs:
        assert tuple(document["outputs"]) == OUTPUT_KEYS
        assert tuple(reference["counts"]) == tuple("ETOVKXH")


def test_summary_reads_back_all_records_without_output_column_temporaries(tmp_path, monkeypatch):
    path, spill = tmp_path / "report.ndjson", tmp_path / "spill"
    spill.mkdir()
    calls, temporaries = [], []
    original_reader, original_temporary = pf_sink.SinkReader, run_json.tempfile.TemporaryFile

    class Reader(original_reader):
        def __init__(self, source):
            super().__init__(source)
            self.source = source

        def __exit__(self, *args):
            calls.append((self.source, self.complete))
            return super().__exit__(*args)

    def temporary(*args, **kwargs):
        file = original_temporary(*args, **kwargs)
        assert kwargs["dir"] == str(spill)
        assert os.fstat(file.fileno()).st_nlink == 0
        temporaries.append(file)
        return file

    monkeypatch.setattr(pf_sink, "SinkReader", Reader)
    monkeypatch.setattr(run_json.tempfile, "TemporaryFile", temporary)
    status, text, stderr, _ = full.invoke(
        tmp_path, monkeypatch, library=SummaryLibrary(outputs=True, all_tags=True),
        extra=["--outputs", "--report-sink", str(path), "--spill-dir", str(spill)],
        forbid_output_arrays=True)
    assert status == 0 and stderr == "", text
    assert calls and all(source == str(path) and complete for source, complete in calls)
    # One small prepared JSON file; no temporary sink or per-output columns.
    assert len(temporaries) == 1 and temporaries[0].closed
    assert json.loads(text)["report_sink"] == read_reference(path)


@pytest.mark.parametrize("fault", [
    "truncated", "hash", "counts", "trailing", "receipt", "receipt-count", "incomplete", "run-id",
])
def test_bad_sink_never_publishes_summary(tmp_path, monkeypatch, fault):
    path = tmp_path / "bad.ndjson"
    status, text, stderr, library = full.invoke(
        tmp_path, monkeypatch, library=SummaryLibrary(fault=fault),
        extra=["--report-sink", str(path)])
    assert status == 1 and stderr == ""
    assert json.loads(text)["code"] == "engine_invariant"
    assert len(text.splitlines()) == 1 and "summary" not in json.loads(text)
    assert library.freed == [str(path)]


@pytest.mark.parametrize("field", full.TRADE_FLOATS)
def test_summary_still_refuses_nonfinite_trade_fields(tmp_path, monkeypatch, field):
    status, text, stderr, _ = full.invoke(
        tmp_path, monkeypatch, library=SummaryLibrary(bad_field=field),
        extra=["--report-sink", str(tmp_path / "report.ndjson")])
    assert status == 1 and stderr == ""
    assert json.loads(text)["code"] == "report_result_invalid"
    assert json.loads(text)["args"] == {"field": "trades." + field, "reason": "nonfinite"}


def test_bench_only_body_run_writes_named_path(tmp_path, monkeypatch):
    path = tmp_path / "report.ndjson"
    path.write_bytes(b"previous run\n")
    library = SummaryLibrary()
    original_run = library.run_backtest_full

    def run(*args):
        if library.path != str(path):
            assert path.read_bytes() == b"previous run\n"
        return original_run(*args)

    library.run_backtest_full = run
    status, text, stderr, _ = full.invoke(
        tmp_path, monkeypatch, library=library,
        extra=["--report-sink", str(path), "--bench", "--warmup", "2", "--repeats", "3"])
    assert status == 0 and stderr == "", text
    assert len(library.installed) == len(library.freed) == 6
    assert library.installed[-1] == str(path)
    assert all(source != str(path) and not Path(source).exists() for source in library.installed[:-1])
    assert path.read_bytes() == library.runs[-1][1]
    assert json.loads(text)["report_sink"] == read_reference(path)


def test_full_mode_with_run_id_and_spill_keeps_frozen_bytes(
        tmp_path, monkeypatch, arena_stats):
    before = full.invoke(
        tmp_path, monkeypatch,
        library=full.without_report_sink(full.SinkLibrary(arena_stats=arena_stats)))
    library = SummaryLibrary(arena_stats=arena_stats)
    after = full.invoke(tmp_path, monkeypatch, library=library,
                        extra=["--run-id", "caller id", "--spill-dir", str(tmp_path)])
    assert before[0] == after[0] == 0 and before[2] == after[2] == ""
    assert not before[3].installed
    full.assert_run_profile(json.loads(before[1]))
    full.assert_run_profile(json.loads(after[1]))
    assert after[1] == before[1]
    assert library.descriptors[0][1:] == ("caller id", str(tmp_path))
    assert not Path(library.installed[0]).exists()


def test_explicit_sink_requires_sink_api_but_legacy_full_mode_still_works(tmp_path, monkeypatch):
    source = bars.csv_file(tmp_path)
    status, document, stderr, library = bars.invoke(
        monkeypatch, "--ohlcv", str(source), "--report-sink", str(tmp_path / "report.ndjson"))
    assert status == 1 and stderr == ""
    assert document["code"] == "strategy_library_incompatible"
    assert document["args"] == {"reason": "symbol_missing"}
    assert library.calls[-1] == "strategy_free" and "run_backtest_full" not in library.calls
    assert bars.invoke(monkeypatch, "--ohlcv", str(source))[0] == 0


@pytest.mark.parametrize("flag", ["--report-sink", "--run-id", "--spill-dir"])
@pytest.mark.parametrize("bad", ["embedded\0nul", "bad\udcff"])
def test_sink_options_reject_invalid_c_strings_before_loading(tmp_path, monkeypatch, flag, bad):
    source = bars.csv_file(tmp_path)
    status, document, stderr, library = bars.invoke(
        monkeypatch, "--ohlcv", str(source), flag, bad)
    assert status == 1 and stderr == "" and library.calls == []
    assert document["code"] == "run_request_invalid"
    assert document["args"] == {"option": "arguments"}


@pytest.mark.parametrize("outputs", [False, True])
@pytest.mark.parametrize("pnls", [(10., -7., -0.), (), (-0., 0.), (1e16, 1., -1e16, -2.)])
def test_selected_mode_honours_named_sink(tmp_path, monkeypatch, outputs, pnls, arena_stats):
    # Import at call time: test_run_json_selected_sink imports this module's
    # read_reference and project_summary helpers.
    import test_run_json_selected_sink as selected_sinks

    path, spill = tmp_path / "selected report 漢.ndjson", tmp_path / "spill"
    spill.mkdir()
    run_id = 'selected "summary"\n漢'
    plan = selected_sinks.selected.full_plan()
    before = selected_sinks.invoke(
        tmp_path, monkeypatch, plan=plan, outputs=outputs,
        library=full.without_report_sink(selected_sinks.SelectedSinkLibrary(
            plan, pnls=pnls, outputs=outputs, arena_stats=arena_stats)))
    library = selected_sinks.NamedSinkLibrary(
        plan, pnls=pnls, outputs=outputs, arena_stats=arena_stats)
    after = selected_sinks.invoke(
        tmp_path, monkeypatch, plan=plan, outputs=outputs, library=library,
        extra=["--report-sink", str(path), "--run-id", run_id, "--spill-dir", str(spill)])
    assert (before[0], before[2], after[0], after[2]) == (0, "", 0, ""), (before[1], after[1])
    assert not before[3].installed
    assert path.is_file()
    reference = read_reference(path)
    document, array = json.loads(after[1]), json.loads(before[1])
    assert document["report_sink"] == reference
    assert tuple(document["report_sink"]) == REFERENCE_KEYS
    assert not {"trades", "equity_curve"} & set(document)
    assert {"report_window", "report_shape"} <= set(document)
    if outputs:
        assert tuple(document["outputs"]) == OUTPUT_KEYS
    assert reference["path"] == str(path)
    assert reference["run_id"] == run_id
    assert reference["counts"] == library.counts
    assert reference["counts"]["T"] == len(array["trades"])
    assert reference["counts"]["E"] == len(array["equity_curve"])
    assert reference["sha256"] == library.sha
    assert library.descriptors == [(str(path), run_id, str(spill))]
    assert library.queried == 1 and library.closed == [str(path)]
    assert not library.incarnation_reads and after[4][0].phase == "completed"
    expected = project_summary(json.loads(selected_sinks.masked_bytes(before[1])), reference)
    assert selected_sinks.masked_bytes(after[1]) == json.dumps(
        expected, separators=(",", ":"), allow_nan=False)


def native_summary_compare(tmp_path, module, argv):
    # Project the current array report, including main's annualization and
    # scratch diagnostics, and retain the exact document comparison.
    path = tmp_path / "native-report.ndjson"
    results = []
    for mode, extra in (
        ("array", []),
        ("stream",
         ["--report-sink", str(path), "--run-id", "native proof", "--spill-dir", str(tmp_path)]),
    ):
        result = subprocess.run(
            [sys.executable, "-B", "-c", full.NATIVE_DRIVER, str(ROOT / "docker"),
             str(ROOT / "docker/run_json.py"), mode, "--so", str(module), *map(str, argv), *extra],
            cwd=ROOT, capture_output=True, text=True)
        assert result.returncode == 0 and result.stderr == "", (mode, result.stdout, result.stderr)
        results.append(json.loads(full.mask_bytes(result.stdout)))
    reference = read_reference(path)
    assert reference["counts"]["T"] == len(results[0]["trades"])
    assert reference["counts"]["E"] == len(results[0]["equity_curve"])
    expected = project_summary(results[0], reference)
    assert json.dumps(results[1], separators=(",", ":"), allow_nan=False) == json.dumps(
        expected, separators=(",", ":"), allow_nan=False)
    assert reference["run_id"] == "native proof"


@pytest.mark.parametrize("fixture,outputs", [("PINEFORGE_L9B_STRATEGY_SO", False),
                                            ("PINEFORGE_L9B_OUTPUTS_SO", True)])
@pytest.mark.parametrize("binary", [False, True])
@pytest.mark.parametrize("bench", [False, True])
def test_native_frozen_summary_bytes(tmp_path, fixture, outputs, binary, bench):
    module = os.environ.get(fixture)
    if not module:
        pytest.skip("SKIP lab module not supplied: " + fixture + "; CTest wires native targets")
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
    # Protect section 4.2's default bytes as well as section 4.3's new shape.
    full.native_compare(tmp_path, module, argv)
    native_summary_compare(tmp_path, module, argv)


def test_native_empty_outputs_keep_constants_and_metadata(tmp_path):
    module = os.environ.get("PINEFORGE_L9B_OUTPUTS_SO")
    if not module:
        pytest.skip("SKIP lab module not supplied: PINEFORGE_L9B_OUTPUTS_SO")
    assert Path(module).is_file(), module
    source = bars.csv_file(tmp_path, count=7)
    argv = ["--ohlcv", source, "--input-tf", "1", "--script-tf", "1",
            "--outputs", "--inputs", '{"publish":"false"}']
    native_summary_compare(tmp_path, module, argv)
    reference = read_reference(tmp_path / "native-report.ndjson")
    assert reference["counts"]["O"] == reference["counts"]["V"] == 0
    assert reference["counts"]["K"] == 1


def test_native_sink_open_failure_never_publishes_summary(tmp_path):
    module = os.environ.get("PINEFORGE_L9B_STRATEGY_SO")
    if not module:
        pytest.skip("SKIP lab module not supplied: PINEFORGE_L9B_STRATEGY_SO")
    assert Path(module).is_file(), module
    source = bars.csv_file(tmp_path)
    result = subprocess.run(
        [sys.executable, "-B", str(ROOT / "docker/run_json.py"), "--so", module,
         "--ohlcv", str(source), "--report-sink", str(tmp_path / "missing" / "report.ndjson")],
        cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 1 and result.stderr == "", (result.stdout, result.stderr)
    assert len(result.stdout.splitlines()) == 1
    assert json.loads(result.stdout)["code"] == "report_sink_failed"
    assert "summary" not in json.loads(result.stdout)


CORPUS_OWN_PEAK_RSS = r'''
if sys.platform == "linux":
    own_peak_rss_bytes = None
    for line in pathlib.Path("/proc/self/status").read_text(encoding="ascii").splitlines():
        if line.startswith("VmHWM:"):
            fields = line.split()
            assert len(fields) == 3 and fields[1].isdigit() and fields[2] == "kB", (
                "unparsable Linux VmHWM", line)
            own_peak_rss_bytes = int(fields[1]) * 1024
            break
    assert own_peak_rss_bytes is not None, "missing Linux VmHWM"
    observation["own_peak_rss_bytes"] = own_peak_rss_bytes
'''


CORPUS_PROJECTION_DRIVER = r'''
import argparse, ctypes, json, pathlib, sys
from contextlib import ExitStack
sys.path.insert(0, sys.argv[1])
import run_json as runner

mode, observation_path, module = sys.argv[2:5]
argv = sys.argv[5:]
snapshots, sinks = [], []

def observe(frame, event, result):
    # Observe real return values and the report immediately before free.
    # Do not replace a loader, native export, report builder or sink factory.
    if event == "return" and frame.f_code is runner._new_report_sink.__code__:
        assert result is not None and not result.named
        sinks.append(result)
    if event == "call" and frame.f_code is runner._free_sink_run.__code__:
        report = frame.f_locals["report"]
        assert ctypes.sizeof(report.metrics) == 768
        snapshots.append({
            "metrics": ctypes.string_at(ctypes.addressof(report.metrics), 768).hex(),
            "counters": {name: int(getattr(report, name)) for name in (
                "total_trades", "input_bars_processed", "script_bars_processed",
                "security_feeds_total", "security_eval_complete_total",
                "security_eval_partial_total", "magnifier_sub_bars_total",
                "magnifier_sample_ticks_total", "input_tf_seconds",
                "script_tf_seconds", "script_tf_ratio", "needs_aggregation",
                "bar_magnifier_enabled")},
            "arrays": [bool(report.trades), int(report.trades_len),
                       bool(report.equity_curve), int(report.equity_curve_len)],
            "has_sink": frame.f_locals["sink"] is not None,
        })

def full_oracle():
    # No sink setter is called: this is the real native FULL report, using
    # the producer's settings, array builder, diagnostics and JSON writer.
    # Unknown options fail, so a manifest cannot silently lose configuration.
    parser = argparse.ArgumentParser(allow_abbrev=False)
    for name in ("ohlcv", "ohlcv-bin", "syminfo", "symbol-feeds", "generated-cpp",
                 "spill-dir"):
        parser.add_argument("--" + name, type=pathlib.Path)
    for name in ("inputs", "overrides", "input-tf", "script-tf", "bar-magnifier",
                 "chart-tz", "transpiled"):
        parser.add_argument("--" + name, default="")
    parser.add_argument("--magnifier-samples", type=int, default=4)
    parser.add_argument("--magnifier-dist", default="endpoints")
    parser.add_argument("--magnifier-volume-weighted", action="store_true")
    parser.add_argument("--trade-start-ms", type=int)
    parser.add_argument("--verify-bin", choices=("full", "sidecar"), default="full")
    parser.add_argument("--outputs", action="store_true")
    args = parser.parse_args(argv)
    args.profile = "unjailed"
    inputs = runner.parse_kv_json(args.inputs, "--inputs")
    overrides = runner.parse_kv_json(args.overrides, "--overrides")
    input_tf = runner._utf8_option(args.input_tf.strip(), "--input-tf")
    script_tf = runner._utf8_option(args.script_tf.strip(), "--script-tf")
    magnifier = int(runner.parse_bool(args.bar_magnifier))
    weighted = args.magnifier_volume_weighted and magnifier == 1
    with ExitStack() as resources:
        runner._resolve_chart_source(args)
        if args.ohlcv_bin is not None:
            chart = resources.enter_context(
                runner.load_bars_bin(args.ohlcv_bin, verify_bin=args.verify_bin))
            feed, count = chart["bars"], chart["n"]
            source_sha = chart["source_values_sha256"]
        else:
            feed, count, source_sha = runner.load_bars(args.ohlcv)
        assert count > 0
        first, last = feed[0].timestamp, feed[count - 1].timestamp
        lib = runner.load_strategy(pathlib.Path(module))
        checked = runner.uses_checked_settings(lib)
        if args.outputs:
            runner.require_outputs(lib)
        symbols = runner._run_symbol_feeds(args, resources)
        report = runner.ReportC()
        state = runner.create_strategy(lib, checked)
        try:
            runner.apply_settings(lib, state, inputs, overrides, checked)
            syminfo = {}
            if args.syminfo:
                syminfo = runner.apply_syminfo(lib, state, args.syminfo)
            if symbols:
                runner.install_symbol_feeds(lib, state, symbols)
            if args.trade_start_ms is not None:
                lib.strategy_set_trade_start_time(state, args.trade_start_ms)
            if args.chart_tz:
                lib.strategy_set_chart_timezone(state, args.chart_tz.encode("utf-8"))
            if weighted:
                lib.strategy_set_magnifier_volume_weighted(state, 1)
            if args.outputs:
                runner.enable_outputs(lib, state)
            runner._release_copied_feeds(symbols)
            settings_receipt = runner._release_settings_receipt(lib, state, checked)
            lib.run_backtest_full(
                state, feed, count, input_tf, script_tf, magnifier,
                max(2, args.magnifier_samples), runner.parse_magnifier_dist(args.magnifier_dist),
                ctypes.byref(report))
            failure = runner.run_failure(lib, state)
            if failure is not None:
                runner.write_failure(*failure)
                return 1
            runtime = {
                "input_tf": input_tf.decode(), "script_tf": script_tf.decode(),
                "input_tf_seconds": int(report.input_tf_seconds),
                "script_tf_seconds": int(report.script_tf_seconds),
                "script_tf_ratio": int(report.script_tf_ratio),
                "needs_aggregation": bool(report.needs_aggregation),
                "bar_magnifier": bool(magnifier),
                "magnifier_samples": max(2, args.magnifier_samples),
                "magnifier_dist": args.magnifier_dist.strip().lower() or "endpoints",
                "magnifier_volume_weighted": bool(weighted),
                "trade_start_ms": args.trade_start_ms, "chart_tz": args.chart_tz,
            }
            if syminfo:
                runtime["syminfo"] = syminfo
            if symbols:
                runtime["symbol_feeds"] = runner.symbol_feeds_record(symbols)
            accessor = getattr(lib, "strategy_closed_trade_entry_incarnation", None)
            document = runner.build_report_dict(
                report, args.ohlcv, count, first, last, 0., inputs, overrides, runtime,
                trade_entry_incarnations=(
                    (lambda i: int(accessor(state, i))) if accessor is not None else None))
            runner._validate_report_result(document)
            runner._add_scratch_arena_diagnostics(lib, document["diagnostics"], args)
            if args.outputs:
                document["outputs"] = runner._legacy_streamed_outputs_block(
                    lib, state, validate=True)
                runtime["outputs"] = True
            # Match the real producer's provenance work, including its fallback.
            try:
                provenance = runner.build_provenance(
                    runner.engine_version(lib), None, runner.parse_bool(args.transpiled),
                    inputs, overrides, runtime, source_feed_sha256=source_sha)
                provenance["codegen"]["generated_cpp_sha256"] = (
                    runner._sha256_file(args.generated_cpp) if args.generated_cpp else None)
                cpp_text = ""
                if args.generated_cpp:
                    cpp_text = args.generated_cpp.read_text(encoding="utf-8", errors="replace")
                provenance = runner.normalize_release_provenance(
                    provenance, cpp_text, settings_receipt, checked)
                document["fingerprint"] = runner.build_fingerprint(provenance)
            except Exception:
                document["fingerprint"] = None
            prepared = runner._prepare_report_json(document, resources, args, None)
        finally:
            runner._free_sink_run(lib, state, report, None)
        return runner._write_report(prepared)

sys.setprofile(observe)
try:
    if mode == "full":
        try:
            status = full_oracle()
        except runner.RunFailure as failure:
            runner.write_failure(str(failure), failure.code, failure.code_args)
            status = failure.exit_status
    else:
        assert mode == "temporary"
        status = runner.main(["--so", module, *argv])
finally:
    sys.setprofile(None)
assert len(snapshots) == 1, "expected exactly one native run and cleanup"
assert len(sinks) == (0 if mode == "full" else 1), "wrong report route"
observation = snapshots[0]
observation["sink_calls"] = len(sinks)
if sinks:
    sink = sinks[0]
    assert not sink.named and sink.closed and not pathlib.Path(sink.path).exists()
    observation["sink_counts"] = sink.counts
''' + CORPUS_OWN_PEAK_RSS + r'''
pathlib.Path(observation_path).write_text(json.dumps(observation), encoding="utf-8")
raise SystemExit(status)
'''


def corpus_projection_run(directory, module, argv, mode):
    """Measure the producer's peak RSS; read its artifacts after it exits."""
    assert hasattr(os, "wait4") and sys.platform in ("linux", "darwin"), "wait4 RSS required"
    directory.mkdir()
    document_path, stderr_path = directory / "report.json", directory / "stderr.txt"
    observation_path = directory / "observation.json"
    environment = dict(os.environ, PYTHONHASHSEED="0", PYTHONOPTIMIZE="0",
                       PINEFORGE_RUN_JSON_PROFILE="unjailed")
    for name in ("PF_SCRATCH_ARENA_FD", "PF_SCRATCH_ARENA_BYTES"):
        environment.pop(name, None)
    command = [
        sys.executable, "-B", "-c", CORPUS_PROJECTION_DRIVER, str(ROOT / "docker"),
        mode, str(observation_path), str(Path(module).resolve()), *argv,
    ]
    started = time.perf_counter()
    with document_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        with subprocess.Popen(command, cwd=ROOT, env=environment,
                              stdout=stdout, stderr=stderr) as child:
            _, status, usage = os.wait4(child.pid, 0)
            child.returncode = os.waitstatus_to_exitcode(status)
    elapsed = time.perf_counter() - started
    assert stderr_path.read_bytes() == b"", (mode, stderr_path.read_text())
    assert observation_path.is_file(), (mode, child.returncode, document_path.read_text())
    observation = json.loads(observation_path.read_text())
    observation.update(
        wait4_peak_rss_bytes=int(usage.ru_maxrss) * (1 if sys.platform == "darwin" else 1024),
        wall_seconds=elapsed, cpu_seconds=usage.ru_utime + usage.ru_stime)
    # On Linux the kernel carries the parent's high-water into the child's
    # rusage across the exec, so wait4 cannot measure just this producer.
    if sys.platform == "linux":
        observation["peak_rss_bytes"] = observation["own_peak_rss_bytes"]
    else:
        # Keep Darwin's existing wait4 accounting in bytes; the inheritance
        # was observed on Linux only.
        observation["peak_rss_bytes"] = observation["wait4_peak_rss_bytes"]
    assert observation["peak_rss_bytes"] > 0, observation
    return child.returncode, json.loads(document_path.read_text()), observation


@pytest.mark.skipif(
    sys.platform != "linux",
    reason="Linux VmHWM separates the child's own peak from the parent's inherited high-water")
def test_corpus_projection_measures_own_peak_rss(tmp_path, monkeypatch):
    parent_allocation_bytes = 96 * 1024 * 1024
    child_allocation_bytes = 24 * 1024 * 1024
    page_size = os.sysconf("SC_PAGE_SIZE")
    child_driver = r'''
import json, pathlib, sys
allocation_bytes, page_size = map(int, sys.argv[5:])
allocation = bytearray(allocation_bytes)
for offset in range(0, allocation_bytes, page_size):
    allocation[offset] = 1
print("{}")
observation = {}
''' + CORPUS_OWN_PEAK_RSS + r'''
pathlib.Path(sys.argv[3]).write_text(json.dumps(observation), encoding="utf-8")
'''
    monkeypatch.setattr(sys.modules[__name__], "CORPUS_PROJECTION_DRIVER", child_driver)
    parent_allocation = bytearray(parent_allocation_bytes)
    try:
        for offset in range(0, parent_allocation_bytes, page_size):
            parent_allocation[offset] = 1
        status, _, observation = corpus_projection_run(
            tmp_path / "rss", __file__, [str(child_allocation_bytes), str(page_size)], "full")
        assert status == 0, observation
        own_peak = observation["own_peak_rss_bytes"]
        assert child_allocation_bytes <= own_peak < parent_allocation_bytes // 2, observation
        assert observation["peak_rss_bytes"] == own_peak, observation
        wait4_peak = observation["wait4_peak_rss_bytes"]
        assert wait4_peak >= parent_allocation_bytes, observation
        assert wait4_peak - own_peak >= parent_allocation_bytes // 2, observation
    finally:
        del parent_allocation


def assert_projection_bits(expected, actual, field):
    """Include signed zero and every JSON double; no tolerance or rounding."""
    assert type(actual) is type(expected), field
    if isinstance(expected, float):
        assert struct.pack("!d", actual) == struct.pack("!d", expected), field
    elif isinstance(expected, dict):
        assert actual.keys() == expected.keys(), field
        for name in expected:
            assert_projection_bits(expected[name], actual[name], field + "." + name)
    elif isinstance(expected, list):
        assert len(actual) == len(expected), field
        for index, (left, right) in enumerate(zip(expected, actual)):
            assert_projection_bits(left, right, f"{field}[{index}]")
    else:
        assert actual == expected, field


def test_native_corpus_subset_frozen_summary_bytes(tmp_path, record_property):
    """Consume ready corpus modules/feeds, never build or derive them.

    PINEFORGE_L9B_CORPUS_CASES names a nonempty JSON list of
    {"so": ".../corpus/validation/<probe>/strategy.so", "argv": [...],
     "projection": "calendar" | "terminal" | "short-run" | "failure"}.
    All four projections are required. Failure rows additionally pin
    "code" and "args"; they must fail during real execution, after installing
    the temporary sink. A short-run feed contains one or two script bars.
    """
    manifest = os.environ.get("PINEFORGE_L9B_CORPUS_CASES")
    if not manifest:
        pytest.skip("SKIP corpus proof requires PINEFORGE_L9B_CORPUS_CASES (a JSON list of cases)")
    assert Path(manifest).is_file(), manifest
    cases = json.loads(Path(manifest).read_text())
    assert isinstance(cases, list) and cases, "the corpus proof must name real probes"
    required = {"calendar", "terminal", "short-run", "failure"}
    assert required <= {case["projection"] for case in cases}, "incomplete corpus projections"
    corpus = (ROOT / "corpus").resolve()
    inventory = json.loads((corpus / "run_manifest.json").read_text())
    assert inventory["kind"] == "pineforge-corpus-run-manifest"
    # Validate the entire manifest before running anything. Missing modules
    # fail; neither globbing existing modules nor filtering can shrink proof.
    forbidden = {"--so", "--profile", "--report-sink", "--report-sink-fd", "--bench",
                 "--report-policy", "--validate-window-only", "--capabilities-json"}
    for case in cases:
        module = Path(case["so"]).resolve()
        assert module.is_file(), case
        assert module.parent.relative_to(corpus).as_posix() in inventory["tapes"], case
        for name in ("generated.cpp", "strategy.pine", "inputs.json"):
            assert (module.parent / name).is_file(), (case, name)
        assert case["projection"] in required, case
        assert isinstance(case["argv"], list) and all(isinstance(v, str) for v in case["argv"])
        assert not {v.split("=", 1)[0] for v in case["argv"]} & forbidden, case
        if case["projection"] == "failure":
            assert isinstance(case["code"], str) and case["code"], case
            assert isinstance(case["args"], dict), case
    for index, case in enumerate(cases):
        before = corpus_projection_run(tmp_path / f"{index}-full", case["so"], case["argv"], "full")
        after = corpus_projection_run(
            tmp_path / f"{index}-temporary", case["so"], case["argv"], "temporary")
        status, reference, full_observation = before
        sink_status, document, sink_observation = after
        label = f"{index}:{case['projection']}:{case['so']}"
        assert status == sink_status == (1 if case["projection"] == "failure" else 0), label
        assert not full_observation["has_sink"] and full_observation["sink_calls"] == 0, label
        assert sink_observation["has_sink"] and sink_observation["sink_calls"] == 1, label
        assert sink_observation["arrays"] == [False, 0, False, 0], label
        metric_bytes = bytes.fromhex(full_observation["metrics"])
        assert len(metric_bytes) == 768, label
        assert bytes.fromhex(sink_observation["metrics"]) == metric_bytes, label
        assert sink_observation["counters"] == full_observation["counters"], label
        assert sink_observation["peak_rss_bytes"] <= (
            full_observation["peak_rss_bytes"] + SINK_RSS_ALLOWANCE_BYTES), (label, before[2], after[2])
        record_property(label, json.dumps({"full": full_observation, "temporary": sink_observation}))
        if status:
            assert_projection_bits(reference, document, label + ".failure")
            assert document["code"] == case["code"] and document["args"] == case["args"], label
            assert not {"summary", "metrics", "report_sink"} & document.keys(), label
            continue
        # elapsed_seconds is the only nondeterministic report field here.
        # Keep every metric, diagnostic, counter, row and provenance field.
        reference.pop("elapsed_seconds")
        document.pop("elapsed_seconds")
        assert_projection_bits(reference, document, label + ".report")
        assert full_observation["arrays"][3] == len(reference["equity_curve"]) > 0, label
        assert sink_observation["sink_counts"]["E"] == len(reference["equity_curve"]), label
        assert sink_observation["sink_counts"]["T"] == len(reference["trades"]), label
        if case["projection"] == "calendar":
            curve = reference["equity_curve"]
            assert run_json.fmt_utc(curve[0]["time_ms"])[:7] != (
                run_json.fmt_utc(curve[-1]["time_ms"])[:7]), label
        elif case["projection"] == "terminal":
            assert any(trade["open_at_end"] for trade in reference["trades"]), label
        elif case["projection"] == "short-run":
            assert 1 <= len(reference["equity_curve"]) <= 2, label


@pytest.mark.parametrize("flag", ["--report-sink", "--run-id", "--spill-dir"])
def test_sink_flags_are_catalogued_run_request_options(flag):
    """Fail-before: the base maps an argparse refusal of these flags to "arguments"."""
    option = flag[2:].replace("-", "_")
    assert option in run_json._RUN_REQUEST_OPTIONS
    assert run_json.argparse_option(f"argument {flag}: expected one argument") == option
    catalog = json.loads((ROOT / "docker/run_failure_codes.json").read_text(encoding="utf-8"))
    assert catalog["codes"]["run_request_invalid"]["args"]["option"]["values"].count(option) == 1
