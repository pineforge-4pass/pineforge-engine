"""S4E E-03: selected-window calendar nulls survive fd-sink conversion.

Only the native library and planner are doubles. run_json, the selected
report producer, fd wire writer and standalone converter remain real.
"""
import base64
import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))

import pineforge_sink_to_json_test as replay
import test_run_json_sink_fd as fd


class _CalendarLibrary(fd.SelectedFdLibrary):
    def __init__(self, plan):
        super().__init__(plan)
        # Match the existing wire fixture's engine identity; an unrelated
        # provenance mismatch must not hide the calendar regression.
        self.pf_version_string = lambda: b"test"
        self.sessions = []
        self.strategy_set_syminfo_session = self.set_session

    def set_session(self, state, session):
        self.sessions.append(session)
        return 0

    def _run(self, *args):
        super()._run(*args)
        # The inherited fixture intentionally fills arbitrary metric counts.
        # Supply counts from its trades so clean metadata needs no repair.
        metrics = args[-1]._obj.metrics
        for name, pnls in (("all", self.pnls), ("longs", self.pnls[::2]),
                           ("shorts", self.pnls[1::2])):
            replay.fill_metric_counts(getattr(metrics, name), pnls)


def _windows(document):
    return (document["report_window"],
            document["applied_runtime"]["report_window"],
            document["fingerprint"]["provenance"]["runtime"]["report_window"])


def _selected_fd_report(tmp_path, session):
    syminfo = tmp_path / "syminfo.json"
    syminfo.write_text(json.dumps({"session": session}), encoding="utf-8")
    extra = ["--syminfo", str(syminfo)]
    plan = fd.selected.full_plan()

    # The independent full-document oracle uses the real array producer.
    # No expected field or fingerprint is taken from converted output.
    array_library = fd.full.without_report_sink(_CalendarLibrary(plan))
    reference = fd.selected_invoke(tmp_path, array_library, extra)
    assert (reference[0], reference[2]) == (0, ""), reference
    assert not array_library.installed

    library = _CalendarLibrary(plan)
    with fd.Stream() as stream:
        produced = fd.selected_invoke(
            tmp_path, library,
            [*extra, "--report-sink-fd", str(stream.writer), "--run-id", fd.RUN_ID],
            no_files=True)
    assert (produced[0], produced[2]) == (0, ""), produced
    assert library.closed == library.installed == [stream.writer]
    assert library.queried == library.summary_queries == 1
    assert library.sessions == array_library.sessions == (
        [session.encode("utf-8")] if session else [])

    document, expected = json.loads(produced[1]), json.loads(reference[1])
    fd.full.assert_run_profile(document)
    fd.full.assert_run_profile(expected)
    # Only phase durations vary between these executions. Preserve the
    # fd producer's entire timing record in the conversion expectation.
    expected["diagnostics"]["phase_timing"] = document["diagnostics"]["phase_timing"]
    assert document == replay.projection(expected)
    assert all(window == document["report_window"] for window in _windows(document))
    fingerprint = document["fingerprint"]
    assert fingerprint["version"] == fingerprint["provenance"]["schema_version"] == 2
    raw = base64.b64decode(fingerprint["token"], validate=True)
    assert json.loads(raw) == fingerprint["provenance"]
    assert fingerprint["digest"] == "sha256:" + hashlib.sha256(raw).hexdigest()

    # Retain the actual fd stream, not a synthetic sink or report dictionary.
    sink, metadata = tmp_path / "selected.sink", tmp_path / "stdout.json"
    sink.write_bytes(stream.data)
    metadata.write_text(produced[1], encoding="utf-8")
    return sink, metadata, document, expected


def _assert_round_trip(sink, metadata, document, expected, session):
    result = replay.invoke(sink, metadata, launcher=True, extra=("--run-id", fd.RUN_ID))
    assert result.returncode == 0, result.stderr.decode()
    converted = json.loads(result.stdout)
    fd.full.assert_run_profile(converted)
    for window in _windows(converted):
        assert window["calendar"]["session"] == session
        if session is None:
            assert window["calendar"]["session"] is None
    assert converted["fingerprint"]["version"] == 2
    assert converted["fingerprint"]["token"] == document["fingerprint"]["token"]
    assert converted["fingerprint"]["digest"] == document["fingerprint"]["digest"]
    assert converted == expected
    assert result.stdout == replay.report_bytes(expected)
    lines = replay.diagnostics(result)
    assert lines[-1]["event"] == "converter_complete"
    assert lines[-1]["sink_verified"] is True
    assert lines[-1]["metadata_drops"] == 0, lines


@pytest.mark.parametrize("session", ["24x7", ""], ids=["24x7", "empty-session"])
def test_selected_window_null_session_survives_conversion(tmp_path, session):
    sink, metadata, document, expected = _selected_fd_report(tmp_path, session)
    # selected_window_report.calendar_record defines both spellings as null.
    assert all(window["calendar"]["session"] is None for window in _windows(document))
    _assert_round_trip(sink, metadata, document, expected, None)


def test_selected_window_text_session_survives_conversion(tmp_path):
    session = "0000-0000:1234567"
    sink, metadata, document, expected = _selected_fd_report(tmp_path, session)
    assert all(window["calendar"]["session"] == session for window in _windows(document))
    _assert_round_trip(sink, metadata, document, expected, session)


@pytest.mark.parametrize("field,value", [
    ("session", 17),
    ("session", ["24x7"]),
    ("timezone", None),
], ids=["numeric-session", "list-session", "null-timezone"])
def test_selected_window_invalid_calendar_value_is_rejected(tmp_path, field, value):
    sink, metadata, document, _ = _selected_fd_report(tmp_path, "24x7")
    for window in _windows(document):
        window["calendar"][field] = value
    metadata.write_bytes(replay.report_bytes(document))
    result = replay.invoke(sink, metadata, launcher=True, extra=("--run-id", fd.RUN_ID))
    # Invalid presentation is rejected by dropping it with a diagnostic;
    # the existing converter policy still delivers the verified sink.
    assert result.returncode == 0, result.stderr.decode()
    converted = json.loads(result.stdout)
    assert all(field not in window["calendar"] for window in _windows(converted))
    lines = replay.diagnostics(result)
    for prefix in ("$.report_window", "$.applied_runtime.report_window",
                   "$.fingerprint.provenance.runtime.report_window"):
        assert any(line["event"] == "metadata_drop"
                   and line["path"] == prefix + ".calendar." + field
                   and line["reason"] == "invalid_value" for line in lines), lines
