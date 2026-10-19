"""L9c: a fixture sink must reproduce the array harness's CSV bytes.

Only native calls are replaced. Strategy.run, L9a's sink owner/receipt checks,
pf_sink's strict reader, report-window filtering and the CSV writers are real.
No build, test or repository script was run by the edit-only executor.
"""
from __future__ import annotations

import csv
import ctypes
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "docker"))

import run_strategy as rs
import run_json
import test_run_json_bin as bars
from test_run_json_sink import SinkLibrary, TRADE_FIELDS, header, write_sink


def trade_fixture():
    """Non-associative PnL, signed zeros, lot precision and unsorted fills."""
    trades = []
    for index, (pnl, qty, minute) in enumerate((
        (.1234567, 923941.16, 1),
        (1.e16, .30000000000000004, 3),
        (-1.e16, .00000001, 2),
        (-.00000051, 897902.68, 3),
        (-0., 1., 5),
    )):
        trades.append({
            "entry_time": bars.T0 + minute * 60000,
            "exit_time": bars.T0 + (minute + 1) * 60000,
            "entry_price": 1.082320123456789,
            "exit_price": 1.07836987654321,
            "pnl": pnl,
            "pnl_pct": -.000051 if index % 2 else 1.23456789,
            "is_long": index % 2 == 0,
            "max_runup": -0. if index == 4 else 12.3456789,
            "max_drawdown": -0. if index == 4 else .00000051,
            "qty": qty,
            "commission": .123456789,
            "entry_bar_index": minute,
            "exit_bar_index": minute + 1,
            "open_at_end": index == 4,
            "entry_incarnation": 0 if index == 1 else 41 + index,
            "entry_id": 'entry "\n漢',
            "exit_id": "exit,short" if index % 2 else "",
            "exit_comment": "range end" if index == 4 else "",
            "close_cause": 6 if index == 4 else 1,
        })
    return trades


class HarnessLibrary(SinkLibrary):
    """The same fixture populates either ReportC arrays or the engine sink."""

    def __init__(self, trades, *, fault=None, bad_field=None, bad_order=False):
        self.trades = trades
        self.bad_order = bad_order
        super().__init__(pnls=tuple(trade["pnl"] for trade in trades),
                         fault=fault, bad_field=bad_field)
        for name in ("strategy_set_trace_enabled",
                     "strategy_set_broker_state_hash_recording",
                     "strategy_set_trade_start_time"):
            setattr(self, name, self.export(name, lambda *_args: None))
        for field in ("entry_incarnation", "entry_id", "exit_id",
                      "exit_comment", "close_cause"):
            name = "strategy_closed_trade_" + field
            setattr(self, name, self.export(name, self.accessor(field)))

    def accessor(self, field):
        def read(state, index):
            assert self.path is None, "sink rows must not call retained-ledger accessors"
            value = self.trades[index][field]
            return value.encode("utf-8") if isinstance(value, str) else value
        return read

    def run(self, state, feed, count, *args):
        bars.Library.run(self, state, feed, count, *args)
        report = args[-1]._obj
        native_trades = (rs.TradeC * len(self.trades))()
        rows = []
        for index, trade in enumerate(self.trades):
            for field, _ctype in rs.TradeC._fields_:
                setattr(native_trades[index], field, trade[field])
            sink_trade = dict(trade, n=index + 1,
                              side="long" if trade["is_long"] else "short")
            if self.bad_field and index == len(self.trades) - 1:
                sink_trade[self.bad_field] = None
            if self.bad_order and index == 0:
                sink_trade["n"] = 2
            rows.append(["T", *(sink_trade[field] for field in TRADE_FIELDS)])
        curve = (rs.EquityPointC * count)()
        hashes = (ctypes.c_uint64 * count)()
        trace = (rs.TraceEntryC * count)()
        trace_names = ("fixture", 'quote "\n漢')
        names = (ctypes.c_char_p * 2)(
            *(name.encode("utf-8") for name in trace_names))
        for index in range(count):
            curve[index] = rs.EquityPointC(feed[index].timestamp, 10000. + index, -0.)
            hashes[index] = (1 << 63) + index
            trace[index] = rs.TraceEntryC(
                feed[index].timestamp, index, index % 2, float(index))
            rows.insert(index, ["E", curve[index].time_ms,
                                curve[index].equity, curve[index].open_profit])
        report.total_trades = len(self.trades)
        report.net_profit = sum(trade["pnl"] for trade in self.trades)
        report.input_bars_processed = report.script_bars_processed = count
        report.bar_magnifier_enabled = 1
        if self.path is None:
            report.trades, report.trades_len = native_trades, len(self.trades)
            report.equity_curve, report.equity_curve_len = curve, count
            report.broker_state_hash, report.broker_state_hash_len = hashes, count
            report.trace, report.trace_len = trace, count
            report.trace_names, report.trace_names_len = names, 2
        else:
            for index in range(count):
                rows.extend([
                    ["X", feed[index].timestamp, index,
                     trace_names[index % 2], float(index)],
                    ["H", format(hashes[index], "016x")],
                ])
            self.counts, self.sha = write_diagnostic_sink(
                self.path, rows, fault=self.fault)
        self.backing = [native_trades, curve, hashes, trace, names]


