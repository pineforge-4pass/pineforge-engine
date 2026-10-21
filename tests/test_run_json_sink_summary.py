"""CR-150 L9B: named-sink summaries checked against the frozen full producer.

Only native calls are replaced in the Python rows. The real reader, report
assembly and JSON writer run unchanged. Native rows consume lab-built modules;
this edit-only lane does not execute tests or build those modules.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
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
    """Independent section-4.3 projection of the frozen producer's document."""
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
        tmp_path, monkeypatch, frozen, outputs, pnls, arena_stats):
    """Fail-before: the base parser refuses --report-sink and emits no summary."""
    path = tmp_path / "report 漢.ndjson"
    spill = tmp_path / "spill"
    spill.mkdir()
    extra = ["--outputs"] if outputs else []
    before = full.invoke(tmp_path, monkeypatch, frozen,
                         library=full.SinkLibrary(
                             outputs=outputs, pnls=pnls, arena_stats=arena_stats), extra=extra)
    library = SummaryLibrary(
        outputs=outputs, pnls=pnls, all_tags=outputs, arena_stats=arena_stats)
    after = full.invoke(tmp_path, monkeypatch, library=library,
                        extra=[*extra, "--report-sink", str(path), "--run-id", "run\"\n漢",
                               "--spill-dir", str(spill)],
                        forbid_output_arrays=outputs)
    assert before[0] == after[0] == 0 and before[2] == after[2] == "", after[1]
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
        tmp_path, monkeypatch, frozen, arena_stats):
    before = full.invoke(
        tmp_path, monkeypatch, frozen, library=full.SinkLibrary(arena_stats=arena_stats))
    library = SummaryLibrary(arena_stats=arena_stats)
    after = full.invoke(tmp_path, monkeypatch, library=library,
                        extra=["--run-id", "caller id", "--spill-dir", str(tmp_path)])
    assert before[0] == after[0] == 0 and before[2] == after[2] == ""
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
def test_selected_mode_honours_named_sink(tmp_path, monkeypatch, frozen, outputs, pnls, arena_stats):
    # Import at call time: test_run_json_selected_sink imports this module's
    # read_reference and project_summary helpers.
    import test_run_json_selected_sink as selected_sinks

    path, spill = tmp_path / "selected report 漢.ndjson", tmp_path / "spill"
    spill.mkdir()
    run_id = 'selected "summary"\n漢'
    plan = selected_sinks.selected.full_plan()
    before = selected_sinks.invoke(
        tmp_path, monkeypatch, runner=frozen, plan=plan, outputs=outputs,
        library=selected_sinks.SelectedSinkLibrary(
            plan, pnls=pnls, outputs=outputs, arena_stats=arena_stats))
    library = selected_sinks.NamedSinkLibrary(
        plan, pnls=pnls, outputs=outputs, arena_stats=arena_stats)
    after = selected_sinks.invoke(
        tmp_path, monkeypatch, plan=plan, outputs=outputs, library=library,
        extra=["--report-sink", str(path), "--run-id", run_id, "--spill-dir", str(spill)])
    assert (before[0], before[2], after[0], after[2]) == (0, "", 0, ""), (before[1], after[1])
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
    # full.NATIVE_DRIVER extends the frozen diagnostics from its explicit
    # file-scratch fixture when W2's stats export is available; project every
    # key below and retain the exact serialized-document comparison.
    frozen_path = tmp_path / "frozen_run_json.py"
    frozen_path.write_bytes(full._frozen_source_bytes())
    path = tmp_path / "native-report.ndjson"
    results = []
    for mode, producer, extra in (
        ("frozen", frozen_path, []),
        ("stream", ROOT / "docker/run_json.py",
         ["--report-sink", str(path), "--run-id", "native proof", "--spill-dir", str(tmp_path)]),
    ):
        result = subprocess.run(
            [sys.executable, "-B", "-c", full.NATIVE_DRIVER, str(ROOT / "docker"),
             str(producer), mode, "--so", str(module), *map(str, argv), *extra],
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


def test_native_corpus_subset_frozen_summary_bytes(tmp_path):
    manifest = os.environ.get("PINEFORGE_L9B_CORPUS_CASES")
    if not manifest:
        pytest.skip("SKIP corpus proof requires PINEFORGE_L9B_CORPUS_CASES from CORE-TOP")
    cases = json.loads(Path(manifest).read_text())
    assert cases, "the corpus proof must name real probes"
    for case in cases:
        assert Path(case["so"]).is_file(), case
        full.native_compare(tmp_path, case["so"], case["argv"])
        native_summary_compare(tmp_path, case["so"], case["argv"])


@pytest.mark.parametrize("flag", ["--report-sink", "--run-id", "--spill-dir"])
def test_sink_flags_are_catalogued_run_request_options(flag):
    """Fail-before: the base maps an argparse refusal of these flags to "arguments"."""
    option = flag[2:].replace("-", "_")
    assert option in run_json._RUN_REQUEST_OPTIONS
    assert run_json.argparse_option(f"argument {flag}: expected one argument") == option
    catalog = json.loads((ROOT / "docker/run_failure_codes.json").read_text(encoding="utf-8"))
    assert catalog["codes"]["run_request_invalid"]["args"]["option"]["values"].count(option) == 1
