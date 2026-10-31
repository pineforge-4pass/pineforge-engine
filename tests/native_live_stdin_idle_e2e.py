"""Stdin deadlines exclude report work, bound dribbled lines and allow no limit."""
import json
import math
import os
from pathlib import Path
import select
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

        def command(ledger, history=warmup, *, idle=1, message=1):
            return [runner, "run", "--strategy", library, "--warmup", str(history),
                    "--feed", "-", "--ledger", str(ledger), "--mode", "bars",
                    "--input-tf", "D", "--script-tf", "D", "--symbol", "TEST:EXAMPLE",
                    "--syminfo", "mintick=0.25", "--syminfo", "pointvalue=2.5",
                    "--syminfo", "qty_step=0.001", "--override", "commission_value=0.1",
                    "--override", "slippage=1", "--report-jsonl=full",
                    "--feed-idle-timeout", str(idle), "--feed-message-timeout", str(message)]

        # Preload one atomic pipe write before starting the reader. The burst
        # fits in one stdin read, including an unterminated final JSONL line.
        # Full reports over the enlarged warmup exceed stdout's pipe capacity.
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
            readable, _, _ = select.select([process.stdout], [], [], 180)
            assert readable, "burst produced no stdout within 180 seconds"
            # Deliberate slow stdout consumer: leave reports unread for two
            # seconds, blocking write() inside the consume pass as in production.
            # Readiness starts this stimulus after warmup, without draining bytes.
            time.sleep(2)
            output, errors = process.communicate(timeout=180)
            reports = 0
            summary = None
            final = None
            pre_eof_report_bytes = 0
            for line in output.splitlines(keepends=True):
                document = json.loads(line)
                if "inputs_committed" in document:
                    summary = document
                    continue
                reports += 1
                if reports < count:
                    pre_eof_report_bytes += len(line.encode())
                assert document["schema_version"] == "pineforge-native-report/v1"
                assert document["input_cursor"] == reports
                final = document
            # Fixed portable bound: four times a 64 KiB pipe. Count only reports
            # in the first consume pass, excluding the last line completed by EOF.
            assert pre_eof_report_bytes > 262144, (
                "slow stdout consumer cannot force a blocked consume pass: "
                f"only {pre_eof_report_bytes} report bytes before EOF")
            assert process.returncode == 0, (process.returncode, errors)
            assert reports == count, reports
            assert summary["inputs_committed"] == count, summary
            assert summary["inputs_processed"] == count, summary
            with sqlite3.connect(ledger) as database:
                assert database.execute("SELECT count(*) FROM inputs").fetchone() == (count,)
                assert database.execute(
                    "SELECT max(input_cursor) FROM report_snapshots").fetchone() == (count,)
            print(f"PASS stdin burst: {count} committed inputs; "
                  f"{pre_eof_report_bytes} pre-EOF report bytes, slow stdout consumer", flush=True)
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

        timeout_error = "pineforge-live: native stdin idle or message timeout; 0 actions not sent"

        def dribble(name, chunks, message, expect_timeout):
            ledger = root / (name + ".sqlite")
            process = subprocess.Popen(command(ledger, silent_warmup, idle=3, message=message),
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            try:
                # Start the dribble after warmup and the first committed report.
                process.stdin.write(json.dumps(dict(type="bar", bar=bars[30])) + "\n")
                process.stdin.flush()
                readable, _, _ = select.select([process.stdout], [], [], 180)
                assert readable, (name, "readiness bar produced no stdout within 180 seconds")
                committed = json.loads(process.stdout.readline())
                assert committed["input_cursor"] == 1, committed
                sent_at = time.monotonic()
                for index, chunk in enumerate(chunks):
                    if index:
                        # Dribbling sender stimulus: each continuation arrives
                        # within idle=3, but assembling the line takes over 1 s.
                        time.sleep(0.4)
                    if process.poll() is not None:
                        break
                    try:
                        process.stdin.write(chunk)
                        process.stdin.flush()
                    except BrokenPipeError:
                        break
                if expect_timeout:
                    # Check during the dribble, with stdin still open. Waiting
                    # after its last byte would let a refreshed clock expire too.
                    assert not process.stdin.closed
                    assert process.poll() == 1, (
                        name, "message deadline did not end intake during the dribble",
                        process.poll())
                else:
                    assert process.poll() is None, (name, process.poll())
                    assert time.monotonic() - sent_at > 1
                    process.stdin.close()
                    process.stdin = None
                output, errors = process.communicate(timeout=60)
                expected_count = 1 if expect_timeout else 2
                assert process.returncode == (1 if expect_timeout else 0), (
                    name, process.returncode, output, errors)
                if expect_timeout:
                    assert timeout_error in errors.splitlines(), (name, errors)
                    assert not output, (name, output)
                else:
                    documents = [json.loads(line) for line in output.splitlines()]
                    assert len(documents) == 2, (name, documents)
                    assert documents[0]["schema_version"] == "pineforge-native-report/v1"
                    assert documents[0]["input_cursor"] == 2, documents[0]
                    assert documents[1]["inputs_committed"] == 2, documents[1]
                    assert documents[1]["inputs_processed"] == 2, documents[1]
                with sqlite3.connect(ledger) as database:
                    assert database.execute("SELECT count(*) FROM inputs").fetchone() == (
                        expected_count,), name
            finally:
                if process.poll() is None:
                    process.terminate()
                process.communicate()
            print(f"PASS stdin {name}: {expected_count} committed inputs", flush=True)

        dribble("partial-dribble", ["{"] + [" "] * 8, 1, True)
        valid_line = json.dumps(dict(type="bar", bar=bars[31]), separators=(",", ":")) + "\n"
        chunks = [valid_line[index:index + 12] for index in range(0, len(valid_line), 12)]
        dribble("valid-dribble-unlimited", chunks, 0, False)
        dribble("valid-dribble-bounded", chunks, 1, True)

        for name, idle, expected_count in (("unlimited-gap", 0, 2), ("bounded-gap", 1, 1)):
            ledger = root / (name + ".sqlite")
            process = subprocess.Popen(command(ledger, silent_warmup, idle=idle, message=0),
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            try:
                process.stdin.write(json.dumps(dict(type="bar", bar=bars[30])) + "\n")
                process.stdin.flush()
                committed = json.loads(process.stdout.readline())
                assert committed["input_cursor"] == 1, committed
                # Silent sender stimulus: hold stdin open without bytes for 2 s.
                # The sibling with idle=1 proves this gap crosses a live deadline.
                time.sleep(2)
                if idle:
                    assert process.poll() == 1, ("idle deadline did not fire", process.poll())
                    assert not process.stdin.closed
                else:
                    assert process.poll() is None, ("zero idle bound ended intake", process.poll())
                    process.stdin.write(json.dumps(dict(type="bar", bar=bars[31])) + "\n")
                    process.stdin.flush()
                    process.stdin.close()
                    process.stdin = None
                output, errors = process.communicate(timeout=15)
                assert process.returncode == (1 if idle else 0), (
                    name, process.returncode, output, errors)
                if idle:
                    assert timeout_error in errors.splitlines(), errors
                    assert not output, output
                else:
                    documents = [json.loads(line) for line in output.splitlines()]
                    assert len(documents) == 2, documents
                    assert documents[0]["schema_version"] == "pineforge-native-report/v1"
                    assert documents[0]["input_cursor"] == 2, documents[0]
                    assert documents[1]["inputs_committed"] == 2, documents[1]
                    assert documents[1]["inputs_processed"] == 2, documents[1]
                with sqlite3.connect(ledger) as database:
                    assert database.execute("SELECT count(*) FROM inputs").fetchone() == (
                        expected_count,), name
            finally:
                if process.poll() is None:
                    process.terminate()
                process.communicate()
            print(f"PASS stdin {name}: {expected_count} committed inputs", flush=True)


if __name__ == "__main__":
    main()
