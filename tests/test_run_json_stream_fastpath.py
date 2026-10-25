"""CR150-CX / S-272: compact report bytes and bounded writer call counts.

The two private serializer functions below are copied verbatim from
b0f4341478's docker/run_json.py. They remain the piecewise oracle; only the
stream wrapper types are shared with production, never its writer functions.
"""
import ctypes
from collections import Counter
import io
import json
import math
from pathlib import Path
import sys
import tracemalloc

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))

import run_json
from pf_sink import Record, SinkFormatError

_JSONColumn = run_json._JSONColumn
_JSONRecordArray = run_json._JSONRecordArray
_JSONUTF8 = run_json._JSONUTF8


def _stream_json(value, destination):
    """json.dump's compact bytes, with bounded arrays supplied by the sink."""
    compound = isinstance(value, (dict, list, tuple, _JSONRecordArray))
    active = getattr(destination, "_json_active", None)
    owner = active is None
    if owner:
        active = destination._json_active = set()
    marker = id(value)
    if compound:
        if marker in active:
            raise ValueError("Circular reference detected")
        active.add(marker)
    try:
        _stream_json_value(value, destination)
    finally:
        if compound:
            active.remove(marker)
        if owner:
            del destination._json_active


def _stream_json_value(value, destination):
    if isinstance(value, _JSONColumn):
        destination.write("[")
        value.file.seek(0)
        while chunk := value.file.read(64 * 1024):
            destination.write(chunk)
        destination.write("]")
    elif isinstance(value, _JSONRecordArray):
        destination.write("[")
        first = True
        for item in value.records():
            if not first:
                destination.write(",")
            _stream_json(item, destination)
            first = False
        destination.write("]")
    elif isinstance(value, dict):
        destination.write("{")
        for index, (key, item) in enumerate(value.items()):
            if index:
                destination.write(",")
            destination.write(json.dumps(key, separators=(",", ":"), allow_nan=False))
            destination.write(":")
            _stream_json(item, destination)
        destination.write("}")
    elif isinstance(value, (list, tuple)):
        destination.write("[")
        for index, item in enumerate(value):
            if index:
                destination.write(",")
            _stream_json(item, destination)
        destination.write("]")
    elif isinstance(value, str):
        # Encode string interiors in fixed character windows; slicing at a
        # Unicode code point preserves ensure_ascii's surrogate/escape bytes.
        destination.write('"')
        for offset in range(0, len(value), 4096):
            destination.write(json.dumps(value[offset:offset + 4096], allow_nan=False)[1:-1])
        destination.write('"')
    elif isinstance(value, _JSONUTF8):
        destination.write('"')
        for chunk in value.chunks():
            destination.write(json.dumps(chunk, allow_nan=False)[1:-1])
        destination.write('"')
    else:
        destination.write(json.dumps(value, separators=(",", ":"), allow_nan=False))


class _Destination:
    def __init__(self):
        self.calls = []

    def write(self, text):
        self.calls.append(text)
        return len(text)

    def bytes(self):
        return "".join(self.calls).encode("ascii")


def _outcome(writer, document):
    destination = _Destination()
    try:
        writer(document, destination)
    except Exception as error:
        failure = (type(error), str(error))
    else:
        failure = None
    assert not hasattr(destination, "_json_active")
    return destination, failure


def _assert_bytes(document):
    reference, old_failure = _outcome(_stream_json, document)
    actual, new_failure = _outcome(run_json._stream_json, document)
    assert old_failure is None, old_failure
    assert new_failure == old_failure
    assert actual.bytes() == reference.bytes()
    return actual


def _assert_failure(document, error_type):
    reference, old_failure = _outcome(_stream_json, document)
    actual, new_failure = _outcome(run_json._stream_json, document)
    assert old_failure is not None and old_failure[0] is error_type
    assert new_failure == old_failure
    assert actual.bytes() == reference.bytes()


def _array(rows):
    return _JSONRecordArray(lambda: iter(rows))


def _point(index):
    return {"time_ms": 2**53 + 1 + index, "equity": 10000.0 + index,
            "open_profit": -0.0}


SCALARS = (
    0, -1, 2**53 + 1, 2**100, -(2**100),
    0.0, -0.0, 1e-7, 1e16, 1e22, 5e-324, -5e-324, 0.1 + 0.2,
    True, False, None, "",
    'quotes" backslash\\ controls\x00\b\f\n\r\t\x1f',
    "漢字 café \U0001f680 \u2028",
    "lone high \ud800 lone low \udfff",
    "a" * 4095 + "\U0001f680" + "b" * 4096 + "\ud800",
)


@pytest.mark.parametrize("value", SCALARS, ids=range(len(SCALARS)))
def test_scalar_record_byte_identity(value):
    _assert_bytes({'key"\\\n漢\ud800': value, "truth": True, "integer": 1})


FLOAT_EDGE_VALUES = (
    -0.0, 0.0, 1e16, 1e22, 5e-324,
    1.7976931348623157e308, 0.1 + 0.2, -1e-7,
)
STRING_EDGE_VALUES = ('\ud800', "\U0001f680", '"', "\\", "\x00\b\f\n\r\t\x1f")
EDGE_VALUE_IDS = (
    "negative-zero", "positive-zero", "large-1e16", "large-1e22",
    "minimum-subnormal", "maximum-finite", "binary-sum", "negative-1e-7",
    "lone-surrogate", "non-bmp", "quote", "backslash", "controls",
)


@pytest.mark.parametrize("shape", ("record", "bare"), ids=("record", "bare"))
@pytest.mark.parametrize("value", FLOAT_EDGE_VALUES + STRING_EDGE_VALUES,
                         ids=EDGE_VALUE_IDS)
def test_old_path_edge_values_inside_records_and_as_bare_elements(value, shape):
    if shape == "record":
        document = _array(({"edge": value, "anchor": True, "count": 2},))
    else:
        document = _array((value,))
    _assert_bytes(document)


