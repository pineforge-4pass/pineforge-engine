"""Ratio interval CLI, immutable replay, final refresh and batch export."""
import copy
import json
import math
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from native_live_report_e2e import apply_delta, equal, write_csv


def checked(command, *, input_text=None, success=True):
    result = subprocess.run(command, input=input_text, text=True, capture_output=True)
    assert (result.returncode == 0) == success, (
        command, result.returncode, result.stdout, result.stderr)
    return result


def stored_rows(path):
    with sqlite3.connect(path) as database:
        return database.execute(
            "SELECT input_cursor,payload FROM report_snapshots ORDER BY input_cursor").fetchall()


def output_rows(result):
    lines = result.stdout.splitlines()
    summary = json.loads(lines[-1])
    return [json.loads(line) for line in lines[:-1]], summary


def ratio_fields(report):
    values = report["metrics"]["equity"]
    return {key: values[key] for key in (
        "sharpe_monthly", "sharpe_bar", "sortino_bar", "sortino_monthly")}


def main():
    runner, oracle, library = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pineforge-ratio-interval-") as temporary:
        root = Path(temporary)
        split, count, interval = 30, 131, 7
        bars = []
        for index in range(split + count):
            opening = round(100 + 15 * math.sin(index / 4), 2)
            close = round(100 + 15 * math.sin((index + 1) / 4), 2)
            bars.append(dict(ts_open=1704067200000 + index * 86400000, o=opening,
                             h=max(opening, close) + 2, l=min(opening, close) - 2,
                             c=close, v=10))
        warmup, feed = root / "warmup.csv", root / "feed.jsonl"
        write_csv(warmup, bars[:split])
        feed_text = "".join(json.dumps(dict(type="bar", bar=bar)) + "\n" for bar in bars[split:])
        feed.write_text(feed_text)

        def command(ledger, k=None, source=None):
            result = [runner, "run", "--strategy", library, "--warmup", str(warmup),
                      "--feed", str(feed) if source is None else source, "--ledger", str(ledger),
                      "--mode", "bars", "--input-tf", "D", "--script-tf", "D",
                      "--symbol", "TEST:EXAMPLE", "--syminfo", "mintick=0.25",
                      "--syminfo", "pointvalue=2.5", "--syminfo", "qty_step=0.001",
                      "--override", "commission_value=0.1", "--override", "slippage=1",
                      "--report-jsonl"]
            if k is not None:
                result += ["--ratio-interval", str(k)]
            return result

        # The independent oracle calls run_backtest_full on each input prefix.
        # Neither the interval runner nor its carried state supplies expectations.
        batches = {}
        for cursor in range(count + 1):
            snapshot = root / "batch.csv"
            write_csv(snapshot, bars[:split + cursor])
            actions = root / "batch-actions.jsonl"
            result = checked([oracle, library, str(snapshot), "D", str(actions),
                              "--input-tf", "D", "--distribution", "3"])
            batches[cursor] = json.loads(result.stdout)
        assert any(isinstance(batch["metrics"]["equity"]["sharpe_monthly"], (int, float))
                   for batch in batches.values())

        default_path, exact_path = root / "default.sqlite3", root / "exact.sqlite3"
        default = checked(command(default_path))
        exact = checked(command(exact_path, 1))
        assert default.stdout == exact.stdout
        assert stored_rows(default_path) == stored_rows(exact_path)
        default_rows, _ = output_rows(default)
        assert len(default_rows) == count
        document = {}
        apply_delta(document, json.loads(stored_rows(default_path)[0][1]))
        for cursor, delta in enumerate(default_rows, 1):
            assert "ratios_as_of_input" not in delta
            apply_delta(document, delta)
            equal(batches[cursor], document["report"])

        def compare_interval(path, result, final_cursor, extra_refresh):
            rows, summary = output_rows(result)
            assert summary["inputs_committed"] == final_cursor
            assert len(rows) == final_cursor + int(extra_refresh)
            accumulated = {}
            apply_delta(accumulated, json.loads(stored_rows(path)[0][1]))
            as_of = 0
            for cursor, delta in enumerate(rows[:final_cursor], 1):
                if cursor % interval == 0 or (cursor == final_cursor and not extra_refresh):
                    as_of = cursor
                assert delta["ratios_as_of_input"] == as_of
                apply_delta(accumulated, delta)
                expected = copy.deepcopy(batches[cursor])
                expected["metrics"]["equity"].update(ratio_fields(batches[as_of]))
                equal(expected, accumulated["report"])
            if extra_refresh:
                assert rows[-1]["ratios_as_of_input"] == final_cursor
                apply_delta(accumulated, rows[-1])
            equal(batches[final_cursor], accumulated["report"])
            before = stored_rows(path)
            for cursor in (0, 1, 6, 7, min(9, final_cursor), final_cursor):
                exported = checked([runner, "report", "--ledger", str(path),
                                    "--at-input", str(cursor)])
                equal(batches[cursor], json.loads(exported.stdout)["report"])
            assert stored_rows(path) == before
            return accumulated

        interval_path = root / "interval.sqlite3"
        interval_result = checked(command(interval_path, interval))
        compare_interval(interval_path, interval_result, count, False)
        before = stored_rows(interval_path)
        replay_rows, replay_summary = output_rows(checked(command(interval_path, interval)))
        assert not replay_rows and replay_summary["inputs_processed"] == 0
        assert stored_rows(interval_path) == before
        changed = checked(command(interval_path, interval + 1), success=False)
        assert "identity" in changed.stderr

        pipe_path = root / "pipe.sqlite3"
        pipe_result = checked(command(pipe_path, interval, "-"), input_text=feed_text)
        compare_interval(pipe_path, pipe_result, count, True)
        # The extra stdout refresh never rewrites the last immutable input row.
        assert json.loads(stored_rows(pipe_path)[-1][1])["ratios_as_of_input"] == count // interval * interval
        pipe_replay = checked(command(pipe_path, interval, "-"), input_text=feed_text)
        rows, summary = output_rows(pipe_replay)
        assert summary["inputs_processed"] == 0 and len(rows) == 1
        assert rows[0]["ratios_as_of_input"] == count
        max_path = root / "max.sqlite3"
        limited = checked(command(max_path, interval) + ["--max-events", "9"])
        compare_interval(max_path, limited, 9, False)
        for bad in ("0", "-1", "1.5", "word", "18446744073709551616"):
            path = root / "invalid.sqlite3"
            checked(command(path, bad), success=False)
            assert not path.exists()
        checked(command(root / "duplicate.sqlite3", interval)
                + ["--ratio-interval", str(interval)], success=False)
    print("ratio interval: batch prefixes, final refresh, export and replay")


if __name__ == "__main__":
    main()
