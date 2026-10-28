"""Test support for the replay R/Q records. Nothing here is a production replay provider.

ANSWER.md:169-177,197-218,232-246; DELTA.md:60-68,104-110.
Expected operands below come from the fixed integer-price order schedule,
never from a report's scalar/marker. All success captures run a real module.
"""
from __future__ import annotations

import contextlib
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import types

import pf_sink
import run_json

ROOT = Path(__file__).resolve().parents[1]
START, MINUTE = 1736121600000, 60000
RECUT_FIELDS = ["qty", "commission", "pnl", "pnl_pct", "max_runup", "max_drawdown"]
R_FIELDS = (
    "n", "settlement_index", "parent_id", "slice_index", "slice_count", "recut_fields",
    "child_index", "transaction_qty", "whole_qty", "whole_pnl", "whole_commission",
    "whole_max_runup", "whole_max_drawdown", "whole_exit_fee", "transaction_fee",
)
Q_FIELDS = ("point_index", "time_ms", "settlement_count", "net_profit_sum",
            "equity_basis", "equity_term")
S_FIELDS = ("initial_capital", "pointvalue", "net_profit_sum")
MARKER_FIELDS = R_FIELDS[2:]
POINT_FIELDS = Q_FIELDS[2:]
PRIMARY_FLOATS = ("entry_price", "exit_price", "qty", "pnl", "pnl_pct",
                  "max_runup", "max_drawdown", "commission")


def bits(value):
    assert type(value) in (int, float), value
    return struct.pack(">d", float(value))


def same(actual, expected):
    assert bits(actual) == bits(expected), (actual, expected, bits(actual).hex(), bits(expected).hex())