@pytest.mark.parametrize("count", (0, 1, 511, 512, 513, 1024, 1025))
@pytest.mark.parametrize("kind", ("records", "scalars", "mixed"))
def test_array_batch_boundary_byte_identity(count, kind):
    def rows():
        for index in range(count):
            if kind == "records" or kind == "mixed" and index % 2:
                yield _point(index)
            else:
                yield SCALARS[index % len(SCALARS)]

    _assert_bytes(_JSONRecordArray(rows))


@pytest.mark.parametrize("kind", ("records", "scalars"))
def test_array_write_count(kind):
    count = 10000
    document = _JSONRecordArray(
        lambda: (_point(index) if kind == "records" else index
                 for index in range(count)))
    actual = _assert_bytes(document)
    # Contract: batch=512, two brackets, ceil(N/512) batch writes.
    # K=3 allows the final partial batch: floor(N/512) + 1 + 2 = 22.
    # The old numeric three-key record path makes 13 writes per record,
    # N-1 separator writes and two brackets: 14*N+1 = 140001.
    # Old scalar arrays similarly make 2*N+1 = 20001 writes.
    assert len(actual.calls) <= count // 512 + 3


def test_scalar_record_write_count():
    actual = _assert_bytes(_point(7))
    assert len(actual.calls) == 1


@pytest.mark.parametrize("kind", ("list", "dict", "tuple", "array", "column", "utf8"))
def test_nested_container_byte_identity(kind):
    buffer = ctypes.create_string_buffer(b'quote"\\\n' + "漢".encode() + b"\xff")
    column = object.__new__(_JSONColumn)
    column.file = io.StringIO()
    # Build the already encoded column with the OLD path too.
    _stream_json(_point(3), column.file)
    nested = {
        "list": [True, {"child": _array((1, None, "漢"))}],
        "dict": {"child": _array((False, -0.0))},
        "tuple": (None, {"child": _array(())}),
        "array": _array((_point(5), "nested")),
        "column": column,
        "utf8": _JSONUTF8(ctypes.addressof(buffer)),
    }[kind]
    # Pending batches must flush on both sides of each nested value.
    document = _array((_point(0), 1, {"nested": nested}, _point(2), "tail"))
    _assert_bytes(document)


class _SliceString(str):
    def __getitem__(self, index):
        return super().__getitem__(index).upper()


class _Integer(int):
    pass


class _Float(float):
    pass


class _ReverseItems(dict):
    def items(self):
        return reversed(list(super().items()))


def test_subclasses_and_non_string_keys_byte_identity():
    # A str subclass's slice spelling is observable in the old piecewise
    # path; json.dumps of the entire record would bypass __getitem__.
    _assert_bytes(_array((
        {"value": _SliceString("mixed case")},
        _SliceString("scalar mixed case"),
        {"integer": _Integer(2**53 + 1), "float": _Float(-0.0)},
        _ReverseItems(first=1, second="two"),
        {42: True, None: "none", (1, 2): "tuple", 2.5: "float"},
        {_SliceString("key"): "value"},
    )))


@pytest.mark.parametrize("value", (float("nan"), float("inf"), -float("inf")),
                         ids=("nan", "inf", "negative-inf"))
def test_nonfinite_scalar_record_partial_bytes(value):
    _assert_failure({"before": 'quote"漢', "value": value, "after": 17}, ValueError)


@pytest.mark.parametrize("value", (float("nan"), float("inf"), -float("inf")),
                         ids=("nan", "inf", "negative-inf"))
@pytest.mark.parametrize("position", (0, 17, 511, 512, 513))
@pytest.mark.parametrize("field", ("equity", "open_profit", None),
                         ids=("record-equity", "record-open-profit", "scalar"))
def test_nonfinite_array_partial_bytes(value, position, field):
    visited = []

    def rows():
        for index in range(position + 3):
            visited.append(index)
            if field is None:
                yield value if index == position else index
            else:
                point = _point(index)
                if index == position:
                    point[field] = value
                yield point

    document = _JSONRecordArray(rows)
    reference, old_failure = _outcome(_stream_json, document)
    old_visited = visited[:]
    visited.clear()
    actual, new_failure = _outcome(run_json._stream_json, document)
    assert old_failure is not None and old_failure[0] is ValueError
    assert new_failure == old_failure
    assert actual.bytes() == reference.bytes()
    assert visited == old_visited == list(range(position + 1))


@pytest.mark.parametrize("kind", ("dict", "list", "tuple", "array"))
def test_circular_reference_partial_bytes(kind):
    if kind == "dict":
        cycle = {"before": 1}
        cycle["self"] = cycle
    elif kind == "list":
        cycle = [1]
        cycle.append(cycle)
    elif kind == "tuple":
        child = []
        cycle = (1, child)
        child.append(cycle)
    else:
        cycle = _JSONRecordArray(lambda: iter((1, cycle)))
    _assert_failure(_array((_point(0), cycle, _point(2))), ValueError)


@pytest.mark.parametrize("count", (1, 512, 513))
def test_iterator_failure_preserves_pending_bytes(count):
    def rows():
        for index in range(count):
            yield _point(index)
        raise RuntimeError("record producer failed")

    _assert_failure(_JSONRecordArray(rows), RuntimeError)


def test_reused_nested_value_is_not_a_cycle():
    shared = {"points": _array((_point(0), _point(1)))}
    _assert_bytes(_array((_point(2), shared, shared, _point(3))))


# Round 2 oracle: these three functions are the pre-round-2 bodies verbatim.
# Only their unchanged dependencies (the small native report producer, wrapper
# types and annualization arithmetic) are shared with the implementation.
RunFailure = run_json.RunFailure
_engine_invariant = run_json._engine_invariant
_sink_trade = run_json._sink_trade
build_report_dict = run_json.build_report_dict
_annualization_block = run_json._annualization_block


