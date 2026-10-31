"""Pin the reserved final report window, budgeted columns and independent reader."""
from contextlib import ExitStack
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "docker"))

import run_json
import run_json_fused_test_support as support
from run_json_fused_test import _configure_outputs, _sink_case
from run_json_trade_scan_test import _event, _mixed
from run_json_wrapper_work_test import _mutate


def test_artifact_window_literal_contract(tmp_path):
    assert run_json._BoundedJSONFile.window == 64 * 1024
    assert run_json._UncachedSinkFile.window == 64 * 1024
    assert run_json._UncachedSinkFile.window == run_json._BULK_E_WINDOW
    with ExitStack() as resources:
        final = run_json._json_temporary(resources, str(tmp_path), window=1024 * 1024)
        ordinary = run_json._json_temporary(resources, str(tmp_path))
        assert final.window == len(final.buffer) == 1024 * 1024
        assert ordinary.window == len(ordinary.buffer) == 64 * 1024
        assert run_json._BoundedJSONFile.window == 64 * 1024
        assert run_json._UncachedSinkFile.window == 64 * 1024


@pytest.mark.parametrize("window", (0, -4096, 4095, 65537))
def test_artifact_window_rejects_invalid_alignment(tmp_path, window):
    with ExitStack() as resources:
        with pytest.raises(ValueError, match="positive multiple of 4096"):
            run_json._json_temporary(resources, str(tmp_path), window=window)


def _assert_fused_windows(tmp_path, monkeypatch):
    opened, prepared = [], []
    original_init = run_json._BoundedJSONFile.__init__

    def initialize(file, stream, window=None):
        original_init(file, stream, window)
        opened.append(file)

    def consume(artifact, document):
        prepared.append((artifact, document))

    with monkeypatch.context() as patch:
        patch.setattr(run_json._BoundedJSONFile, "__init__", initialize)
        with _sink_case(tmp_path, _mixed(), outputs=True) as (sink, library, report):
            outcome = support.assemble(
                run_json, sink, library, report, outputs=True,
                configure=_configure_outputs, consume=consume, consume_before_close=True)
    assert outcome.failure is None, outcome.failure
    assert len(prepared) == 1
    artifact, document = prepared[0]
    assert artifact.window == 1024 * 1024, "FINAL_ARTIFACT_WINDOW"
    outputs = document["outputs"]
    columns = [
        document["trades"], document["equity_curve"],
        outputs["bars"]["open_ms"], outputs["bars"]["close_ms"],
        outputs["events"], *(series["values"] for series in outputs["series"]),
    ]
    assert len(columns) == 7
    assert all(type(column) is run_json._JSONColumn for column in columns)
    assert [column.file.window for column in columns] == [
        *([1024 * 1024] * 5), *([64 * 1024] * 2)]
    readers = [file for file in opened if isinstance(file, run_json._UncachedSinkFile)]
    assert len(readers) == 1
    assert len(opened) == 9
    assert set(opened) == {artifact, *readers, *(column.file for column in columns)}
    assert all(file.window == 64 * 1024 for file in readers)


def test_fused_report_reserves_final_artifact_and_budgets_source_columns(tmp_path, monkeypatch):
    _assert_fused_windows(tmp_path, monkeypatch)


def test_fused_window_check_detects_missing_final_argument(tmp_path, monkeypatch):
    changed = _mutate(
        run_json._prepare_stream_report,
        ", _artifact_windows(resources).report_window", "")
    monkeypatch.setattr(run_json, "_prepare_stream_report", changed)
    with pytest.raises(AssertionError, match="FINAL_ARTIFACT_WINDOW"):
        _assert_fused_windows(tmp_path, monkeypatch)


def test_multimegabyte_fused_report_bytes_ignore_final_window(tmp_path, monkeypatch):
    rows = _mixed()
    events = []
    for sequence in range(1, 5):
        event = _event('quote"\\\n漢' + "x" * (800 * 1024))
        event[1], event[6] = sequence, sequence - 1
        events.append(event)
    rows[2:3] = events
    ordinary_prepare = _mutate(
        run_json._prepare_stream_report,
        "_artifact_windows(resources).report_window",
        "_BoundedJSONFile.window")
    reports, windows = [], []

    def consume(artifact, document):
        windows.append(artifact.window)
        chunks = []
        while chunk := artifact.read(65536):
            chunks.append(chunk.encode("ascii"))
        reports.append(b"".join(chunks))

    for prepare in (run_json._prepare_stream_report, ordinary_prepare):
        with monkeypatch.context() as patch:
            patch.setattr(run_json, "_prepare_stream_report", prepare)
            with _sink_case(tmp_path, rows, outputs=True) as (sink, library, report):
                outcome = support.assemble(
                    run_json, sink, library, report, outputs=True,
                    configure=_configure_outputs, consume=consume)
        assert outcome.failure is None, outcome.failure
    assert windows == [run_json._ARTIFACT_LARGE_WINDOW, run_json._BoundedJSONFile.window]
    assert len(reports) == 2 and len(reports[0]) > 3 * windows[0]
    assert reports[0] == reports[1]
