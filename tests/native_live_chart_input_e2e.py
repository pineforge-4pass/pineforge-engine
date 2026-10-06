"""Chart-clock warmup, confirmed bars and minute/chart tick-boundary proofs."""
import argparse
from datetime import datetime, timedelta
import json
import math
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from zoneinfo import ZoneInfo

from native_live_report_e2e import action_key, equal, write_csv


def checked(command):
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=180)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return json.loads(result.stdout)


def chart_rows(timeframe, session=False):
    starts = []
    zone = ZoneInfo("America/New_York")
    if session:
        day = datetime(2025, 3, 7, 9, 30, tzinfo=zone)
        while len(starts) < 96:
            if day.weekday() < 5:
                starts.extend(int((day + timedelta(minutes=15 * index)).timestamp() * 1000)
                              for index in range(26))
            day += timedelta(days=1)
    elif timeframe in ("D", "W"):
        day = datetime(2025, 3, 3, tzinfo=zone)
        starts = [int((day + timedelta(days=index * (7 if timeframe == "W" else 1))).timestamp() * 1000)
                  for index in range(96)]
    else:
        starts = [1735689600000 + index * int(timeframe) * 60000 for index in range(96)]
    rows = []
    for index, stamp in enumerate(starts[:96]):
        opening = round(100 + 15 * math.sin(index / 4), 2)
        close = round(100 + 15 * math.sin((index + 1) / 4), 2)
        rows.append(dict(ts_open=stamp, o=opening, h=max(opening, close) + 2,
                         l=min(opening, close) - 2, c=close, v=4))
    return rows


def config_for(timeframe, session=False):
    return dict(symbol="TEST:EXAMPLE", script_tf=timeframe,
                timezone="America/New_York" if session or timeframe in ("D", "W") else "UTC",
                chart_timezone="UTC", session="0930-1600:23456" if session else "24x7",
                syminfo=[["mintick", "0.25"], ["pointvalue", "2.5"], ["qty_step", "0.001"]],
                inputs=[], overrides=[["commission_value", "0.1"], ["slippage", "1"]])


def config_args(config):
    result = ["--symbol", config["symbol"], "--script-tf", config["script_tf"],
              "--timezone", config["timezone"], "--chart-timezone", config["chart_timezone"],
              "--session", config["session"]]
    for key in ("syminfo", "inputs", "overrides"):
        flag = {"inputs": "--input", "overrides": "--override", "syminfo": "--syminfo"}[key]
        for name, value in config[key]:
            result += [flag, f"{name}={value}"]
    return result