def _validate_report_result(out: dict, *, validate_summary: bool = True) -> None:
    """Required results are finite; only native metric absence is nullable.
    Metric keys come from the producer's ctypes descriptors and alias map,
    after native computation has distinguished absence from computed NaN.
    The sink pre-pass validates one trade here at a time; its completed summary
    is checked by the default full validation before the report is serialized."""
    for trade in out["trades"]:
        for key, field in (
            ("entry_price", "trades.entry_price"),
            ("exit_price", "trades.exit_price"),
            ("qty", "trades.qty"),
            ("pnl", "trades.pnl"),
            ("pnl_pct", "trades.pnl_pct"),
            ("max_runup", "trades.max_runup"),
            ("max_drawdown", "trades.max_drawdown"),
            ("commission", "trades.commission"),
        ):
            if trade[key] is None or not math.isfinite(trade[key]):
                raise RunFailure(f"the report result {field} is not finite",
                                 "report_result_invalid",
                                 {"field": field, "reason": "nonfinite"})
    if not validate_summary:
        return
    for key, field in (
        ("win_rate_pct", "summary.win_rate_pct"),
        ("net_pnl", "summary.net_pnl"),
        ("avg_trade", "summary.avg_trade"),
        ("best_trade", "summary.best_trade"),
        ("worst_trade", "summary.worst_trade"),
        ("max_drawdown", "summary.max_drawdown"),
    ):
        if not math.isfinite(out["summary"][key]):
            raise RunFailure(f"the report result {field} is not finite",
                             "report_result_invalid",
                             {"field": field, "reason": "nonfinite"})
    for block in ("all", "longs", "shorts", "equity"):
        for name, value in out["metrics"][block].items():
            if value is not None and not math.isfinite(value):
                metric_field = f"metrics.{block}.{name}"
                raise RunFailure(f"the report result {metric_field} is not finite",
                                 "report_result_invalid",
                                 {"field": metric_field, "reason": "nonfinite"})
    for point in out["equity_curve"]:
        for key, field in (
            ("equity", "equity_curve.equity"),
            ("open_profit", "equity_curve.open_profit"),
        ):
            if point[key] is None or not math.isfinite(point[key]):
                raise RunFailure(f"the report result {field} is not finite",
                                 "report_result_invalid",
                                 {"field": field, "reason": "nonfinite"})


def _sink_summary(report, sink):
    n, wins, losses = 0, 0, 0
    cum, peak, max_dd = 0.0, 0.0, 0.0
    best, worst = 0.0, 0.0
    fd_mode = getattr(sink, "fd_mode", False)
    if fd_mode:
        summary = sink.summary
        if summary is None or summary.complete != 1 or summary.count_t != sink.counts["T"]:
            raise _engine_invariant("the engine's report sink summary is incomplete")
        invalid = int(summary.nonfinite_t_field)
        if invalid:
            # Literal assignments are also the ownership checker's field vocabulary.
            if invalid == 5:
                field = "trades.entry_price"
            elif invalid == 6:
                field = "trades.exit_price"
            elif invalid == 7:
                field = "trades.qty"
            elif invalid == 8:
                field = "trades.pnl"
            elif invalid == 9:
                field = "trades.pnl_pct"
            elif invalid == 10:
                field = "trades.max_runup"
            elif invalid == 11:
                field = "trades.max_drawdown"
            elif invalid == 12:
                field = "trades.commission"
            else:
                raise _engine_invariant("the report sink summary has an unknown trade field")
            raise RunFailure(f"the report result {field} is not finite",
                             "report_result_invalid", {"field": field, "reason": "nonfinite"})
        n, wins, losses = int(summary.count_t), int(summary.wins), int(summary.losses)
        best, worst = float(summary.best_trade), float(summary.worst_trade)
        max_dd = float(summary.max_drawdown)
    for record in () if fd_mode else sink.records("T"):
        trade = _sink_trade(record)
        if trade["n"] != n + 1:
            raise _engine_invariant("the report sink trade rows are out of report order")
        _validate_report_result({"trades": (trade,)}, validate_summary=False)
        pnl = trade["pnl"]
        # Preserve Python's original left folds, including ties at +/-0.
        best = max(best, pnl) if n else pnl
        worst = min(worst, pnl) if n else pnl
        n += 1
        wins += int(pnl > 0)
        losses += int(pnl < 0)
        cum += pnl
        peak = max(peak, cum)
        drawdown = cum - peak
        if not math.isfinite(cum) or not math.isfinite(drawdown):
            raise RunFailure("the report result summary.max_drawdown is not finite",
                             "report_result_invalid",
                             {"field": "summary.max_drawdown", "reason": "nonfinite"})
        max_dd = min(max_dd, drawdown)
    return {
        "total_trades": n,
        "wins": wins,
        "losses": losses,
        "win_rate_pct": round((wins / n * 100.0) if n else 0.0, 4),
        "net_pnl": float(report.net_profit),
        "avg_trade": (float(report.net_profit) / n) if n else 0.0,
        "best_trade": best,
        "worst_trade": worst,
        "max_drawdown": max_dd,
        "bars_processed": int(report.input_bars_processed),
    }


