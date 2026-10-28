"""Replay R/Q engine integration runs; no canned ReportC or replay dictionaries.

Citations refer to the delivered ANSWER.md, DELTA.md and DELTA-2.md.
See tests/replay_rq_fixture.hpp for the base paths, eligibility facts and guards.
CTest supplies the compiled fixture and planner; source-only preflight cannot.
"""
from __future__ import annotations

import copy
import math

import pytest

import replay_rq_support as rq
import pf_sink

FINITE_FIELDS = [
    ("R", "transaction_qty", "trades.recut.transaction_qty"),
    ("R", "whole_qty", "trades.recut.whole_qty"),
    ("R", "whole_pnl", "trades.recut.whole_pnl"),
    ("R", "whole_commission", "trades.recut.whole_commission"),
    ("R", "whole_max_runup", "trades.recut.whole_max_runup"),
    ("R", "whole_max_drawdown", "trades.recut.whole_max_drawdown"),
    ("R", "whole_exit_fee", "trades.recut.whole_exit_fee"),
    ("R", "transaction_fee", "trades.recut.transaction_fee"),
    ("Q", "net_profit_sum", "equity_curve.net_profit_sum"),
    ("Q", "equity_term", "equity_curve.equity_term"),
]


def assert_fifo(document, parent, expected, *, first=0, fee=1):
    rows = [row for row in document["trades"]
            if row["recut"] is not None and row["recut"]["parent_id"] == str(parent)]
    rows.sort(key=lambda row: row["recut"]["slice_index"])
    # Independent intersection walk: original source order, child order [1,0].
    wanted_slices, lot, left = [], 0, expected[0]["qty"]
    for child in (1, 0):
        transaction = expected[child]["qty"]
        wanted = transaction
        while wanted > 0:
            quantity = min(left, wanted)
            wanted_slices.append((first + lot, child, transaction, quantity))
            left = left - quantity
            wanted = wanted - quantity
            if left == 0:
                lot += 1
                if lot < len(expected):
                    left = expected[lot]["qty"]
    assert lot == len(expected)
    assert len(rows) == len(wanted_slices) == 3
    for index, (row, (settlement, child, transaction, quantity)) in enumerate(
            zip(rows, wanted_slices)):
        marker, whole = row["recut"], expected[settlement - first]
        assert row["settlement_index"] == settlement
        assert marker["slice_index"] == index and marker["slice_count"] == len(rows)
        assert marker["parent_id"] == str(parent)
        assert marker["recut_fields"] == rq.RECUT_FIELDS
        assert type(marker["child_index"]) is int and marker["child_index"] == child
        rq.same(marker["transaction_qty"], transaction)
        rq.same(marker["whole_qty"], whole["qty"])
        for field in ("pnl", "commission", "max_runup", "max_drawdown"):
            rq.same(marker["whole_" + field], whole[field])
        if fee:
            rq.same(marker["whole_exit_fee"], whole["qty"] * fee)
            rq.same(marker["transaction_fee"], transaction * fee)
        else:
            assert marker["whole_exit_fee"] is None and marker["transaction_fee"] is None
        # Sig10 leaves EVERY operand here unchanged: integers and .25/.5/.75
        # shares only. This oracle never calls the producer's round helper.
        share = quantity / whole["qty"]
        entry_part = (whole["commission"] - whole["qty"] * fee) * share
        exit_part = ((transaction * fee) * quantity) / transaction
        commission = entry_part + exit_part
        pnl = ((whole["pnl"] + whole["commission"]) * share) - commission
        rq.same(row["qty"], quantity)
        rq.same(row["commission"], commission)
        rq.same(row["pnl"], pnl)
        rq.same(row["pnl_pct"], (pnl / ((whole["entry_price"] * quantity) * 1.0)) * 100.0)
        rq.same(row["max_runup"], whole["max_runup"] * share)
        rq.same(row["max_drawdown"], whole["max_drawdown"] * share)


