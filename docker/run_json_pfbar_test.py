"""File-contract and csv2bin proofs (run on the Linux test host, never at authoring).

The red commit deliberately precedes pf_bars and the csv2bin dispatch. Tests use
the real CLI and real files; no strategy library or engine build is required.
"""
from __future__ import annotations

import csv
import hashlib
import importlib
import json
import math
import os
import stat
import struct
import subprocess
import sys
from pathlib import Path

import pytest
import run_json


ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "docker" / "run_json.py"
HEADER = "timestamp,open,high,low,close,volume"
T0 = 1735776000000
LIMIT = 2**53 - 1
PREFIX = b"pineforge:ohlcv:barc-le:v1\0"
BAR = struct.Struct("<5dq")
REQUIRE_PFBAR_CORPUS_ENV = "PINEFORGE_REQUIRE_PFBAR_CORPUS"


@pytest.fixture
def pf():
    # An absent module must fail, never skip, on the pre-implementation tree.
    return importlib.import_module("pf_bars")


def source(tmp_path, rows=None, *, header=HEADER, raw=None):
    path = tmp_path / "input.csv"
    if raw is not None:
        path.write_bytes(raw)
    else:
        rows = [(T0, 1, 2, .5, 1.5, 3)] if rows is None else rows
        path.write_text(header + "\n" + "".join(
            ",".join(str(cell) for cell in row) + "\n" for row in rows), encoding="utf-8")
    return path


def finals(output):
    return output, output.with_suffix(".close.bin"), Path(str(output) + ".json")


def convert(pf, tmp_path, rows=None, *, feed=False, tf="1", symbol="ETHUSDT", **kwargs):
    path = source(tmp_path, rows, **kwargs)
    output = tmp_path / "out.bin"
    metadata = pf.convert_csv(path, output, feed=feed, symbol=symbol, tf=tf)
    return output, metadata


def refused(pf, tmp_path, reason, *, feed=False, rows=None, **kwargs):
    path = source(tmp_path, rows, **kwargs)
    output = tmp_path / "out.bin"
    with pytest.raises(pf.BarsError) as caught:
        pf.convert_csv(path, output, feed=feed, symbol="ETHUSDT", tf="1")
    assert caught.value.code == ("symbol_feeds_refused" if feed else "chart_bars_unreadable")
    assert caught.value.code_args == {"reason": reason}
    assert not any(path.exists() for path in finals(output))
    return caught.value


def cli(*args):
    return subprocess.run([sys.executable, "-B", str(CLI), "csv2bin", *map(str, args)],
                          capture_output=True, text=True)


def test_csv2bin_chart_cli(tmp_path):
    """Red reason: today's harness has no csv2bin subcommand (requires --so)."""
    path = source(tmp_path)
    output = tmp_path / "chart.ETHUSDT.1.bin"
    result = cli("--chart", "--symbol", "ETHUSDT", "--tf", "1", path, output)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert result.stdout == result.stderr == ""
    assert output.read_bytes() == BAR.pack(1, 2, .5, 1.5, 3, T0)
    assert not output.with_suffix(".close.bin").exists()