def _build_sink_report(report, ohlcv_path, n_bars, first_ts, last_ts, elapsed,
                       applied_inputs, applied_overrides, applied_runtime, sink,
                       window_anchor=False):
    if report.trades or report.trades_len or report.equity_curve or report.equity_curve_len:
        raise _engine_invariant("a report with a sink must contain summary scalars only")
    # Reuse the unchanged producer for its small fields and exact key order.
    # The C ABI supplied no arrays, so this cannot walk native report rows.
    out = build_report_dict(report, ohlcv_path, n_bars, first_ts, last_ts, elapsed,
                            applied_inputs, applied_overrides, applied_runtime,
                            window_anchor=window_anchor)
    out["summary"] = _sink_summary(report, sink)
    if getattr(sink, "fd_mode", False):
        _validate_report_result(out)  # the engine's summary checked every T
        diagnostics = sink.diagnostics
        endpoints = [
            {"time_ms": int(point.time_ms), "equity": float(point.equity),
             "open_profit": float(point.open_profit)}
            for point in (diagnostics.first, diagnostics.last)
        ] if diagnostics.count_e else ()
        first_bar_trade = ({"entry_bar_index": 0},) if diagnostics.trade_at_start else ()
        out["diagnostics"].update(
            _annualization_block(endpoints, out["metrics"], first_bar_trade, window_anchor))
        # stdout.json has the small report only. The wrapper/finalizer owns
        # the stream's artifact and receipt; no child-local path names it.
        out.pop("trades")
        out.pop("equity_curve")
        return out

    def point_at(index):
        # Diagnostics need only the endpoints. Revisit the verified sink in
        # bounded memory rather than retaining its equity history. Consume
        # the pass through its trailer so records() checks its digest and
        # closes the reader even when the requested point is the first one.
        point = None
        for i, row in enumerate(sink.records("E")):
            if i == index:
                point = row.as_dict()
        if point is None:
            raise _engine_invariant("the report sink equity count differs from its receipt")
        return point

    trades = _JSONRecordArray(
        lambda: (_sink_trade(row) for row in sink.records("T")), sink.counts["T"])
    equity_curve = _JSONRecordArray(
        lambda: (row.as_dict() for row in sink.records("E")), sink.counts["E"], point_at)
    out["equity_curve"] = equity_curve
    _validate_report_result(out)  # the summary pre-pass already checked every T
    out["diagnostics"].update(
        _annualization_block(equity_curve, out["metrics"], trades, window_anchor))
    if sink.named:
        reference = {
            "path": sink.path,
            "schema": sink.header["schema"],
            "version": sink.header["version"],
            "run_id": sink.header["run_id"],
            "counts": sink.counts,
            "sha256": sink.sha256,
        }
        # Insert the reference at the old trades position; all other small
        # keys retain the frozen producer's order and values.
        return {
            ("report_sink" if key == "trades" else key):
                (reference if key == "trades" else value)
            for key, value in out.items() if key != "equity_curve"
        }
    out["trades"] = trades
    return out


_E_FIELDS = ("time_ms", "equity", "open_profit")
_T_FIELDS = (
    "n", "side", "entry_time", "exit_time", "entry_price", "exit_price", "qty",
    "pnl", "pnl_pct", "max_runup", "max_drawdown", "commission", "entry_bar_index",
    "exit_bar_index", "open_at_end", "entry_incarnation", "close_cause", "entry_id",
    "exit_id", "exit_comment",
)
_START_MS = 1_700_000_000_000
_DAY_MS = 86_400_000


def _equity(offset, equity, open_profit=-0.0):
    return Record("E", _E_FIELDS, (_START_MS + offset, equity, open_profit))


def _trade(number, entry_bar_index=1, pnl=0.0, **changes):
    row = dict(zip(_T_FIELDS, (
        number, "long", _START_MS, _START_MS + _DAY_MS, 100.0, 101.0, 2.0,
        pnl, 1.0, 2.0, -1.0, 0.5, entry_bar_index, 2, False, 0, 0, "", "", "",
    )))
    row.update(changes)
    return Record("T", _T_FIELDS, tuple(row[name] for name in _T_FIELDS))


class _StubReader:
    """Replay memory rows; count every strict-reader __next__, including EOF."""
    def __init__(self, sink):
        self.sink = sink
        self.rows = iter(sink.rows)
        self.header = sink.expected_header
        self.counts = sink.expected_counts
        self.sha256 = ""
        self.complete = False
        self.number = sink.opens = sink.opens + 1

    def __iter__(self):
        return self

    def __next__(self):
        self.sink.next_calls += 1
        try:
            record = next(self.rows)
        except StopIteration:
            if self.number == self.sink.refuse_pass:
                raise SinkFormatError("trailer SHA-256 mismatch")
            self.sha256 = self.sink.expected_sha256
            self.complete = True
            self.sink.exhausted += 1
            raise
        self.sink.visits[record.tag] += 1
        return record

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        return False


class _StubSink(run_json._ReportSink):
    """Use real verify()/records() over a counted in-memory strict-reader stub."""
    def __init__(self, rows, *, named=False, refuse_pass=None, receipt_fault=None):
        self.rows = tuple(rows)
        self.expected_counts = dict.fromkeys("ETOVKXH", 0)
        for record in self.rows:
            self.expected_counts[record.tag] += 1
        self.expected_header = {
            "schema": "pf-report-sink", "version": 1, "run_id": "same-run",
        }
        self.expected_sha256 = "a" * 64
        self.header, self.counts, self.sha256 = {}, {}, ""
        self.named, self.run_id = named, "same-run"
        self.path = "/same/reviewable/report.sink"
        self.summary, self.summary_failure, self.diagnostics = None, None, None
        self.calls, self.visits = Counter(), Counter()
        self.opens = self.next_calls = self.exhausted = 0
        self.refuse_pass, self.receipt_fault = refuse_pass, receipt_fault

    def records(self, tag=None):
        self.calls[tag] += 1
        return super().records(tag)

    def strategy_report_sink_receipt_v1(self, state, pointer):
        receipt = pointer._obj
        for tag in "ETOVKXH":
            setattr(receipt, "count_" + tag.lower(), self.expected_counts[tag])
        receipt.sha256_hex = self.expected_sha256.encode("ascii")
        receipt.complete = int(self.receipt_fault != "incomplete")
        if self.receipt_fault == "digest":
            receipt.sha256_hex = b"b" * 64
        return 0