def write_diagnostic_sink(path, rows, *, fault=None):
    # Reuse L9a's fixture writer and fixed schema; add v1 X/H descriptors so
    # the auxiliary CLI outputs are exercised alongside the CSV.
    from unittest import mock
    import test_run_json_sink as fixture

    def diagnostic_header(outputs=False):
        value = header(outputs)
        value["records"].extend([
            {"type": "X", "fields": ["timestamp", "bar_index", "name", "value"],
             "types": ["i64", "i64", "str", "f64?"],
             "units": ["unix_ms", "", "", ""]},
            {"type": "H", "fields": ["hash"], "types": ["hex64"], "units": [""]},
        ])
        return value

    with mock.patch.object(fixture, "header", diagnostic_header):
        return write_sink(path, rows, fault=fault)


def strategy(library):
    result = rs.Strategy.__new__(rs.Strategy)
    result.lib = library
    result._setup_signatures()
    return result


@pytest.fixture(autouse=True)
def clean_environment(monkeypatch):
    for name in ("PINEFORGE_REPORT_SINK", "PINEFORGE_REQUESTS_ROOT",
                 rs.MAGNIFIER_FEED_ENV, rs.LANE_QTY_STEP_ENV,
                 rs.SESSION_CALENDAR_ENV):
        monkeypatch.delenv(name, raising=False)


@pytest.mark.parametrize("empty", [False, True])
@pytest.mark.parametrize("window", [
    None,
    (bars.T0 + 120000, bars.T0 + 180000),
    (bars.T0 + 300000, bars.T0 + 300000),
    (bars.T0 + 900000, bars.T0 + 960000),
])
def test_fixture_sink_csv_equals_array_bytes(tmp_path, monkeypatch, empty, window):
    source = bars.csv_file(tmp_path, count=8)
    trades = [] if empty else trade_fixture()
    array_library = HarnessLibrary(trades)
    array = strategy(array_library).run(source)
    expected = tmp_path / "array.csv"
    array_rows = rs._filter_trades_to_window(array["trades"], window)
    rs.write_engine_trades_csv(array_rows, expected)
    assert not array_library.installed

    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trades)
    report = strategy(library).run(source)
    assert len(library.installed) == 1, "sink-on must install the native sink"
    assert library.queried == 1, "sink-on must verify the engine receipt"
    with report:
        assert not isinstance(report["trades"], list)
        assert list(report["trades"]) == array["trades"]
        assert list(report["trace_names"]) == array["trace_names"]
        assert list(report["trace"]) == array["trace"]
        assert list(report["broker_state_hash"]) == array["broker_state_hash"]
        assert list(report["equity_curve_time_ms"]) == array["equity_curve_time_ms"]
        assert len(report["trades"]) == len(trades)
        assert rs._entered_before_tv(report["trades"], bars.T0 + 180000) == (
            rs._entered_before_tv(array["trades"], bars.T0 + 180000))
        sink_rows = rs._filter_trades_to_window(report["trades"], window)
        assert len(sink_rows) == len(array_rows)
        actual = tmp_path / "sink.csv"
        rs.write_engine_trades_csv(sink_rows, actual)
        assert actual.read_bytes() == expected.read_bytes()
        # A second pass cannot depend on the first iterator's cursor.
        rs.write_engine_trades_csv(sink_rows, actual)
        assert actual.read_bytes() == expected.read_bytes()
        assert rs._report_terminal_quote_applied(
            report, (bars.T0 + 420000, 1.), "15") == (
                rs._report_terminal_quote_applied(
                    array, (bars.T0 + 420000, 1.), "15"))
    assert report.sink.file.closed
    assert library.calls.index("report_free") < library.calls.index("strategy_free")


