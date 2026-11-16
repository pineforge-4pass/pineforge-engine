"""Startup calendar-grid admission and realtime boundary regressions."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta
import json
import math
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from zoneinfo import ZoneInfo

from native_live_chart_input_e2e import (
    actions, checked, config_args, config_for, oracle_command, require_refusal, write_events,
)
from native_live_report_e2e import action_key, equal, write_csv


def session_windows(session):
    """Opening/closing wall-clock times of each window in a "HHMM-HHMM,...:days" session."""
    windows = []
    for window in session.split(":")[0].split(","):
        opening, closing = window.split("-")
        windows.append((int(opening[:2]), int(opening[2:]), int(closing[:2]), int(closing[2:])))
    return windows


def calendar_rows(timeframe, timezone, session, first_day):
    zone = ZoneInfo(timezone)
    day = datetime.fromisoformat(first_day).replace(tzinfo=zone)
    intervals = []
    for day_offset in range(6):
        trading_day = day + timedelta(days=day_offset)
        if session != "24x7" and trading_day.weekday() >= 5:
            continue
        if session == "0930-1600:23456":
            opening = trading_day.replace(hour=9, minute=30)
            closing = trading_day.replace(hour=16)
        elif "," in session:
            period = int(timeframe) * 60000
            for hour, minute, end_hour, end_minute in session_windows(session):
                start = int(trading_day.replace(hour=hour, minute=minute).timestamp() * 1000)
                end = int(trading_day.replace(hour=end_hour, minute=end_minute).timestamp() * 1000)
                intervals.extend((stamp, min(stamp + period, end))
                                 for stamp in range(start, end, period))
            continue
        elif session == "1700-1700:23456":
            opening = (trading_day - timedelta(days=1)).replace(hour=17)
            closing = trading_day.replace(hour=17)
        else:
            opening = trading_day
            closing = trading_day + timedelta(days=1)
        start = int(opening.timestamp() * 1000)
        end = int(closing.timestamp() * 1000)
        period = int(timeframe) * 60000
        intervals.extend((stamp, min(stamp + period, end))
                         for stamp in range(start, end, period))
    rows = []
    for index, (stamp, end) in enumerate(intervals):
        opening = round(100 + 15 * math.sin(index / 4), 2)
        close = round(100 + 15 * math.sin((index + 1) / 4), 2)
        rows.append(dict(ts_open=stamp, o=opening, h=max(opening, close) + 2,
                         l=min(opening, close) - 2, c=close, v=4))
    return rows, intervals


def prints(rows, intervals):
    events = []
    sequence = 1
    for bar, (stamp, end) in zip(rows, intervals):
        duration = end - stamp
        pending = [dict(type="tick", ts=stamp + offset, seq=sequence + index,
                        price=price, qty=1)
                   for index, (offset, price) in enumerate(zip(
                       (0, duration // 3, 2 * duration // 3, duration - 1),
                       (bar["o"], bar["h"], bar["l"], bar["c"])))]
        sequence += 4
        pending.extend(dict(type="time", ts=boundary)
                       for boundary in range(stamp + 60000, end + 1, 60000))
        events.extend(sorted(pending, key=lambda event: (event["ts"], event["type"] != "time")))
    return events


def cells():
    yield "7", "UTC", "24x7", "2025-01-01", False
    yield "60", "UTC", "0930-1130,1300-1500:23456", "2025-01-06", False
    yield "30", "UTC", "0930-1130,1300-1500:23456", "2025-01-06", True
    # The reopen is on the 60-minute grid here, so the same chart period is admitted.
    yield "60", "UTC", "0900-1200,1300-1500:23456", "2025-01-06", True
    yield "60", "Asia/Tokyo", "0900-1130,1230-1500:23456", "2025-01-06", False
    for first_day in ("2025-03-06", "2025-10-30"):
        for timeframe in ("240", "45", "120"):
            yield timeframe, "America/New_York", "24x7", first_day, False
        for session in ("1700-1700:23456", "0930-1600:23456"):
            yield "240", "America/New_York", session, first_day, False
        yield "240", "UTC", "24x7", first_day, True
        for session in ("24x7", "1700-1700:23456", "0930-1600:23456"):
            yield "60", "America/New_York", session, first_day, True


def prove(root, runner, oracle, library, cell, probe):
    timeframe, timezone, session, first_day, admitted = cell
    name = "-".join((timeframe, timezone.replace("/", "_"), session.replace(":", "_"), first_day))
    target = root / name
    target.mkdir(parents=True, exist_ok=True)
    rows, intervals = calendar_rows(timeframe, timezone, session, first_day)
    config = config_for(timeframe, timezone=timezone)
    config["session"] = session
    config_path = target / "config.json"
    config_path.write_text(json.dumps(config))
    full, warmup = target / "full.csv", target / "warmup.csv"
    write_csv(full, rows)
    split = 2
    write_csv(warmup, rows[:split])
    batch_actions = target / "batch-actions.jsonl"
    if admitted:
        batch = checked(oracle_command(oracle, library, full, config_path, batch_actions, timeframe))
        (target / "batch-report.json").write_text(json.dumps(batch))
        assert batch["trades_len"] > 0, (name, "vacuous batch")
    receipts = []
    for mode in ("bars", "ticks"):
        feed = target / f"{mode}.jsonl"
        events = ([dict(type="bar", bar=bar) for bar in rows[split:]] if mode == "bars"
                  else prints(rows[split:], intervals[split:]))
        write_events(feed, events)
        ledger = target / f"{mode}.sqlite"
        command = [runner, "run", "--strategy", library, "--warmup", warmup,
                   "--feed", feed, "--ledger", ledger, "--mode", mode] + config_args(config)
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
        (target / f"{mode}.log").write_text(result.stdout + result.stderr)
        receipt = dict(timeframe=timeframe, timezone=timezone, session=session, first_day=first_day,
                       mode=mode, returncode=result.returncode, stderr=result.stderr.strip(),
                       ledger_created=ledger.exists(), expected_admitted=admitted or mode == "ticks")
        receipts.append(receipt)
        print(json.dumps(receipt), flush=True)
        if probe:
            continue
        if not admitted and mode == "bars":
            require_refusal(result,
                f"pineforge-live: chart delivery for a {timeframe} chart on this session calendar is not supported yet:")
            assert "its bars do not tile the calendar's trading days" in result.stderr, receipt
            assert not ledger.exists() and not Path(str(ledger) + ".lock").exists(), receipt
            continue
        assert result.returncode == 0, receipt
        direct_actions = target / f"direct-{mode}-actions.jsonl"
        direct = checked(oracle_command(oracle, library, warmup, config_path, direct_actions, timeframe)
                         + ["--stream-feed", feed])
        assert direct["trades_len"] > 0, (name, mode, "vacuous stream")
        actual = checked([runner, "report", "--ledger", ledger])["report"]
        equal(direct, actual, "calendar runner/direct")
        if mode == "bars":
            equal(batch, actual, "calendar chart batch/runner")
            equal([action_key(row) for row in actions(batch_actions)],
                  [action_key(row) for row in actions(direct_actions)], "calendar physical actions")
    return receipts


def confirmed_reporter_cells():
    # The issue's two controls and four failures, all at 09:30 local.
    yield "UTC daily control", "UTC", "1D", ("2026-03-05", "2026-03-06", "2026-03-09")
    yield "NY daily winter control", "America/New_York", "1D", (
        "2026-01-08", "2026-01-09", "2026-01-12")
    yield "NY daily spring", "America/New_York", "1D", (
        "2026-03-05", "2026-03-06", "2026-03-09")
    yield "NY daily autumn", "America/New_York", "1D", (
        "2026-10-29", "2026-10-30", "2026-11-02")
    yield "NY weekly spring", "America/New_York", "1W", (
        "2026-02-23", "2026-03-02", "2026-03-09")
    yield "NY weekly autumn", "America/New_York", "1W", (
        "2026-10-19", "2026-10-26", "2026-11-02")


def prove_confirmed_reporter(root, oracle, library):
    failures = []
    for name, timezone, timeframe, dates in confirmed_reporter_cells():
        target = root / ("confirmed-" + name.replace(" ", "-"))
        target.mkdir(parents=True, exist_ok=True)
        zone = ZoneInfo(timezone)
        rows = [
            dict(ts_open=int(datetime.fromisoformat(day + "T09:30:00").replace(
                tzinfo=zone).timestamp() * 1000),
                 o=100 + index, h=102 + index, l=99 + index, c=101 + index, v=1000)
            for index, day in enumerate(dates)
        ]
        config = dict(symbol="TEST:EXAMPLE", script_tf=timeframe, timezone=timezone,
                      chart_timezone=timezone, session="0930-1600:23456",
                      syminfo=[], inputs=[], overrides=[])
        config_path = target / "config.json"
        config_path.write_text(json.dumps(config))
        full, warmup, feed = target / "full.csv", target / "warmup.csv", target / "feed.jsonl"
        write_csv(full, rows)
        write_csv(warmup, rows[:2])
        write_events(feed, [dict(type="bar", bar=rows[2])])
        batch_path, stream_path = target / "batch-actions.jsonl", target / "stream-actions.jsonl"
        # The existing oracle loads the unchanged runner/examples/strategy.cpp
        # module and calls its C ABI. This is direct confirmed input, without a
        # live-runner startup request or any tick/clock event.
        common = ["--input-tf", timeframe, "--distribution", "3", "--config", config_path]
        try:
            batch = checked([oracle, library, full, timeframe, batch_path] + common)
            streamed = checked([oracle, library, warmup, timeframe, stream_path]
                               + common + ["--stream-feed", feed])
            assert batch["input_bars_processed"] == streamed["input_bars_processed"] == 3, name
            assert batch["script_bars_processed"] == streamed["script_bars_processed"] == 3, name
            equal(batch, streamed, name + " confirmed batch/stream report")
            expected_actions, actual_actions = actions(batch_path), actions(stream_path)
            equal([action_key(row) for row in expected_actions],
                  [action_key(row) for row in actual_actions], name + " confirmed physical actions")
            assert len(actual_actions) == 1, (name, "pending warmup entry must fill")
            fill = actual_actions[0]
            assert fill["timestamp"] == rows[2]["ts_open"] and fill["bar_index"] == 2, (name, fill)
            assert fill["order"]["id"] == "Long" and fill["order"]["leg"] == "entry", (name, fill)
            assert fill["order"]["action"] == "buy" and not fill["order"]["reduce_only"], (name, fill)
            assert fill["order"]["contracts"] == 1 and fill["order"]["price"] == 102, (name, fill)
        except AssertionError as failure:
            # Measure every reporter cell, including the controls, before the
            # normal CTest row fails. A failed subprocess includes its raw error.
            failures.append((name, str(failure)))
    assert not failures, failures


def confirmed_runner_rows(timeframe, first_day):
    # Advance civil trading dates, not elapsed UTC days/weeks. Python's
    # zoneinfo supplies an independent 09:30 New York fixture calendar.
    day = datetime.fromisoformat(first_day).replace(
        hour=9, minute=30, tzinfo=ZoneInfo("America/New_York"))
    rows = []
    while len(rows) < 9:
        if day.weekday() < 5:
            index = len(rows)
            rows.append(dict(ts_open=int(day.timestamp() * 1000), o=100 + index,
                             h=102 + index, l=99 + index, c=101 + index, v=1000))
        day += timedelta(days=7 if timeframe == "1W" else 1)
    return rows


def confirmed_runner_values(report, records, rows, split):
    # The unchanged example orders at closes 1/3/5/7; market fills occur
    # at openings 2/4/6/8. Two unit longs earn (104-102)+(108-106)=4,
    # without commission, slippage or currency conversion in this fixture.
    assert report["input_bars_processed"] == report["script_bars_processed"] == 9, report
    assert report["trades_len"] == report["total_trades"] == 2, report
    assert report["net_profit"] == 4 and report["metrics"]["all"]["commission_paid"] == 0, report
    assert len(report["trades"]) == 2, report
    for trade, entry, exit_ in zip(report["trades"], (2, 6), (4, 8)):
        assert trade["entry_time"] == rows[entry]["ts_open"], trade
        assert trade["exit_time"] == rows[exit_]["ts_open"], trade
        assert trade["entry_bar_index"] == entry and trade["exit_bar_index"] == exit_, trade
        assert trade["entry_price"] == 100 + entry and trade["exit_price"] == 100 + exit_, trade
        assert trade["qty"] == 1 and trade["is_long"] == 1 and trade["open_at_end"] == 0, trade
        assert trade["pnl"] == 2 and trade["commission"] == 0, trade
    expected = [(rows[index]["ts_open"], index, leg, side, 1, 100 + index, leg == "exit")
                for index, leg, side in ((2, "entry", "buy"), (4, "exit", "sell"),
                                         (6, "entry", "buy"), (8, "exit", "sell"))
                if index >= split]
    actual = [(row["timestamp"], row["bar_index"], row["order"]["leg"], row["order"]["action"],
               row["order"]["contracts"], row["order"]["price"], row["order"]["reduce_only"])
              for row in records]
    assert actual == expected, (expected, actual)
    for row in records:
        assert row["order"]["entry_incarnation"] > 0, row
        if row["order"]["leg"] == "entry":
            assert row["order"]["id"] == "Long", row


def calendar_ledger_actions(ledger):
    with sqlite3.connect(ledger) as database:
        return [json.loads(row[0]) for row in
                database.execute("SELECT payload FROM events ORDER BY ordinal")]


def prove_confirmed_runner(root, runner, oracle, library):
    # Fri -> Mon is 71/73 h at the March/November change; consecutive
    # Monday weekly openings are 167/169 h. These are calendar expectations,
    # not values learned from a successful engine run.
    cells = (("1D", "2026-03-05", 71), ("1D", "2026-10-29", 73),
             ("1W", "2026-02-23", 167), ("1W", "2026-10-19", 169))
    for timeframe, first_day, transition_hours in cells:
        target = root / f"confirmed-runner-{timeframe}-{first_day}"
        target.mkdir(parents=True, exist_ok=True)
        rows = confirmed_runner_rows(timeframe, first_day)
        assert rows[2]["ts_open"] - rows[1]["ts_open"] == transition_hours * 3600000
        config = dict(symbol="TEST:EXAMPLE", script_tf=timeframe, timezone="America/New_York",
                      chart_timezone="America/New_York", session="0930-1600:23456",
                      syminfo=[], inputs=[], overrides=[])
        config_path, full = target / "config.json", target / "full.csv"
        config_path.write_text(json.dumps(config))
        write_csv(full, rows)
        batch_path = target / "batch-actions.jsonl"
        batch = checked(oracle_command(oracle, library, full, config_path, batch_path, timeframe))
        batch_actions = actions(batch_path)
        confirmed_runner_values(batch, batch_actions, rows, 0)
        (target / "batch-report.json").write_text(json.dumps(batch))
        commands, prefix_report = {}, None
        for split in (1, 2, 3):
            # Split 2 ends warmup before the change, split 3 after it.
            # Split 1 lets one realtime input commit before the restart,
            # leaving the transition and its pending entry for the next process.
            warmup, feed = target / f"warmup-{split}.csv", target / f"feed-{split}.jsonl"
            write_csv(warmup, rows[:split])
            write_events(feed, [dict(type="bar", bar=bar) for bar in rows[split:]])
            direct_path = target / f"direct-{split}-actions.jsonl"
            direct = checked(oracle_command(oracle, library, warmup, config_path,
                                            direct_path, timeframe) + ["--stream-feed", feed])
            equal(batch, direct, "confirmed calendar batch/direct report")
            equal([action_key(row) for row in batch_actions],
                  [action_key(row) for row in actions(direct_path)], "confirmed calendar batch/direct fills")
            command = [runner, "run", "--strategy", library, "--warmup", warmup,
                       "--feed", feed, "--mode", "bars", "--input-tf", timeframe] + config_args(config)
            commands[split] = command
            ledger = target / f"uninterrupted-{split}.sqlite"
            summary = checked(command + ["--ledger", ledger])
            assert summary["inputs_processed"] == summary["inputs_committed"] == 9 - split, summary
            document = checked([runner, "report", "--ledger", ledger])
            records = calendar_ledger_actions(ledger)
            confirmed_runner_values(document["report"], records, rows, split)
            equal(batch, document["report"], "confirmed calendar batch/runner report")
            equal(batch["trades"], document["closed_trades"], "confirmed calendar closed fills")
            expected = [row for row in batch_actions if row["origin_input_index"] >= split]
            equal([action_key(row) for row in expected],
                  [action_key(row) for row in records], "confirmed calendar live fills")
            if split == 1:
                ledger = target / "restart.sqlite"
                restart = command + ["--ledger", ledger]
                # subprocess.run returns only after this runner has stopped.
                # Its ledger contains one real input, still before the change.
                stopped = checked(restart + ["--max-events", "1"])
                assert stopped["inputs_processed"] == stopped["inputs_committed"] == 1, stopped
                prefix_report = checked([runner, "report", "--ledger", ledger])
                assert prefix_report["report"]["input_bars_processed"] == 2, prefix_report
                assert prefix_report["report"]["trades_len"] == 0, prefix_report
                assert calendar_ledger_actions(ledger) == [], ledger
                with sqlite3.connect(ledger) as database:
                    assert database.execute("SELECT COUNT(*) FROM inputs").fetchone()[0] == 1
                # A new process replays that committed prefix, then crosses DST.
                resumed = checked(restart)
                assert resumed["prefix_skipped"] == 1 and resumed["inputs_processed"] == 7, resumed
                assert resumed["inputs_committed"] == 8, resumed
                recovered = checked([runner, "report", "--ledger", ledger])
                recovered_actions = calendar_ledger_actions(ledger)
                confirmed_runner_values(recovered["report"], recovered_actions, rows, split)
                equal(batch, recovered["report"], "confirmed calendar batch/restarted report")
                equal(document, recovered, "confirmed calendar uninterrupted/restarted report")
                equal(records, recovered_actions, "confirmed calendar uninterrupted/restarted orders and fills")
        assert prefix_report is not None
        # A missing eligible daily/weekly session is a gap. 09:31 is not an
        # opening. Each failure follows one valid committed pre-change input;
        # neither may append an input, action, fill or report to that prefix.
        for name, wrong in (("missing-session", rows[3]),
                            ("misaligned", dict(rows[2], ts_open=rows[2]["ts_open"] + 60000))):
            feed, ledger = target / f"{name}.jsonl", target / f"{name}.sqlite"
            write_events(feed, [dict(type="bar", bar=rows[1]), dict(type="bar", bar=wrong)])
            command = commands[1].copy()
            command[command.index("--feed") + 1] = feed
            result = subprocess.run(list(map(str, command + ["--ledger", ledger])),
                                    text=True, capture_output=True)
            (target / f"{name}.log").write_text(result.stdout + result.stderr)
            require_refusal(result, "pineforge-live: input bar cadence must follow the configured input timeframe and session calendar")
            with sqlite3.connect(ledger) as database:
                assert database.execute("SELECT COUNT(*) FROM inputs").fetchone()[0] == 1
            equal(prefix_report, checked([runner, "report", "--ledger", ledger]),
                  "confirmed calendar refused input preserves report")
            assert calendar_ledger_actions(ledger) == [], name


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runner")
    parser.add_argument("oracle")
    parser.add_argument("library")
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chart-calendar-") as temporary:
        root = args.output_dir or Path(temporary)
        root.mkdir(parents=True, exist_ok=True)
        receipts = []
        def verify_cell(cell):
            return prove(root, args.runner, args.oracle, args.library, cell, args.probe)
        with ThreadPoolExecutor(max_workers=4) as workers:
            for result in workers.map(verify_cell, cells()):
                receipts.extend(result)
        prove_confirmed_reporter(root, args.oracle, args.library)
        prove_confirmed_runner(root, args.runner, args.oracle, args.library)
        (root / "matrix.json").write_text(json.dumps(receipts, indent=2))
    print("chart-calendar boundary probe complete" if args.probe else "chart-calendar contract PASS", flush=True)


if __name__ == "__main__":
    main()