class _OldSink(_StubSink):
    # Verbatim pre-round-2 _ReportSink.verify, independent of the new read-back.
    def verify(self, lib, state, report):
        from pf_sink import SinkFormatError
        try:
            with _open_uncached_sink(self.path) as reader:
                for _ in reader:
                    pass
                self.header, self.counts, self.sha256 = reader.header, reader.counts, reader.sha256
        except SinkFormatError as error:
            raise _engine_invariant(f"the engine's report sink is invalid: {error}") from error
        if self.header["run_id"] != self.run_id:
            raise _engine_invariant("the report sink run id differs from the request")
        receipt = ReportSinkReceiptC()
        receipt.struct_size, receipt.version = ctypes.sizeof(receipt), 1
        if lib.strategy_report_sink_receipt_v1(state, ctypes.byref(receipt)) != 0 or receipt.complete != 1:
            raise _engine_invariant("the engine's report sink receipt is incomplete")
        counts = {tag: getattr(receipt, "count_" + tag.lower()) for tag in "ETOVKXH"}
        if (any(counts[tag] != self.counts.get(tag, 0) for tag in counts)
                or bytes(receipt.sha256_hex) != self.sha256.encode("ascii")
                or int(report.total_trades) != self.counts["T"]):
            raise _engine_invariant("the report sink trailer differs from its engine receipt")


ReportSinkReceiptC = run_json.ReportSinkReceiptC


def _open_uncached_sink(path):
    # The old verify body resolves this alias; each exercise installs its own
    # reader factory. No file is opened and no hard-coded report is returned.
    return run_json._open_uncached_sink(path)


def _exercise_sink(monkeypatch, builder, sink_type, rows, *, named=False,
                   window_anchor=False, configure=None, writer=None, **sink_options):
    sink = sink_type(rows, named=named, **sink_options)
    monkeypatch.setattr(run_json, "_open_uncached_sink", lambda path: _StubReader(sink))
    report = run_json.ReportC()
    report.total_trades = sink.expected_counts["T"]
    report.metrics.all.num_trades = report.total_trades
    report.input_bars_processed = report.script_bars_processed = sink.expected_counts["E"]
    report.net_profit = sum(row.as_dict()["pnl"] for row in rows if row.tag == "T")
    report.metrics.equity.cagr, report.metrics.equity.calmar = 25.0, 2.0
    if configure is not None:
        configure(report)
    destination, document, failure = _Destination(), None, None
    try:
        sink.verify(sink, None, report)
        document = builder(report, Path("fixed.csv"), 3, _START_MS, _START_MS + _DAY_MS,
                           0.125, {}, {}, {}, sink, window_anchor=window_anchor)
        (run_json._stream_json if writer is None else writer)(document, destination)
    except Exception as error:
        failure = (type(error), str(error), getattr(error, "code", None),
                   getattr(error, "code_args", None))
    assert not hasattr(destination, "_json_active")
    return sink, document, destination.bytes(), failure


def test_sink_report_pass_count(monkeypatch):
    rows = [_equity(0, 100.0), _equity(_DAY_MS, 100.0),
            _equity(2 * _DAY_MS, 1e300), _trade(1)]
    old, _, old_bytes, old_failure = _exercise_sink(
        monkeypatch, _build_sink_report, _OldSink, rows, writer=_stream_json)
    actual, _, actual_bytes, failure = _exercise_sink(
        monkeypatch, run_json._build_sink_report, _StubSink, rows)
    assert failure == old_failure is None
    assert actual_bytes == old_bytes
    # Old: one E validation + five endpoint walks + one writer E = seven
    # records("E") calls; verify also consumes the entire strict reader.
    assert old.calls["E"] >= 7
    # New budgets count verification too: one verified E/T read-back and
    # one output pass per tag. T<=3 is the brief's ceiling; reuse of the
    # verified summary and any-trade flag tightens it to two here.
    assert actual.calls["E"] + 1 <= 2
    assert actual.calls["T"] + 1 <= 2
    assert actual.calls[None] == 0
    # records(tag) still parses all tags. Three physical opens are allowed:
    # verified read-back, writer T, writer E; each includes the trailer EOF.
    assert actual.opens <= 3
    assert actual.next_calls <= 3 * (len(rows) + 1)
    assert actual.visits["E"] <= 3 * actual.expected_counts["E"]
    assert actual.visits["T"] <= 3 * actual.expected_counts["T"]
    assert actual.exhausted == actual.opens
    assert old.next_calls >= 8 * (len(rows) + 1)


@pytest.mark.parametrize("points", (
    (),
    (_equity(0, 100.0),),
    (_equity(0, 100.0, 7.0), _equity(_DAY_MS, 101.0),
     _equity(2 * _DAY_MS, 1e300)),
    (_equity(0, 100.0), _equity(_DAY_MS, 101.0),
     _equity(2 * _DAY_MS, float("inf"))),
), ids=("empty", "single", "first-open-profit", "nonfinite-last"))
@pytest.mark.parametrize("named", (False, True))
@pytest.mark.parametrize("window_anchor", (False, True))
def test_sink_report_hostile_pass_bounds(monkeypatch, points, named, window_anchor):
    rows = [*points, _trade(1)]
    _, _, old_bytes, old_failure = _exercise_sink(
        monkeypatch, _build_sink_report, _OldSink, rows, named=named,
        window_anchor=window_anchor, writer=_stream_json)
    sink, _, actual_bytes, failure = _exercise_sink(
        monkeypatch, run_json._build_sink_report, _StubSink, rows, named=named,
        window_anchor=window_anchor)
    assert failure == old_failure
    assert actual_bytes == old_bytes
    # The same fixed budget applies even when diagnostics short-circuit or
    # validation refuses the last point: verification + at most one writer/tag.
    assert sink.calls["E"] + 1 <= 2
    assert sink.calls["T"] + 1 <= 2
    assert sink.opens <= 3
    assert sink.next_calls <= 3 * (len(rows) + 1)
    assert sink.exhausted == sink.opens