def test_csv2bin_feed_cli_and_exact_sidecar(tmp_path):
    path = source(tmp_path)
    output = tmp_path / "feed.ETHUSDT.1D.bin"
    result = cli("--feed", "--symbol", "ETHUSDT", "--tf", "D", path, output)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert result.stdout == result.stderr == ""
    bars = BAR.pack(1, 2, .5, 1.5, 3, T0)
    closes = struct.pack("<q", T0 + 86400000)
    expected = {"format": "pf_bar_t/v1", "record_size": 48, "symbol": "ETHUSDT",
                "tf": "1D", "count": 1, "first_ts": T0, "last_ts": T0,
                "sha256": hashlib.sha256(bars).hexdigest(),
                "close_sha256": hashlib.sha256(closes).hexdigest()}
    assert output.read_bytes() == bars
    assert output.with_suffix(".close.bin").read_bytes() == closes
    assert Path(str(output) + ".json").read_bytes() == (
        json.dumps(expected, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        + "\n").encode("ascii")


@pytest.mark.parametrize("feed,reason", [(False, "value"), (True, "feed_value_not_number")])
def test_cli_refusal_is_exit_two_one_json_line_on_stderr(tmp_path, feed, reason):
    path = source(tmp_path, [(T0, "bad", 2, .5, 1.5, 3)])
    output = tmp_path / "out.bin"
    result = cli("--feed" if feed else "--chart", "--symbol", "ETHUSDT", "--tf", "1",
                 path, output)
    assert result.returncode == 2
    assert result.stdout == ""
    assert len(result.stderr.splitlines()) == 1
    document = json.loads(result.stderr)
    assert document["engine"] == "pineforge"
    assert document["code"] == ("symbol_feeds_refused" if feed else "chart_bars_unreadable")
    assert document["args"] == {"reason": reason}
    assert document["error"]


@pytest.mark.parametrize("timestamp", [2**63, -2**63 - 1], ids=["above-int64", "below-int64"])
@pytest.mark.parametrize("later_row", [False, True], ids=["first-row", "later-row"])
def test_cli_chart_timestamp_overflow_is_value_refusal(tmp_path, timestamp, later_row):
    rows = [(T0, 1, 2, .5, 1.5, 3)] if later_row else []
    path = source(tmp_path, rows + [(timestamp, 1, 2, .5, 1.5, 3)])
    output = tmp_path / "out.bin"
    result = cli("--chart", "--symbol", "ETHUSDT", "--tf", "1", path, output)
    assert result.returncode == 2
    assert result.stdout == ""
    assert len(result.stderr.splitlines()) == 1
    document = json.loads(result.stderr)
    assert document["engine"] == "pineforge"
    assert document["code"] == "chart_bars_unreadable"
    assert document["args"] == {"reason": "value"}
    assert not any(path.exists() for path in finals(output))


def test_cli_unexpected_converter_exception_is_one_json_line(pf, tmp_path, monkeypatch, capsys):
    def broken_converter(*args, **kwargs):
        raise RuntimeError("injected converter fault\nsecond line")
    monkeypatch.setattr(pf, "convert_csv", broken_converter)
    path = source(tmp_path)
    status = run_json.main(["csv2bin", "--chart", "--symbol", "ETHUSDT", "--tf", "1",
                            str(path), str(tmp_path / "out.bin")])
    captured = capsys.readouterr()
    assert status == 1 and captured.out == ""
    assert len(captured.err.splitlines()) == 1
    document = json.loads(captured.err)
    assert document["code"] == "harness_internal_error"
    assert document["args"] == {}
    assert document["error"] == \
        "harness internal error: RuntimeError: injected converter fault\nsecond line"


@pytest.mark.parametrize("arguments", [[], ["--chart", "--feed"], ["--unknown"]])
def test_cli_usage_refusal_is_also_one_json_line(tmp_path, arguments):
    result = cli(*arguments)
    assert result.returncode == 2
    assert result.stdout == ""
    assert len(result.stderr.splitlines()) == 1
    document = json.loads(result.stderr)
    assert document["code"] == "run_request_invalid"
    assert document["args"] == {"option": "arguments"}


@pytest.mark.parametrize("mode,code", [("--chart", "chart_bars_unreadable"),
                                      ("--feed", "symbol_feeds_refused")])
def test_cli_invalid_timeframe_uses_mode_failure_code(tmp_path, mode, code):
    path = source(tmp_path)
    output = tmp_path / "out.bin"
    result = cli(mode, "--symbol", "ETHUSDT", "--tf", "4h", path, output)
    assert result.returncode == 2 and result.stdout == ""
    assert len(result.stderr.splitlines()) == 1
    document = json.loads(result.stderr)
    assert document["code"] == code
    assert document["args"] == {"reason": "timeframe_invalid"}
    assert sorted(p.name for p in tmp_path.iterdir()) == ["input.csv"]


def test_layout_is_headerless_48_bytes_and_little_endian(pf, tmp_path):
    output, metadata = convert(pf, tmp_path)
    assert pf.BAR_RECORD.size == 48
    assert pf.CLOSE_RECORD.size == 8
    assert output.read_bytes() == BAR.pack(1, 2, .5, 1.5, 3, T0)
    assert metadata == json.loads(Path(str(output) + ".json").read_text())
    assert metadata["sha256"] == hashlib.sha256(output.read_bytes()).hexdigest()
    assert "close_sha256" not in metadata


def test_symbol_is_the_exact_lake_string_and_sidecar_is_canonical_ascii(pf, tmp_path):
    output, metadata = convert(pf, tmp_path, symbol="ETH/台")
    assert metadata["symbol"] == "ETH/台"
    assert Path(str(output) + ".json").read_bytes() == (
        json.dumps(metadata, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        + "\n").encode("ascii")
    assert set(metadata) == {"format", "record_size", "symbol", "tf", "count", "first_ts",
                             "last_ts", "sha256"}


def test_conventional_names_escape_each_utf8_byte(pf):
    assert pf.conventional_name("BINANCE:ETHUSDT", "D", feed=True) == \
        "feed.BINANCE_ETHUSDT.1D.bin"
    assert pf.conventional_name("ETH/台._-", "1") == "chart.ETH____._-.1.bin"


@pytest.mark.parametrize("tf,expected", [("D", "1D"), ("W", "1W"), ("M", "1M"),
                                         ("S", "1S"), ("99999", "99999"),
                                         ("9999S", "9999S"), ("240", "240")])
def test_timeframe_canonicalization_matches_run_json(pf, tmp_path, tf, expected):
    output, metadata = convert(pf, tmp_path, tf=tf)
    assert metadata["tf"] == expected == run_json.symbol_timeframe(tf)
    assert output.exists()


@pytest.mark.parametrize("tf", ["", "0", "00", "01", "4h", "60m", "1d", " D", "D ",
                               "100000", "10000S", "+1", "1.5"])
@pytest.mark.parametrize("feed", [False, True], ids=["chart", "feed"])
def test_timeframe_invalid_before_any_output(pf, tmp_path, tf, feed):
    path = source(tmp_path)
    output = tmp_path / "out.bin"
    with pytest.raises(pf.BarsError) as caught:
        pf.convert_csv(path, output, feed=feed, symbol="ETHUSDT", tf=tf)
    assert caught.value.code == ("symbol_feeds_refused" if feed else "chart_bars_unreadable")
    assert caught.value.code_args == {"reason": "timeframe_invalid"}
    assert "timeframe_invalid" in (pf.FEED_REASONS if feed else pf.CHART_REASONS)
    assert sorted(p.name for p in tmp_path.iterdir()) == ["input.csv"]


@pytest.mark.parametrize("cell,bits", [("-0", 0x8000000000000000), ("0", 0),
                                     ("nan", 0x7ff8000000000000),
                                     ("NaN", 0x7ff8000000000000),
                                     ("-nan", 0xfff8000000000000)])
@pytest.mark.parametrize("feed", [False, True], ids=["chart", "feed"])
def test_volume_preserves_zero_sign_and_nan_payload(pf, tmp_path, cell, bits, feed):
    output, _ = convert(pf, tmp_path, [(T0, 1, 2, .5, 1.5, cell)], feed=feed)
    assert struct.unpack_from("<Q", output.read_bytes(), 32)[0] == bits
    _, _, csv_hash = run_json.load_bars(tmp_path / "input.csv")
    assert hashlib.sha256(PREFIX + output.read_bytes()).hexdigest() == csv_hash


def test_negative_zero_prices_are_not_normalized_or_ohlc_validated(pf, tmp_path):
    output, _ = convert(pf, tmp_path, [(T0, "-0", "-0", "-0", "-0", 0)])
    assert struct.unpack_from("<4Q", output.read_bytes()) == (0x8000000000000000,) * 4
    # Deliberately nonsensical OHLC remains an engine concern (DESIGN 1.4).
    output, _ = convert(pf, tmp_path, [(T0, -1, -3, 5, 2, 0)])
    assert output.read_bytes() == BAR.pack(-1, -3, 5, 2, 0, T0)


@pytest.mark.parametrize("index", [0, 1, 2, 3, 4, 5],
                         ids=["timestamp", "open", "high", "low", "close", "volume"])
def test_chart_non_numeric_cell_is_value(pf, tmp_path, index):
    row = [T0, 1, 2, .5, 1.5, 3]
    row[index] = "bad"
    refused(pf, tmp_path, "value", rows=[row])


@pytest.mark.parametrize("index", [1, 2, 3, 4], ids=["open", "high", "low", "close"])
@pytest.mark.parametrize("cell", ["NaN", "inf", "-inf"])
def test_chart_nonfinite_price_is_value(pf, tmp_path, index, cell):
    row = [T0, 1, 2, .5, 1.5, 3]
    row[index] = cell
    refused(pf, tmp_path, "value", rows=[row])


@pytest.mark.parametrize("cell", ["-1", "inf", "-inf"])
def test_chart_negative_or_infinite_volume_is_value(pf, tmp_path, cell):
    refused(pf, tmp_path, "value", rows=[(T0, 1, 2, .5, 1.5, cell)])


def test_chart_missing_columns(pf, tmp_path):
    refused(pf, tmp_path, "columns", header="timestamp,open,high,low,close",
            rows=[(T0, 1, 2, .5, 1.5)])


def test_chart_short_row_is_value(pf, tmp_path):
    refused(pf, tmp_path, "value", rows=[(T0, 1)])


def test_chart_empty_volume_is_value(pf, tmp_path):
    refused(pf, tmp_path, "value", rows=[(T0, 1, 2, .5, 1.5, "")])


def test_chart_noninteger_timestamp_is_value(pf, tmp_path):
    refused(pf, tmp_path, "value", rows=[("1.5", 1, 2, .5, 1.5, 3)])


@pytest.mark.parametrize("timestamp", [2**63, -2**63 - 1], ids=["above-int64", "below-int64"])
@pytest.mark.parametrize("later_row", [False, True], ids=["first-row", "later-row"])
def test_chart_integer_timestamp_outside_int64_is_value(pf, tmp_path, timestamp, later_row):
    rows = [(T0, 1, 2, .5, 1.5, 3)] if later_row else []
    refused(pf, tmp_path, "value", rows=rows + [(timestamp, 1, 2, .5, 1.5, 3)])


def test_chart_reordered_columns_and_extra_column(pf, tmp_path):
    output, _ = convert(pf, tmp_path, header="extra,volume,close,low,high,open,timestamp",
                        rows=[("ignored", 3, 1.5, .5, 2, 1, T0)])
    assert output.read_bytes() == BAR.pack(1, 2, .5, 1.5, 3, T0)


def test_chart_timestamp_keeps_the_int64_domain(pf, tmp_path):
    rows = [(-2**63, 1, 2, .5, 1.5, 3), (2**63 - 1, 1, 2, .5, 1.5, 3)]
    output, _ = convert(pf, tmp_path, rows)
    assert output.read_bytes() == b"".join(BAR.pack(1, 2, .5, 1.5, 3, row[0]) for row in rows)


def test_chart_not_utf8_is_value(pf, tmp_path):
    refused(pf, tmp_path, "value", raw=b"\xff" + HEADER.encode() + b"\n")


def test_chart_bom_is_not_stripped(pf, tmp_path):
    refused(pf, tmp_path, "columns", raw=("\ufeff" + HEADER +
            f"\n{T0},1,2,.5,1.5,3\n").encode("utf-8"))


@pytest.mark.parametrize("feed", [False, True], ids=["chart", "feed"])
def test_header_only_is_empty_and_writes_no_bin(pf, tmp_path, feed):
    refused(pf, tmp_path, "empty", feed=feed, rows=[])


def test_zero_byte_chart_is_empty(pf, tmp_path):
    refused(pf, tmp_path, "empty", raw=b"")


@pytest.mark.parametrize("delta", [0, -60000], ids=["duplicate", "descending"])
def test_chart_order(pf, tmp_path, delta):
    refused(pf, tmp_path, "order", rows=[(T0, 1, 2, .5, 1.5, 3),
                                          (T0 + delta, 1, 2, .5, 1.5, 3)])


def test_chart_first_defective_row_stops_before_later_bad_value(pf, tmp_path):
    refused(pf, tmp_path, "order", rows=[(T0, 1, 2, .5, 1.5, 3),
                                          (T0, 1, 2, .5, 1.5, 3),
                                          (T0 + 60000, "bad", 2, .5, 1.5, 3)])


def test_chart_parse_precedes_order_in_the_same_row(pf, tmp_path):
    refused(pf, tmp_path, "value", rows=[(T0, 1, 2, .5, 1.5, 3),
                                          (T0, "bad", 2, .5, 1.5, 3)])


def test_feed_bom_reordered_columns_and_extra_columns(pf, tmp_path):
    output, metadata = convert(pf, tmp_path, feed=True,
        header="\ufeffextra,close,low,high,open,timestamp", rows=[("ignored", 1.5, .5, 2, 1, T0)])
    row = BAR.unpack(output.read_bytes())
    assert row[:4] == (1, 2, .5, 1.5) and math.isnan(row[4]) and row[5] == T0
    assert metadata["count"] == 1


@pytest.mark.parametrize("cell", ["", " ", "NaN"])
def test_feed_empty_or_nan_volume(pf, tmp_path, cell):
    output, _ = convert(pf, tmp_path, [(T0, 1, 2, .5, 1.5, cell)], feed=True)
    assert math.isnan(BAR.unpack(output.read_bytes())[4])


def test_feed_columns_missing(pf, tmp_path):
    refused(pf, tmp_path, "feed_columns_missing", feed=True,
            header="timestamp,open,high,low", rows=[(T0, 1, 2, .5)])


def test_zero_byte_feed_has_missing_columns(pf, tmp_path):
    refused(pf, tmp_path, "feed_columns_missing", feed=True, raw=b"")


@pytest.mark.parametrize("index", range(7),
                         ids=["timestamp", "open", "high", "low", "close", "volume", "time_close"])
def test_feed_value_not_number(pf, tmp_path, index):
    row = [T0, 1, 2, .5, 1.5, 3, T0 + 60000]
    row[index] = "bad"
    refused(pf, tmp_path, "feed_value_not_number", feed=True, rows=[row],
            header=HEADER + ",time_close")


@pytest.mark.parametrize("cell", ["1.5", "NaN", "inf", "1e3"])
def test_feed_noninteger_timestamp_is_not_number(pf, tmp_path, cell):
    refused(pf, tmp_path, "feed_value_not_number", feed=True,
            rows=[(cell, 1, 2, .5, 1.5, 3)])


@pytest.mark.parametrize("index", [1, 2, 3, 4], ids=["open", "high", "low", "close"])
@pytest.mark.parametrize("cell", ["NaN", "inf", "-inf"])
def test_feed_nonfinite_price_is_invalid(pf, tmp_path, index, cell):
    row = [T0, 1, 2, .5, 1.5, 3]
    row[index] = cell
    refused(pf, tmp_path, "feed_value_invalid", feed=True, rows=[row])


@pytest.mark.parametrize("cell", ["-1", "inf", "-inf"])
def test_feed_negative_or_infinite_volume_is_invalid(pf, tmp_path, cell):
    refused(pf, tmp_path, "feed_value_invalid", feed=True,
            rows=[(T0, 1, 2, .5, 1.5, cell)])


@pytest.mark.parametrize("timestamp,close", [(LIMIT + 1, LIMIT + 2), (-LIMIT - 1, 0),
                                          (T0, LIMIT + 1), (T0, -LIMIT - 1)])
def test_feed_time_out_of_range(pf, tmp_path, timestamp, close):
    refused(pf, tmp_path, "feed_time_out_of_range", feed=True, header=HEADER + ",time_close",
            rows=[(timestamp, 1, 2, .5, 1.5, 3, close)])


def test_feed_derived_close_out_of_range(pf, tmp_path):
    refused(pf, tmp_path, "feed_time_out_of_range", feed=True,
            rows=[(LIMIT, 1, 2, .5, 1.5, 3)])


def test_feed_time_range_is_inclusive(pf, tmp_path):
    output, _ = convert(pf, tmp_path, feed=True, header=HEADER + ",time_close",
        rows=[(-LIMIT, 1, 2, .5, 1.5, 3, -LIMIT + 1),
              (LIMIT - 1, 1, 2, .5, 1.5, 3, LIMIT)])
    assert output.with_suffix(".close.bin").read_bytes() == struct.pack("<2q", -LIMIT + 1, LIMIT)


@pytest.mark.parametrize("timestamp", [-62135596800001, 253402300800000, 253399622400000])
def test_feed_month_calendar_range(pf, tmp_path, timestamp):
    path = source(tmp_path, [(timestamp, 1, 2, .5, 1.5, 3)])
    with pytest.raises(pf.BarsError) as caught:
        pf.convert_csv(path, tmp_path / "out.bin", feed=True, symbol="ETHUSDT", tf="1M")
    assert caught.value.code_args == {"reason": "feed_time_out_of_range"}


def test_feed_not_utf8_csv(pf, tmp_path):
    refused(pf, tmp_path, "feed_not_utf8_csv", feed=True, raw=b"\xff" + HEADER.encode() + b"\n")


@pytest.mark.parametrize("feed,reason", [(False, "value"), (True, "feed_not_utf8_csv")])
def test_csv_parser_error(pf, tmp_path, feed, reason):
    previous = csv.field_size_limit(64)
    try:
        refused(pf, tmp_path, reason, feed=feed, rows=[(T0, "1" * 65, 2, .5, 1.5, 3)])
    finally:
        csv.field_size_limit(previous)


@pytest.mark.parametrize("delta", [0, -60000], ids=["duplicate", "descending"])
def test_feed_not_increasing(pf, tmp_path, delta):
    refused(pf, tmp_path, "feed_not_increasing", feed=True,
            rows=[(T0, 1, 2, .5, 1.5, 3), (T0 + delta, 1, 2, .5, 1.5, 3)])


@pytest.mark.parametrize("close", [T0 - 1, T0], ids=["before-open", "equal-open"])
def test_feed_close_before_or_equal_open(pf, tmp_path, close):
    refused(pf, tmp_path, "feed_close_time_invalid", feed=True, header=HEADER + ",time_close",
            rows=[(T0, 1, 2, .5, 1.5, 3, close)])


def test_feed_close_after_next_open(pf, tmp_path):
    refused(pf, tmp_path, "feed_close_time_invalid", feed=True, header=HEADER + ",time_close",
            rows=[(T0, 1, 2, .5, 1.5, 3, T0 + 60001),
                  (T0 + 60000, 1, 2, .5, 1.5, 3, T0 + 120000)])


def test_feed_last_close_is_checked(pf, tmp_path):
    refused(pf, tmp_path, "feed_close_time_invalid", feed=True, header=HEADER + ",time_close",
            rows=[(T0, 1, 2, .5, 1.5, 3, T0 + 60000),
                  (T0 + 60000, 1, 2, .5, 1.5, 3, T0 + 60000)])


def test_feed_order_precedes_close_for_the_same_lookahead(pf, tmp_path):
    refused(pf, tmp_path, "feed_not_increasing", feed=True, header=HEADER + ",time_close",
            rows=[(T0, 1, 2, .5, 1.5, 3, T0), (T0, 1, 2, .5, 1.5, 3, T0)])


def test_feed_earlier_close_defect_precedes_later_order_defect(pf, tmp_path):
    refused(pf, tmp_path, "feed_close_time_invalid", feed=True, header=HEADER + ",time_close",
            rows=[(T0, 1, 2, .5, 1.5, 3, T0),
                  (T0 + 60000, 1, 2, .5, 1.5, 3, T0 + 120000),
                  (T0 + 60000, 1, 2, .5, 1.5, 3, T0 + 120000)])


@pytest.mark.parametrize("reason,last", [
    ("feed_value_not_number", (T0 + 120000, "bad", 2, .5, 1.5, 3, "")),
    ("feed_value_invalid", (T0 + 120000, "inf", 2, .5, 1.5, 3, "")),
    ("feed_time_out_of_range", (LIMIT + 1, 1, 2, .5, 1.5, 3, "")),
])
@pytest.mark.parametrize("earlier", ["order", "close"])
def test_feed_later_first_pass_defect_wins_over_order_or_close(pf, tmp_path, reason, last, earlier):
    second = T0 if earlier == "order" else T0 + 60000
    first_close = T0 + 60000 if earlier == "order" else T0
    refused(pf, tmp_path, reason, feed=True, header=HEADER + ",time_close",
            rows=[(T0, 1, 2, .5, 1.5, 3, first_close),
                  (second, 1, 2, .5, 1.5, 3, T0 + 120000), last])


def test_feed_parse_precedes_value_and_range_checks(pf, tmp_path):
    refused(pf, tmp_path, "feed_value_not_number", feed=True, header=HEADER + ",time_close",
            rows=[(LIMIT + 1, "inf", 2, .5, 1.5, -1, "bad")])


def test_feed_value_precedes_time_range(pf, tmp_path):
    refused(pf, tmp_path, "feed_value_invalid", feed=True,
            rows=[(LIMIT + 1, "inf", 2, .5, 1.5, 3)])


def test_feed_first_first_pass_defect_stops_before_later_row(pf, tmp_path):
    refused(pf, tmp_path, "feed_value_invalid", feed=True,
            rows=[(T0, "inf", 2, .5, 1.5, 3), (T0 + 60000, "bad", 2, .5, 1.5, 3)])


@pytest.mark.parametrize("opened,tf,closed", [
    (1735776000000, "1", 1735776060000), (1735776000000, "15", 1735776900000),
    (1735776000000, "1S", 1735776001000), (1735776000000, "1D", 1735862400000),
    (1735776000000, "1W", 1736380800000), (1738281600000, "1M", 1740700800000),
    (1706659200000, "1M", 1709164800000),
    (1738281600123, "1M", 1740700800123),
])
def test_shared_close_time_vectors(pf, tmp_path, opened, tf, closed):
    output, _ = convert(pf, tmp_path, [(opened, 1, 2, .5, 1.5, 3)], feed=True, tf=tf)
    assert output.with_suffix(".close.bin").read_bytes() == struct.pack("<q", closed)
    assert pf.bar_close_ms(opened, tf) == closed == run_json._bar_close_ms(opened, tf)


def test_feed_explicit_session_close_empty_fallback_and_next_open_equality(pf, tmp_path):
    output, _ = convert(pf, tmp_path, feed=True, tf="1D", header=HEADER + ",time_close",
        rows=[(1735828200000, 1, 2, .5, 1.5, 3, 1735851600000),
              (1735914600000, 1, 2, .5, 1.5, 3, " ")])
    assert output.with_suffix(".close.bin").read_bytes() == \
        struct.pack("<2q", 1735851600000, 1736001000000)
    output, _ = convert(pf, tmp_path, feed=True,
        rows=[(T0, 1, 2, .5, 1.5, 3), (T0 + 60000, 1, 2, .5, 1.5, 3)])
    assert output.with_suffix(".close.bin").read_bytes() == struct.pack("<2q", T0 + 60000, T0 + 120000)


@pytest.mark.parametrize("feed,reason", [(False, "io"), (True, "feed_unreadable")])
def test_input_unreadable(pf, tmp_path, feed, reason):
    with pytest.raises(pf.BarsError) as caught:
        pf.convert_csv(tmp_path / "missing.csv", tmp_path / "out.bin",
                       feed=feed, symbol="ETHUSDT", tf="1")
    assert caught.value.code_args == {"reason": reason}


@pytest.mark.parametrize("feed", [False, True], ids=["chart", "feed"])
@pytest.mark.parametrize("index", [0, 1, 2], ids=["bars", "close", "sidecar"])
def test_output_nonregular_precheck_happens_before_any_write(pf, tmp_path, feed, index):
    path = source(tmp_path)
    output = tmp_path / "out.bin"
    paths = finals(output)
    for p in paths:
        p.write_bytes(b"old generation\n") if p != paths[index] else p.mkdir()
    before = {p.name: p.read_bytes() for p in paths if p.is_file()}
    with pytest.raises(pf.BarsError) as caught:
        pf.convert_csv(path, output, feed=feed, symbol="ETHUSDT", tf="1")
    assert caught.value.code_args == {"reason": "io"}
    assert {p.name: p.read_bytes() for p in paths if p.is_file()} == before
    assert not list(tmp_path.glob("*.tmp"))


def test_output_io_error_has_the_output_reason(pf, tmp_path):
    path = source(tmp_path)
    for feed in (False, True):
        with pytest.raises(pf.BarsError) as caught:
            pf.convert_csv(path, tmp_path / "absent" / "out.bin", feed=feed,
                           symbol="ETHUSDT", tf="1")
        assert caught.value.code_args == {"reason": "io"}


def test_fresh_private_directory_exemption_preserves_bytes_and_refuses_existing(pf, tmp_path, monkeypatch):
    path = source(tmp_path)
    output = tmp_path / "out.bin"
    real_fsync = os.fsync
    def file_only(fd):
        assert not stat.S_ISDIR(os.fstat(fd).st_mode)
        real_fsync(fd)
    monkeypatch.setattr(pf.os, "fsync", file_only)
    metadata = pf.convert_csv(path, output, feed=True, symbol="ETHUSDT", tf="1",
                              fresh_private_directory=True)
    old = {p: p.read_bytes() for p in finals(output)}
    with pytest.raises(pf.BarsError) as caught:
        pf.convert_csv(path, output, feed=True, symbol="ETHUSDT", tf="1",
                       fresh_private_directory=True)
    assert caught.value.code_args == {"reason": "io"}
    assert {p: p.read_bytes() for p in finals(output)} == old
    assert metadata["count"] == 1


def test_overwrite_unlinks_old_commit_record_then_fsyncs_each_rename(pf, tmp_path, monkeypatch):
    path = source(tmp_path)
    output = tmp_path / "out.bin"
    for p in finals(output):
        p.write_bytes(b"old generation\n")
    events = []
    real_unlink, real_replace, real_fsync = os.unlink, os.replace, os.fsync
    def unlink(p, *args, **kwargs):
        events.append(("unlink", Path(p).name))
        return real_unlink(p, *args, **kwargs)
    def replace(src, dst):
        events.append(("rename", Path(src).name, Path(dst).name))
        assert not finals(output)[2].exists()
        return real_replace(src, dst)
    def fsync(fd):
        events.append(("fsync", "dir" if stat.S_ISDIR(os.fstat(fd).st_mode) else "file"))
        return real_fsync(fd)
    monkeypatch.setattr(pf.os, "unlink", unlink)
    monkeypatch.setattr(pf.os, "replace", replace)
    monkeypatch.setattr(pf.os, "fsync", fsync)
    pf.convert_csv(path, output, feed=True, symbol="ETHUSDT", tf="1")
    renames = [(i, event) for i, event in enumerate(events) if event[0] == "rename"]
    assert [event[2] for _, event in renames] == [p.name for p in finals(output)]
    assert events[:2] == [("unlink", "out.bin.json"), ("fsync", "dir")]
    for index, event in renames:
        assert events[index - 1] == ("fsync", "file")
        assert events[index + 1] == ("fsync", "dir")


@pytest.mark.parametrize("failed_rename", [1, 2, 3], ids=["bars", "close", "sidecar"])
def test_failed_publication_never_leaves_an_old_sidecar(pf, tmp_path, monkeypatch, failed_rename):
    path = source(tmp_path)
    output = tmp_path / "out.bin"
    for p in finals(output):
        p.write_bytes(b"old generation\n")
    real_replace = os.replace
    calls = 0
    def replace(src, dst):
        nonlocal calls
        calls += 1
        if calls == failed_rename:
            raise OSError("injected rename failure")
        return real_replace(src, dst)
    monkeypatch.setattr(pf.os, "replace", replace)
    with pytest.raises(pf.BarsError) as caught:
        pf.convert_csv(path, output, feed=True, symbol="ETHUSDT", tf="1")
    assert caught.value.code_args == {"reason": "io"}
    assert not finals(output)[2].exists()


@pytest.mark.parametrize("feed", [False, True], ids=["chart", "feed"])
def test_size_refusal(pf, feed):
    with pytest.raises(pf.BarsError) as caught:
        pf.bar_count(49, feed=feed)
    assert caught.value.code_args == {"reason": "size"}


@pytest.mark.parametrize("feed", [False, True], ids=["chart", "feed"])
def test_count_refusal_and_boundary_without_allocating_large_files(pf, feed):
    assert pf.bar_count(48 * (2**31 - 1), feed=feed) == 2**31 - 1
    with pytest.raises(pf.BarsError) as caught:
        pf.bar_count(48 * 2**31, feed=feed)
    assert caught.value.code_args == {"reason": "count"}


@pytest.mark.parametrize("feed", [False, True], ids=["chart", "feed"])
def test_zero_record_bin_refusal(pf, feed):
    with pytest.raises(pf.BarsError) as caught:
        pf.bar_count(0, feed=feed)
    assert caught.value.code_args == {"reason": "empty"}


def test_close_file_size_refusal(pf):
    with pytest.raises(pf.BarsError) as caught:
        pf.check_close_size(7, 1)
    assert caught.value.code == "symbol_feeds_refused"
    assert caught.value.code_args == {"reason": "close_file"}


@pytest.mark.parametrize("feed", [False, True], ids=["chart", "feed"])
def test_missing_sidecar_refusal(pf, tmp_path, feed):
    with pytest.raises(pf.BarsError) as caught:
        pf.read_sidecar(tmp_path / "absent.bin.json", feed=feed)
    assert caught.value.code_args == {"reason": "sidecar"}


@pytest.mark.parametrize("raw", [b"not json", b"[]", b"\xff"])
def test_unreadable_sidecar_refusal(pf, tmp_path, raw):
    path = tmp_path / "out.bin.json"
    path.write_bytes(raw)
    with pytest.raises(pf.BarsError) as caught:
        pf.read_sidecar(path)
    assert caught.value.code_args == {"reason": "sidecar"}


@pytest.mark.parametrize("key,value", [("count", 2), ("count", True), ("format", "v2"),
                                     ("record_size", 49), ("first_ts", T0 - 1),
                                     ("last_ts", T0 + 1), ("sha256", "0" * 64)])
def test_sidecar_contract_mismatch(pf, tmp_path, key, value):
    output, metadata = convert(pf, tmp_path)
    actual_digest = hashlib.sha256(output.read_bytes()).hexdigest()
    metadata[key] = value
    with pytest.raises(pf.BarsError) as caught:
        pf.check_sidecar(metadata, count=1, first_ts=T0, last_ts=T0, sha256=actual_digest)
    assert caught.value.code_args == {"reason": "sidecar"}


def test_feed_close_digest_mismatch_is_sidecar(pf, tmp_path):
    _, metadata = convert(pf, tmp_path, feed=True)
    with pytest.raises(pf.BarsError) as caught:
        pf.check_sidecar(metadata, count=1, first_ts=T0, last_ts=T0,
                         feed=True, close_sha256="0" * 64)
    assert caught.value.code == "symbol_feeds_refused"
    assert caught.value.code_args == {"reason": "sidecar"}


def test_sidecar_only_check_does_not_compare_digests_or_symbol_to_an_index(pf, tmp_path):
    _, metadata = convert(pf, tmp_path, feed=True, symbol="LAKE")
    metadata["sha256"] = metadata["close_sha256"] = "0" * 64
    pf.check_sidecar(metadata, count=1, first_ts=T0, last_ts=T0, feed=True)


@pytest.mark.parametrize("feed,reason", [(False, "order"), (True, "feed_not_increasing")])
def test_shared_load_pass_order_rule(pf, feed, reason):
    pf.check_order(T0, T0 + 1, feed=feed)
    for timestamp in (T0, T0 - 1):
        with pytest.raises(pf.BarsError) as caught:
            pf.check_order(T0, timestamp, feed=feed)
        assert caught.value.code_args == {"reason": reason}


def test_streaming_crosses_one_mib_buffer_and_matches_the_csv_identity(pf, tmp_path):
    path = tmp_path / "many.csv"
    count = 140000  # exceeds 1 MiB in both the 48-byte bar and 8-byte close streams
    with path.open("w", encoding="utf-8", newline="") as stream:
        stream.write(HEADER + "\n")
        for i in range(count):
            stream.write(f"{T0 + i * 60000},1,2,.5,1.5,-0\n")
    output = tmp_path / "out.bin"
    metadata = pf.convert_csv(path, output, feed=True, symbol="ETHUSDT", tf="1")
    assert output.stat().st_size == 48 * count
    assert output.with_suffix(".close.bin").stat().st_size == 8 * count
    assert metadata["first_ts"] == T0 and metadata["last_ts"] == T0 + (count - 1) * 60000
    expected = run_json._load_symbol_feed(path, "ETHUSDT", "1")["record"]["source_values_sha256"]
    digest = hashlib.sha256(b"pineforge:symbol-feed:barc-close-le:v1\0")
    with output.open("rb") as bars, output.with_suffix(".close.bin").open("rb") as closes:
        for _ in range(count):
            digest.update(bars.read(48))
            digest.update(closes.read(8))
    assert digest.hexdigest() == expected


def _require_materialized_corpus_csv(path):
    """Public preflight may lack corpus/LFS; strict lab proof must require it."""
    unavailable = "file absent"
    if path.is_file():
        with path.open("rb") as stream:
            if not stream.read(64).startswith(b"version https://git-lfs.github.com/spec/v1"):
                return
        unavailable = "Git LFS pointer; CSV not materialized"
    if os.environ.get(REQUIRE_PFBAR_CORPUS_ENV, "") not in ("", "0"):
        pytest.fail(f"{REQUIRE_PFBAR_CORPUS_ENV} requires the materialized corpus CSV: "
                    f"{path} ({unavailable})", pytrace=False)
    pytest.skip(f"SKIP private fixtures not present: {path} ({unavailable}); "
                f"set {REQUIRE_PFBAR_CORPUS_ENV}=1 to require the corpus")


@pytest.mark.parametrize("data", [None, b"version https://git-lfs.github.com/spec/v1\n"
                                      b"oid sha256:" + b"0" * 64 + b"\nsize 123\n"],
                         ids=["absent", "lfs-pointer"])
@pytest.mark.parametrize("required", [None, "", "0", "1", "yes"],
                         ids=["unset", "empty", "zero", "one", "nonzero-text"])
def test_corpus_availability_skip_or_required_failure(tmp_path, monkeypatch, data, required):
    path = tmp_path / "corpus.csv"
    if data is not None:
        path.write_bytes(data)
    if required is None:
        monkeypatch.delenv(REQUIRE_PFBAR_CORPUS_ENV, raising=False)
    else:
        monkeypatch.setenv(REQUIRE_PFBAR_CORPUS_ENV, required)
    strict = required not in (None, "", "0")
    with pytest.raises(pytest.fail.Exception if strict else pytest.skip.Exception) as caught:
        _require_materialized_corpus_csv(path)
    message = str(caught.value)
    assert str(path) in message
    assert ("file absent" if data is None else "Git LFS pointer") in message
    if strict:
        assert f"{REQUIRE_PFBAR_CORPUS_ENV} requires" in message
    else:
        assert message.startswith("SKIP private fixtures not present: ")


def test_corpus_availability_accepts_materialized_csv_in_strict_mode(tmp_path, monkeypatch):
    path = source(tmp_path)
    monkeypatch.setenv(REQUIRE_PFBAR_CORPUS_ENV, "1")
    _require_materialized_corpus_csv(path)


def test_converted_corpus_source_values_sha256_matches_load_bars(pf, tmp_path):
    """The L1 acceptance check uses the complete checked-in corpus CSV."""
    path = ROOT / "corpus" / "data" / "ohlcv_ETH-USDT-USDT_1m.csv"
    _require_materialized_corpus_csv(path)
    output = tmp_path / "chart.ETHUSDT.1.bin"
    metadata = pf.convert_csv(path, output, symbol="ETHUSDT", tf="1")
    bars, count, expected = run_json.load_bars(path)
    digest = hashlib.sha256(PREFIX)
    with output.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    assert count == metadata["count"]
    assert digest.hexdigest() == expected
    assert metadata["first_ts"] == bars[0].timestamp
    assert metadata["last_ts"] == bars[count - 1].timestamp