def test_sink_csv_is_forward_only_and_keeps_the_legacy_cells(tmp_path, monkeypatch):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    source = bars.csv_file(tmp_path, count=8)
    library = HarnessLibrary(trade_fixture())
    report = strategy(library).run(source)
    assert library.installed, "sink-on must install the native sink"
    with report:
        def refuse(*_args):
            pytest.fail("CSV export must not size, index or materialize the trade history")
        with monkeypatch.context() as patch:
            patch.setattr(type(report["trades"]), "__len__", refuse)
            patch.setattr(type(report["trades"]), "__getitem__", refuse)
            rs.write_engine_trades_csv(report["trades"], tmp_path / "sink.csv")
    content = (tmp_path / "sink.csv").read_bytes()
    assert content.count(b"\r\n") == 11
    with (tmp_path / "sink.csv").open(newline="") as file:
        reader = csv.DictReader(file)
        rows = list(reader)
    assert reader.fieldnames == [
        "Trade #", "Type", "Date and time", "Price", "Qty", "Net PnL",
        "Net PnL %", "Favorable excursion USD", "Adverse excursion USD",
        "Cumulative PnL", "Engine entry incarnation", "Engine range-end",
    ]
    assert [row["Trade #"] for row in rows] == [
        "5", "5", "4", "4", "3", "3", "2", "2", "1", "1"]
    assert [row["Type"] for row in rows[:4]] == [
        "Exit long", "Entry long", "Exit short", "Entry short"]
    assert rows[0]["Engine range-end"] == "open"
    assert rows[0]["Engine entry incarnation"] == ""
    assert rows[1]["Engine entry incarnation"] == "45"
    assert rows[1]["Engine range-end"] == ""
    assert rows[-1]["Qty"] == "923941.16"
    assert rows[0]["Adverse excursion USD"] == "0.000000"
    assert rows[0]["Net PnL"] == "-0.000000"
    running = 0.
    for trade, pair in zip(trade_fixture(), reversed(rows[::2])):
        running += trade["pnl"]
        assert pair["Cumulative PnL"] == f"{running:.6f}"


def invoke_main(tmp_path, monkeypatch, library, output, *, extra=()):
    source = tmp_path / "chart.csv"
    if not source.exists():
        bars.csv_file(tmp_path, count=8)
    probe = tmp_path / "probe"
    probe.mkdir(exist_ok=True)
    runner = strategy(library)
    monkeypatch.setattr(rs, "Strategy", lambda _path: runner)
    monkeypatch.setattr(sys, "argv", [
        "run_strategy.py", str(probe), "--ohlcv", str(source),
        "--no-trim-output", "--output", str(output), *extra,
    ])
    return rs.main()


