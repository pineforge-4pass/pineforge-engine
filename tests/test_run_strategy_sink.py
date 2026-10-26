"""L9c: a fixture sink must reproduce the array harness's CSV bytes.

Only native calls are replaced. Strategy.run, L9a's sink owner/receipt checks,
pf_sink's strict reader, report-window filtering and the CSV writers are real.

The named owner-only sink gives every reader its own offset. Canaries compare
decoded T/E rows with engine counts; sweep logs retain passing/failing canaries
and kept paths, and coverage is checked against the probes actually run.
"""
from __future__ import annotations

import csv
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "docker"))

import run_strategy as rs
import check_sink_canary_log as canary_log
import run_json
import pf_sink
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
        # What the engine was handed, and how big the sink was when the engine
        # was freed: read from the files themselves, not from harness output.
        self.engine_view, self.sizes = [], []
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

    def install(self, state, pointer):
        status = super().install(state, pointer)
        info = os.stat(self.path)
        self.engine_view.append({
            "path": self.path, "regular": stat.S_ISREG(info.st_mode),
            "mode": stat.S_IMODE(info.st_mode), "size": info.st_size,
        })
        return status

    def free(self, state):
        if self.path is not None:
            self.sizes.append(os.path.getsize(self.path))
        super().free(state)

    def receipt(self, state, pointer):
        result = super().receipt(state, pointer)
        # Independent engine-side expectation from the fixture feed, not a
        # number obtained by parsing the sink or its rewritten trailer.
        pointer._obj.count_e = self.equity_points
        return result

    def run(self, state, feed, count, *args):
        bars.Library.run(self, state, feed, count, *args)
        self.equity_points = count
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
    for name in ("PINEFORGE_REPORT_SINK", rs.KEEP_REPORT_SINK_ENV,
                 "PINEFORGE_CANARY_LOG",
                 "PINEFORGE_REQUESTS_ROOT",
                 rs.MAGNIFIER_FEED_ENV, rs.LANE_QTY_STEP_ENV,
                 rs.SESSION_CALENDAR_ENV):
        monkeypatch.delenv(name, raising=False)


@pytest.fixture(autouse=True)
def sink_directory(tmp_path, monkeypatch):
    """Every sink file of a test is created in this directory (the temp dir
    the harness picks). A sink file must not outlive its test unless the
    keep switch asked for it, and that test removes it itself."""
    directory = tmp_path / "sinks"
    directory.mkdir()
    monkeypatch.setattr(tempfile, "tempdir", str(directory))
    yield directory
    assert not list(directory.glob("pineforge-report-sink-*")), (
        "a sink file was left behind")


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
    assert library.queried == 2, "verify the receipt and save its independent E count"
    assert report.receipt_count_e == len(array["equity_curve_time_ms"])
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
    assert report.sink.closed and not Path(report.sink.path).exists()
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


# The canary's one machine-greppable line, exactly as the sweep will grep it.
CANARY = re.compile(
    r"sink_canary: probe=(?P<probe>\S+) bytes=(?P<bytes>\d+) records=(?P<records>\d+) "
    r"trades=(?P<trades>\d+) equity=(?P<equity>\d+) (?P<verdict>ok|FAIL \S.*)")


def canary_lines(text):
    return [line for line in text.splitlines() if line.startswith("sink_canary:")]


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
    assert owners and all(sink.closed for sink in owners)


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
    assert owners and all(sink.closed for sink in owners)
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
    assert library.installed and library.queried == 2


def test_csv_rechecks_the_sink_before_replacing_existing_output(tmp_path, monkeypatch):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture())
    report = strategy(library).run(bars.csv_file(tmp_path))
    assert library.installed
    destination = tmp_path / "engine_trades.csv"
    previous = b"previous verified CSV\r\n"
    destination.write_bytes(previous)
    with report:
        # The sink is an ordinary named file: change it the way any other
        # process could, through its real path.
        sink_file = Path(report.sink.path)
        original = sink_file.read_bytes()
        changed = original.replace(b'"long"', b'"short"', 1)
        assert changed != original
        sink_file.write_bytes(changed)
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
    assert owners and all(sink.closed for sink in owners)