@pytest.mark.parametrize("named", (False, True), ids=("unnamed", "named"))
@pytest.mark.parametrize("window_anchor", (False, True))
@pytest.mark.parametrize("points", (
    (),
    (_equity(0, 100.0),),
    (_equity(0, 100.0), _equity(_DAY_MS, 101.0)),
    (_equity(0, 100.0), _equity(1000, 101.0)),
    (_equity(0, 100.0, 3.0), _equity(1000, 101.0)),
    (_equity(0, 100.0, 3.0), _equity(2 * _DAY_MS, 1e300)),
    (_equity(0, 100.0), _equity(_DAY_MS, 100.0),
     _equity(2 * _DAY_MS, 1e300)),
), ids=("empty", "one", "two", "subday", "open-profit-subday",
        "open-profit-overflow", "last-differs-from-second"))
def test_sink_report_old_path_bytes(monkeypatch, named, window_anchor, points):
    # The three-point case has a finite second-point growth and overflowing
    # last-point growth. Substituting the second endpoint must change bytes.
    rows = [*points, _trade(1, pnl=-0.0), _trade(2, pnl=10.0),
            _trade(3, pnl=-7.0)]
    old_sink, old, old_bytes, old_failure = _exercise_sink(
        monkeypatch, _build_sink_report, _OldSink, rows, named=named,
        window_anchor=window_anchor, writer=_stream_json)
    sink, actual, actual_bytes, failure = _exercise_sink(
        monkeypatch, run_json._build_sink_report, _StubSink, rows, named=named,
        window_anchor=window_anchor)
    assert failure == old_failure is None
    assert actual["diagnostics"].get("annualization") == old["diagnostics"].get("annualization")
    assert actual_bytes == old_bytes
    assert sink.exhausted == sink.opens
    if named:
        assert "trades" not in actual and "equity_curve" not in actual
        assert actual["report_sink"] == old["report_sink"]
        assert tuple(actual["report_sink"]) == (
            "path", "schema", "version", "run_id", "counts", "sha256",
        )
        assert sink.opens == 1
    assert old_sink.exhausted == old_sink.opens


@pytest.mark.parametrize("entry_bars", ((0, 1), (1, 0), (1, -1), (1, 2)))
@pytest.mark.parametrize("window_anchor", (False, True))
def test_sink_report_checks_any_trade_at_start(monkeypatch, entry_bars, window_anchor):
    rows = [_equity(0, 100.0), _equity(2 * _DAY_MS, 1e300)]
    rows.extend(_trade(index + 1, entry) for index, entry in enumerate(entry_bars))
    _, old, old_bytes, old_failure = _exercise_sink(
        monkeypatch, _build_sink_report, _OldSink, rows, window_anchor=window_anchor,
        writer=_stream_json)
    _, actual, actual_bytes, failure = _exercise_sink(
        monkeypatch, run_json._build_sink_report, _StubSink, rows, window_anchor=window_anchor)
    assert failure == old_failure is None
    assert actual["diagnostics"] == old["diagnostics"]
    assert actual_bytes == old_bytes


@pytest.mark.parametrize("named", (False, True))
@pytest.mark.parametrize("value", (None, float("nan"), float("inf"), -float("inf")))
@pytest.mark.parametrize("field", ("equity", "open_profit"))
@pytest.mark.parametrize("position", (0, 1, 2))
def test_sink_nonfinite_equity_error_order(monkeypatch, named, value, field, position):
    points = [_equity(0, 100.0, 3.0), _equity(_DAY_MS, 101.0),
              _equity(2 * _DAY_MS, 1e300)]
    point = points[position].as_dict()
    point[field] = value
    points[position] = Record("E", _E_FIELDS, tuple(point[key] for key in _E_FIELDS))
    rows = [*points, _trade(1)]
    _, _, old_bytes, old_failure = _exercise_sink(
        monkeypatch, _build_sink_report, _OldSink, rows, named=named, writer=_stream_json)
    sink, _, actual_bytes, failure = _exercise_sink(
        monkeypatch, run_json._build_sink_report, _StubSink, rows, named=named)
    assert old_failure is not None
    assert failure == old_failure
    assert failure[2:] == (
        "report_result_invalid", {"field": f"equity_curve.{field}", "reason": "nonfinite"},
    )
    assert actual_bytes == old_bytes == b""
    assert sink.exhausted == 1  # trailer verification still finishes first


def _capture_sink_build_failure(monkeypatch, builder, sink_type, rows, *, before_build=None):
    sink = sink_type(rows)
    monkeypatch.setattr(run_json, "_open_uncached_sink", lambda path: _StubReader(sink))
    report = run_json.ReportC()
    report.total_trades = report.metrics.all.num_trades = sink.expected_counts["T"]
    report.input_bars_processed = report.script_bars_processed = sink.expected_counts["E"]
    report.net_profit = sum(row.as_dict()["pnl"] for row in rows if row.tag == "T")
    report.metrics.equity.cagr, report.metrics.equity.calmar = 25.0, 2.0
    sink.verify(sink, None, report)
    if before_build is not None:
        before_build(sink)
    with pytest.raises(RunFailure) as failure:
        builder(report, Path("fixed.csv"), 3, _START_MS, _START_MS + _DAY_MS,
                0.125, {}, {}, {}, sink)
    return sink, failure.value


def _assert_no_fold_traceback(error, *extra_codes):
    forbidden = (run_json._SinkReportDiagnostics.trades.__code__,
                 run_json._ReportSink.verify.__code__, *extra_codes)
    traceback = error.__traceback__
    while traceback is not None:
        assert traceback.tb_frame.f_code not in forbidden
        traceback = traceback.tb_next


