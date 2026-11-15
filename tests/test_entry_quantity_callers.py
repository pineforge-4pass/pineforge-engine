"""Issue #716 entry patterns through the production JSON and LIVE callers.

The modules are hand-written translations of the two literal entry patterns,
not codegen or parser tests. They link the real PineStrategyHost/runtime and
provide no metadata setter, quantity calculator, stream implementation or
report accessor. CTest must supply both CMake-built modules.

The retained TradingView exports are BINANCE:BTCUSDT, 60 minutes,
2024-01-01..2024-03-01, exported 2026-11-14 at 14:50:24.088Z (item 1)
and 14:50:24.961Z (item 2). Their quantities/prices and quote-currency PnL
are independent value oracles. Their display timezone is unknown: timestamps
below are checked against the input feed, never an inferred TV timezone.
Funding/precedence/gridless controls use Decimal arithmetic, not TV authority.
"""
from dataclasses import dataclass
import csv
from decimal import Decimal, ROUND_FLOOR, ROUND_HALF_UP
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[1]
FEED = ROOT / "tests/fixtures/qty_step_mincontract/btc60/feed/bars.csv"
TAPES = ROOT / "tests/fixtures/entry_quantity"
STEP = Decimal("0.00001")
EXPLICIT = "0.123456789"


@dataclass(frozen=True)
class Case:
    pattern: str
    metadata: dict
    grid: Decimal | None
    capital: str = "100000"
    explicit_control: bool = False
    tv_item: str | None = None


DECLARATIONS = (
    ("step-only", {"qty_step": 0.00001}),
    ("minimum-only", {"mincontract": 0.00001}),
    ("explicit-first", {"qty_step": 0.00001, "mincontract": 0.001}),
    ("minimum-first", {"mincontract": 0.001, "qty_step": 0.00001}),
)
CASES = [
    pytest.param(Case(pattern, metadata, STEP,
                      tv_item=item if name in ("step-only", "minimum-only") else None),
                 id=f"{item}-{name}")
    for pattern, item in (("percent", "item1"), ("explicit", "item2"))
    for name, metadata in DECLARATIONS
] + [
    pytest.param(Case("percent", {}, None), id="item1-gridless-declined"),
    pytest.param(Case("explicit", {}, None), id="item2-gridless-raw"),
    pytest.param(Case("percent", {"qty_step": 0.00001}, STEP, capital="42613.57"),
                 id="item1-affordable-control"),
    pytest.param(Case("percent", {"qty_step": 0.00001}, STEP, capital="42613.56"),
                 id="item1-unaffordable-control"),
    pytest.param(Case("percent", {"qty_step": 0.00001}, STEP, explicit_control=True),
                 id="item1-explicit-control"),
]


def required_file(variable):
    value = os.environ.get(variable)
    assert value, f"CTest must supply {variable}"
    path = Path(value)
    assert path.is_file(), f"required CMake-built fixture is missing: {path}"
    return path


@pytest.fixture(scope="module")
def modules():
    return {
        "percent": required_file("PINEFORGE_ENTRY_PERCENT_LIBRARY"),
        "explicit": required_file("PINEFORGE_ENTRY_EXPLICIT_LIBRARY"),
    }


def bars_for(case):
    with FEED.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    assert len(rows) == 1441
    assert rows[1]["close"] == "42613.56"
    assert rows[2]["open"] == "42613.57"
    assert rows[-1]["close"] == "61501.11"
    assert [int(row["timestamp"]) for row in rows] == [
        1704067200000 + index * 3600000 for index in range(1441)
    ]
    # Retain the whole measured history for literal-pattern/TV rows.
    # Controls need only the signal, fill and one later observation.
    return rows if case.tv_item else rows[:4]


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def checked_json(command):
    result = subprocess.run(list(map(str, command)), cwd=ROOT,
                            capture_output=True, text=True)
    observed = f"command={command!r}\nstdout={result.stdout}\nstderr={result.stderr}"
    # A LIVE refusal of mincontract is a caller admission failure, not a
    # quantity observation. Missing modules/import/build failures are setup
    # failures and must never be reported as the intended quantity RED.
    assert result.returncode == 0, observed
    document = json.loads(result.stdout)
    assert "error" not in document, observed
    return document


def quantity_contract(case, rows):
    capital = Decimal(case.capital)
    explicit = case.pattern == "explicit" or case.explicit_control
    raw = Decimal(EXPLICIT) if explicit else capital / Decimal(rows[1]["close"])
    quantity = raw if case.grid is None else (
        (raw / case.grid).to_integral_value(rounding=ROUND_FLOOR) * case.grid
    )
    affordable = quantity > 0 and quantity * Decimal(rows[2]["open"]) <= capital
    return quantity, affordable