def test_cli_csv_and_diagnostic_values_equal_array_path(tmp_path, monkeypatch):
    # Retain the class while invoke_main replaces only the CLI constructor.
    real_strategy = rs.Strategy
    trades = trade_fixture()
    array_csv = tmp_path / "array.csv"
    array_trace = tmp_path / "array-trace.json"
    array_library = HarnessLibrary(trades)
    assert invoke_main(tmp_path, monkeypatch, array_library, array_csv,
                       extra=("--trace-json", str(array_trace),
                              "--broker-state-hash")) == 0
    array_hash_bytes = (tmp_path / "broker_state_hash.json").read_bytes()
    monkeypatch.setattr(rs, "Strategy", real_strategy)
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trades)
    owners = []
    make_sink = rs._new_harness_report_sink
    def track(*args):
        sink = make_sink(*args)
        owners.append(sink)
        return sink
    monkeypatch.setattr(rs, "_new_harness_report_sink", track)
    actual_csv = tmp_path / "sink.csv"
    sink_trace = tmp_path / "sink-trace.json"
    assert invoke_main(tmp_path, monkeypatch, library, actual_csv,
                       extra=("--trace-json", str(sink_trace),
                              "--broker-state-hash")) == 0
    assert library.installed, "the CLI must honor PINEFORGE_REPORT_SINK"
    assert actual_csv.read_bytes() == array_csv.read_bytes()
    assert sink_trace.read_bytes() == array_trace.read_bytes()
    assert json.loads(sink_trace.read_text()) == json.loads(array_trace.read_text())
    assert (tmp_path / "broker_state_hash.json").read_bytes() == array_hash_bytes
    assert owners and all(sink.file.closed for sink in owners)


@pytest.mark.parametrize("fault", ["hash", "truncated", "receipt", "incomplete"])
def test_bad_sink_never_overwrites_csv_and_releases_resources(
        tmp_path, monkeypatch, fault):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture(), fault=fault)
    destination = tmp_path / "engine_trades.csv"
    previous = b"previous verified CSV\r\n"
    destination.write_bytes(previous)
    owners = []
    make_sink = rs._new_harness_report_sink
    def track(*args):
        sink = make_sink(*args)
        owners.append(sink)
        return sink
    monkeypatch.setattr(rs, "_new_harness_report_sink", track)
    with pytest.raises(run_json.RunFailure, match="sink"):
        invoke_main(tmp_path, monkeypatch, library, destination)
    assert library.installed, "the failure must exercise a real fixture sink"
    assert destination.read_bytes() == previous
    assert owners and all(sink.file.closed for sink in owners)
    assert "report_free" in library.calls and "strategy_free" in library.calls


@pytest.mark.parametrize("field", [
    "entry_price", "exit_price", "qty", "pnl", "pnl_pct",
    "max_runup", "max_drawdown", "commission",
])
def test_null_trade_field_is_refused_before_csv(tmp_path, monkeypatch, field):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture(), bad_field=field)
    destination = tmp_path / "engine_trades.csv"
    with pytest.raises(RuntimeError, match=f"non-finite.*{field}"):
        invoke_main(tmp_path, monkeypatch, library, destination)
    assert library.installed
    assert not destination.exists()


def test_report_order_is_checked_even_with_a_valid_receipt(tmp_path, monkeypatch):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture(), bad_order=True)
    with pytest.raises(RuntimeError, match="out of report order"):
        strategy(library).run(bars.csv_file(tmp_path))
    assert library.installed and library.queried == 1


def test_csv_rechecks_the_sink_before_replacing_existing_output(tmp_path, monkeypatch):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture())
    report = strategy(library).run(bars.csv_file(tmp_path))
    assert library.installed
    destination = tmp_path / "engine_trades.csv"
    previous = b"previous verified CSV\r\n"
    destination.write_bytes(previous)
    with report:
        report.sink.file.seek(0)
        original = report.sink.file.read()
        changed = original.replace(b'"long"', b'"short"', 1)
        assert changed != original
        report.sink.file.seek(0)
        report.sink.file.write(changed)
        report.sink.file.truncate()
        report.sink.file.flush()
        with pytest.raises(run_json.RunFailure, match="sink"):
            rs.write_engine_trades_csv(report["trades"], destination)
    assert destination.read_bytes() == previous