@pytest.mark.parametrize("kind", ("trade", "equity"))
@pytest.mark.parametrize("value", (float("nan"), float("inf"), -float("inf")))
def test_sink_deferred_failure_matches_old_path(monkeypatch, kind, value):
    # Ordinary failures have identical metadata on the old and new paths.
    # This parity pin is expected to pass before the object-storage change.
    rows = [_equity(0, value if kind == "equity" else 100.0),
            _trade(1, entry_price=value if kind == "trade" else 100.0),
            _equity(_DAY_MS, 101.0)]
    _, old = _capture_sink_build_failure(
        monkeypatch, _build_sink_report, _OldSink, rows)
    sink, actual = _capture_sink_build_failure(
        monkeypatch, run_json._build_sink_report, _StubSink, rows)
    assert type(actual) is type(old)
    assert str(actual) == str(old)
    assert actual.code == old.code
    assert actual.code_args == old.code_args
    assert actual.exit_status == old.exit_status
    _assert_no_fold_traceback(actual)
    assert sink.exhausted == sink.opens == 1


@pytest.mark.parametrize("kind", ("trade", "equity"))
@pytest.mark.parametrize("failure_type", (RunFailure, run_json.StrategyLibraryError),
                         ids=("exit-status-2", "subclass"))
def test_sink_deferred_failure_keeps_object(monkeypatch, kind, failure_type):
    if failure_type is RunFailure:
        injected = RunFailure("x", "report_result_invalid", {"field": "f"}, exit_status=2)
    else:
        injected = failure_type("x", {"field": "f"})
    summary, validate = run_json._sink_summary, run_json._validate_report_result

    def injected_summary(report, sink, *, records=None):
        if records is not None:
            next(records, None)  # fail inside the fold, with reader frames live
            raise injected
        return summary(report, sink)

    def injected_validate(out, *, validate_summary=True, equity_only=False):
        if equity_only:
            raise injected
        return validate(out, validate_summary=validate_summary, equity_only=equity_only)

    if kind == "trade":
        monkeypatch.setattr(run_json, "_sink_summary", injected_summary)
    else:
        monkeypatch.setattr(run_json, "_validate_report_result", injected_validate)

    def check_stored(sink):
        stored = sink.summary_failure if kind == "trade" else sink.diagnostics.equity_failure
        assert stored is injected
        # Check BEFORE assembly raises it; no fold/reader frame may be retained.
        assert stored.__traceback__ is None
        # Assembly must use the real raise sites, without the injection frames.
        monkeypatch.setattr(run_json, "_sink_summary", summary)
        monkeypatch.setattr(run_json, "_validate_report_result", validate)

    rows = [_equity(0, 100.0), _trade(1), _equity(_DAY_MS, 101.0), _trade(2)]
    sink, actual = _capture_sink_build_failure(
        monkeypatch, run_json._build_sink_report, _StubSink, rows,
        before_build=check_stored)
    assert type(actual) is failure_type
    assert actual.exit_status == injected.exit_status
    assert actual is injected
    _assert_no_fold_traceback(actual, injected_summary.__code__, injected_validate.__code__)
    assert sink.exhausted == sink.opens == 1


@pytest.mark.parametrize("conflict", (
    "trade-field", "trade-order", "drawdown", "summary", "metric",
    "equity-field-order", "equity-point-order", "trailer", "receipt",
))
@pytest.mark.parametrize("order", ("equity-first", "trade-first", "interleaved"))
def test_sink_preserves_competing_error_priority(monkeypatch, conflict, order):
    rows = [_equity(0, 100.0, float("inf")),
            _equity(_DAY_MS, None), _trade(1)]
    options = {}
    configure = None
    if conflict == "trade-field":
        rows[-1] = _trade(1, entry_price=None, exit_price=float("inf"))
    elif conflict == "trade-order":
        rows[-1] = _trade(2, entry_price=None)
    elif conflict == "drawdown":
        rows[-1:] = [_trade(1, pnl=1e308), _trade(2, pnl=1e308)]
    elif conflict == "summary":
        configure = lambda report: setattr(report, "net_profit", float("inf"))
    elif conflict == "metric":
        configure = lambda report: setattr(report.metrics.all, "gross_profit", float("inf"))
    elif conflict == "equity-field-order":
        rows[0] = _equity(0, None, float("inf"))
    elif conflict == "trailer":
        options["refuse_pass"] = 1
    elif conflict == "receipt":
        options["receipt_fault"] = "incomplete"
    if order == "trade-first":
        rows = [row for row in rows if row.tag == "T"] + [
            row for row in rows if row.tag == "E"]
    elif order == "interleaved":
        rows = [rows[0], *rows[2:], rows[1]]
    _, _, old_bytes, old_failure = _exercise_sink(
        monkeypatch, _build_sink_report, _OldSink, rows, configure=configure,
        writer=_stream_json, **options)
    _, _, actual_bytes, failure = _exercise_sink(
        monkeypatch, run_json._build_sink_report, _StubSink, rows,
        configure=configure, **options)
    assert old_failure is not None
    assert failure == old_failure
    assert actual_bytes == old_bytes == b""


@pytest.mark.parametrize("writer_pass", (2, 3), ids=("trade-writer", "equity-writer"))
def test_sink_writer_verifies_trailer_before_publication(monkeypatch, writer_pass):
    rows = [_equity(0, 100.0), _equity(1000, 101.0), _trade(1)]
    sink = _StubSink(rows, refuse_pass=writer_pass)
    monkeypatch.setattr(run_json, "_open_uncached_sink", lambda path: _StubReader(sink))
    report = run_json.ReportC()
    report.total_trades = report.metrics.all.num_trades = 1
    sink.verify(sink, None, report)
    document = run_json._build_sink_report(
        report, Path("fixed.csv"), 2, _START_MS, _START_MS + 1000,
        0.125, {}, {}, {}, sink)
    # _prepare_stream_report serializes into a private artifact before
    # publication. This in-memory private artifact exercises the same writer.
    published = []
    with pytest.raises(RunFailure, match="trailer SHA-256 mismatch") as failure:
        artifact = _Destination()
        run_json._stream_json(document, artifact)
        published.append(artifact.bytes())
    assert failure.value.code == "engine_invariant"
    assert published == []


class _DiscardDestination:
    def __init__(self):
        self.longest_write = 0
        self.writes = 0

    def write(self, text):
        self.longest_write = max(self.longest_write, len(text))
        self.writes += 1
        return len(text)


