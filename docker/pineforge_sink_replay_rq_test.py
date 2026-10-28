"""Replay companion R/Q transport and fail-closed exact replay.

Every valid input originates in the CMake-built real strategy. Negative wire
tests mutate that capture and repair the receipt unless the receipt itself
is under test; a synthetic header cannot conceal a broken native producer.
"""
from __future__ import annotations

import copy
import hashlib

import pytest

import replay_rq_support as rq
import pf_sink


def capture(tmp_path, monkeypatch, shape="fifo", *, selected=False):
    result = rq.invoke(tmp_path, monkeypatch, shape, route="named", selected=selected)
    rq.successful(result)
    healthy = rq.joined(result)
    rq.assert_exact(healthy, shape, capital=16384.0 if selected else 10000.0)
    rq.assert_q_schedule(healthy, shape, selected_anchor=selected)
    return result


def objects(path):
    return [rq.load_line(line) for line in path.read_bytes().splitlines()]


def emit_mutation(path, values):
    """Test wire writer seam; uses the REAL emitted descriptors and S/Q/R."""
    values = copy.deepcopy(values)
    counts = {descriptor["type"]: 0 for descriptor in values[0]["records"]}
    for row in values[1:-1]:
        counts[row[0]] += 1
    payload = b"".join(rq.wire(value) for value in values[:-1])
    values[-1]["counts"] = counts
    values[-1]["sha256"] = hashlib.sha256(payload).hexdigest()
    path.write_bytes(payload + rq.wire(values[-1]))
    return path


def first(values, tag):
    return next(row for row in values[1:-1] if row[0] == tag)


def typed_error(path, code):
    with pytest.raises(pf_sink.ReplaySinkFormatError) as caught:
        rq.replay_records(path)
    assert caught.value.code == code


def unavailable(result, reason):
    # This is the PRODUCTION reader/validator's verdict, not a test fallback.
    with pytest.raises(pf_sink.ReplaySinkFormatError) as caught:
        rq.replay_records(result.companion, result.primary)
    assert caught.value.code == "exact_replay_unavailable"
    assert caught.value.code_args["reason"] == reason
    assert "exact replay unavailable" in str(caught.value).lower()


def test_companion_rq_descriptors_rows_receipt_and_primary_binding_from_real_run(
        tmp_path, monkeypatch):
    """ANSWER:169-177,187,215-218,246; DELTA-2:73,80."""
    result = capture(tmp_path, monkeypatch)
    records, reader = rq.replay_records(result.companion, result.primary)
    values = objects(result.companion)
    assert values[0]["schema"] == "pf-report-replay" and values[0]["version"] == 1
    descriptors = {row["type"]: row for row in reader.header["records"]}
    assert set(descriptors) == {"S", "R", "Q"}
    assert descriptors["R"]["fields"] == list(rq.R_FIELDS)
    assert descriptors["Q"]["fields"] == list(rq.Q_FIELDS)
    assert descriptors["S"]["fields"] == list(rq.S_FIELDS)
    assert descriptors["R"]["types"] == [
        "i64", "i64?", "str?", "i64?", "i64?", "str[]?", "i64?",
        "f64?", "f64?", "f64?", "f64?", "f64?", "f64?", "f64?", "f64?"]
    assert descriptors["Q"]["types"] == ["i64", "i64", "i64?", "f64?", "str", "f64"]
    assert descriptors["R"]["units"] == [
        "", "", "", "", "", "", "", "qty", "qty", "account_ccy", "account_ccy",
        "account_ccy", "account_ccy", "account_ccy", "account_ccy"]
    assert descriptors["Q"]["units"] == ["", "unix_ms", "", "account_ccy", "", "account_ccy"]
    assert reader.counts == {"S": 1, "Q": 8, "R": 3}
    assert reader.trailer["counts"] == reader.counts
    raw = result.companion.read_bytes().splitlines(keepends=True)
    assert all(line.endswith(b"\n") for line in raw)
    assert reader.sha256 == hashlib.sha256(b"".join(raw[:-1])).hexdigest()
    primary, primary_reader = rq.primary_records(result.primary)
    assert reader.trailer["base_sha256"] == primary_reader.sha256
    assert len(rq.rows_with_tag(records, "R")) == len(rq.rows_with_tag(primary, "T")) == 3
    assert len(rq.rows_with_tag(records, "Q")) == len(rq.rows_with_tag(primary, "E")) == 8
    rq.assert_exact(rq.joined(result), "fifo")
    rq.assert_q_schedule(rq.joined(result), "fifo")


