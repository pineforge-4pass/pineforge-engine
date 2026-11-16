"""Unit tests of evidence intake, never substitute TradingView measurements."""
import csv
from decimal import Decimal
from fractions import Fraction
import math
import struct

import pytest

import sma_quantity_reconstruction as intake


def word(value):
    return struct.pack(">d", value).hex()


def forms():
    # Settings descriptors only: no fabricated TV source/capture binding.
    return [{"offset_steps": offset,
             "applied": dict(intake.APPLIED, offset_steps=offset)}
            for offset in intake.OFFSETS]


def test_captured_offset_forms_are_listed_once():
    assert set(intake.offset_cases(forms())) == set(intake.OFFSETS)
    # One captured form is a valid case; the other two are then replayed as
    # native controls and never reported as measured.
    assert set(intake.offset_cases(forms()[:1])) == {"0"}
    with pytest.raises(ValueError, match="at least one"):
        intake.offset_cases([])
    with pytest.raises(ValueError, match="duplicate"):
        intake.offset_cases(forms() + forms()[:1])


@pytest.mark.parametrize("field,value", [
    ("qty_step", "0.0001"),
    ("feed_origin", intake.FIRST - intake.HOUR),
    ("source_gate", "close"),
    ("account_fx", "0.999"),
    ("initial_capital", "100000"),
    ("calc_on_order_fills", True),
])
def test_applied_state_cannot_be_silently_changed(field, value):
    items = forms()
    items[0]["applied"][field] = value
    with pytest.raises(ValueError, match="applied settings"):
        intake.offset_cases(items)


def test_quantity_comparison_preserves_one_step_and_one_ulp():
    exact = intake.native_decimal(word(0.125))
    below = intake.native_decimal(word(0.12499))
    adjacent = intake.native_decimal(word(math.nextafter(0.125, 1.0)))
    assert exact == Decimal("0.125")
    assert (exact - below) / intake.STEP == 1
    assert adjacent > exact
    assert 0 < (adjacent - exact) / intake.STEP < 1


def test_lot_steps_separate_a_step_from_a_last_place():
    exact = intake.native_decimal(word(0.125))
    below = intake.native_decimal(word(0.12499))
    adjacent = intake.native_decimal(word(math.nextafter(0.125, 1.0)))
    assert intake.lot_steps(exact) == (12500, 0)
    assert intake.lot_steps(below) == (12499, 0)
    lots, residue = intake.lot_steps(adjacent)
    # One unit in the last place is the same lot with a visible residue: a
    # one-step difference and a representation difference stay apart.
    assert lots == 12500 and 0 < residue < intake.STEP / 1000000


@pytest.mark.parametrize("value", ["0", "z" * 16, "0" * 17])
def test_native_words_are_not_decimal_strings(value):
    with pytest.raises(ValueError, match="binary64"):
        intake.binary(value)


def test_nonfinite_trade_quantity_is_an_input_error():
    with pytest.raises(ValueError, match="nonfinite native"):
        intake.native_decimal(word(math.nan))


def test_digest_failure_never_becomes_a_behavior_difference(tmp_path):
    path = tmp_path / "source.pine"
    path.write_text("untrusted input\n", encoding="utf-8")
    with pytest.raises(ValueError, match="input digest differs"):
        intake.bound_file(tmp_path, {"path": path.name, "sha256": "0" * 64})


def test_tv_header_requires_quote_currency(tmp_path):
    path = tmp_path / "untrusted.csv"
    path.write_text("Trade number,Type,Price USD\n", encoding="utf-8")
    with pytest.raises(ValueError, match="quote-currency"):
        intake.tv_trades(path)


def test_duplicate_raw_tv_legs_are_rejected_without_dropping_rows(tmp_path):
    # Deliberately invalid parser data, not an expected trade tape.
    path = tmp_path / "duplicate.csv"
    columns = ["Trade number", "Type", "Date and time", "Signal", "Price USDT",
               "Size (qty)", "Net PnL USDT", "Commission USDT"]
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for _ in range(2 * len(intake.SLOTS)):
            writer.writerow(dict(zip(columns, [
                "1", "Entry long", "2024-01-01 11:00", "invalid parser data",
                "125", "0.125", "0", "0",
            ])))
    with pytest.raises(ValueError, match="duplicate TV leg"):
        intake.tv_trades(path)


def test_mathematical_boundary_label_does_not_create_a_fill_oracle():
    prices = [{"close": "125"}] * 3
    assert intake.rational_boundary(prices, 2, "0")
    assert not intake.rational_boundary(prices, 2, "-0.25")
    assert not intake.rational_boundary(prices, 2, "0.25")
    assert intake.exact_lots(prices, 2, "0") == 12500
    assert intake.exact_lots(prices, 2, "-0.25") == Fraction(49999, 4)


def test_signal_values_decode_to_their_exact_doubles():
    # Parser data in the script's own encoding: whole + fraction / 2**52.
    assert intake.dyadic("3:2251799813685248") == 3.5
    assert intake.dyadic("42556:2837267765231616") == 42556 + 2837267765231616 / 2 ** 52
    assert math.isnan(intake.dyadic("unsupported:NaN"))
    # 2**-52 beside 42556 is below that double's last place: a text that is
    # not the encoding of one double is refused, never rounded.
    with pytest.raises(ValueError, match="not an exact double"):
        intake.dyadic("42556:1")
    for text in ("unsupported:0.5", "42556", "42556:4503599627370496", "0:1", "-1:0"):
        with pytest.raises(ValueError, match="signal value"):
            intake.dyadic(text)


def test_signal_is_read_whole_or_refused():
    text = "i731|t=1704074400000|b=2|s=3:2251799813685248|s1=unsupported:NaN|q=1:0"
    decoded = intake.signal(text)
    assert (decoded["time"], decoded["bar"]) == (1704074400000, 2)
    assert decoded["sma"] == 3.5 and decoded["request"] == 1.0
    assert math.isnan(decoded["previous_sma"])
    with pytest.raises(ValueError, match="unreadable signal"):
        intake.signal(text + "|extra")
    with pytest.raises(ValueError, match="unreadable signal"):
        intake.signal("invalid parser data")


def test_signal_words_compare_bit_for_bit():
    assert intake.same_word(word(3.5), 3.5)
    assert not intake.same_word(word(math.nextafter(3.5, 4.0)), 3.5)
    assert intake.same_word(word(math.nan), math.nan)
    assert not intake.same_word(word(3.5), math.nan)
    assert not intake.same_word(word(math.nan), 3.5)