@pytest.mark.parametrize("route", ["array", "sink"])
def test_c2a_any_permutation_has_empty_marker_and_bit_identical_source_values(
        tmp_path, monkeypatch, route):
    """DELTA:60-68,127-136; ANSWER:197,202-214; C1 DELTA:104-110."""
    result = rq.invoke(tmp_path, monkeypatch, "any", route=route)
    observation = rq.successful(result)
    document = result.document
    assert observation["groups"] == 1 and observation["physical"] == [2]
    rows = document["trades"]
    assert len(rows) == 2
    assert [row["settlement_index"] for row in rows] == [1, 0]
    expected = rq.sources("any")
    for slice_index, (row, source_index) in enumerate(zip(rows, (1, 0))):
        marker = row["recut"]
        assert marker["parent_id"] == str(observation["parents"][0])
        assert marker["slice_index"] == slice_index and marker["slice_count"] == 2
        assert marker["recut_fields"] == [] and marker["child_index"] == source_index
        assert marker["whole_exit_fee"] is None and marker["transaction_fee"] is None
        rq.same(marker["transaction_qty"], expected[source_index]["qty"])
        for field in rq.PRIMARY_FLOATS:
            rq.same(row[field], expected[source_index][field])
        assert row["side"] == "long" and row["open_at_end"] is False
        assert row["entry_bar_index"] == source_index + 1 and row["exit_bar_index"] == 3
        assert row["entry_time"] == rq.START + (source_index + 1) * rq.MINUTE
        assert row["exit_time"] == rq.START + 3 * rq.MINUTE
        assert type(row["entry_incarnation"]) is int and row["entry_incarnation"] > 0
    assert rows[0]["entry_incarnation"] != rows[1]["entry_incarnation"]
    rq.assert_exact(document, "any")
    rq.assert_q_schedule(document, "any")


@pytest.mark.parametrize("route", ["array", "sink"])
def test_c2b_fifo_three_slices_retain_whole_operands_and_enabled_fee_quotes(
        tmp_path, monkeypatch, route):
    """DELTA:138-158; ANSWER:197,202-214,241-245; C1 DELTA:104-110."""
    result = rq.invoke(tmp_path, monkeypatch, "fifo", route=route)
    observation = rq.successful(result)
    assert observation["groups"] == 1 and observation["physical"] == [2]
    assert_fifo(result.document, observation["parents"][0], rq.sources("fifo"))
    assert [row["settlement_index"] for row in result.document["trades"]] == [0, 0, 1]
    rq.assert_exact(result.document, "fifo")
    rq.assert_q_schedule(result.document, "fifo")