@pytest.mark.parametrize("keep", [False, True])
def test_cli_rerun_uses_fresh_sinks_and_closes_both(
        tmp_path, monkeypatch, sink_directory, capsys, keep):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    if keep:
        monkeypatch.setenv(rs.KEEP_REPORT_SINK_ENV, "all")
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
    assert len(owners) == 2 and all(sink.closed for sink in owners)
    reference = tmp_path / "array.csv"
    rs.write_engine_trades_csv(
        rs._filter_trades_to_window(
            trade_fixture(), (bars.T0 + 120000, bars.T0 + 300000)),
        reference)
    assert destination.read_bytes() == reference.read_bytes()
    # Two sinks, one probe: the canary speaks once, for the final report.
    assert len(canary_lines(capsys.readouterr().out)) == 1
    # Only the final report can be a kept artifact: the discarded rerun's
    # sink is unlinked even when the switch names every probe.
    leftovers = sorted(sink_directory.glob("pineforge-report-sink-*"))
    try:
        assert [path.name for path in leftovers] == (
            [Path(library.installed[1]).name] if keep else [])
    finally:
        for path in leftovers:
            path.unlink()


# --- the portable sink: a named file, one offset per reader --------------------

class PathLibrary:
    """Accepts any sink the owner installs and remembers the path it was given."""

    def __init__(self, status=0):
        self.status, self.paths = status, []

    def strategy_set_report_sink_v1(self, state, pointer):
        self.paths.append(os.fsdecode(pointer._obj.path))
        return self.status


def test_engine_gets_a_named_owner_only_file_and_every_reader_starts_at_zero(
        tmp_path, monkeypatch, sink_directory):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture())
    report = strategy(library).run(bars.csv_file(tmp_path, count=8))
    (seen,) = library.engine_view
    path = Path(seen["path"])
    # What the engine was handed: a real, absolute, regular, owner-only file in
    # the temp directory; never a descriptor path (/proc/self/fd, /dev/fd),
    # whose open file description, and with it the offset, can be shared.
    assert path.is_absolute() and path.parent == sink_directory
    assert not seen["path"].startswith(("/proc/", "/dev/"))
    assert seen["regular"] and seen["size"] == 0
    assert seen["mode"] & 0o077 == 0
    with report:
        sink = report.sink
        assert Path(sink.path) == path and path.is_file()
        raw = path.read_bytes()
        assert raw.endswith(b"\n") and len(raw) == library.sizes[0] > 0
        # Two live SinkReaders, advanced alternately: each owns its offset and
        # sees every row from the start.
        pairs = list(zip(sink.records("E"), sink.records("E")))
        assert len(pairs) == sink.counts["E"] == 8
        assert all(left.as_dict() == right.as_dict() for left, right in pairs)
        # Plain opens of the real path behave the same: a shared offset would
        # hand the second reader the file's second line.
        with path.open("rb") as one, path.open("rb") as two:
            head = one.readline()
            assert head and head == two.readline()
            assert one.read() == two.read() == raw[len(head):]
        # The broker-hash export zips E with H, two readers live at once. The
        # expected pairs come from the fixture's inputs, not from the output.
        assert list(zip(report["equity_curve_time_ms"],
                        report["broker_state_hash"])) == [
            (bars.T0 + index * 60000, (1 << 63) + index) for index in range(8)]
    # close() unlinks the file; a closed owner names nothing any more, so a
    # view read after close fails loudly instead of reading a stranger's file.
    assert sink.closed and not path.exists()
    sink.close()
    with pytest.raises(run_json.RunFailure, match="sink"):
        list(sink.records("T"))


def test_sink_owner_unlinks_once_and_tolerates_a_vanished_file(sink_directory):
    library = PathLibrary()
    sink = run_json._ReportSink(library, 7, directory=sink_directory)
    path = Path(library.paths[0])
    assert path == Path(sink.path) and path.is_file() and path.stat().st_size == 0
    assert sink.directory == str(sink_directory)
    sink.close()
    assert sink.closed and not path.exists()
    sink.close()  # idempotent
    vanished = run_json._ReportSink(library, 7, directory=sink_directory)
    os.unlink(vanished.path)
    vanished.close()  # FileNotFoundError is tolerated
    assert vanished.closed