def _windowed_peak(document):
    destination = _DiscardDestination()
    tracemalloc.start()
    try:
        run_json._stream_json(document, destination)
        return tracemalloc.get_traced_memory()[1]
    finally:
        tracemalloc.stop()


def _assert_long_string_windowed(document):
    # The OLD writer remains the byte oracle, including for the 10 MiB value.
    _assert_bytes(document)
    peak = _windowed_peak(document)
    assert run_json._STREAM_FAST_STR_LIMIT == 4096
    # The source slice, json.dumps result and interior slice are at most
    # three 4096-character windows. K is a fixed allowance for writer frames,
    # tracemalloc and allocator bookkeeping; it does not scale with the input.
    K = 64 * 1024
    assert peak <= 3 * 4096 + K


def test_long_dict_string_uses_windowed_memory():
    document = {
        "before": 17,
        "text": "x" * (10 * 1024 * 1024),
        "after": True,
        "empty": None,
    }
    _assert_long_string_windowed(document)


def test_long_record_array_string_uses_windowed_memory():
    _assert_long_string_windowed(_array(("x" * (10 * 1024 * 1024),)))


def test_fast_string_limit_boundary_write_count():
    limit = run_json._STREAM_FAST_STR_LIMIT
    first_size = limit // 2
    at_limit = {"first": "a" * first_size,
                "second": "b" * (limit - first_size)}
    over_limit = {"first": "a" * first_size,
                  "second": "b" * (limit - first_size + 1)}
    at_actual = _assert_bytes(at_limit)
    over_actual = _assert_bytes(over_limit)
    assert len(at_actual.calls) == 1
    assert len(over_actual.calls) > 1


def test_stream_batch_byte_bound_and_oversized_record():
    class CheckedDiscardDestination(_DiscardDestination):
        def __init__(self, expected):
            super().__init__()
            self.expected = expected
            self.total_bytes = 0

        def write(self, text):
            encoded = text.encode("ascii")
            end = self.total_bytes + len(encoded)
            # Compare every byte at its oracle offset, retaining no output.
            assert encoded == self.expected[self.total_bytes:end]
            self.total_bytes = end
            return super().write(text)

    # The fallback lets the count-only, pre-round-3 code reach the size
    # assertion rather than fail because the new constant does not exist.
    batch_bytes = getattr(run_json, "_STREAM_BATCH_BYTES", 256 * 1024)
    records = [{"text": chr(65 + index % 26) * (64 * 1024)}
               for index in range(64)]
    oversized = {"text": "z" * (2 * 1024 * 1024)}
    for rows in (records, [oversized],
                 [{"text": "before"}, oversized, {"text": "after"}]):
        document = _array(rows)
        reference, failure = _outcome(_stream_json, document)
        assert failure is None, failure
        expected = reference.bytes()
        destination = CheckedDiscardDestination(expected)
        run_json._stream_json(document, destination)
        assert not hasattr(destination, "_json_active")
        assert destination.total_bytes == len(expected)
        longest_record = max(
            len(json.dumps(row, separators=(",", ":"), allow_nan=False))
            for row in rows)
        # Before round 3, 64 < 512: the final flush joins all 4 MiB of
        # strings into one write, exceeding 256 KiB + one record's JSON.
        assert destination.longest_write <= batch_bytes + longest_record
        if longest_record > batch_bytes:
            # An oversized record is one write, including its leading comma
            # when present. Neighbours must each be written separately too.
            leading_comma = int(rows[0] is not oversized)
            assert destination.longest_write == longest_record + leading_comma
            assert destination.writes == len(rows) + 2


def _fixed_width_points(count):
    # All timestamps and integer-valued doubles have the same decimal width
    # throughout both populations. JSON record length is therefore constant;
    # the data still varies with every index and no list of rows is built.
    for index in range(count):
        yield {"time_ms": _START_MS + index, "equity": float(1000000 + index),
               "open_profit": -0.0}


def test_stream_batch_single_write_bound():
    lengths = [
        len(json.dumps(point, separators=(",", ":"), allow_nan=False))
        for point in _fixed_width_points(3)
    ]
    longest_record = max(lengths)
    maxima = []
    for count in (10000, 100000):
        destination = _DiscardDestination()
        document = {"equity_curve": _JSONRecordArray(
            lambda count=count: _fixed_width_points(count), count)}
        run_json._stream_json(document, destination)
        maxima.append(destination.longest_write)
    batch = run_json._STREAM_BATCH_RECORDS
    # C=batch is exactly one comma per record in a full noninitial batch;
    # the dict key/brackets are smaller than that batch. No output is kept.
    assert maxima[1] <= batch * longest_record + batch
    assert abs(maxima[1] - maxima[0]) <= longest_record + 1


def _stream_peak(count):
    rows = _fixed_width_points(count)
    document = {"equity_curve": _JSONRecordArray(lambda: rows, count)}
    destination = _DiscardDestination()
    # Start after constructing the generator and document; measure the same
    # dict -> _JSONRecordArray -> scalar-record route used for real reports.
    tracemalloc.start()
    try:
        run_json._stream_json(document, destination)
        return tracemalloc.get_traced_memory()[1]
    finally:
        tracemalloc.stop()


def test_stream_peak_is_independent_of_bar_count():
    record_bytes = max(len(json.dumps(point, separators=(",", ":"), allow_nan=False))
                       for point in _fixed_width_points(3))
    chunk_bytes = run_json._STREAM_BATCH_RECORDS * (record_bytes + 1)
    # K permits four extra joined-text chunks plus a fixed 64 KiB allowance
    # for encoder/frame/allocator bookkeeping. Both runs should hold the same
    # batch of strings, one joined chunk and one current row, independent of N.
    allowance = 4 * chunk_bytes + 64 * 1024
    small = _stream_peak(20000)
    large = _stream_peak(200000)
    assert large <= small + allowance