def tick_events(rows, timeframe, minute_boundaries):
    events = []
    sequence = 1
    for bar in rows:
        stamp = bar["ts_open"]
        duration = int(timeframe) * 60000
        path = (bar["o"], bar["h"], bar["l"], bar["c"])
        offsets = (0, duration // 3, 2 * duration // 3, duration - 1)
        pending = [dict(type="tick", ts=stamp + offset, seq=sequence + index, price=price, qty=1)
                   for index, (offset, price) in enumerate(zip(offsets, path))]
        sequence += 4
        boundaries = range(60000, duration + 1, 60000) if minute_boundaries else (duration,)
        pending += [dict(type="time", ts=stamp + offset) for offset in boundaries]
        events += sorted(pending, key=lambda event: (event["ts"], event["type"] != "time"))
    return events


def write_events(path, events):
    path.write_text("".join(json.dumps(event) + "\n" for event in events))


def actions(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def oracle_command(oracle, library, csv, config_path, action_path, timeframe):
    return [oracle, library, csv, timeframe, action_path, "--input-tf", timeframe,
            "--distribution", "3", "--config", config_path, "--calendar-check"]


def prove(root, runner, oracle, library, timeframe, session, engine_only, split=24):
    rows = chart_rows(timeframe, session)
    config = config_for(timeframe, session)
    config_path = root / "config.json"
    config_path.write_text(json.dumps(config))
    full, warmup, feed = (root / name for name in ("full.csv", "warmup.csv", "feed.jsonl"))
    write_csv(full, rows)
    write_csv(warmup, rows[:split])
    batch_actions = root / "batch-actions.jsonl"
    batch = checked(oracle_command(oracle, library, full, config_path, batch_actions, timeframe))
    (root / "batch-report.json").write_text(json.dumps(batch))
    print(f"INPUT-N batch input={timeframe} script={timeframe}: calendar accepted; "
          f"ENDPOINTS=3; trades={batch['trades_len']}", flush=True)
    assert batch["trades_len"] > 0, (timeframe, "vacuous chart reference")
    modes = [("bars", False)]
    if not session and timeframe not in ("D", "W"):
        modes += [("ticks", False), ("ticks", True)]
    for mode, minute_boundaries in modes:
        events = ([dict(type="bar", bar=bar) for bar in rows[split:]] if mode == "bars" else
                  tick_events(rows[split:], timeframe, minute_boundaries))
        write_events(feed, events)
        direct_actions = root / "direct-actions.jsonl"
        direct = checked(oracle_command(oracle, library, warmup, config_path, direct_actions, timeframe)
                         + ["--stream-feed", feed])
        (root / f"direct-{mode}-{minute_boundaries}.json").write_text(json.dumps(direct))
        if mode == "bars":
            equal(batch, direct, "chart batch/direct stream")
            equal([action_key(row) for row in actions(batch_actions)],
                  [action_key(row) for row in actions(direct_actions)], "chart physical actions")
        drift = "none"
        try:
            equal(batch, direct)
        except AssertionError as error:
            drift = "print-path (" + str(error.args[0][0]) + ")"
        print(f"INPUT-N engine script={timeframe} session={session} mode={mode} "
              f"minute_boundaries={minute_boundaries}: accepted; chart drift={drift}", flush=True)
        if engine_only:
            continue
        for explicit in (False, True):
            ledger = root / f"{timeframe}-{session}-{mode}-{minute_boundaries}-{explicit}.sqlite"
            command = [runner, "run", "--strategy", library, "--warmup", warmup, "--feed", feed,
                       "--ledger", ledger, "--mode", mode] + config_args(config)
            if explicit:
                command += ["--input-tf", timeframe]
            checked(command)
            report = checked([runner, "report", "--ledger", ledger])
            equal(direct, report["report"], "chart runner/direct stream")
            with sqlite3.connect(ledger) as database:
                actual = [json.loads(row[0]) for row in database.execute("SELECT payload FROM events ORDER BY ordinal")]
            expected = [row for row in actions(direct_actions) if row["origin_input_index"] >= split]
            assert expected and actual, "vacuous live actions"
            equal([action_key(row) for row in expected], [action_key(row) for row in actual], "live actions")
            checked(command)
            equal(report, checked([runner, "report", "--ledger", ledger]), "recovery")
        print(f"CHART runner script={timeframe} mode={mode}: report/actions BITWISE IDENTICAL; replay IDENTICAL", flush=True)


def refusals(root, runner, library):
    warmup, feed = root / "refusal.csv", root / "empty.jsonl"
    write_csv(warmup, chart_rows("15")[:24])
    feed.write_text("")
    base = [runner, "run", "--strategy", library, "--warmup", warmup, "--feed", feed,
            "--script-tf", "15", "--symbol", "TEST:EXAMPLE", "--mode", "bars"]
    for mode in ("bars", "ticks"):
        ledger = root / f"refused-{mode}.sqlite"
        command = base[:-1] + [mode, "--input-tf", "1", "--ledger", ledger]
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=30)
        assert result.returncode == 1 and "input-tf" in result.stderr and "script-tf" in result.stderr
        assert "15" in result.stderr and ("omit" in result.stderr or "remove" in result.stderr)
        assert not ledger.exists() and not Path(str(ledger) + ".lock").exists()
    for name, rows in (("1m", chart_rows("1")[:24]), ("gap", chart_rows("15")[:24:2]),
                       ("unaligned", [{**bar, "ts_open": bar["ts_open"] + 60000} for bar in chart_rows("15")[:24]])):
        write_csv(warmup, rows)
        ledger = root / f"bad-warmup-{name}.sqlite"
        result = subprocess.run(list(map(str, base + ["--ledger", ledger])), text=True, capture_output=True, timeout=30)
        assert result.returncode == 1 and ("warmup" in result.stderr or "input" in result.stderr), result.stderr
        assert not ledger.exists()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("oracle")
    parser.add_argument("library")
    parser.add_argument("--engine-only", action="store_true")
    parser.add_argument("--timeframe", choices=("1", "5", "15", "60", "D", "W"))
    parser.add_argument("--split", type=int, default=24)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chart-input-") as temporary:
        root = args.output_dir or Path(temporary)
        root.mkdir(parents=True, exist_ok=True)
        clocks = ((args.timeframe, False),) if args.timeframe else (
            ("1", False), ("5", False), ("15", False), ("60", False),
            ("15", True), ("D", False), ("W", False))
        for timeframe, session in clocks:
            target = root / f"{timeframe}-{session}"
            target.mkdir(exist_ok=True)
            prove(target, args.runner, args.oracle, args.library, timeframe, session, args.engine_only, args.split)
        if not args.engine_only:
            refusals(root, args.runner, args.library)
    print("chart-input contract PASS", flush=True)


if __name__ == "__main__":
    main()
