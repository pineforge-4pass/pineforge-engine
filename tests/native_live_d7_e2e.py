"""Replay externally supplied chart-delivery regression fixtures with the engine oracle."""
import argparse
import csv
import json
from pathlib import Path
import sqlite3
import subprocess

from native_live_chart_input_e2e import config_args, oracle_command
from native_live_report_e2e import action_key, equal, write_csv


def checked(command, destination):
    with destination.open("w") as output, destination.with_suffix(".stderr").open("w") as errors:
        result = subprocess.run(list(map(str, command)), stdout=output, stderr=errors, timeout=1800)
    assert result.returncode == 0, (command, result.returncode, destination.with_suffix(".stderr").read_text())
    return json.loads(destination.read_text())


def rows(path):
    with path.open() as source:
        return [dict(ts_open=int(row["timestamp"]), o=float(row["open"]), h=float(row["high"]),
                     l=float(row["low"]), c=float(row["close"]), v=float(row["volume"]))
                for row in csv.DictReader(source)]


def batched(destination, events):
    pending = []
    with destination.open("w") as output:
        for event in events:
            pending.append(event)
            if len(pending) == 256:
                output.write(json.dumps(dict(type="batch", events=pending), separators=(",", ":")) + "\n")
                pending = []
        if pending:
            output.write(json.dumps(dict(type="batch", events=pending), separators=(",", ":")) + "\n")


def physical_actions(path, split):
    with path.open() as source:
        return [action_key(record) for record in map(json.loads, source)
                if record["origin_input_index"] >= split]


def first_drift(expected, actual):
    try:
        equal(expected, actual)
    except AssertionError as error:
        return str(error.args[0][0])
    return None