def test_c2c_two_recut_groups_in_one_bar_have_disjoint_settlements_and_parents(
        tmp_path, monkeypatch):
    """DELTA:160-166,62,66; C1 DELTA:104-110. Real C ABI tick lifecycle."""
    result = rq.invoke(tmp_path, monkeypatch, "two_groups", route="array")
    observation = rq.successful(result)
    assert observation["groups"] == 2 and observation["physical"] == [2, 2]
    first, second = observation["parents"]
    assert first != second
    rows = result.document["trades"]
    assert len(rows) == 6
    assert {row["exit_bar_index"] for row in rows} == {3}
    assert {row["exit_time"] // rq.MINUTE for row in rows} == {
        (rq.START + 3 * rq.MINUTE) // rq.MINUTE}
    expected = rq.sources("two_groups")
    assert_fifo(result.document, first, expected[:2])
    assert_fifo(result.document, second, expected[2:], first=2)
    groups = {}
    for row in rows:
        groups.setdefault(row["recut"]["parent_id"], set()).add(row["settlement_index"])
    assert groups == {str(first): {0, 1}, str(second): {2, 3}}
    rq.assert_exact(result.document, "two_groups")
    rq.assert_q_schedule(result.document, "two_groups")


def test_c2d_sealed_retired_group_is_replayed_from_companion_with_stable_global_rows(
        tmp_path, monkeypatch):
    """DELTA:68,88-89,168-172; DELTA-2:73,78-80; ANSWER:217-218."""
    before = rq.invoke(tmp_path / "resident", monkeypatch, "sealed", route="array")
    resident = rq.successful(before)
    assert resident["base"] == resident["prefix"] == 0
    rq.assert_exact(before.document, "sealed")
    rq.assert_q_schedule(before.document, "sealed")
    streamed = rq.invoke(tmp_path / "streamed", monkeypatch, "sealed", route="named")
    retired = rq.successful(streamed)
    assert retired["physical"] == [2] and retired["first"] == [6] and retired["end"] == [9]
    assert retired["base"] == retired["prefix"] and retired["base"] >= retired["end"][0]
    # Read R from the negotiated stream after destruction of the real handle.
    document = rq.joined(streamed)
    assert len(document["trades"]) == 6 + 3 + 3
    assert len(before.document["trades"]) == len(document["trades"])
    assert [row["n"] for row in document["trades"]] == list(range(1, 13))
    assert_fifo(document, retired["parents"][0], rq.sources("sealed")[6:8], first=6)
    for left, right in zip(before.document["trades"], document["trades"]):
        assert left["n"] == right["n"]
        assert left["settlement_index"] == right["settlement_index"]
        assert left["recut"] == right["recut"]
        for field in rq.PRIMARY_FLOATS:
            rq.same(left[field], right[field])
    rq.assert_exact(document, "sealed")
    rq.assert_q_schedule(document, "sealed")


def test_marker_moves_with_distinguishable_slices_through_real_same_bar_sort(
        tmp_path, monkeypatch):
    """DELTA:68,86-87. Existing phase-key injection forces real sort cycles."""
    result = rq.invoke(tmp_path, monkeypatch, "sort", route="array")
    observation = rq.successful(result)
    assert observation["groups"] == 1 and observation["sort_first"] == 1
    rows = result.document["trades"]
    assert [row["recut"]["slice_index"] for row in rows] == [0, 2, 1]
    assert [row["qty"] for row in rows] == [1.0, 1.0, 3.0]
    assert [row["recut"]["child_index"] for row in rows] == [1, 0, 1]
    assert_fifo(result.document, observation["parents"][0], rq.sources("sort"))
    rq.assert_exact(result.document, "sort")
    rq.assert_q_schedule(result.document, "sort")


def test_fifo_without_fees_still_is_money_recut_and_has_null_fee_operands(tmp_path, monkeypatch):
    """DELTA:64,154; ANSWER:213-214. Branch semantics, not value-change guessing."""
    result = rq.invoke(tmp_path, monkeypatch, "fifo_no_fees")
    observation = rq.successful(result)
    assert_fifo(result.document, observation["parents"][0], rq.sources("fifo_no_fees"), fee=0)
    rq.assert_exact(result.document, "fifo_no_fees")
    rq.assert_q_schedule(result.document, "fifo_no_fees")


@pytest.mark.parametrize("route", ["array", "sink"])
def test_unamended_and_terminal_rows_have_neutral_markers_and_terminal_equity_operand(
        tmp_path, monkeypatch, route):
    """ANSWER:197-201,239; DELTA:60,66,108. Terminal P&L never enters N."""
    result = rq.invoke(tmp_path, monkeypatch, "ordinary", route=route)
    rq.successful(result)
    rows = result.document["trades"]
    assert len(rows) == 2
    assert [row["settlement_index"] for row in rows] == [0, None]
    assert [row["recut"] for row in rows] == [None, None]
    assert [row["open_at_end"] for row in rows] == [False, True]
    rq.same(rows[1]["pnl"], (144.0 - 128.0) - (1.0 + 1.0))
    rq.assert_exact(result.document, "ordinary")
    rq.assert_q_schedule(result.document, "ordinary")
    last = result.document["equity_curve"][-1]
    assert last["equity_basis"] == "range_end"
    rq.same(last["open_profit"], 0.0)
    assert rq.bits(last["equity"]) != rq.bits(10000.0 + result.document["replay"]["net_profit_sum"])


@pytest.mark.parametrize("route", ["array", "sink"])
def test_nondyadic_unamended_pnl_is_the_exact_settlement_operand(
        tmp_path, monkeypatch, route):
    # Closes: 3 @ 100.7 -> 100.9, 5 @ 101.7 -> 101.9, 13 @ 102.9 -> 102.7.
    # These decimal tenths survive tick snapping unchanged but are not binary
    # exact; 13 * 0.07 also rounds in binary64. Sig10 changes all three P&Ls
    # and their final fold, so rounded report operands cannot pass this vector.
    result = rq.invoke(tmp_path, monkeypatch, "nondyadic", route=route)
    observation = rq.successful(result)
    assert observation["groups"] == 0 and observation["physical"] == []
    document = result.document
    rows = document["trades"]
    assert len(rows) == 4
    assert [row["settlement_index"] for row in rows] == [0, 1, 2, None]
    assert [row["open_at_end"] for row in rows] == [False, False, False, True]
    assert all(row["recut"] is None for row in rows)
    expected = rq.sources("nondyadic")
    terminal = rq.nondyadic_source(1, 103.7, 104.9)
    for row, wanted in zip(rows, expected + [terminal]):
        for field in ("entry_price", "exit_price", "qty", "commission", "pnl"):
            rq.same(row[field], wanted[field])
    assert [row["entry_bar_index"] for row in rows] == [1, 3, 5, 7]
    assert [row["exit_bar_index"] for row in rows] == [2, 4, 6, 8]
    rq.assert_exact(document, "nondyadic")
    rq.assert_q_schedule(document, "nondyadic")

    presented = [+0.0]
    for row in rows[:3]:
        presented.append(presented[-1] + row["pnl"])
    total = document["replay"]["net_profit_sum"]
    rq.same(presented[-1], total)
    assert rq.bits(presented[-1] + rows[3]["pnl"]) != rq.bits(total)

    records, _ = rq.replay_records(result.companion)
    scalars = rq.rows_with_tag(records, "S")
    assert len(scalars) == 1
    rq.same(scalars[0]["net_profit_sum"], presented[-1])
    replay_rows = rq.rows_with_tag(records, "R")
    assert len(replay_rows) == 4
    for trade, row in zip(rows, replay_rows):
        assert row["n"] == trade["n"]
        assert row["settlement_index"] == trade["settlement_index"]
        assert all(row[field] is None for field in rq.MARKER_FIELDS)
    points = rq.rows_with_tag(records, "Q")
    assert len(points) == len(document["equity_curve"])
    for point in document["equity_curve"] + points:
        if point["net_profit_sum"] is not None:
            cutoff = point["settlement_count"]
            assert type(cutoff) is int and 0 <= cutoff <= 3
            rq.same(point["net_profit_sum"], presented[cutoff])

    def sig10(value):
        return float("%.10g" % value)

    # Negative control: precision is deliberately lost only in this fold.
    rounded = [+0.0]
    for row in rows[:3]:
        rounded.append(rounded[-1] + sig10(row["pnl"]))
    assert any(rq.bits(sig10(row["pnl"])) != rq.bits(row["pnl"]) for row in rows[:3])
    assert rq.bits(rounded[-1]) != rq.bits(total)


@pytest.mark.parametrize("route", ["array", "sink"])
def test_terminal_quote_basis_uses_pre_capital_report_operand_not_raw_n(tmp_path, monkeypatch, route):
    """ANSWER:200-201,239,248: distinct terminal_quote formula and retimed Q2."""
    result = rq.invoke(tmp_path, monkeypatch, "quote", route=route)
    rq.successful(result)
    assert len(result.document["trades"]) == 2
    terminal = result.document["trades"][-1]
    assert terminal["open_at_end"] is True and terminal["settlement_index"] is None
    rq.same(terminal["exit_price"], 144.0)
    rq.assert_exact(result.document, "quote")
    rq.assert_q_schedule(result.document, "quote")
    point = result.document["equity_curve"][-1]
    assert point["time_ms"] == rq.START + 60 * rq.MINUTE
    assert point["equity_basis"] == "terminal_quote"
    assert rq.bits(point["equity_term"]) != rq.bits(point["net_profit_sum"])
    records, _ = rq.replay_records(result.companion)
    q = rq.rows_with_tag(records, "Q")[-1]
    assert q["time_ms"] == point["time_ms"]
    assert q["settlement_count"] == point["settlement_count"]
    assert q["equity_basis"] == point["equity_basis"]
    rq.same(q["net_profit_sum"], point["net_profit_sum"])
    rq.same(q["equity_term"], point["equity_term"])


@pytest.mark.parametrize("route", ["array", "sink"])
def test_zero_trade_q_records_and_s3_have_positive_zero_bits(tmp_path, monkeypatch, route):
    """DELTA:108; DELTA-2:35,41,60. Check parsed bits before any re-encoding."""
    result = rq.invoke(tmp_path, monkeypatch, "zero", route=route)
    rq.successful(result)
    assert result.document["trades"] == []
    rq.assert_exact(result.document, "zero")
    rq.assert_q_schedule(result.document, "zero")
    for point in result.document["equity_curve"]:
        assert point["settlement_count"] == 0
        rq.same(point["net_profit_sum"], 0.0)


@pytest.mark.parametrize("route", ["array", "sink"])
def test_selected_anchor_null_history_and_duplicate_time_q_join_use_point_index(
        tmp_path, monkeypatch, route):
    """ANSWER:198-201,215-216; DELTA:108,115. Effective selected capital."""
    result = rq.invoke(tmp_path, monkeypatch, "zero", route=route, selected=True)
    rq.successful(result)
    points = result.document["equity_curve"]
    assert len(points) == 8 + 1
    assert points[0]["time_ms"] == points[1]["time_ms"] == rq.START
    assert points[0]["equity_basis"] == "anchor"
    assert points[0]["settlement_count"] is None and points[0]["net_profit_sum"] is None
    assert points[1]["equity_basis"] == "kernel_open" and points[1]["settlement_count"] == 0
    rq.assert_exact(result.document, "zero", capital=16384.0)
    rq.assert_q_schedule(result.document, "zero", selected_anchor=True)
    records, _ = rq.replay_records(result.companion)
    qs = rq.rows_with_tag(records, "Q")
    assert len(qs) == len(points)
    assert [row["point_index"] for row in qs] == list(range(len(points)))
    for point, row in zip(points, qs):
        assert row["time_ms"] == point["time_ms"]
        assert {key: row[key] for key in rq.POINT_FIELDS} == {
            key: point[key] for key in rq.POINT_FIELDS}


def test_array_and_sink_json_pass_through_every_rq_member_from_the_real_run(tmp_path, monkeypatch):
    """ANSWER JT/JQ:184-185, PC:186; opt-in joins, not dict-builder echoes."""
    array = rq.invoke(tmp_path / "array", monkeypatch, route="array")
    sink = rq.invoke(tmp_path / "sink", monkeypatch, route="sink")
    rq.successful(array)
    rq.successful(sink)
    for result in (array, sink):
        rq.assert_exact(result.document, "fifo")
        rq.assert_q_schedule(result.document, "fifo")
    assert array.document["trades"] == sink.document["trades"]
    assert array.document["equity_curve"] == sink.document["equity_curve"]
    records, _ = rq.replay_records(sink.companion)
    rs, qs = rq.rows_with_tag(records, "R"), rq.rows_with_tag(records, "Q")
    assert len(rs) == 3 and len(qs) == 8
    for trade, row in zip(sink.document["trades"], rs):
        assert trade["n"] == row["n"] and trade["settlement_index"] == row["settlement_index"]
        assert trade["recut"] == {key: row[key] for key in rq.MARKER_FIELDS}
    for point, row in zip(sink.document["equity_curve"], qs):
        assert {key: point[key] for key in rq.POINT_FIELDS} == {
            key: row[key] for key in rq.POINT_FIELDS}


@pytest.mark.parametrize("route", ["array", "sink"])
def test_exact_replay_uses_booking_order_when_any_report_order_changes_the_sum(
        tmp_path, monkeypatch, route):
    """DELTA:106,110,117; DELTA-2:35. Kill a report-sum substitute for S3."""
    result = rq.invoke(tmp_path, monkeypatch, "cancellation", route=route)
    observation = rq.successful(result)
    assert observation["physical"] == [3] and observation["groups"] == 1
    rows = result.document["trades"]
    assert [row["settlement_index"] for row in rows] == [2, 0, 1]
    expected = rq.sources("cancellation")
    for row in rows:
        marker = row["recut"]
        source = expected[row["settlement_index"]]
        assert marker["parent_id"] == str(observation["parents"][0])
        assert marker["recut_fields"] == []
        assert marker["whole_exit_fee"] is None and marker["transaction_fee"] is None
        for field in rq.PRIMARY_FLOATS:
            rq.same(row[field], source[field])
    rq.assert_exact(result.document, "cancellation", capital=float(1 << 60))
    rq.assert_q_schedule(result.document, "cancellation")
    presented = 0.0
    for row in rows:
        presented = presented + row["pnl"]
    assert rq.bits(presented) != rq.bits(result.document["replay"]["net_profit_sum"])


@pytest.mark.parametrize("tag,field,label", FINITE_FIELDS)
@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf], ids=["nan", "inf", "negative-inf"])
def test_sink_join_rejects_nonfinite_rq_values_after_a_real_native_capture(
        tmp_path, monkeypatch, tag, field, label, bad):
    """ANSWER JT/JQ finite validation:184-185. Fault only after real decoding."""
    original_next = pf_sink.ReplaySinkReader.__next__
    injected = []

    def corrupted_next(reader):
        record = original_next(reader)
        if record.tag == tag:
            values = list(record.values)
            values[record.fields.index(field)] = bad
            injected.append(record.tag)
            return record._replace(values=tuple(values))
        return record

    monkeypatch.setattr(pf_sink.ReplaySinkReader, "__next__", corrupted_next)
    result = rq.invoke(tmp_path, monkeypatch, route="sink")
    assert injected, "real companion was not decoded; the fault path was not exercised"
    assert result.status == 1
    assert result.document["code"] == "report_result_invalid"
    assert result.document["args"] == {"field": label, "reason": "nonfinite"}