def test_sink_owner_keeps_its_file_only_when_asked(sink_directory):
    sink = run_json._ReportSink(PathLibrary(), 7, directory=sink_directory)
    sink.keep = True
    sink.close()
    path = Path(sink.path)
    try:
        assert sink.closed and path.is_file()
    finally:
        path.unlink()


def test_sink_owner_leaves_no_file_when_the_engine_refuses_it(sink_directory):
    with pytest.raises(run_json.RunFailure, match="refused"):
        run_json._ReportSink(PathLibrary(status=-1), 7, directory=sink_directory)
    assert not list(sink_directory.iterdir())


def test_sink_owner_without_a_directory_has_the_existing_failure_code(tmp_path):
    with pytest.raises(run_json.RunFailure) as error:
        run_json._ReportSink(PathLibrary(), 7, directory=tmp_path / "missing")
    assert error.value.code == "report_sink_failed"


# --- the canary: every sink-on probe proves it read a real, complete sink -------

def test_cli_prints_one_canary_line_with_the_fixtures_own_counts(
        tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    trades = trade_fixture()
    library = HarnessLibrary(trades)
    assert invoke_main(tmp_path, monkeypatch, library, tmp_path / "sink.csv") == 0
    lines = canary_lines(capsys.readouterr().out)
    assert len(lines) == 1
    found = CANARY.fullmatch(lines[0])
    assert found and found["verdict"] == "ok"
    assert found["probe"] == "probe"  # the strategy directory's basename
    assert int(found["trades"]) == int(found["records"]) == len(trades)
    with (tmp_path / "chart.csv").open() as source:
        assert int(found["equity"]) == sum(1 for _ in csv.DictReader(source))
    # The size the engine's sink had when the engine was freed, read from the
    # file by the stand-in library, not taken from the harness's own output.
    assert int(found["bytes"]) == library.sizes[0] > 0


def test_canary_accepts_a_real_sink_that_holds_no_trades(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary([])
    source = bars.csv_file(tmp_path, count=8)
    with strategy(library).run(source) as report:
        rs._sink_canary("fixture", report)
    (line,) = canary_lines(capsys.readouterr().out)
    found = CANARY.fullmatch(line)
    assert found and found["verdict"] == "ok" and found["probe"] == "fixture"
    assert int(found["records"]) == int(found["trades"]) == 0
    with source.open() as feed:
        assert int(found["equity"]) == sum(1 for _ in csv.DictReader(feed))
    assert int(found["bytes"]) == library.sizes[0] > 0


def test_canary_checks_equity_against_the_saved_engine_receipt(
        tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    source = bars.csv_file(tmp_path)
    with source.open() as feed:
        expected = sum(1 for _ in csv.DictReader(feed))
    library = HarnessLibrary(trade_fixture())
    with strategy(library).run(source) as report:
        assert report.receipt_count_e == expected
        # Neither summary script-bar counts nor cached reader counts are the
        # canary's E oracle. The independent receipt still matches the file.
        report["script_bars_processed"] += 1
        report.sink.counts["E"] += 1
        rs._sink_canary("fixture", report)
    (line,) = canary_lines(capsys.readouterr().out)
    found = CANARY.fullmatch(line)
    assert found and found["verdict"] == "ok"
    assert int(found["equity"]) == expected


@pytest.mark.parametrize("fault", ["refused", "incomplete", "negative"])
def test_a_missing_engine_equity_receipt_fails_without_leaving_a_sink(
        tmp_path, monkeypatch, sink_directory, fault):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture())
    query = library.strategy_report_sink_receipt_v1

    def fail_the_second_query(state, pointer):
        result = query(state, pointer)
        if library.queried == 2:
            if fault == "refused":
                return -1
            if fault == "incomplete":
                pointer._obj.complete = 0
            else:
                pointer._obj.count_e = -1
        return result

    monkeypatch.setattr(library, "strategy_report_sink_receipt_v1", fail_the_second_query)
    with pytest.raises(RuntimeError, match="equity receipt is incomplete"):
        strategy(library).run(bars.csv_file(tmp_path))
    assert library.queried == 2
    assert "report_free" in library.calls and "strategy_free" in library.calls
    assert not list(sink_directory.iterdir())


@pytest.mark.parametrize("damage", ["corrupt", "short"])
def test_canary_fails_for_damaged_equity_even_when_every_trade_is_intact(
        tmp_path, monkeypatch, capsys, damage):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    source = bars.csv_file(tmp_path)
    with source.open() as feed:
        expected = sum(1 for _ in csv.DictReader(feed))
    trades = trade_fixture()
    library = HarnessLibrary(trades)
    with strategy(library).run(source) as report:
        rows = [[record.tag, *record.values] for record in report.sink.records()]
        original_trades = [row for row in rows if row[0] == "T"]
        last_equity = max(index for index, row in enumerate(rows) if row[0] == "E")
        if damage == "corrupt":
            rows[last_equity][1] = True  # bool is invalid in E's i64 time field
        else:
            del rows[last_equity]
        counts, sha = write_diagnostic_sink(report.sink.path, rows)
        # Deliberately let the strict reader accept a newly sealed, short E
        # sequence: only the independently saved engine count can reject it.
        # Invalid E types must still fail strict decoding, with T untouched.
        report.sink.sha256, report.sink.counts = sha, counts
        # The rewritten file is the sealed sink this test wants read back, so
        # its identity (inode and size) replaces the one recorded at verify.
        sealed = os.stat(report.sink.path)
        report.sink._identity = sealed.st_ino, sealed.st_size
        assert report.receipt_count_e == expected
        written = [json.loads(line) for line in Path(report.sink.path).read_text().splitlines()]
        assert [row for row in written if isinstance(row, list) and row[0] == "T"] == original_trades
        if damage == "short":
            assert sum(1 for _ in report.sink.records("T")) == len(trades)
        with pytest.raises(RuntimeError, match="sink canary failed"):
            rs._sink_canary("fixture", report)
    (line,) = canary_lines(capsys.readouterr().out)
    found = CANARY.fullmatch(line)
    assert found and found["verdict"].startswith("FAIL ")
    if damage == "short":
        assert int(found["records"]) == int(found["trades"]) == len(trades)
        assert int(found["equity"]) == expected - 1
        assert f"{expected} engine equity points" in found["verdict"]
    else:
        assert "the sink cannot be read back" in found["verdict"]


@pytest.mark.parametrize("damage, reason", [
    ("empty", "the sink file is empty"),
    ("truncated", "the sink cannot be read back"),
    ("missing", "the sink cannot be read back"),
    ("short", "T records decoded for"),
])
def test_canary_fails_the_probe_for_an_empty_truncated_missing_or_short_sink(
        tmp_path, monkeypatch, capsys, damage, reason):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    trades = trade_fixture()
    library = HarnessLibrary(trades)
    report = strategy(library).run(bars.csv_file(tmp_path, count=8))
    path = Path(report.sink.path)
    with report:
        if damage == "empty":
            path.write_bytes(b"")
        elif damage == "truncated":
            content = path.read_bytes()
            path.write_bytes(content[:len(content) // 2])
        elif damage == "missing":
            path.unlink()
        else:
            # The engine reports one trade more than the sink holds.
            report["total_trades"] += 1
        with pytest.raises(RuntimeError, match="sink canary failed"):
            rs._sink_canary("fixture", report)
    (line,) = canary_lines(capsys.readouterr().out)
    found = CANARY.fullmatch(line)
    assert found and found["verdict"].startswith("FAIL ") and reason in found["verdict"]
    assert found["probe"] == "fixture"
    assert int(found["trades"]) == len(trades) + (damage == "short")
    assert int(found["records"]) == (len(trades) if damage == "short" else 0)


def test_cli_fails_the_probe_when_the_sink_is_lost_before_close(
        tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    library = HarnessLibrary(trade_fixture())
    real_write = rs.write_engine_trades_csv

    def write_then_empty_the_sink(trades, path):
        real_write(trades, path)
        Path(trades.sink.path).write_bytes(b"")

    monkeypatch.setattr(rs, "write_engine_trades_csv", write_then_empty_the_sink)
    with pytest.raises(RuntimeError, match="sink canary failed"):
        invoke_main(tmp_path, monkeypatch, library, tmp_path / "sink.csv")
    out = capsys.readouterr().out
    (line,) = canary_lines(out)
    found = CANARY.fullmatch(line)
    assert found and found["verdict"] == "FAIL the sink file is empty"
    assert (int(found["bytes"]), int(found["records"])) == (0, 0)
    assert int(found["trades"]) == len(trade_fixture())
    assert "net_profit=" not in out, "a failed probe must not print its success line"


def test_sink_off_run_prints_no_canary(tmp_path, monkeypatch, capsys):
    library = HarnessLibrary(trade_fixture())
    assert invoke_main(tmp_path, monkeypatch, library, tmp_path / "array.csv") == 0
    out = capsys.readouterr().out
    assert "sink_canary:" not in out and "sink_kept:" not in out
    assert not library.installed


# --- the test-only keep switch --------------------------------------------------

@pytest.mark.parametrize("value, kept", [
    ("all", True), ("probe", True), ("other, probe", True),
    ("other", False), ("probe-2", False), ("", False),
])
def test_keep_switch_keeps_the_files_of_the_probes_it_names(
        tmp_path, monkeypatch, capsys, sink_directory, value, kept):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    monkeypatch.setenv(rs.KEEP_REPORT_SINK_ENV, value)
    trades = trade_fixture()
    library = HarnessLibrary(trades)
    assert invoke_main(tmp_path, monkeypatch, library, tmp_path / "sink.csv") == 0
    out = capsys.readouterr().out
    leftovers = sorted(sink_directory.glob("pineforge-report-sink-*"))
    try:
        if not kept:
            assert not leftovers and "sink_kept:" not in out
        else:
            assert [path.name for path in leftovers] == [Path(library.installed[0]).name]
            assert f"sink_kept: probe=probe path={leftovers[0]}" in out.splitlines()
            # The artifact is the complete sink the probe verified: non-empty,
            # with a trailer, and as many T rows as the fixture has trades.
            with pf_sink.SinkReader(leftovers[0]) as reader:
                assert sum(1 for record in reader if record.tag == "T") == len(trades)
                assert reader.complete and leftovers[0].stat().st_size == library.sizes[0] > 0
    finally:
        for path in leftovers:
            path.unlink()


def test_keep_switch_is_off_unless_it_names_the_probe():
    assert rs._keep_report_sink("greedy", {}) is False
    assert rs._keep_report_sink("greedy", {rs.KEEP_REPORT_SINK_ENV: "  "}) is False
    assert rs._keep_report_sink("greedy", {rs.KEEP_REPORT_SINK_ENV: "all"}) is True
    assert rs._keep_report_sink("greedy", {rs.KEEP_REPORT_SINK_ENV: "a, greedy ,b"}) is True
    assert rs._keep_report_sink("greedy", {rs.KEEP_REPORT_SINK_ENV: "greedy-2,gree"}) is False


def test_probe_id_is_the_directory_basename_without_whitespace():
    assert rs._probe_id(Path("/corpus/basic/greedy")) == "greedy"
    assert rs._probe_id(Path("/corpus/my probe")) == "my_probe"
    assert rs._probe_id(Path("/")) == "-"


# --- sweep logging: use actual fixture canaries, never hand-written counts -----

CANARY_TOOL = ROOT / "scripts" / "check_sink_canary_log.py"


@pytest.fixture
def sweep_probe_outputs(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    source = bars.csv_file(tmp_path)
    outputs = {}
    for probe in ("alpha", "beta", "broken"):
        library = HarnessLibrary(trade_fixture())
        with strategy(library).run(source) as report:
            # Exercise real sink_kept output too, then let context exit remove
            # the fixture file; these tests need only the captured output.
            report.sink.keep = True
            try:
                if probe == "broken":
                    report["total_trades"] += 1
                    with pytest.raises(RuntimeError, match="sink canary failed"):
                        rs._sink_canary(probe, report)
                else:
                    rs._sink_canary(probe, report)
            finally:
                report.sink.keep = False
        outputs[probe] = capsys.readouterr().out
        (line,) = canary_lines(outputs[probe])
        found = CANARY.fullmatch(line)
        assert found and found["probe"] == probe
        assert (found["verdict"] == "ok") == (probe != "broken")
    return outputs


def run_canary_tool(*args, input=None):
    return subprocess.run(
        [sys.executable, "-I", str(CANARY_TOOL), *map(str, args)],
        input=input, capture_output=True, text=True, check=False,
    )


@pytest.mark.parametrize("from_files", [False, True])
def test_canary_log_writer_appends_passing_and_failing_lines_only(
        tmp_path, sweep_probe_outputs, from_files):
    log = tmp_path / "canaries.log"
    expected = ""
    for probe in ("alpha", "broken"):
        output = sweep_probe_outputs[probe]
        mixed = ("ordinary runner output\n sink_canary: indented noise\n"
                 "sink_canary_extra: noise\n" + output + "net_profit=ignored\n")
        if from_files:
            path = tmp_path / f"{probe}.log"
            path.write_text(mixed, encoding="utf-8")
            result = run_canary_tool("append", log, path)
        else:
            result = run_canary_tool("append", log, input=mixed)
        assert result.returncode == 0, result.stderr
        expected += output
        assert log.read_text(encoding="utf-8") == expected
    assert "FAIL " in expected and " ok\n" in expected and "sink_kept:" in expected


@pytest.mark.parametrize("probe_source", ["arguments", "file"])
def test_canary_log_checker_accepts_exact_probe_coverage(
        tmp_path, sweep_probe_outputs, probe_source):
    probes = ["alpha", "beta"]
    log = tmp_path / "canaries.log"
    for probe in probes:
        canary_log.append_lines(log, sweep_probe_outputs[probe].splitlines(True))
    if probe_source == "file":
        path = tmp_path / "ran-probes.txt"
        path.write_text("# the sweep's actual selection\n" + "".join(
            f"corpus/validation/{probe}/ # ran this probe\n" for probe in probes),
            encoding="utf-8")
        result = run_canary_tool("check", log, "--probes-file", path)
    else:
        result = run_canary_tool(
            "check", log, "--probes", *(f"corpus/validation/{probe}" for probe in probes))
    assert result.returncode == 0, result.stderr
    assert f"ok probes={len(probes)}" in result.stdout


def test_canary_log_subset_summary_counts_parsed_successes(
        tmp_path, sweep_probe_outputs):
    probes = ["alpha", "beta"]
    log = tmp_path / "canaries.log"
    for probe in probes:
        canary_log.append_lines(log, sweep_probe_outputs[probe].splitlines(True))
    result = run_canary_tool("check", log, "--subset-summary", "--probes", *probes)
    assert result.returncode == 0, result.stderr
    assert (f"sink-on subset: {len(probes)} sink_canary ok lines for "
            f"{len(probes)} probes") in result.stdout.splitlines()


@pytest.mark.parametrize("damage", [
    None, "missing-line", "duplicate", "unexpected", "missing-file", "empty-file",
    "outside-directory", "shared-file", "malformed",
])
def test_kept_sink_manifest_hashes_real_harness_artifacts(
        tmp_path, monkeypatch, capsys, sink_directory, damage):
    monkeypatch.setenv("PINEFORGE_REPORT_SINK", "1")
    probe_dir = Path("corpus/validation/alpha  probe")
    probe = rs._probe_id(probe_dir)
    log = tmp_path / "kept.log"
    with strategy(HarnessLibrary(trade_fixture())).run(bars.csv_file(tmp_path)) as report:
        report.sink.keep = True
        try:
            rs._sink_canary(probe, report)
            output = capsys.readouterr().out
            path = Path(report.sink.path)
            raw = path.read_bytes()
            expected = (f"sink_kept: probe={probe} "
                        f"sha256={hashlib.sha256(raw).hexdigest()} bytes={len(raw)}")
            probes = [str(probe_dir)]
            if damage == "missing-line":
                output = "\n".join(canary_lines(output)) + "\n"
            elif damage == "duplicate":
                output += next(line for line in output.splitlines()
                               if line.startswith("sink_kept:")) + "\n"
            elif damage == "unexpected":
                output = output.replace(f"sink_kept: probe={probe} ",
                                        "sink_kept: probe=stranger ")
            elif damage == "missing-file":
                path.unlink()
            elif damage == "empty-file":
                path.write_bytes(b"")
            elif damage == "outside-directory":
                outside = tmp_path / "outside.sink"
                outside.write_bytes(raw)
                output = output.replace(f"path={path}", f"path={outside}")
            elif damage == "shared-file":
                probes.append("beta")
                output += f"sink_kept: probe=beta path={path}\n"
            elif damage == "malformed":
                output = output.replace(f"path={path}", "no-path")
            canary_log.append_lines(log, output.splitlines(True))
            result = run_canary_tool(
                "kept", log, "--directory", sink_directory, "--probes", *probes)
            if damage is None:
                assert result.returncode == 0, result.stderr
                assert result.stdout.splitlines() == [
                    f"sink-on kept sinks: {len(probes)}", expected,
                ]
            else:
                assert result.returncode == 1, result.stdout + result.stderr
                assert "sink_canary_log:" in result.stderr
                assert "sink-on kept sinks:" not in result.stdout
        finally:
            report.sink.keep = False


@pytest.mark.parametrize("damage, offenders", [
    ("missing", ("beta",)),
    ("duplicate", ("alpha",)),
    ("wrong-names-same-count", ("alpha", "beta")),
    ("failed", ("broken",)),
    ("unexpected", ("beta",)),
    ("malformed", ("beta",)),
    ("missing-equity-field", ("beta",)),
    ("duplicate-expectation", ("alpha",)),
])
def test_canary_log_checker_names_every_offending_probe(
        tmp_path, sweep_probe_outputs, damage, offenders):
    probes = ["alpha", "beta"]
    outputs = [sweep_probe_outputs[probe] for probe in probes]
    if damage == "missing":
        outputs.pop()
    elif damage == "duplicate":
        outputs.append(outputs[0])
    elif damage == "wrong-names-same-count":
        outputs[1] = outputs[0]
    elif damage == "failed":
        probes.append("broken")
        outputs.append(sweep_probe_outputs["broken"])
    elif damage == "unexpected":
        probes.pop()
    elif damage == "malformed":
        outputs[1] = outputs[1].replace(" ok\n", " unrecognized\n")
    elif damage == "missing-equity-field":
        outputs[1] = re.sub(r" equity=\d+", "", outputs[1])
    else:
        probes.append(probes[0])
    log = tmp_path / "canaries.log"
    canary_log.append_lines(log, "".join(outputs).splitlines(True))
    result = run_canary_tool("check", log, "--probes", *probes)
    assert result.returncode == 1, result.stdout + result.stderr
    for probe in offenders:
        assert f"probe={probe}:" in result.stderr


def corpus_driver_fixture(tmp_path, outputs):
    """Run the real shell driver with native runs replaying fixture output.

    No build, feed derivation, report generation or parity script executes.
    Only the new canary collector/checker runs as a real Python child.
    """
    root = tmp_path / "sweep"
    scripts = root / "scripts"
    scripts.mkdir(parents=True)
    shutil.copy2(ROOT / "scripts" / "run_corpus.sh", scripts / "run_corpus.sh")
    shutil.copy2(CANARY_TOOL, scripts / CANARY_TOOL.name)
    corpus = root / "corpus"
    corpus.mkdir()
    (corpus / "CMakeLists.txt").write_text("# fixture\n")
    output_dir = root / "outputs"
    output_dir.mkdir()
    for probe, output in outputs.items():
        directory = corpus / "validation" / probe
        directory.mkdir(parents=True)
        (directory / "generated.cpp").write_text("// fixture\n")
        (directory / "strategy.so").write_bytes(b"fixture")
        (output_dir / f"{probe}.log").write_text(
            "runner output before the canary\n" + output + "runner output after it\n",
            encoding="utf-8")
    runner = scripts / "fixture-python"
    runner.write_text(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        "if [[ \"$1\" == -I ]]; then\n"
        "    exec \"$PF_CANARY_TEST_PYTHON\" \"$@\"\n"
        "fi\n"
        "case \"$1\" in\n"
        "    scripts/run_strategy.py)\n"
        "        probe=\"${2##*/}\"\n"
        "        cat \"$PF_CANARY_TEST_OUTPUTS/$probe.log\"\n"
        "        [[ \"$probe\" != broken ]]\n"
        "        ;;\n"
        "    scripts/derive_corpus_feeds.py|scripts/verify_corpus.py|"
        "scripts/regen_validation_report.py|scripts/validation_report_self_test.py)\n"
        "        ;;\n"
        "    *) exit 99 ;;\n"
        "esac\n",
        encoding="utf-8")
    runner.chmod(0o755)
    env = os.environ.copy()
    for name in ("PINEFORGE_CANARY_LOG", "ONLY", "RUNNER", "REGEN",
                 "SKIP_RUN", "SKIP_BUILD", "SKIP_VERIFY"):
        env.pop(name, None)
    env.update({
        "PYTHON": str(runner), "SKIP_BUILD": "1", "SKIP_VERIFY": "1",
        "PF_CANARY_TEST_PYTHON": sys.executable,
        "PF_CANARY_TEST_OUTPUTS": str(output_dir),
    })
    return root, env


def run_corpus_driver(root, env):
    return subprocess.run(
        ["bash", str(root / "scripts" / "run_corpus.sh")], cwd=root, env=env,
        capture_output=True, text=True, check=False,
    )


@pytest.mark.parametrize("failed", [False, True])
def test_full_sweep_truncates_once_and_keeps_passing_and_failing_canaries(
        tmp_path, sweep_probe_outputs, failed):
    probes = ["alpha", "beta"] + (["broken"] if failed else [])
    outputs = {probe: sweep_probe_outputs[probe] for probe in probes}
    root, env = corpus_driver_fixture(tmp_path, outputs)
    log = root / "canaries.log"
    log.write_text("stale run must be removed\n", encoding="utf-8")
    env["PINEFORGE_CANARY_LOG"] = str(log)
    result = run_corpus_driver(root, env)
    assert result.returncode == int(failed), result.stdout + result.stderr
    expected = "".join(outputs[probe] for probe in sorted(probes))
    assert log.read_text(encoding="utf-8") == expected
    checked = run_canary_tool("check", log, "--probes", *probes)
    assert checked.returncode == int(failed)
    if failed:
        assert "probe=broken:" in checked.stderr
        assert "refusing to verify stale engine_trades.csv" in result.stderr
    else:
        assert f"ok probes={len(probes)}" in result.stdout


def test_full_sweep_checks_the_filtered_selection_and_rejects_missing_canaries(
        tmp_path, sweep_probe_outputs):
    outputs = {probe: sweep_probe_outputs[probe] for probe in ("alpha", "beta")}
    root, env = corpus_driver_fixture(tmp_path, outputs)
    log = root / "canaries.log"
    env.update({"PINEFORGE_CANARY_LOG": str(log), "ONLY": "beta"})
    result = run_corpus_driver(root, env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert log.read_text(encoding="utf-8") == outputs["beta"]
    # A runner silently taking its array path must fail coverage even though
    # it reports success. Start another sweep with no real canary output.
    (root / "outputs" / "beta.log").write_text("net_profit=ignored\n")
    result = run_corpus_driver(root, env)
    assert result.returncode == 1
    assert log.read_text(encoding="utf-8") == ""
    assert "probe=beta:" in result.stderr


def test_full_sweep_without_a_canary_log_keeps_existing_output_behavior(
        tmp_path, sweep_probe_outputs):
    outputs = {"alpha": sweep_probe_outputs["alpha"]}
    root, env = corpus_driver_fixture(tmp_path, outputs)
    untouched = root / "canaries.log"
    previous = b"an unrelated file\n"
    untouched.write_bytes(previous)
    result = run_corpus_driver(root, env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert untouched.read_bytes() == previous
    assert "sink_canary:" not in result.stdout
    assert "sink_kept:" not in result.stdout
    assert "sink_canary_log:" not in result.stdout + result.stderr