@pytest.mark.parametrize("missing", [
    "pf_report_sink_version", "strategy_set_report_sink_v1",
    "strategy_report_sink_receipt_v1",
])
def test_requested_sink_cannot_silently_fall_back(tmp_path, monkeypatch, missing):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture())
    delattr(library, missing)
    with pytest.raises(RuntimeError, match="requires the report sink ABI"):
        strategy(library).run(bars.csv_file(tmp_path))
    assert "run_backtest_full" not in library.calls
    assert "report_free" in library.calls and "strategy_free" in library.calls


def test_explicit_sink_off_needs_no_sink_exports(tmp_path, monkeypatch):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "0")
    library = HarnessLibrary(trade_fixture())
    for name in ("pf_report_sink_version", "strategy_set_report_sink_v1",
                 "strategy_report_sink_receipt_v1"):
        delattr(library, name)
    report = strategy(library).run(bars.csv_file(tmp_path))
    assert report["trades"] == trade_fixture()
    assert not library.installed


def test_csv_write_failure_still_closes_the_sink(tmp_path, monkeypatch):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture())
    destination = tmp_path / "directory.csv"
    destination.mkdir()
    owners = []
    make_sink = rs._new_harness_report_sink
    def track(*args):
        sink = make_sink(*args)
        owners.append(sink)
        return sink
    monkeypatch.setattr(rs, "_new_harness_report_sink", track)
    with pytest.raises(OSError):
        invoke_main(tmp_path, monkeypatch, library, destination)
    assert library.installed
    assert owners and all(sink.file.closed for sink in owners)


def test_cli_rerun_uses_fresh_sinks_and_closes_both(tmp_path, monkeypatch):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture())
    owners = []
    make_sink = rs._new_harness_report_sink
    def track(*args):
        sink = make_sink(*args)
        owners.append(sink)
        return sink
    monkeypatch.setattr(rs, "_new_harness_report_sink", track)
    monkeypatch.setattr(rs, "_load_tv_entry_span",
                        lambda *_args: (bars.T0 + 240000, bars.T0 + 300000))
    monkeypatch.setattr(rs, "_apply_range_end_regime", lambda *_args: "fixture")
    monkeypatch.setattr(rs, "_tv_first_bar", lambda *_args: (bars.T0, "fixture"))
    def window(feed, first, last, interval, tv_first=None):
        return rs.TvEntryWindow(
            bars.T0 if tv_first is not None else bars.T0 + 180000,
            last, bars.T0 + 120000, first, first, first - interval,
            tv_first)
    monkeypatch.setattr(rs, "_tv_entry_emit_window", window)
    monkeypatch.setattr(rs, "_describe_tv_entry_window", lambda *_args: "fixture")
    source = bars.csv_file(tmp_path, count=8)
    probe = tmp_path / "probe"
    probe.mkdir()
    runner = strategy(library)
    monkeypatch.setattr(rs, "Strategy", lambda _path: runner)
    destination = tmp_path / "sink.csv"
    monkeypatch.setattr(sys, "argv", [
        "run_strategy.py", str(probe), "--ohlcv", str(source),
        "--output", str(destination),
    ])
    assert rs.main() == 0
    assert library.calls.count("run_backtest_full") == 2
    assert len(library.installed) == 2
    assert library.installed[0] != library.installed[1]
    assert len(owners) == 2 and all(sink.file.closed for sink in owners)
    reference = tmp_path / "array.csv"
    rs.write_engine_trades_csv(
        rs._filter_trades_to_window(
            trade_fixture(), (bars.T0 + 120000, bars.T0 + 300000)),
        reference)
    assert destination.read_bytes() == reference.read_bytes()