def run_case(args, case):
    config = json.loads((case / "config.json").read_text())
    timeframe = config["script_tf"]
    target = args.output / case.name
    target.mkdir(parents=True, exist_ok=True)
    library = args.libraries / case.name / "strategy.so"
    warmup = case / "warmup-chart.csv"
    ledger = target / "bars.sqlite"
    command = [args.runner, "run", "--strategy", library, "--warmup", warmup,
               "--ledger", ledger, "--mode", "bars"] + config_args(config)
    if "d4-" in case.name and config["timezone"] == "America/New_York":
        for mode in ("bars", "ticks"):
            refused = target / f"refused-{mode}.sqlite"
            result = subprocess.run(list(map(str, [args.runner, "run", "--strategy", library,
                "--warmup", warmup, "--ledger", refused, "--mode", mode] + config_args(config))),
                text=True, capture_output=True, timeout=30)
            assert result.returncode == 1 and "daily/weekly chart delivery on a daylight-saving calendar is not supported yet" in result.stderr
            assert not refused.exists() and not Path(str(refused) + ".lock").exists()
        verdict = dict(case=case.name, bars="REFUSED D/W daylight-saving calendar", ticks="REFUSED", timezone=config["timezone"])
        (target / "verdict.json").write_text(json.dumps(verdict, indent=2))
        print(json.dumps(verdict), flush=True)
        return
    warm = rows(warmup)
    live = rows(case / "bars-chart.csv")
    full = target / "full-chart.csv"
    write_csv(full, warm + live)
    feed = target / "bars.jsonl"
    batched(feed, (dict(type="bar", bar=bar) for bar in live))
    batch_actions = target / "batch-actions.jsonl"
    batch = checked(oracle_command(args.oracle, library, full, case / "config.json", batch_actions, timeframe), target / "batch.json")
    assert batch["trades_len"] > 0, (case.name, "vacuous chart reference")
    checked(command + ["--feed", feed], target / "bars-summary.json")
    report = checked([args.runner, "report", "--ledger", ledger], target / "bars-report.json")
    equal(batch, report["report"], case.name + ".bars.report")
    with sqlite3.connect(ledger) as database:
        actual = [action_key(json.loads(row[0])) for row in database.execute("SELECT payload FROM events ORDER BY ordinal")]
    expected = physical_actions(batch_actions, len(warm))
    equal(expected, actual, case.name + ".bars.actions")
    verdict = dict(case=case.name, bars="BITWISE IDENTICAL chart batch", actions=len(actual), trades=batch["trades_len"],
                   chart_net_profit=batch["net_profit"])
    if args.before_runner:
        before_rows = rows(case / "warmup-1m.csv") + rows(case / "bars-1m.csv")
        before_full, before_feed = target / "full-1m.csv", target / "bars-1m.jsonl"
        write_csv(before_full, before_rows)
        batched(before_feed, (dict(type="bar", bar=bar) for bar in before_rows[len(rows(case / "warmup-1m.csv")):]))
        checked([args.before_runner, "run", "--strategy", library, "--warmup", case / "warmup-1m.csv",
                          "--feed", before_feed, "--ledger", target / "before.sqlite", "--mode", "bars",
                          "--input-tf", "1"] + config_args(config), target / "before-summary.json")
        before_report = checked([args.before_runner, "report", "--ledger", target / "before.sqlite"], target / "before-report.json")["report"]
        before_batch = checked([args.oracle, library, before_full, timeframe, target / "before-batch-actions.jsonl",
                                "--input-tf", "1", "--distribution", "3", "--config", case / "config.json",
                                "--calendar-check"], target / "before-batch.json")
        equal(before_batch, before_report, case.name + ".before.input1.reference")
        verdict["before_net_profit"] = before_report["net_profit"]
        excluded = {"input_bars_processed", "input_tf_seconds", "needs_aggregation", "script_tf_ratio"}
        verdict["before_trading_drift"] = first_drift({key: value for key, value in batch.items() if key not in excluded},
                                                     {key: value for key, value in before_report.items() if key not in excluded})
    if not args.bars_only:
        tape = case / "tape.jsonl"
        if not tape.exists():
            with tape.open("wb") as output:
                subprocess.run(["zstd", "-q", "-d", "-c", str(case / "tape.jsonl.zst")], stdout=output, check=True, timeout=300)
        tick_feed = target / "ticks.jsonl"
        with tape.open() as source:
            batched(tick_feed, map(json.loads, source))
        tick_actions = target / "tick-reference-actions.jsonl"
        canonical = checked(oracle_command(args.oracle, library, warmup, case / "config.json", tick_actions, timeframe) +
                            ["--stream-feed", tick_feed], target / "tick-reference.json")
        tick_ledger = target / "ticks.sqlite"
        tick_command = [args.runner, "run", "--strategy", library, "--warmup", warmup, "--feed", tick_feed,
                        "--ledger", tick_ledger, "--mode", "ticks"] + config_args(config)
        checked(tick_command, target / "ticks-summary.json")
        tick_report = checked([args.runner, "report", "--ledger", tick_ledger], target / "ticks-report.json")["report"]
        equal(canonical, tick_report, case.name + ".ticks.canonical")
        with sqlite3.connect(tick_ledger) as database:
            actual = [action_key(json.loads(row[0])) for row in database.execute("SELECT payload FROM events ORDER BY ordinal")]
        equal(physical_actions(tick_actions, len(warm)), actual, case.name + ".ticks.actions")
        verdict["ticks"] = "BITWISE IDENTICAL direct tick canonical"
        verdict["tick_chart_drift"] = first_drift(batch, canonical)
        verdict["tick_net_profit"] = canonical["net_profit"]
    (target / "verdict.json").write_text(json.dumps(verdict, indent=2))
    print(json.dumps(verdict), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runner", type=Path)
    parser.add_argument("oracle", type=Path)
    parser.add_argument("cases", type=Path)
    parser.add_argument("libraries", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--before-runner", type=Path)
    parser.add_argument("--bars-only", action="store_true")
    parser.add_argument("--case")
    args = parser.parse_args()
    for case in sorted(args.cases.iterdir()):
        if case.is_dir() and (case / "bars-chart.csv").exists() and (not args.case or args.case == case.name):
            run_case(args, case)
    print("D7 chart-delivery fixture contract PASS", flush=True)


if __name__ == "__main__":
    main()
