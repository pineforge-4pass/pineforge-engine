"""Compare generated confirmed-bar runner actions and every report field to batch."""
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from native_live_report_e2e import action_key, equal, write_csv


def checked(command):
    result = subprocess.run(command, text=True, capture_output=True, timeout=180)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return json.loads(result.stdout)


def bars():
    result = []
    seed = 12345
    price = 100.0
    import math
    def next_value():
        nonlocal seed
        seed = (seed * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        return ((seed >> 33) % 2001) / 1000.0 - 1.0
    def rounded(value):
        return math.floor(value * 100 + 0.5) / 100
    for index in range(1620):
        opening = price
        close = max(5.0, opening + next_value() * 0.8 + math.sin(index / 97.0) * 0.15)
        high = max(opening, close) + abs(next_value()) * 0.6
        low = max(1.0, min(opening, close) - abs(next_value()) * 0.6)
        result.append(dict(ts_open=1577836800000 + index * 60000, o=rounded(opening),
                           h=rounded(high), l=rounded(low), c=rounded(close), v=5 + abs(next_value()) * 40))
        price = rounded(close)
    return result


def main():
    runner, oracle, name, library, *clock = sys.argv[1:]
    timeframe = clock[0] if clock else "1"
    tape = bars()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        full = root / "full.csv"
        write_csv(full, tape)
        batch_actions = root / "batch-actions.jsonl"
        batch = checked([oracle, library, str(full), timeframe, str(batch_actions), "--confirmed"])
        expected = [json.loads(line) for line in batch_actions.read_text().splitlines()]
        assert batch["trades_len"] > 0, (name, "no batch trades")
        splits = (30, 33, 500, 1500) if name in ("daily_close", "constant_daily") else (30, 33, 500)
        for split in splits:
            warmup = root / "warmup.csv"
            write_csv(warmup, tape[:split])
            feed = root / "feed.jsonl"
            feed.write_text("".join(json.dumps(dict(type="bar", bar=bar)) + "\n" for bar in tape[split:]))
            ledger = root / f"{split}.sqlite"
            command = [runner, "run", "--strategy", library, "--warmup", str(warmup),
                       "--feed", str(feed), "--ledger", str(ledger), "--mode", "bars",
                       "--input-tf", "1", "--script-tf", timeframe, "--symbol", "BINANCE:ETHUSDT.P",
                       "--syminfo", "type=crypto", "--syminfo", "currency=USDT",
                       "--syminfo", "basecurrency=ETH", "--syminfo", "mintick=0.01",
                       "--syminfo", "pointvalue=1", "--syminfo", "qty_step=0.001"]
            summary = checked(command)
            assert summary["confirmed_bar_capabilities"]["version"] == 1
            report = checked([runner, "report", "--ledger", str(ledger)])
            equal(batch, report["report"], name + ".report")
            with sqlite3.connect(ledger) as database:
                actual = [json.loads(row[0]) for row in database.execute("SELECT payload FROM events ORDER BY ordinal")]
            live_expected = [row for row in expected
                             if row["origin_input_index"] - row["origin_input_index"] % int(timeframe)
                             + int(timeframe) - 1 >= split]
            assert actual and live_expected, (name, "no live actions")
            equal([action_key(row) for row in live_expected], [action_key(row) for row in actual], name + ".actions")
            before = report
            checked(command)
            equal(before, checked([runner, "report", "--ledger", str(ledger)]), name + ".replay")
            print(f"confirmed admission {name} script={timeframe} split={split}: actions={len(actual)} report=BITWISE IDENTICAL replay=IDENTICAL", flush=True)


if __name__ == "__main__":
    main()