def test_unamended_r_null_tail_terminal_r2_null_and_q_terminal_operand(tmp_path, monkeypatch):
    """ANSWER:175-177,197-201. R3..R15 null, terminal additionally R2 null."""
    result = capture(tmp_path, monkeypatch, "ordinary")
    records, _ = rq.replay_records(result.companion, result.primary)
    rows = [record for record in records if record.tag == "R"]
    assert len(rows) == 2
    assert rows[0].values[0:2] == (1, 0)
    assert rows[1].values[0:2] == (2, None)
    assert rows[0].values[2:] == (None,) * 13
    assert rows[1].values[2:] == (None,) * 13
    document = rq.joined(result)
    rq.assert_exact(document, "ordinary")
    rq.assert_q_schedule(document, "ordinary")


@pytest.mark.parametrize("tag,mutation", [
    ("R", "short"), ("R", "extra"), ("Q", "short"), ("Q", "extra"),
])
def test_rq_arity_errors_are_typed_after_an_authentic_capture(
        tmp_path, monkeypatch, tag, mutation):
    """ANSWER:173-177; PC:186. Correct receipt cannot hide bad arity."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    row = first(values, tag)
    if mutation == "short":
        row.pop()
    else:
        row.append(None)
    emit_mutation(result.companion, values)
    typed_error(result.companion, tag.lower() + "_record_invalid")


@pytest.mark.parametrize("position,bad", [
    (1, True), (1, 0), (1, 1.5), (2, -1), (2, True), (2, (1 << 53)),
    (3, None), (3, 7), (3, "007"), (3, "-1"), (3, str(1 << 64)),
    (4, -1), (4, False), (5, 0), (5, 1.5),
    (6, None), (6, ["pnl"]), (6, ["unknown"]), (6, rq.RECUT_FIELDS * 2),
    (7, -1), (7, True), (8, "2"), (9, None), (10, None),
    (11, False), (12, []), (13, {}), (14, "2"), (15, True),
])
def test_r_record_types_bounds_branch_field_list_and_applicable_operands(
        tmp_path, monkeypatch, position, bad):
    """ANSWER:175-177,202-214; DELTA:62-66,203."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    first(values, "R")[position] = bad
    emit_mutation(result.companion, values)
    typed_error(result.companion, "r_record_invalid")


@pytest.mark.parametrize("position,bad", [
    (1, -1), (1, True), (1, 0.5), (2, None), (2, "0"),
    (3, None), (3, -1), (3, True), (3, 0.5), (3, (1 << 53)),
    (4, None), (4, "0"), (5, "invented"), (5, False), (6, None), (6, "0"),
])
def test_q_record_types_bounds_closed_basis_set_and_nonanchor_null_refusal(
        tmp_path, monkeypatch, position, bad):
    """ANSWER:175-177,198-201; DELTA:108."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    first(values, "Q")[position] = bad
    emit_mutation(result.companion, values)
    typed_error(result.companion, "q_record_invalid")


@pytest.mark.parametrize("tag,position", [("R", index) for index in range(8, 16)]
                         + [("Q", 4), ("Q", 6)])
@pytest.mark.parametrize("token", [b"1e400", b"-1e400", b"NaN", b"Infinity"])
def test_rq_wire_nonfinite_values_are_typed_errors(tmp_path, monkeypatch, tag, position, token):
    """ANSWER:177,184-186. Mutate only a real record's numeric token."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    row = first(values, tag)
    # A string sentinel safely identifies one token; no synthetic native value.
    row[position] = "replay-nonfinite-token"
    payload = b"".join(rq.wire(value) for value in values[:-1])
    payload = payload.replace(b'"replay-nonfinite-token"', token, 1)
    values[-1]["sha256"] = hashlib.sha256(payload).hexdigest()
    result.companion.write_bytes(payload + rq.wire(values[-1]))
    typed_error(result.companion, tag.lower() + "_record_invalid")


