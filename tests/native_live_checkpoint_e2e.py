"""Exercise checkpoint diagnostics, optional writes and full verification through the CLI."""
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time


def equal(actual, expected):
    assert actual == expected, {"actual": actual, "expected": expected}


def invoke(arguments, code=0):
    result = subprocess.run(arguments, text=True, capture_output=True, timeout=30)
    assert result.returncode == code, (arguments, result.returncode, code, result.stdout, result.stderr)
    return result


def query(path, sql):
    with sqlite3.connect(path) as database:
        return database.execute(sql).fetchone()[0]


def execute(path, sql):
    with sqlite3.connect(path) as database:
        database.executescript(sql)


def diagnostic_lines(errors):
    prefix = "pineforge-live: "
    return [json.loads(line[len(prefix):])["checkpoint_diagnostic"]
            for line in errors.splitlines()
            if line.startswith(prefix + '{"checkpoint_diagnostic":')]


def write_diagnostic(actual, reason, budget=0, build=0, io=0):
    equal(actual["code"], "write_failed")
    equal(actual["reason"], reason)
    equal(actual["budget_count"], budget)
    equal(actual["build_count"], build)
    equal(actual["io_count"], io)
    equal(actual["count"], budget + build + io)
    equal(actual["count"], sum(actual[key] for key in ("budget_count", "build_count", "io_count")))


