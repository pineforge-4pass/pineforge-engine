"""Full-report and physical-action equivalence for pending-entry brackets."""
import argparse
import itertools
import json
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from native_live_report_e2e import action_key, equal, write_csv


def checked(command):
    result = subprocess.run(command, text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return json.loads(result.stdout)


def tape(timeframe, timing, direction, exit_kind, offset):
    result = []
    exit_bar = 2 if timing == "same" else 3
    rising = (direction == "short") == (exit_kind == "stop")
    sign = 1 if rising else -1
    for index in range(5 * timeframe):
        script_index, child = divmod(index, timeframe)
        opening = close = 100.0
        high, low = 100.1, 99.9
        if script_index == exit_bar and child >= min(3, timeframe - 1):
            close = 100.0 + sign * 5
            opening = 100.0 if timeframe == 1 else close
            high, low = max(100.1, opening, 100.0 + sign * 10), min(99.9, opening, 100.0 + sign * 10)
        result.append(dict(ts_open=1743465600000 + index * 60000,
                           o=opening + offset, h=high + offset,
                           l=low + offset, c=close + offset, v=10))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("oracle")
    parser.add_argument("library")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--unprimed", action="store_true")
    parser.add_argument("--distribution", type=int, choices=range(6))
    args = parser.parse_args()
    temporary = None
    if args.output is None:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
    else:
        root = args.output
        root.mkdir(parents=True, exist_ok=True)
    rows = []
    for timeframe, entry, timing, direction, exit_kind, offset in itertools.product(
            (1, 5, 15, 60), ("market", "stop", "limit"), ("same", "later"),
            ("long", "short"), ("stop", "limit"), (0.0, 0.003)):
        name = f"tf{timeframe}-{entry}-{timing}-{direction}-{exit_kind}-{'off' if offset else 'on'}"
        directory = root / name
        directory.mkdir(parents=True, exist_ok=True)
        bars = tape(timeframe, timing, direction, exit_kind, offset)
        full, warmup, feed = directory / "full.csv", directory / "warmup.csv", directory / "feed.jsonl"
        write_csv(full, bars)
        write_csv(warmup, bars[:timeframe])
        feed.write_text("".join(json.dumps(dict(type="bar", bar=bar)) + "\n" for bar in bars[timeframe:]))
        config = dict(symbol="TEST:BRACKET", session="24x7", timezone="UTC", chart_timezone="UTC",
                      syminfo=[["type", "stock"], ["mintick", "0.01"], ["pointvalue", "1"], ["qty_step", "1"]],
                      inputs=[["Entry kind", entry], ["Exit timing", timing], ["Direction", direction],
                              ["Exit kind", exit_kind], ["Price offset", str(offset)]], overrides=[])
        config["inputs"].append(["Pre-arm exit", "false" if args.unprimed else "true"])
        if args.distribution is not None:
            config["magnifier_distribution"] = args.distribution
        configuration = directory / "config.json"
        configuration.write_text(json.dumps(config))
        batch_actions = directory / "batch-actions.jsonl"
        batch = checked([args.oracle, args.library, str(full), str(timeframe), str(batch_actions),
                         "--config", str(configuration)])
        (directory / "batch-report.json").write_text(json.dumps(batch, sort_keys=True) + "\n")
        assert batch["trades_len"] == 1 and not batch["trades"][0]["open_at_end"], (name, batch["trades"])
        ledger = directory / "ledger.sqlite"
        command = [args.runner, "run", "--strategy", args.library, "--warmup", str(warmup),
                   "--feed", str(feed), "--ledger", str(ledger), "--mode", "bars", "--input-tf", "1",
                   "--script-tf", str(timeframe), "--symbol", config["symbol"], "--session", "24x7",
                   "--timezone", "UTC", "--chart-timezone", "UTC"]
        for key, value in config["syminfo"]:
            command.extend(["--syminfo", f"{key}={value}"])
        for key, value in config["inputs"]:
            command.extend(["--input", f"{key}={value}"])
        checked(command)
        stream = checked([args.runner, "report", "--ledger", str(ledger)])["report"]
        (directory / "stream-report.json").write_text(json.dumps(stream, sort_keys=True) + "\n")
        with sqlite3.connect(ledger) as database:
            actual = [json.loads(row[0]) for row in database.execute("SELECT payload FROM events ORDER BY ordinal")]
        (directory / "stream-actions.jsonl").write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in actual))
        expected = [json.loads(line) for line in batch_actions.read_text().splitlines()]
        differences = []
        for label, left, right in (("report", batch, stream),
                                   ("actions", [action_key(row) for row in expected], [action_key(row) for row in actual])):
            try:
                equal(left, right, name + "." + label)
            except AssertionError as error:
                differences.append(str(error))
        rows.append(dict(cell=name, timeframe=timeframe, entry=entry, timing=timing, direction=direction,
                         exit_kind=exit_kind, offset=offset, equal=not differences, differences=differences,
                         batch_trade=batch["trades"][0], stream_trade=stream["trades"][0]))
        print(f"{name}: {'DIVERGES ' + '; '.join(differences) if differences else 'BITWISE IDENTICAL'}", flush=True)
    (root / "matrix.json").write_text(json.dumps(rows, indent=2) + "\n")
    failures = sum(not row["equal"] for row in rows)
    print(f"same calculation bracket: {len(rows)} cells, {failures} divergences", flush=True)
    if temporary is not None:
        temporary.cleanup()
    if failures and not args.measure:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