@pytest.mark.parametrize("ordinal", [(1 << 53) + 1, (1 << 64) - 1])
def test_parent_id_full_uint64_decimal_string_survives_writer_reader_seam(
        tmp_path, monkeypatch, ordinal):
    """DELTA:62,203; ANSWER:177. Explicit permitted ordinal injection seam."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    for row in values[1:-1]:
        if row[0] == "R":
            row[3] = str(ordinal)
    emit_mutation(result.companion, values)
    records, _ = rq.replay_records(result.companion, result.primary)
    rows = rq.rows_with_tag(records, "R")
    assert len(rows) == 3
    assert [row["parent_id"] for row in rows] == [str(ordinal)] * 3
    assert all(type(row["parent_id"]) is str for row in rows)
    for row in values[1:-1]:
        if row[0] == "R":
            row[3] = ordinal
    emit_mutation(result.companion, values)
    typed_error(result.companion, "r_record_invalid")


@pytest.mark.parametrize("tag,reason", [("R", "r_coverage_invalid"), ("Q", "q_coverage_invalid")])
def test_missing_record_cannot_pass_with_repaired_companion_counts_and_hash(
        tmp_path, monkeypatch, tag, reason):
    """ANSWER:246; DELTA-2:73,78-79. R/T and Q/E association is mandatory."""
    result = capture(tmp_path, monkeypatch, "sealed")
    values = objects(result.companion)
    target = first(values, tag)
    values.remove(target)
    emit_mutation(result.companion, values)
    unavailable(result, reason)


@pytest.mark.parametrize("fault,reason", [
    ("removed", "missing_companion"), ("trailer_removed", "missing_trailer"),
    ("r_truncated", "count_mismatch"), ("hash", "sha256_mismatch"),
    ("binding", "base_sha256_mismatch"), ("trailing_bytes", "trailing_bytes"),
])
def test_sealed_exact_replay_is_unavailable_for_incomplete_or_unbound_companion(
        tmp_path, monkeypatch, fault, reason):
    """DELTA-2:73,78-80. No presented-row or tolerance fallback is accepted."""
    result = capture(tmp_path, monkeypatch, "sealed")
    observation = result.observations[0]
    assert observation["base"] >= observation["end"][0]
    lines = result.companion.read_bytes().splitlines(keepends=True)
    if fault == "removed":
        result.companion.unlink()
    elif fault == "trailer_removed":
        result.companion.write_bytes(b"".join(lines[:-1]))
    elif fault == "r_truncated":
        at = next(index for index, line in enumerate(lines[1:-1], 1)
                  if rq.load_line(line)[0] == "R")
        result.companion.write_bytes(b"".join(lines[:at] + lines[at + 1:]))
    elif fault == "trailing_bytes":
        result.companion.write_bytes(b"".join(lines) + b"\n")
    else:
        trailer = rq.load_line(lines[-1])
        key = "sha256" if fault == "hash" else "base_sha256"
        trailer[key] = ("1" if trailer[key][0] == "0" else "0") + trailer[key][1:]
        result.companion.write_bytes(b"".join(lines[:-1]) + rq.wire(trailer))
    unavailable(result, reason)


def test_other_runs_companion_cannot_bind_to_a_verified_primary(tmp_path, monkeypatch):
    """ANSWER:218; DELTA-2:80."""
    first_run = capture(tmp_path / "one", monkeypatch)
    other = capture(tmp_path / "other", monkeypatch, "ordinary")
    first_run.companion.write_bytes(other.companion.read_bytes())
    unavailable(first_run, "base_sha256_mismatch")


@pytest.mark.parametrize("fault,reason", [
    ("duplicate_n", "r_coverage_invalid"),
    ("q_wrong_time", "q_coverage_invalid"),
    ("q_duplicate_index", "q_coverage_invalid"),
    ("cutoff_oob", "replay_mismatch"),
    ("settlement_gap", "settlement_coverage_invalid"),
    ("whole_pnl_disagrees", "settlement_operand_mismatch"),
    ("whole_qty_disagrees", "settlement_operand_mismatch"),
    ("duplicate_slice", "parent_coverage_invalid"),
    ("slice_count_disagrees", "parent_coverage_invalid"),
])
def test_bound_companion_checks_global_and_parent_coverage_and_shared_operands(
        tmp_path, monkeypatch, fault, reason):
    """DELTA:62,66,106,108; ANSWER:246."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    rs = [row for row in values[1:-1] if row[0] == "R"]
    qs = [row for row in values[1:-1] if row[0] == "Q"]
    assert [row[2] for row in rs] == [0, 0, 1]
    if fault == "duplicate_n":
        rs[1][1] = rs[0][1]
    elif fault == "q_wrong_time":
        qs[0][2] += rq.MINUTE
    elif fault == "q_duplicate_index":
        qs[1][1] = qs[0][1]
    elif fault == "cutoff_oob":
        qs[0][3] = 3
    elif fault == "settlement_gap":
        rs[2][2] = 2
    elif fault == "whole_pnl_disagrees":
        rs[1][10] += 1.0
    elif fault == "whole_qty_disagrees":
        rs[1][9] += 1.0
    elif fault == "duplicate_slice":
        rs[1][4] = rs[0][4]
    else:
        rs[1][5] += 1
    emit_mutation(result.companion, values)
    unavailable(result, reason)