def main():
    runner, library = sys.argv[1:]
    children = []
    with tempfile.TemporaryDirectory(prefix="pineforge-checkpoint-cli-") as directory:
        root = Path(directory)
        warmup = root / "warmup.csv"
        warmup.write_text("timestamp,open,high,low,close,volume\n" +
                          "".join(f"{index * 60000},100,102,99,101,4\n" for index in range(3)))
        config = root / "native.json"
        config.write_text(json.dumps({
            "run": {"session_key": "checkpoint-test", "run_number": 1},
            "clock": {"input_tf": "1", "script_tf": "1", "timezone": "UTC",
                      "session": "24x7", "chart_timezone": ""},
            "instrument": {"ticker": "MOCK", "tickerid": "TEST:MOCK", "type": "crypto",
                           "currency": "USDT", "basecurrency": "ETH", "description": "",
                           "volumetype": ""},
            "execution": {"initial_capital": 10000, "point_value": 1, "account_fx": 1,
                          "price_tick": 0.01, "slippage_ticks": 0, "fee_kind": "CashPerExecution",
                          "fee_value": 6, "quantity_grid": None, "close_execution": "NextEligiblePoint",
                          "max_abs_units": None, "max_open_lots": None,
                          "allowed_open_directions": 3, "initial_margin_fraction": None},
        }))
        events = [dict(type="bar", bar=dict(ts_open=index * 60000, o=100, h=102, l=99, c=101, v=4))
                  for index in range(3, 10)]
        feed = root / "feed.jsonl"
        feed.write_text("".join(json.dumps(event) + "\n" for event in events))

        def command(ledger, source=feed, interval=2):
            return [runner, "run", "--strategy", library, "--warmup", str(warmup),
                    "--native-config", str(config), "--mode", "bars", "--ledger", str(ledger),
                    "--feed", str(source), "--checkpoint-interval", str(interval)]

        def report(ledger, *options, code=0):
            return invoke([runner, "report", "--ledger", str(ledger), *options], code)

        def seed(name):
            ledger = root / (name + ".sqlite")
            equal(json.loads(invoke(command(ledger)).stdout)["inputs_committed"], len(events))
            equal(query(ledger, "SELECT group_concat(input_cursor,',') FROM "
                               "(SELECT input_cursor FROM report_checkpoints ORDER BY input_cursor)"), "2,4")
            return ledger

        def wait_for(process, predicate, label):
            deadline = time.monotonic() + 20
            last = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    output, errors = process.communicate()
                    raise AssertionError((label, process.returncode, output, errors))
                try:
                    last = predicate()
                    if last:
                        return last
                except (FileNotFoundError, json.JSONDecodeError, sqlite3.OperationalError) as error:
                    last = str(error)
                time.sleep(0.02)
            raise AssertionError((label, "condition not reached", last))

        def start(ledger, count, name):
            health = root / (name + ".status.json")
            process = subprocess.Popen(command(ledger, "-") +
                                       ["--from-input", str(count), "--status-file", str(health),
                                        "--feed-idle-timeout", "60", "--feed-message-timeout", "60"],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            children.append(process)
            wait_for(process, lambda: json.loads(health.read_text()).get("state") == "running",
                     "service running")
            return process, health

        def finish(process):
            output, errors = process.communicate(timeout=30)
            equal(process.returncode, 0)
            return json.loads(output), errors

        def send(process, event):
            process.stdin.write(json.dumps(event) + "\n")
            process.stdin.flush()

        try:
            ledger = seed("full-verification")
            expected = report(ledger, "--at-input", "4").stdout
            equal(report(ledger, "--verify-full", "--at-input", "4").stdout, expected)
            equal(report(ledger, "--verify-full").stdout, report(ledger).stdout)
            execute(ledger, "UPDATE inputs SET state_hash='changed' WHERE input_index=0;")
            equal(report(ledger, "--at-input", "4").stdout, expected)
            refused = report(ledger, "--verify-full", "--at-input", "4", code=1)
            equal(refused.stdout, "")
            assert "native ledger report integrity mismatch" in refused.stderr, refused.stderr
            print("PASS full verification reaches the chain below the checkpoint", flush=True)

            ledger = seed("startup-diagnostic")
            expected = report(ledger).stdout
            execute(ledger, "DROP TRIGGER report_checkpoint_no_update;"
                            "UPDATE report_checkpoints SET payload=payload || ' ' WHERE input_cursor=4;")
            process, health = start(ledger, len(events), "startup-diagnostic")
            status = json.loads(health.read_text())
            equal(status["checkpoint_diagnostic"]["code"], "payload_hash_mismatch")
            equal(status["checkpoint_diagnostic"]["cursor"], 4)
            equal(status["checkpoint_diagnostic"]["reason"], None)
            equal([status["checkpoint_diagnostic"][key] for key in
                   ("count", "budget_count", "build_count", "io_count")], [0, 0, 0, 0])
            equal(status["ready"], True)
            equal(report(ledger).stdout, expected)
            summary, errors = finish(process)
            equal(summary["inputs_processed"], 0)
            equal([(item["code"], item["cursor"]) for item in diagnostic_lines(errors)],
                  [("payload_hash_mismatch", 4)])
            equal(json.loads(health.read_text())["checkpoint_diagnostic"], status["checkpoint_diagnostic"])
            print("PASS startup fallback publishes its diagnostic to stderr and service status", flush=True)

            ledger = seed("consistent-edit")
            expected = report(ledger, "--at-input", "4").stdout
            payload = query(ledger, "SELECT payload FROM report_checkpoints WHERE input_cursor=4")
            wrong = payload.replace('"input_cursor":4', '"input_cursor":3', 1)
            assert wrong != payload, (payload, wrong)
            with sqlite3.connect(ledger) as database:
                database.execute("DROP TRIGGER report_checkpoint_no_update")
                changed = database.execute("UPDATE report_checkpoints SET payload=?,payload_hash=? "
                                           "WHERE input_cursor=4",
                                           (wrong, hashlib.sha256(wrong.encode()).hexdigest()))
                equal(changed.rowcount, 1)
            equal(report(ledger, "--at-input", "4").stdout, wrong + "\n")
            equal(report(ledger, "--verify-full", "--at-input", "4").stdout, expected)
            refused = invoke(command(ledger), 1)
            assert "native replay report checkpoint mismatch" in refused.stderr, refused.stderr
            equal(query(ledger, "SELECT COUNT(*) FROM inputs"), len(events))
            print("PASS a consistently edited checkpoint is refused by replay equality", flush=True)

            ledger = root / "optional-write.sqlite"
            process, health = start(ledger, 0, "optional-write")
            execute(ledger, "CREATE TRIGGER reject_report_checkpoint BEFORE INSERT ON report_checkpoints "
                            "BEGIN SELECT RAISE(ABORT,'checkpoint test'); END;")
            send(process, events[0])
            send(process, events[1])
            wait_for(process, lambda: (
                json.loads(health.read_text()).get("checkpoint_diagnostic") or {}).get("cursor") == 2,
                "checkpoint write diagnostic")
            status = json.loads(health.read_text())
            equal(status["checkpoint_diagnostic"]["code"], "write_failed")
            equal(status["checkpoint_diagnostic"]["count"], 1)
            write_diagnostic(status["checkpoint_diagnostic"], "io", io=1)
            equal(status["state"], "running")
            equal(status["ready"], True)
            equal(query(ledger, "SELECT COUNT(*) FROM inputs"), 2)
            equal(query(ledger, "SELECT COUNT(*) FROM report_snapshots WHERE input_cursor=2"), 1)
            equal(query(ledger, "SELECT COUNT(*) FROM report_integrity WHERE input_cursor=2"), 1)
            equal(query(ledger, "SELECT COUNT(*) FROM report_checkpoints"), 0)
            send(process, events[2])
            wait_for(process, lambda: query(ledger, "SELECT COUNT(*) FROM inputs") == 3,
                     "next input commits after checkpoint failure")
            execute(ledger, "DROP TRIGGER reject_report_checkpoint;")
            send(process, events[3])
            wait_for(process, lambda: query(ledger, "SELECT MAX(input_cursor) FROM report_checkpoints") == 4,
                     "next geometric checkpoint succeeds")
            equal(report(ledger).stdout, report(ledger, "--verify-full").stdout)
            summary, errors = finish(process)
            equal(summary["inputs_committed"], 4)
            equal(summary["inputs_processed"], 4)
            equal([(item["code"], item["cursor"]) for item in diagnostic_lines(errors)],
                  [("write_failed", 2)])
            equal([item["count"] for item in diagnostic_lines(errors)], [1])
            equal(diagnostic_lines(errors), [status["checkpoint_diagnostic"]])
            equal(json.loads(health.read_text())["checkpoint_diagnostic"], status["checkpoint_diagnostic"])
            print("PASS a failed checkpoint leaves its input durable and the service running", flush=True)

            ledger = root / "upgrade.sqlite"
            short_feed = root / "short.jsonl"
            short_feed.write_text("".join(json.dumps(event) + "\n" for event in events[:3]))
            invoke(command(ledger, short_feed, interval=100))
            expected = report(ledger, "--verify-full").stdout
            execute(ledger, "DROP TABLE report_checkpoints;")
            process, health = start(ledger, 3, "upgrade")
            equal(query(ledger, "SELECT COUNT(*) FROM inputs"), 3)
            equal(query(ledger, "SELECT COUNT(*) FROM report_checkpoints"), 1)
            equal(query(ledger, "SELECT MAX(input_cursor) FROM report_checkpoints"), 3)
            equal(report(ledger).stdout, expected)
            equal(json.loads(health.read_text())["checkpoint_diagnostic"], None)
            finish(process)
            print("PASS upgrade checkpoints at open before any new input", flush=True)

            ledger = root / "legacy-digests.sqlite"
            invoke(command(ledger, short_feed, interval=100))
            expected = report(ledger, "--verify-full").stdout
            execute(ledger, "DROP TABLE report_checkpoints; DROP TABLE report_integrity;")
            process, health = start(ledger, 3, "legacy-digests")
            equal(query(ledger, "SELECT COUNT(*) FROM inputs"), 3)
            equal(query(ledger, "SELECT MAX(input_cursor) FROM report_checkpoints"), 3)
            equal(report(ledger).stdout, expected)
            equal(json.loads(health.read_text())["checkpoint_diagnostic"]["code"], "write_failed")
            status = json.loads(health.read_text())
            write_diagnostic(status["checkpoint_diagnostic"], "build", build=1)
            _, errors = finish(process)
            equal(diagnostic_lines(errors), [status["checkpoint_diagnostic"]])
            print("PASS legacy digests are checkpointed after startup replay before intake", flush=True)
        finally:
            for process in children:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=10)


if __name__ == "__main__":
    main()
