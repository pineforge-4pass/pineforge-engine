"""Compare all cumulative ABI report fields and physical actions to batch."""
import csv
import json
import math
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile


def checked(command, expected=0):
    result = subprocess.run(command, text=True, capture_output=True, timeout=45)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    return result.stdout


def equal(expected, actual, path="report"):
    if isinstance(expected, dict):
        assert expected.keys() == actual.keys(), (path, expected.keys(), actual.keys())
        for name in expected:
            equal(expected[name], actual[name], path + "." + name)
    elif isinstance(expected, list):
        assert len(expected) == len(actual), (path, len(expected), len(actual))
        for index, (left, right) in enumerate(zip(expected, actual)):
            equal(left, right, f"{path}[{index}]")
    elif isinstance(expected, (int, float)):
        assert struct.pack("!d", expected) == struct.pack("!d", actual), (path, expected, actual)
    else:
        assert expected == actual, (path, expected, actual)


def action_key(record):
    order = record["order"]
    return [record["timestamp"], record["bar_index"], order["id"], order["action"],
            order["leg"], order["contracts"], order["price"], order["reduce_only"],
            order["entry_incarnation"]]


def write_csv(path, bars):
    with path.open("w") as output:
        writer = csv.writer(output)
        writer.writerow(["timestamp", "open", "high", "low", "close", "volume"])
        for bar in bars:
            writer.writerow([bar[name] for name in ("ts_open", "o", "h", "l", "c", "v")])


def main():
    runner, oracle, *libraries = sys.argv[1:]
    strategies = 0
    comparisons = 0
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        bars = []
        for index in range(180):
            opening = round(100 + 15 * math.sin(index / 4), 2)
            close = round(100 + 15 * math.sin((index + 1) / 4), 2)
            bars.append(dict(ts_open=1704067200000 + index * 60000, o=opening,
                             h=max(opening, close) + 2, l=min(opening, close) - 2, c=close, v=10))
        warmup = root / "warmup.csv"
        split = 40
        write_csv(warmup, bars[:split])
        feed = root / "feed.jsonl"
        feed.write_text("".join(json.dumps(dict(type="bar", bar=bar)) + "\n" for bar in bars[split:]))
        for number, library in enumerate(libraries):
            for timeframe in (1, 5):
                ledger = root / f"ledger-{number}-{timeframe}.sqlite"
                command = [runner, "run", "--strategy", library, "--warmup", str(warmup),
                           "--feed", str(feed), "--ledger", str(ledger), "--mode", "bars",
                           "--script-tf", str(timeframe), "--symbol", "TEST:EXAMPLE",
                           "--syminfo", "mintick=0.25", "--syminfo", "pointvalue=2.5",
                           "--syminfo", "qty_step=0.001", "--override", "commission_value=0.1",
                           "--override", "slippage=1", "--report-jsonl"]
                mirror = [json.loads(line) for line in checked(command).splitlines()]
                assert len(mirror) == len(bars) - split + 1
                for cursor in (5, 20, 75, 140):
                    snapshot = root / "snapshot.csv"
                    write_csv(snapshot, bars[:split + cursor])
                    batch_actions = root / "batch-actions.jsonl"
                    batch = json.loads(checked([oracle, library, str(snapshot), str(timeframe), str(batch_actions)]))
                    exported = checked([runner, "report", "--ledger", str(ledger), "--at-input", str(cursor)])
                    report = json.loads(exported)
                    equal(batch, report["report"])
                    equal(batch["equity_curve"][-1]["equity"], report["equity"])
                    equal(batch["equity_curve"][-1]["open_profit"], report["open_profit"])
                    equal([trade for trade in batch["trades"] if not trade["open_at_end"]], report["closed_trades"])
                    equal(report, mirror[cursor - 1])
                    with sqlite3.connect(ledger) as database:
                        live = [json.loads(row[0]) for row in database.execute(
                            "SELECT payload FROM events WHERE input_index<? ORDER BY ordinal", (cursor,))]
                        stored = database.execute("SELECT payload FROM report_snapshots WHERE input_cursor=?", (cursor,)).fetchone()[0]
                    assert exported == stored + "\n"
                    expected = [json.loads(line) for line in batch_actions.read_text().splitlines()]
                    expected = [row for row in expected if row["origin_input_index"] >= split]
                    equal([action_key(row) for row in expected], [action_key(row) for row in live], "actions")
                    comparisons += 1
                before = checked([runner, "report", "--ledger", str(ledger)])
                checked(command)
                assert checked([runner, "report", "--ledger", str(ledger)]) == before
                checked([runner, "report", "--ledger", str(ledger), "--at-input", "9999"], 1)
                checked([runner, "report", "--ledger", str(ledger), "--deployment", "wrong"], 1)
            strategies += 1
            print(f"REPORT==BATCH {Path(library).name}: 2 timeframes x 4 cursors; restart exact", flush=True)
    print(f"REPORT==BATCH: {strategies}/{len(libraries)} strategies; {comparisons} cursor comparisons", flush=True)


if __name__ == "__main__":
    main()
