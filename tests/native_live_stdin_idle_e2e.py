"""Stdin deadlines exclude a burst's report work and still bound silence."""
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

from native_live_report_e2e import equal, write_csv


def main():
    runner, oracle, library = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pineforge-stdin-idle-") as temporary:
        root = Path(temporary)
        split, count = 65536, 24
        bars = []
        for index in range(split + count):
            opening = round(100 + 15 * math.sin(index / 4), 2)
            close = round(100 + 15 * math.sin((index + 1) / 4), 2)
            bars.append(dict(ts_open=1704067200000 + index * 86400000, o=opening,
                             h=max(opening, close) + 2, l=min(opening, close) - 2,
                             c=close, v=10))
        warmup = root / "warmup.csv"
        write_csv(warmup, bars[:split])

        def command(ledger, history=warmup):
            return [runner, "run", "--strategy", library, "--warmup", str(history),
                    "--feed", "-", "--ledger", str(ledger), "--mode", "bars",
                    "--input-tf", "D", "--script-tf", "D", "--symbol", "TEST:EXAMPLE",
                    "--syminfo", "mintick=0.25", "--syminfo", "pointvalue=2.5",
                    "--syminfo", "qty_step=0.001", "--override", "commission_value=0.1",
                    "--override", "slippage=1", "--report-jsonl=full",
                    "--feed-idle-timeout", "1", "--feed-message-timeout", "1"]

        # Preload one atomic pipe write before starting the reader. The burst
        # fits in one stdin read, including an unterminated final JSONL line.
        # Full reports over the enlarged warmup make the consume pass expensive;
        # read and check each report as it arrives instead of retaining them all.
        feed = "\n".join(json.dumps(dict(type="bar", bar=bar), separators=(",", ":"))
                         for bar in bars[split:]).encode()
        ledger = root / "burst.sqlite"
        read_fd, write_fd = os.pipe()
        with os.fdopen(read_fd, "rb") as source, os.fdopen(write_fd, "wb") as sink:
            assert len(feed) <= os.fpathconf(sink.fileno(), "PC_PIPE_BUF")
            sink.write(feed)
            sink.close()
            process = subprocess.Popen(command(ledger), stdin=source,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True)
        try:
            reports = 0
            summary = None
            final = None
            first_report_at = None
            last_report_at = None
            for line in process.stdout:
                received_at = time.monotonic()
                document = json.loads(line)
                if "inputs_committed" in document:
                    summary = document
                    continue
                if first_report_at is None:
                    first_report_at = received_at
                reports += 1
                if reports < count:
                    last_report_at = received_at
                assert document["schema_version"] == "pineforge-native-report/v1"
                assert document["input_cursor"] == reports
                final = document
            errors = process.stderr.read()
            assert process.wait() == 0, errors
            assert reports == count, reports
            assert summary["inputs_committed"] == count, summary
            assert summary["inputs_processed"] == count, summary
            with sqlite3.connect(ledger) as database:
                assert database.execute("SELECT count(*) FROM inputs").fetchone() == (count,)
                assert database.execute(
                    "SELECT max(input_cursor) FROM report_snapshots").fetchone() == (count,)
            print(f"PASS stdin burst: {count} committed inputs; "
                  f"pre-EOF report span {last_report_at - first_report_at:.3f}s", flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
            process.communicate()

        # A batch oracle checks that the successful burst committed the actual
        # bars, including the last line completed by EOF.
        snapshot = root / "batch.csv"
        write_csv(snapshot, bars)
        result = subprocess.run(
            [oracle, library, str(snapshot), "D", str(root / "batch-actions.jsonl"),
             "--input-tf", "D", "--distribution", "3"],
            text=True, capture_output=True)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        equal(json.loads(result.stdout), final["report"])

        silent_warmup = root / "silent-warmup.csv"
        write_csv(silent_warmup, bars[:30])
        silent_ledger = root / "silent.sqlite"
        process = subprocess.Popen(command(silent_ledger, silent_warmup),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            sent_at = time.monotonic()
            process.stdin.write(json.dumps(dict(type="bar", bar=bars[30])) + "\n")
            process.stdin.flush()
            committed = json.loads(process.stdout.readline())
            assert committed["input_cursor"] == 1, committed
            # wait() leaves stdin open: only the runner's deadline ends intake.
            assert process.wait(timeout=15) == 1
            assert not process.stdin.closed
            assert time.monotonic() - sent_at >= 1
            output, errors = process.communicate()
            assert not output, output
            assert ("pineforge-live: native stdin idle or message timeout; 0 actions not sent"
                    in errors.splitlines()), errors
            with sqlite3.connect(silent_ledger) as database:
                assert database.execute("SELECT count(*) FROM inputs").fetchone() == (1,)
        finally:
            if process.poll() is None:
                process.terminate()
            process.communicate()
        print("PASS stdin silence: one committed bar, open pipe, exact timeout", flush=True)


if __name__ == "__main__":
    main()
