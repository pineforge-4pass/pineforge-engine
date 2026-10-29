"""Red-first parser tests for the proposed pf-report-replay/v1 companion."""
from __future__ import annotations

import hashlib
import io
import json

import pytest

import pf_sink


def _wire(value, *, allow_nan=False):
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            separators=(",", ":"),
            allow_nan=allow_nan,
        ).encode("utf-8")
        + b"\n"
    )


_S_HEADER = {
    "schema": "pf-report-replay",
    "version": 1,
    "run_id": "replay",
    "engine": "pineforge",
    "records": [{
        "type": "S",
        "fields": ["initial_capital", "pointvalue", "net_profit_sum"],
        "types": ["f64", "f64", "f64"],
        "units": ["account_ccy", "quote_ccy", "account_ccy"],
    }],
}


def _companion(rows=(), *, counts=None, base_sha256=None, extra=b""):
    body = _wire(_S_HEADER)
    for row in rows:
        body += _wire(row, allow_nan=True)
    if counts is None:
        counts = {"S": sum(row[0] == "S" for row in rows)}
    trailer = {
        "trailer": "pf-report-replay",
        "version": 1,
        "counts": counts,
        "sha256": hashlib.sha256(body).hexdigest(),
    }
    if base_sha256 is not None:
        trailer["base_sha256"] = base_sha256
    return body + _wire(trailer) + extra


def _read(data):
    reader = pf_sink.ReplaySinkReader(stream=io.BytesIO(data))
    return list(reader), reader


@pytest.mark.parametrize(
    "values",
    [(5_000.0, 2.5, 17.0), (12_345.0, 0.25, -6.0)],
    ids=["positive-profit", "negative-profit"],
)
def test_companion_has_exactly_one_s_record_and_its_own_receipt(values):
    records, reader = _read(_companion(
        [["S", *values]],
        base_sha256="a" * 64,
    ))

    assert reader.complete
    assert reader.counts == {"S": 1}
    assert [record.tag for record in records] == ["S"]
    assert records[0].fields == (
        "initial_capital", "pointvalue", "net_profit_sum"
    )
    assert records[0].values == values
    assert reader.trailer["base_sha256"] == "a" * 64
    assert reader.trailer["sha256"] == reader.sha256


@pytest.mark.parametrize(
    "row",
    [
        ["S", 5_000.0, 2.5],
        ["S", 5_000.0, 2.5, 17.0, 99.0],
        ["S", "5000.0", 2.5, 17.0],
        ["S", True, 2.5, 17.0],
        ["S", 5_000.0, None, 17.0],
        ["S", 5_000.0, float("nan"), 17.0],
        ["S", 5_000.0, float("inf"), 17.0],
    ],
    ids=[
        "short",
        "long",
        "string",
        "boolean",
        "null",
        "nan",
        "infinity",
    ],
)
def test_malformed_s_record_is_a_typed_companion_error(row):
    with pytest.raises(pf_sink.ReplaySinkFormatError) as caught:
        _read(_companion([row]))

    assert caught.value.code == "s_record_invalid"


def test_missing_s_record_is_rejected_even_with_a_valid_hash_and_trailer():
    with pytest.raises(pf_sink.ReplaySinkFormatError) as caught:
        _read(_companion([]))

    assert caught.value.code == "missing_s"


def test_duplicate_s_records_are_rejected():
    with pytest.raises(pf_sink.ReplaySinkFormatError) as caught:
        _read(_companion([
            ["S", 5_000.0, 2.5, 17.0],
            ["S", 5_000.0, 2.5, 17.0],
        ]))

    assert caught.value.code == "duplicate_s"


def test_trailer_must_follow_s_and_eof_must_follow_the_trailer():
    body = _wire(_S_HEADER)
    trailer = {
        "trailer": "pf-report-replay",
        "version": 1,
        "counts": {"S": 0},
        "sha256": hashlib.sha256(body).hexdigest(),
    }
    with pytest.raises(pf_sink.ReplaySinkFormatError) as caught:
        _read(body + _wire(trailer) + _wire(["S", 5_000.0, 2.5, 17.0]))
    assert caught.value.code == "trailer_out_of_order"

    with pytest.raises(pf_sink.ReplaySinkFormatError) as caught:
        _read(_companion(
            [["S", 5_000.0, 2.5, 17.0]],
            extra=b'{"after":"eof"}\n',
        ))
    assert caught.value.code == "trailing_bytes"


def test_existing_v1_reader_rejects_an_injected_s_record(tmp_path):
    primary_header = {
        "schema": "pf-report-sink",
        "version": 1,
        "run_id": "replay",
        "engine": "pineforge",
        "records": [
            {
                "type": "E",
                "fields": ["time_ms", "equity", "open_profit"],
                "types": ["i64", "f64?", "f64?"],
                "units": ["unix_ms", "account_ccy", "account_ccy"],
            },
            {
                "type": "T",
                "fields": [
                    "n", "side", "entry_time", "exit_time", "entry_price",
                    "exit_price", "qty", "pnl", "pnl_pct", "max_runup",
                    "max_drawdown", "commission", "entry_bar_index",
                    "exit_bar_index", "open_at_end", "entry_incarnation",
                    "close_cause", "entry_id", "exit_id", "exit_comment",
                ],
                "types": [
                    "i64", "str", "i64", "i64", "f64?", "f64?", "f64?",
                    "f64?", "f64?", "f64?", "f64?", "f64?", "i64", "i64",
                    "bool", "u64", "i64", "str", "str", "str",
                ],
                "units": [
                    "", "", "unix_ms", "unix_ms", "quote_ccy", "quote_ccy",
                    "qty", "account_ccy", "percent", "account_ccy",
                    "account_ccy", "account_ccy", "script_bar", "script_bar",
                    "", "", "", "", "", "",
                ],
            },
        ],
    }
    path = tmp_path / "replay-primary-with-injected-s.jsonl"
    path.write_bytes(
        _wire(primary_header)
        + _wire(["S", 5_000.0, 2.5, 17.0])
    )
    with pytest.raises(pf_sink.SinkFormatError):
        with pf_sink.SinkReader(path) as reader:
            list(reader)
