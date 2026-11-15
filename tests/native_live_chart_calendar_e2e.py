"""Startup calendar-grid admission and realtime boundary regressions."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta
import json
import math
from pathlib import Path
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
        (root / "matrix.json").write_text(json.dumps(receipts, indent=2))
    print("chart-calendar boundary probe complete" if args.probe else "chart-calendar contract PASS", flush=True)


if __name__ == "__main__":
    main()