def wire(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8") + b"\n"


def reject_constant(value):
    raise AssertionError("non-JSON numeric constant: " + value)


def load_line(value):
    return json.loads(value, parse_constant=reject_constant)


def csv_tape(path, shape):
    count = 46 if shape == "sealed" else 60 if shape == "quote" else 8
    lines = ["timestamp,open,high,low,close,volume\n"]
    for index in range(count):
        opening = high = low = close = 128
        if (shape == "sealed" and index == 27) or (
                shape in ("any", "fifo", "fifo_no_fees", "sort") and index == 3):
            high = 136
        if shape == "ordinary" and index == 2:
            opening = high = low = close = 136
        if shape == "ordinary" and index == count - 1:
            opening = high = low = close = 144
        if shape == "cancellation":
            large = 1 << 54
            opening = high = low = close = 1 if index < 2 else (
                large * 2 if index == 3 else large)
        lines.append(f"{START + index * MINUTE},{opening},{high},{low},{close},1\n")
    path.write_text("".join(lines), encoding="utf-8")
    return count


class ArrayLibrary:
    """Only hide the old primary-sink feature probe; every call remains native."""
    def __init__(self, library):
        self.library = library

    def __getattr__(self, name):
        if name in ("pf_report_sink_version", "strategy_set_report_sink_v1",
                    "strategy_report_sink_receipt_v1"):
            raise AttributeError(name)
        return getattr(self.library, name)


def invoke(tmp_path, monkeypatch, shape="fifo", *, route="sink", replay=True,
           companion=True, selected=False):
    """Invoke real main()/CDLL; retain OLD-state observations before real free."""
    tmp_path.mkdir(parents=True, exist_ok=True)
    module = Path(os.environ["PINEFORGE_REPLAY_RQ_LIBRARY"])
    assert module.is_file(), module
    chart = tmp_path / "chart.csv"
    count = csv_tape(chart, shape)
    primary, replay_path = tmp_path / "primary.ndjson", tmp_path / "replay.ndjson"
    argv = ["--so", str(module), "--ohlcv", str(chart), "--input-tf", "1",
            "--script-tf", "15" if shape == "quote" else "1",
            "--inputs", json.dumps({"shape": shape}), "--run-id", "replay-rq"]
    if shape == "quote":
        argv += ["--bar-magnifier", "true"]
    if replay:
        argv += ["--report-replay"]
        if companion:
            argv += ["--report-replay-sink", str(replay_path)]
    if route == "named":
        argv += ["--report-sink", str(primary)]
    if selected:
        inventory = tmp_path / "inventory.json"
        inventory.write_bytes(wire({
            "schema": "pineforge-request-feed-inventory/v1", "source_sha256": "0" * 64,
            "artifact_sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
            "primary_chart_timeframe": "1", "entries": [],
        }))
        argv += ["--report-policy", "selected-window/v1", "--window-start-ms", str(START),
                 "--window-end-ms", str(START + (count + 2) * MINUTE),
                 "--preroll-bars", "0", "--fed-start-ms", str(START),
                 "--request-feed-inventory", str(inventory),
                 "--overrides", '{"initial_capital":"16384"}']
    observations = []
    loader = run_json.load_strategy

    def native_load(path):
        lib = loader(path)
        observe = lib.replay_rq_observe
        observe.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        observe.restype = ctypes.c_uint64
        release = lib.strategy_free

        def observed_free(state):
            groups = int(observe(state, 0, 0))
            observations.append({
                "groups": groups,
                "parents": [int(observe(state, 1, i)) for i in range(groups)],
                "first": [int(observe(state, 2, i)) for i in range(groups)],
                "end": [int(observe(state, 3, i)) for i in range(groups)],
                "physical": [int(observe(state, 4, i)) for i in range(groups)],
                "base": int(observe(state, 5, 0)), "prefix": int(observe(state, 6, 0)),
                "sort_first": int(observe(state, 7, 0)),
            })
            release(state)
        lib.strategy_free = observed_free
        return ArrayLibrary(lib) if route == "array" else lib

    out, err = io.StringIO(), io.StringIO()
    with monkeypatch.context() as patch:
        patch.setattr(run_json, "load_strategy", native_load)
        patch.setattr(run_json, "time", types.SimpleNamespace(time=lambda: 1.0))
        patch.setenv("PINEFORGE_RUN_JSON_PROFILE", "unjailed")
        if selected:
            planner = Path(os.environ["PINEFORGE_REPLAY_RQ_PLANNER"])
            assert planner.is_file(), planner
            patch.setattr(run_json, "trusted_planner_path", lambda: str(planner))
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = run_json.main(argv)
    text = out.getvalue()
    assert len(text.splitlines()) == 1 and text.endswith("\n"), (status, text, err.getvalue())
    document = load_line(text)
    return types.SimpleNamespace(status=status, document=document, text=text,
                                 stderr=err.getvalue(), observations=observations,
                                 primary=primary, companion=replay_path, directory=tmp_path,
                                 replay_requested=replay, companion_requested=replay and companion)


def successful(result):
    assert result.status == 0, (result.document, result.stderr)
    assert len(result.observations) == 1, result.observations
    if result.replay_requested:
        assert "replay" in result.document
    if result.companion_requested:
        assert result.companion.is_file(), "the explicitly negotiated companion is missing"
        if "trades" in result.document:
            assert_companion_values(result)
    return result.observations[0]


def primary_records(path):
    with pf_sink.SinkReader(path) as reader:
        records = list(reader)
        assert reader.complete
        return records, reader


def replay_records(path, primary=None):
    # Deliberately looked up inside the call: collection/import must work on BASE.
    kwargs = {} if primary is None else {"primary_path": primary}
    with pf_sink.ReplaySinkReader(path, **kwargs) as reader:
        records = list(reader)
        assert reader.complete
        return records, reader


def rows_with_tag(records, tag):
    return [record.as_dict() for record in records if record.tag == tag]


def assert_companion_values(result):
    """Join every real JSON R/Q member to the real companion using bit identity."""
    records, _ = replay_records(result.companion)
    scalars = rows_with_tag(records, "S")
    assert len(scalars) == 1
    for field in S_FIELDS:
        same(scalars[0][field], result.document["replay"][field])
    rs, qs = rows_with_tag(records, "R"), rows_with_tag(records, "Q")
    assert len(rs) == len(result.document["trades"])
    assert len(qs) == len(result.document["equity_curve"])
    for trade, row in zip(result.document["trades"], rs):
        assert row["n"] == trade["n"] and row["settlement_index"] == trade["settlement_index"]
        if trade["recut"] is None:
            assert all(row[field] is None for field in MARKER_FIELDS)
        else:
            for field in MARKER_FIELDS:
                actual, expected = row[field], trade["recut"][field]
                if field in R_FIELDS[7:] and expected is not None:
                    same(actual, expected)
                else:
                    assert actual == expected
    for index, (point, row) in enumerate(zip(result.document["equity_curve"], qs)):
        assert row["point_index"] == index and row["time_ms"] == point["time_ms"]
        assert row["settlement_count"] == point["settlement_count"]
        assert row["equity_basis"] == point["equity_basis"]
        if point["net_profit_sum"] is None:
            assert row["net_profit_sum"] is None
        else:
            same(row["net_profit_sum"], point["net_profit_sum"])
        same(row["equity_term"], point["equity_term"])


def joined(result):
    """Independent transport join for assertions; never substitutes run_json."""
    base, primary = primary_records(result.primary)
    replay, companion = replay_records(result.companion, result.primary)
    trades, points = rows_with_tag(base, "T"), rows_with_tag(base, "E")
    r_rows, q_rows = rows_with_tag(replay, "R"), rows_with_tag(replay, "Q")
    scalars = rows_with_tag(replay, "S")
    assert len(scalars) == 1
    assert len(r_rows) == len(trades)
    assert len(q_rows) == len(points)
    for number, (trade, row) in enumerate(zip(trades, r_rows), 1):
        assert row["n"] == trade["n"] == number
        trade["settlement_index"] = row["settlement_index"]
        trade["recut"] = (None if row["parent_id"] is None
                          else {key: row[key] for key in MARKER_FIELDS})
    for index, (point, row) in enumerate(zip(points, q_rows)):
        assert row["point_index"] == index and row["time_ms"] == point["time_ms"]
        point.update({key: row[key] for key in POINT_FIELDS})
    assert companion.trailer["base_sha256"] == primary.sha256
    return {"replay": scalars[0], "trades": trades, "equity_curve": points}


def source(qty, entry=128, exit_price=136, fee=1):
    # All intermediate quantities are integers or dyadic fractions. No rounded
    # literals, producer helper, report total or reported P&L seeds this oracle.
    entry_fee = float(fee * qty)
    commission = float((fee * qty) + (fee * qty))
    gross = float((exit_price - entry) * qty)
    # engine_orders.cpp:206-213 reports excursions net of the ENTRY fee:
    # subtract it from favorable profit and add it to the adverse magnitude.
    # Physical pnl still subtracts BOTH entry and exit fees.
    return {"qty": float(qty), "entry_price": float(entry), "exit_price": float(exit_price),
            "pnl": gross - commission, "commission": commission,
            "max_runup": max(0.0, max(0.0, gross) - entry_fee),
            "max_drawdown": max(0.0, max(0.0, -gross) + entry_fee),
            "pnl_pct": ((gross - commission) / float(entry * qty)) * 100.0}


def sources(shape):
    if shape == "zero":
        return []
    if shape == "ordinary":
        return [source(1)]
    if shape == "quote":
        return [source(1, exit_price=128)]
    if shape == "sealed":
        return [source(1, exit_price=128) for _ in range(6)] + [
            source(4), source(2)] + [source(1, exit_price=128) for _ in range(3)]
    if shape == "two_groups":
        return [source(4), source(2), source(4, entry=64, exit_price=72),
                source(2, entry=64, exit_price=72)]
    if shape == "sort":
        return [source(1), source(4)]
    if shape == "cancellation":
        large = float(1 << 54)
        rows = [source(1, entry=entry, exit_price=large, fee=0.5)
                for entry in (1.0, large, large * 2.0)]
        rows[0]["max_runup"] = max(0.0, ((large * 2.0) - 1.0) - 0.5)
        rows[1]["max_runup"] = max(0.0, ((large * 2.0) - large) - 0.5)
        rows[2]["max_drawdown"] = ((large * 2.0) - large) + 0.5
        return rows
    fee = 0 if shape == "fifo_no_fees" else 1
    return [source(4, fee=fee), source(2, fee=fee)]


def assert_exact(document, shape, *, capital=10000.0):
    """C1 independent scenario oracle AND settlement-operand fold; no tolerance."""
    expected = sources(shape)
    same(document["replay"]["initial_capital"], capital)
    same(document["replay"]["pointvalue"], 1.0)
    originals, parents = {}, {}
    for row in document["trades"]:
        index, marker = row["settlement_index"], row["recut"]
        if row["open_at_end"]:
            assert index is None and marker is None
            continue
        assert type(index) is int and 0 <= index < len(expected)
        wanted = expected[index]
        if marker is None:
            pnl = row["pnl"]
            for key in PRIMARY_FLOATS:
                same(row[key], wanted[key])
        else:
            assert set(marker) == set(MARKER_FIELDS)
            parent = marker["parent_id"]
            assert type(parent) is str and parent.isascii() and parent.isdecimal()
            assert str(int(parent)) == parent and 0 <= int(parent) <= (1 << 64) - 1
            assert type(marker["slice_index"]) is int
            assert type(marker["slice_count"]) is int and marker["slice_count"] > 0
            assert marker["recut_fields"] == (
                [] if shape in ("any", "cancellation") else RECUT_FIELDS)
            for key in ("qty", "pnl", "commission", "max_runup", "max_drawdown"):
                same(marker["whole_" + key], wanted[key])
            pnl = marker["whole_pnl"]
            parents.setdefault(parent, []).append(marker)
            signature = tuple(bits(marker[key]) for key in
                              ("whole_qty", "whole_pnl", "whole_commission",
                               "whole_max_runup", "whole_max_drawdown"))
            if index in originals:
                assert originals[index][1] == signature
            originals[index] = (pnl, signature)
        same(pnl, wanted["pnl"])
        if marker is None:
            assert index not in originals
            originals[index] = (pnl, None)
    assert sorted(originals) == list(range(len(expected))), originals
    for rows in parents.values():
        assert {row["slice_count"] for row in rows} == {len(rows)}
        assert sorted(row["slice_index"] for row in rows) == list(range(len(rows)))
    # Two independent folds: expected schedule operands and real retained R10.
    reference, replayed = [0.0], [0.0]
    for index, wanted in enumerate(expected):
        reference.append(reference[-1] + wanted["pnl"])
        replayed.append(replayed[-1] + originals[index][0])
        same(replayed[-1], reference[-1])
    same(document["replay"]["net_profit_sum"], reference[-1])
    same(document["replay"]["net_profit_sum"], replayed[-1])
    assert bits(document["replay"]["net_profit_sum"]) != bytes.fromhex("8000000000000000")
    for point in document["equity_curve"]:
        basis = point["equity_basis"]
        assert basis in {"kernel_open", "range_end", "terminal_quote", "anchor"}
        if basis == "anchor":
            assert point["settlement_count"] is None and point["net_profit_sum"] is None
            same(point["equity_term"], 0.0)
            same(point["equity"], capital)
            continue
        cutoff = point["settlement_count"]
        assert type(cutoff) is int and 0 <= cutoff <= len(expected)
        assert point["net_profit_sum"] is not None
        same(point["net_profit_sum"], reference[cutoff])
        same(point["net_profit_sum"], replayed[cutoff])
        term = point["equity_term"]
        # Match the producer's parenthesization, not subtraction from equity.
        equity = capital + term if basis == "terminal_quote" else (
            capital + reference[cutoff]) + term
        same(point["equity"], equity)
    return reference


def assert_q_schedule(document, shape, *, selected_anchor=False):
    """Independent cutoffs and terms, so all-zero Q history cannot pass C1."""
    close_bars = {
        "zero": [], "any": [3, 3], "fifo": [3, 3], "fifo_no_fees": [3, 3],
        "sort": [3, 3], "ordinary": [2], "quote": [30],
        "two_groups": [3, 3, 3, 3], "cancellation": [4, 4, 4],
        "sealed": [2, 6, 10, 14, 18, 22, 27, 27, 34, 38, 42],
    }[shape]
    points = document["equity_curve"]
    expected_minutes = list(range(0, 60, 15)) if shape == "quote" else list(
        range(46 if shape == "sealed" else 8))
    if shape == "quote":
        expected_minutes[-1] = 60  # Quote retimes the minute-45 completed mark.
    if selected_anchor:
        expected_minutes.insert(0, 0)
        assert points[0]["equity_basis"] == "anchor"
    assert [point["time_ms"] for point in points] == [
        START + minute * MINUTE for minute in expected_minutes]
    for index, point in enumerate(points):
        if point["equity_basis"] == "anchor":
            assert selected_anchor and index == 0, "only the synthetic first point is an anchor"
            continue
        bar = (point["time_ms"] - START) // MINUTE
        assert point["settlement_count"] == len([at for at in close_bars if at <= bar])
        terminal = index == len(points) - 1 and shape in ("ordinary", "quote")
        assert point["equity_basis"] == (
            "terminal_quote" if terminal and shape == "quote"
            else "range_end" if terminal else "kernel_open")
        term = ((144.0 - 128.0) - (1.0 + 1.0)) if terminal else 0.0
        if terminal and shape == "quote":
            term = ((128.0 - 128.0) - (1.0 + 1.0)) + term
        if shape == "cancellation":
            large = float(1 << 54)
            two_lot_average = (1.0 + large) / 2.0
            if bar == 2:
                term = (large - two_lot_average) * 2.0
            if bar == 3:
                three_lot_average = ((two_lot_average * 2.0) + (large * 2.0)) / 3.0
                term = ((large * 2.0) - three_lot_average) * 3.0
        same(point["equity_term"], term)
