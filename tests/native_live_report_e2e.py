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
    result = subprocess.run(command, text=True, capture_output=True)
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
    elif isinstance(expected, int):
        assert type(expected) is type(actual) and expected == actual, (path, expected, actual)
    elif isinstance(expected, float):
        assert struct.pack("!d", expected) == struct.pack("!d", actual), (path, expected, actual)
    else:
        assert expected == actual, (path, expected, actual)


def merge_fields(destination, changes):
    if not isinstance(changes, dict):
        return changes
    if not isinstance(destination, dict):
        destination = {}
    for name, value in changes.items():
        destination[name] = merge_fields(destination.get(name), value)
    return destination


def apply_delta(document, delta):
    assert delta["schema_version"] == "pineforge-native-report-delta/v1"
    merge_fields(document, delta["fields"])
    for name, patch in delta["arrays"].items():
        owner = document if name == "closed_trades" else document.setdefault("report", {})
        values = owner.setdefault(name, [])
        offset = int(patch["offset"])
        assert offset <= len(values), (name, offset, len(values))
        del values[offset:]
        values.extend(patch["items"])


def output_reports(output, schema):
    lines = output.splitlines()
    reports = [json.loads(line) for line in lines[:-1]]
    assert all(row["schema_version"] == schema for row in reports)
    return reports, sum(len(line.encode("utf-8")) + 1 for line in lines[:-1])


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
    for expected, actual in (((1 << 64) - 1, (1 << 64) - 2), (1, True)):
        try:
            equal(expected, actual)
        except AssertionError:
            pass
        else:
            raise AssertionError("report equality loses integer precision or type")
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
        feed = root / "feed.jsonl"
        seed_bars = bars
        for number, library in enumerate(libraries):
            for timeframe in (1, 5):
                bars = [{**bar, "ts_open": 1704067200000 + index * timeframe * 60000}
                        for index, bar in enumerate(seed_bars)]
                write_csv(warmup, bars[:split])
                feed.write_text("".join(json.dumps(dict(type="bar", bar=bar)) + "\n" for bar in bars[split:]))
                ledger = root / f"ledger-{number}-{timeframe}.sqlite"
                full_command = [runner, "run", "--strategy", library, "--warmup", str(warmup),
                           "--feed", str(feed), "--ledger", str(ledger), "--mode", "bars",
                           "--script-tf", str(timeframe), "--symbol", "TEST:EXAMPLE",
                           "--syminfo", "mintick=0.25", "--syminfo", "pointvalue=2.5",
                           "--syminfo", "qty_step=0.001", "--override", "commission_value=0.1",
                           "--override", "slippage=1", "--report-jsonl=full"]
                full_lines = checked(full_command).splitlines()
                full_mirror = [json.loads(line) for line in full_lines[:-1]]
                assert len(full_mirror) == len(bars) - split
                assert all(row["schema_version"] == "pineforge-native-report/v1"
                           for row in full_mirror)

                delta_ledger = root / f"delta-ledger-{number}-{timeframe}.sqlite"
                delta_command = full_command.copy()
                delta_command[delta_command.index(str(ledger))] = str(delta_ledger)
                delta_command[-1] = "--report-jsonl"
                delta_lines = checked(delta_command).splitlines()
                delta_mirror = [json.loads(line) for line in delta_lines[:-1]]
                assert len(delta_mirror) == len(full_mirror)
                assert all(row["schema_version"] == "pineforge-native-report-delta/v1"
                           for row in delta_mirror)
                reconstructed = {}
                for cursor, delta in enumerate(delta_mirror, 1):
                    apply_delta(reconstructed, delta)
                    equal(full_mirror[cursor - 1], reconstructed, "delta stream")
                assert checked([runner, "report", "--ledger", str(delta_ledger)]) == full_lines[-2] + "\n"
                checked(full_command[:-1] + ["--report-jsonl=unsupported"], 1)
                for cursor in (5, 20, 75, 140):
                    snapshot = root / "snapshot.csv"
                    write_csv(snapshot, bars[:split + cursor])
                    batch_actions = root / "batch-actions.jsonl"
                    batch = json.loads(checked([oracle, library, str(snapshot), str(timeframe), str(batch_actions),
                                               "--input-tf", str(timeframe), "--distribution", "3"]))
                    exported = checked([runner, "report", "--ledger", str(ledger), "--at-input", str(cursor)])
                    report = json.loads(exported)
                    equal(batch, report["report"])
                    equal(batch["equity_curve"][-1]["equity"], report["equity"])
                    equal(batch["equity_curve"][-1]["open_profit"], report["open_profit"])
                    equal([trade for trade in batch["trades"] if not trade["open_at_end"]], report["closed_trades"])
                    equal(report, full_mirror[cursor - 1])
                    with sqlite3.connect(ledger) as database:
                        live = [json.loads(row[0]) for row in database.execute(
                            "SELECT payload FROM events WHERE input_index<? ORDER BY ordinal", (cursor,))]
                        stored = database.execute("SELECT payload FROM report_snapshots WHERE input_cursor=?", (cursor,)).fetchone()[0]
                    assert json.loads(stored)["schema_version"] == "pineforge-native-report-delta/v1"
                    assert exported == full_lines[cursor - 1] + "\n"
                    expected = [json.loads(line) for line in batch_actions.read_text().splitlines()]
                    expected = [row for row in expected if row["origin_input_index"] >= split]
                    equal([action_key(row) for row in expected], [action_key(row) for row in live], "actions")
                    comparisons += 1
                if number == 0 and timeframe == 1:
                    growth_bytes = {}
                    growth_feed = root / "growth-feed.jsonl"
                    for count in (8, 80):
                        growth_feed.write_text("".join(
                            json.dumps(dict(type="bar", bar=bar)) + "\n"
                            for bar in bars[split:split + count]))
                        for mode, schema in (("delta", "pineforge-native-report-delta/v1"),
                                             ("full", "pineforge-native-report/v1")):
                            growth_ledger = root / f"growth-{mode}-{count}.sqlite"
                            growth_command = full_command.copy()
                            growth_command[growth_command.index(str(feed))] = str(growth_feed)
                            growth_command[growth_command.index(str(ledger))] = str(growth_ledger)
                            growth_command[-1] = "--report-jsonl" if mode == "delta" else "--report-jsonl=full"
                            reports, written = output_reports(checked(growth_command), schema)
                            assert len(reports) == count
                            growth_bytes[(mode, count)] = written
                    assert growth_bytes[("delta", 80)] < 12 * growth_bytes[("delta", 8)]
                    assert growth_bytes[("full", 80)] > 12 * growth_bytes[("full", 8)]
                if "bracket-atr-trailing-stop-state-01" in library and timeframe == 5:
                    write_csv(snapshot, bars[:split + 3])
                    partial = json.loads(checked([oracle, library, str(snapshot), "5", str(batch_actions),
                                                 "--input-tf", "5", "--distribution", "3"]))
                    live_partial = json.loads(checked([runner, "report", "--ledger", str(ledger), "--at-input", "3"]))["report"]
                    equal(partial, live_partial, "confirmed chart-bar trailing state")
                    print("CHART BAR: trailing equity, trades and every report field BITWISE IDENTICAL", flush=True)
                before = checked([runner, "report", "--ledger", str(ledger)])
                with sqlite3.connect(ledger) as database:
                    database.execute("DROP TABLE report_integrity")
                checked([runner, "report", "--ledger", str(ledger)], 1)
                checked(full_command)
                assert checked([runner, "report", "--ledger", str(ledger)]) == before
                checked([runner, "report", "--ledger", str(ledger), "--at-input", "9999"], 1)
                checked([runner, "report", "--ledger", str(ledger), "--deployment", "wrong"], 1)
            with sqlite3.connect(ledger) as database:
                database.execute("DROP TRIGGER report_no_update")
                database.execute("UPDATE report_snapshots SET payload=payload || ' ' WHERE input_cursor=140")
            for export_cursor in (0, 140):
                refused = checked([runner, "report", "--ledger", str(ledger), "--at-input", str(export_cursor)], 1)
                assert not refused
            strategies += 1
            print(f"REPORT==BATCH {Path(library).name}: 2 timeframes x 4 cursors; restart exact", flush=True)
    print(f"REPORT==BATCH: {strategies}/{len(libraries)} strategies; {comparisons} cursor comparisons", flush=True)


if __name__ == "__main__":
    main()