def assert_values(case, rows, trades, history):
    quantity, affordable = quantity_contract(case, rows)
    assert history == (len(rows), len(rows))
    # This is the intended item-1 RED when JSON drops qty_step: the unfloored
    # default quantity costs more than the available capital at the next open.
    assert len(trades) == (1 if affordable else 0), (case, quantity, trades)
    if not affordable:
        return
    trade = trades[0]
    actual = Decimal(str(trade["qty"]))
    tolerance = max(Decimal("1e-12"), abs(quantity) * Decimal("1e-12"))
    # The intended item-2 RED is the retained extra digits, not a runner error.
    assert abs(actual - quantity) <= tolerance, (case, quantity, trade)
    assert Decimal(str(trade["entry_price"])) == Decimal(rows[2]["open"])
    assert trade["entry_time"] == int(rows[2]["timestamp"])
    assert trade["entry_bar_index"] == 2
    assert trade["exit_time"] == int(rows[-1]["timestamp"])
    assert Decimal(str(trade["exit_price"])) == Decimal(rows[-1]["close"])
    assert trade["open_at_end"] in (True, 1)
    assert trade["commission"] == 0
    expected_pnl = quantity * (Decimal(rows[-1]["close"]) - Decimal(rows[2]["open"]))
    cent = Decimal("0.01")
    assert Decimal(str(trade["pnl"])).quantize(cent, rounding=ROUND_HALF_UP) == (
        expected_pnl.quantize(cent, rounding=ROUND_HALF_UP)
    )
    if case.tv_item:
        with (TAPES / f"{case.tv_item}.tv_trades.csv").open(
                newline="", encoding="utf-8-sig") as handle:
            tape = list(csv.DictReader(handle))
        entries = [row for row in tape if row["Type"] == "Entry long"]
        exits = [row for row in tape if row["Type"] == "Exit long"]
        assert len(entries) == len(exits) == 1
        entry, exit_row = entries[0], exits[0]
        assert quantity == Decimal(entry["Size (qty)"])
        assert abs(actual - Decimal(entry["Size (qty)"])) <= tolerance
        assert Decimal(str(trade["entry_price"])) == Decimal(entry["Price USDT"])
        assert Decimal(str(trade["exit_price"])) == Decimal(exit_row["Price USDT"])
        assert Decimal(str(trade["pnl"])).quantize(cent, rounding=ROUND_HALF_UP) == (
            Decimal(entry["Net PnL USDT"]).quantize(cent, rounding=ROUND_HALF_UP)
        )


@pytest.mark.parametrize("case", CASES)
def test_json_entry_quantity(modules, tmp_path, case):
    rows = bars_for(case)
    bars, metadata = tmp_path / "bars.csv", tmp_path / "syminfo.json"
    write_csv(bars, rows)
    metadata.write_text(json.dumps({"syminfo": {
        "mintick": 0.01, "pointvalue": 1, "timezone": "UTC", "session": "24x7",
        **case.metadata,
    }}), encoding="utf-8")
    document = checked_json([
        sys.executable, "-B", ROOT / "docker/run_json.py",
        "--so", modules[case.pattern], "--ohlcv", bars,
        "--input-tf", "60", "--script-tf", "60", "--bar-magnifier", "false",
        "--syminfo", metadata, "--overrides", json.dumps({"initial_capital": case.capital}),
        "--inputs", json.dumps({"Quantity": EXPLICIT} if case.explicit_control else {}),
    ])
    assert_values(case, rows, document["trades"], (
        document["diagnostics"]["input_bars_processed"],
        document["diagnostics"]["script_bars_processed"],
    ))
    assert document["equity_curve"][0]["equity"] == float(case.capital)
    for trade in document["trades"]:
        assert trade["side"] == "long" and trade["entry_incarnation"] > 0


@pytest.mark.parametrize("case", CASES)
def test_live_entry_quantity(modules, tmp_path, case):
    runner = required_file("PINEFORGE_ENTRY_LIVE_RUNNER")
    rows = bars_for(case)
    warmup, feed, ledger = (tmp_path / name for name in
                             ("warmup.csv", "feed.jsonl", "ledger.sqlite"))
    # The signal is in the live leg, not historical warmup.
    write_csv(warmup, rows[:1])
    with feed.open("w", encoding="utf-8") as handle:
        for row in rows[1:]:
            bar = dict(zip(("o", "h", "l", "c", "v"),
                           (float(row[key]) for key in
                            ("open", "high", "low", "close", "volume"))))
            bar["ts_open"] = int(row["timestamp"])
            handle.write(json.dumps({"type": "bar", "bar": bar}) + "\n")
    command = [
        runner, "run", "--strategy", modules[case.pattern],
        "--warmup", warmup, "--feed", feed, "--ledger", ledger,
        "--mode", "bars", "--input-tf", "60", "--script-tf", "60",
        "--symbol", "BINANCE:BTCUSDT", "--timezone", "UTC",
        "--chart-timezone", "UTC", "--session", "24x7",
        "--override", f"initial_capital={case.capital}",
        "--syminfo", "mintick=0.01", "--syminfo", "pointvalue=1",
    ]
    for key, value in case.metadata.items():
        command += ["--syminfo", f"{key}={value}"]
    if case.explicit_control:
        command += ["--input", f"Quantity={EXPLICIT}"]
    checked_json(command)
    snapshot = checked_json([runner, "report", "--ledger", ledger])
    report = snapshot["report"]
    assert_values(case, rows, report["trades"], (
        report["input_bars_processed"], report["script_bars_processed"],
    ))
    assert report["equity_curve"][0]["equity"] == float(case.capital)
    for trade in report["trades"]:
        assert trade["is_long"] == 1
    assert snapshot["closed_trades"] == []
