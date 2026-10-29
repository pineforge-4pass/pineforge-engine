"""Replay values through the production CLI and a real strategy module.

CTest supplies the CMake-built replay_strategy module. It submits the
same C-ABI orders as test_replay_abi; no accessor, engine, report builder,
serializer, or CLI option is replaced. Oracles use only the input prices and
the scenario's integer quantity, pointvalue and commission.
"""
from __future__ import annotations

import csv
import json
import math
import os
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
START = 1_704_067_200_000
STEP = 60_000
PROFIT_PRICES = (100, 100, 107, 109, 120, 116, 130, 140)
LOSS_PRICES = (100, 100, 97, 99, 120, 126, 130, 143)


@pytest.fixture(scope="module")
def replay_library():
    configured = os.environ.get("PINEFORGE_REPLAY_LIBRARY")
    assert configured, "CTest must supply the CMake-built replay strategy module"
    library = Path(configured)
    assert library.is_file(), f"required real strategy module is missing: {library}"
    return library


def _reject_constant(token):
    raise AssertionError(f"non-JSON numeric constant: {token}")


def _run_cli(library, tmp_path, prices, *, capital, replay, trade=True):
    bars = tmp_path / "bars.csv"
    with bars.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(("timestamp", "open", "high", "low", "close", "volume"))
        for index, price in enumerate(prices):
            writer.writerow((START + index * STEP, price, price, price, price, 10))
    syminfo = tmp_path / "syminfo.json"
    syminfo.write_text(
        json.dumps({"syminfo": {"mintick": 1, "pointvalue": 1,
                                "timezone": "UTC", "session": "24x7"}}),
        encoding="utf-8",
    )
    command = [
        sys.executable, "-B", str(ROOT / "docker/run_json.py"),
        "--so", str(library), "--ohlcv", str(bars),
        "--input-tf", "1", "--script-tf", "1",
        "--bar-magnifier", "false", "--syminfo", str(syminfo),
        "--inputs", json.dumps({"trade": "true" if trade else "false"}),
        "--overrides", json.dumps({"initial_capital": capital}),
    ]
    if replay:
        command.append("--report-replay")
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    detail = f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    assert result.returncode == 0, detail
    lines = result.stdout.splitlines()
    assert len(lines) == 1, detail
    document = json.loads(lines[0], parse_constant=_reject_constant)
    assert not document.get("error"), detail
    return document


@pytest.mark.parametrize(
    "prices,capital",
    [(PROFIT_PRICES, 12_345), (LOSS_PRICES, 5_000)],
    ids=["positive-realized-fold", "negative-realized-fold"],
)
def test_report_replay_cli_matches_independent_engine_close_fold(
        replay_library, tmp_path, prices, capital):
    # The one two-lot long enters at bar 1; its slices exit at bars 2 and 3.
    # The short enters at bar 4 and exits at bar 5. The last long (bar 6)
    # remains open at EOF. Nothing below reads an engine value to form expected.
    closes = (
        (prices[1], prices[2], True),
        (prices[1], prices[3], True),
        (prices[4], prices[5], False),
    )
    quantity = pointvalue = 1
    entry_commission = exit_commission = 1
    per_close = []
    expected = 0.0
    for entry, exit, is_long in closes:
        points = exit - entry if is_long else entry - exit
        whole_lot_pnl = (
            points * quantity * pointvalue - entry_commission - exit_commission
        )
        per_close.append(whole_lot_pnl)
        expected += whole_lot_pnl  # Plain left fold in physical-close order.
    # Profit: (7 - 2) + (9 - 2) + (4 - 2) = 14.
    # Loss:  (-3 - 2) + (-1 - 2) + (-6 - 2) = -16.

    on = _run_cli(
        replay_library, tmp_path, prices, capital=capital, replay=True,
    )
    assert on["replay"] == {
        "initial_capital": capital,
        "pointvalue": pointvalue,
        "net_profit_sum": expected,
    }
    assert all(not isinstance(value, bool) and isinstance(value, (int, float))
               and math.isfinite(value) for value in on["replay"].values())
    # The actual equity series independently checks that the capital override
    # reached the run, rather than merely being echoed in replay.
    assert on["equity_curve"][0]["equity"] == capital
    rows = on["trades"]
    assert len(rows) == 4
    assert [row["open_at_end"] for row in rows] == [False, False, False, True]
    for row, (entry, exit, is_long), pnl in zip(rows[:3], closes, per_close):
        assert row["entry_price"] == entry and row["exit_price"] == exit
        assert row["side"] == ("long" if is_long else "short")
        assert row["qty"] == quantity
        assert row["commission"] == entry_commission + exit_commission
        assert row["pnl"] == pnl
    assert rows[0]["entry_time"] == rows[1]["entry_time"] == START + STEP
    assert rows[0]["entry_bar_index"] == rows[1]["entry_bar_index"]
    assert rows[0]["entry_incarnation"] != 0
    assert rows[0]["entry_incarnation"] == rows[1]["entry_incarnation"]
    assert rows[-1]["side"] == "long" and rows[-1]["qty"] == 1
    assert rows[-1]["entry_price"] == prices[6]
    assert rows[-1]["entry_time"] == START + 6 * STEP
    assert math.isfinite(rows[-1]["pnl"]) and rows[-1]["pnl"] != 0
    # Only a negative control: the presented-row sum includes the open row.
    presented_sum = 0.0
    for row in rows:
        presented_sum += row["pnl"]
    assert presented_sum != expected
    assert on["replay"]["net_profit_sum"] != presented_sum
    assert on["replay"]["net_profit_sum"] != capital + expected

    off = _run_cli(
        replay_library, tmp_path, prices, capital=capital, replay=False,
    )
    assert "replay" not in off
    # Replay adds only these two trade members (ANSWER section 2 R2/R3).
    assert off["trades"] == [
        {key: value for key, value in row.items()
         if key not in ("settlement_index", "recut")}
        for row in rows
    ]


def test_report_replay_cli_zero_trade_run_is_positive_zero(
        replay_library, tmp_path):
    document = _run_cli(
        replay_library, tmp_path, PROFIT_PRICES,
        capital=7_000, replay=True, trade=False,
    )
    assert document["trades"] == []
    assert document["summary"]["total_trades"] == 0
    assert document["replay"]["initial_capital"] == 7_000
    assert document["replay"]["pointvalue"] == 1
    raw_sum = document["replay"]["net_profit_sum"]
    assert not isinstance(raw_sum, bool) and isinstance(raw_sum, (int, float))
    assert raw_sum == 0.0
    assert math.copysign(1.0, raw_sum) == 1.0