@pytest.mark.parametrize("tag,field,label", FINITE_FIELDS)
@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf], ids=["nan", "inf", "negative-inf"])
def test_array_validation_rejects_nonfinite_rq_members_after_real_engine_assembly(
        tmp_path, monkeypatch, tag, field, label, bad):
    """ANSWER:184-185. Fault the native-built array, not an input echo fixture."""
    original_builder = rq.run_json.build_report_dict
    injected = []

    def corrupted_builder(*args, **kwargs):
        document = original_builder(*args, **kwargs)
        key = "trades" if tag == "R" else "equity_curve"
        rows = [copy.deepcopy(row) for row in document[key]]
        assert rows, "native array capture is empty"
        target = rows[0]["recut"] if tag == "R" else rows[0]
        assert type(target) is dict, "required replay marker is missing"
        assert field in target, "required replay member is missing"
        assert math.isfinite(target[field]), "the healthy engine operand was not finite"
        target[field] = bad
        document[key] = rows
        injected.append(field)
        return document

    monkeypatch.setattr(rq.run_json, "build_report_dict", corrupted_builder)
    result = rq.invoke(tmp_path, monkeypatch, route="array", companion=False)
    assert injected == [field], "real array assembly was not exercised exactly once"
    assert result.status == 1
    assert result.document["code"] == "report_result_invalid"
    assert result.document["args"] == {"field": label, "reason": "nonfinite"}


