"""Sink consumers must consume T identity, even when every old accessor refuses."""
from __future__ import annotations

import csv
import json

import pytest

import test_run_json_bin as bars
import test_run_json_sink_fd as fd_fixture
import test_run_strategy_sink as corpus_fixture


ACCESSORS = (
    "strategy_closed_trade_entry_incarnation",
    "strategy_closed_trade_entry_id",
    "strategy_closed_trade_exit_id",
    "strategy_closed_trade_exit_comment",
    "strategy_closed_trade_close_cause",
)

@pytest.fixture(autouse=True)
def clean_environment(monkeypatch):
    for name in (
        "PINEFORGE_REPORT_SINK", "PINEFORGE_REPORT_SINK_FD", "PINEFORGE_RUN_JSON_PROFILE",
        corpus_fixture.rs.KEEP_REPORT_SINK_ENV, "PINEFORGE_CANARY_LOG",
        "PINEFORGE_REQUESTS_ROOT", corpus_fixture.rs.MAGNIFIER_FEED_ENV,
        corpus_fixture.rs.LANE_QTY_STEP_ENV, corpus_fixture.rs.SESSION_CALENDAR_ENV,
    ):
        monkeypatch.delenv(name, raising=False)


def refuse_history(library):
    calls = []

    def refuse(*args):
        calls.append(args)
        raise AssertionError("consumer read a retired trade instead of its T record")

    for name in ACCESSORS:
        setattr(library, name, library.export(name, refuse))
    return calls


@pytest.mark.parametrize("route", ["temporary", "path", "fd"])
def test_json_sink_uses_no_history_accessor(tmp_path, route):
    source = bars.csv_file(tmp_path)
    library = fd_fixture.FdLibrary(pnls=(10.0, -7.0, 0.0))
    calls = refuse_history(library)
    if route == "fd":
        with fd_fixture.Stream("socket") as stream:
            result = fd_fixture.invoke(source, library, [
                "--report-sink-fd", str(stream.writer), "--run-id", fd_fixture.RUN_ID])
        assert stream.data.count(b'["T",') == 3
    else:
        options = ["--report-sink", str(tmp_path / "report.sink")] if route == "path" else []
        result = fd_fixture.invoke(source, library, options)
    assert result[0] == 0 and result[2] == "", result
    document = json.loads(result[1])
    assert document["summary"]["total_trades"] == 3
    assert library.queried > 0
    assert calls == []


def test_corpus_sink_csv_uses_no_history_accessor(tmp_path, monkeypatch):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = corpus_fixture.HarnessLibrary(corpus_fixture.trade_fixture())
    calls = refuse_history(library)
    source = bars.csv_file(tmp_path, count=8)
    with corpus_fixture.strategy(library).run(source) as report:
        rows = list(report["trades"])
        assert len(rows) == len(corpus_fixture.trade_fixture()) > 0
        output = tmp_path / "trades.csv"
        corpus_fixture.rs.write_engine_trades_csv(rows, output)
        with output.open(newline="") as source:
            assert len(list(csv.reader(source))) == len(rows) + 1
    assert library.queried > 0 and calls == []