@pytest.mark.parametrize("tag,position", [("S", 3), ("Q", 4)])
def test_negative_zero_s3_or_q4_is_rejected_before_canonical_encoding(
        tmp_path, monkeypatch, tag, position):
    """DELTA-2:35,41,44,60. Healthy zero-run engine capture is the starting point."""
    result = capture(tmp_path, monkeypatch, "zero")
    values = objects(result.companion)
    row = first(values, tag)
    rq.same(row[position], 0.0)
    row[position] = -0.0
    emit_mutation(result.companion, values)
    assert b"-0.0" in result.companion.read_bytes()
    unavailable(result, "replay_mismatch")


@pytest.mark.parametrize("tag", ["S", "Q"])
def test_exact_validator_rejects_wrong_sum_or_wrong_prefix_even_with_valid_receipt(
        tmp_path, monkeypatch, tag):
    """DELTA:106-110. Typed exact failure, never report-row reconciliation."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    row = first(values, tag)
    row[3 if tag == "S" else 4] += 1.0
    emit_mutation(result.companion, values)
    unavailable(result, "replay_mismatch")


@pytest.mark.parametrize("position", [14, 15])
def test_any_marker_must_have_null_fee_operands(tmp_path, monkeypatch, position):
    """DELTA:64: even enabled commission does not make ANY fee operands applicable."""
    result = capture(tmp_path, monkeypatch, "any")
    values = objects(result.companion)
    row = first(values, "R")
    assert row[6] == [] and row[position] is None
    row[position] = 0.0
    emit_mutation(result.companion, values)
    typed_error(result.companion, "r_record_invalid")


@pytest.mark.parametrize("position", [14, 15])
def test_fifo_fee_operands_are_both_applicable_or_both_null(tmp_path, monkeypatch, position):
    """ANSWER:213-214; DELTA:154. One null cannot describe the executed fee branch."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    first(values, "R")[position] = None
    emit_mutation(result.companion, values)
    typed_error(result.companion, "r_record_invalid")


@pytest.mark.parametrize("position,bad", [(3, 0), (4, 0.0), (5, "kernel_open"), (6, 1.0)])
def test_selected_anchor_q_fields_cannot_be_fabricated_history(
        tmp_path, monkeypatch, position, bad):
    """DELTA:108,115; ANSWER:198-201. Anchor applicability is cross-field."""
    result = capture(tmp_path, monkeypatch, "zero", selected=True)
    values = objects(result.companion)
    row = first(values, "Q")
    assert row[3:7] == [None, None, "anchor", 0.0]
    row[position] = bad
    emit_mutation(result.companion, values)
    typed_error(result.companion, "q_record_invalid")


@pytest.mark.parametrize("target", ["physical", "terminal"])
def test_r2_nullability_is_checked_against_the_primary_trade_kind(tmp_path, monkeypatch, target):
    """ANSWER:175,197; DELTA-2:73. A terminal quote is not a physical close."""
    result = capture(tmp_path, monkeypatch, "ordinary")
    values = objects(result.companion)
    rows = [row for row in values[1:-1] if row[0] == "R"]
    rows[0 if target == "physical" else 1][2] = None if target == "physical" else 1
    emit_mutation(result.companion, values)
    unavailable(result, "r_coverage_invalid")


def test_parser_keeps_an_individual_negative_zero_whole_pnl_operand(tmp_path, monkeypatch):
    """DELTA-2:35,60. Only S3/Q4 forbid -0.0; individual P[j] may carry it."""
    result = capture(tmp_path, monkeypatch)
    values = objects(result.companion)
    for row in values[1:-1]:
        if row[0] == "R" and row[2] == 0:
            row[10] = -0.0
    emit_mutation(result.companion, values)
    # Unbound parsing tests representation, not the now-deliberately-inconsistent
    # primary's accounting. The bound reader is covered by replay_mismatch rows.
    records, _ = rq.replay_records(result.companion)
    rows = [row for row in rq.rows_with_tag(records, "R") if row["settlement_index"] == 0]
    assert len(rows) == 2
    assert all(rq.bits(row["whole_pnl"]) == bytes.fromhex("8000000000000000") for row in rows)