def test_guard_extension_off_negotiates_no_companion_or_annotated_fields(tmp_path, monkeypatch):
    """GUARD, passes on BASE: DELTA:207-212. Not red-first feature evidence."""
    result = rq.invoke(tmp_path, monkeypatch, route="named", replay=False)
    observation = rq.successful(result)
    assert observation["groups"] == 1
    assert "replay" not in result.document
    assert not result.companion.exists()
    records, reader = rq.primary_records(result.primary)
    assert set(reader.counts) == {"E", "T"}
    assert {record.tag for record in records} == {"E", "T"}
    descriptors = {row["type"]: row for row in reader.header["records"]}
    assert descriptors["E"]["fields"] == ["time_ms", "equity", "open_profit"]
    assert descriptors["T"]["fields"] == [
        "n", "side", "entry_time", "exit_time", "entry_price", "exit_price", "qty",
        "pnl", "pnl_pct", "max_runup", "max_drawdown", "commission", "entry_bar_index",
        "exit_bar_index", "open_at_end", "entry_incarnation", "close_cause",
        "entry_id", "exit_id", "exit_comment"]
    plain = rq.invoke(tmp_path / "default", monkeypatch, replay=False)
    rq.successful(plain)
    assert "replay" not in plain.document
    for trade in plain.document["trades"]:
        assert "settlement_index" not in trade and "recut" not in trade
    for point in plain.document["equity_curve"]:
        assert not set(rq.POINT_FIELDS).intersection(point)


def test_guard_seal_fixture_retires_the_entire_affected_group_without_replay(tmp_path, monkeypatch):
    """GUARD, passes on BASE: DELTA:170. Proves C2(d)'s eligibility without R."""
    result = rq.invoke(tmp_path, monkeypatch, "sealed", route="named", replay=False)
    observation = rq.successful(result)
    assert observation["first"] == [6] and observation["end"] == [9]
    assert observation["base"] == observation["prefix"] and observation["base"] >= 9
    records, _ = rq.primary_records(result.primary)
    rows = rq.rows_with_tag(records, "T")
    assert len(rows) == 12
    assert [row["qty"] for row in rows[6:9]] == [2.0, 2.0, 2.0]
