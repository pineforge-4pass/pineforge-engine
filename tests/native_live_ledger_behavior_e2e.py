"""Real CLI ledger contracts; this file is also usable against the frozen CLI.

Keep output artifacts even on failure. --inputs controls the growth population;
the work row always compares N and 10N. --mode delta retains the v1 output-work
checks; delta-v2 also compares observed ratio work with v1 on the same feeds.
"""
import argparse
import copy
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import signal
import sqlite3
import struct
import subprocess
import tempfile
import threading
import time

ARRAYS = {"closed_trades", "trades", "equity_curve", "security_diag",
          "trace", "trace_names", "broker_state_hash"}
RATIOS = {"sharpe_monthly", "sortino_monthly", "sharpe_bar", "sortino_bar"}
SCHEMA = "pineforge-native-report-delta/v2"
TEST_SECRET = "local-ledger-receiver"


def test_environment():
    return dict(os.environ, PINEFORGE_LEDGER_TEST_HMAC=TEST_SECRET,
                NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost")


def same(left, right):
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(same(left[key], right[key]) for key in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(same(a, b) for a, b in zip(left, right))
    if isinstance(left, float):
        return struct.pack("!d", left) == struct.pack("!d", right)
    return left == right


def query(path, sql, args=()):
    with sqlite3.connect(f"file:{path}?mode=ro", uri=True) as db:
        return db.execute(sql, args).fetchall()


def footprint(path):
    wal = Path(str(path) + "-wal")
    return path.stat().st_size + (wal.stat().st_size if wal.exists() else 0)


def free_disk(path):
    disk = os.statvfs(path.parent)
    return disk.f_bavail * disk.f_frsize


def file_hash(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def wait_for(predicate, process):
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        assert process.poll() is None, "runner exited before the semantic assertion"
        try:
            value = predicate()
            if value:
                return value
        except (FileNotFoundError, json.JSONDecodeError, sqlite3.OperationalError):
            pass
        time.sleep(0.01)
    raise AssertionError("runner did not reach the required committed cursor")


def merge(destination, changes, changed_only=False):
    for key, value in changes.items():
        if isinstance(value, dict):
            assert value or not changed_only, ("empty unchanged object", key)
            merge(destination.setdefault(key, {}), value, changed_only)
        else:
            if changed_only:
                assert key not in destination or not same(destination[key], value), ("unchanged field", key)
            destination[key] = copy.deepcopy(value)


def fold(document, delta, changed_only=False):
    merge(document, delta["fields"], changed_only)
    for name, patch in delta["arrays"].items():
        assert name in ARRAYS and set(patch) == {"offset", "items"}
        owner = document if name == "closed_trades" else document.setdefault("report", {})
        previous = owner.setdefault(name, [])
        offset = patch["offset"]
        assert type(offset) is int and 0 <= offset <= len(previous)
        assert isinstance(patch["items"], list)
        if changed_only:
            assert not same(previous[offset:], patch["items"]), ("unchanged array suffix", name)
            if offset < len(previous) and patch["items"]:
                assert not same(previous[offset], patch["items"][0]), ("unchanged leading item", name)
        owner[name] = previous[:offset] + copy.deepcopy(patch["items"])


class Fixture:
    def __init__(self, options):
        self.options = options
        self.root = Path(tempfile.mkdtemp(prefix=options.row + "-", dir=options.artifacts))
        print("artifacts:", self.root, flush=True)
        self.warmup = self.root / "warmup.csv"
        self.warmup.write_text("timestamp,open,high,low,close,volume\n"
                               "1704067200000,100,102,98,101,4\n")
        self.native_config = self.root / "native.json"
        self.native_config.write_text(json.dumps({
            "run": {"session_key": "ledger-behavior", "run_number": 1},
            "clock": {"input_tf": "1", "script_tf": "1", "timezone": "UTC",
                      "session": "24x7", "chart_timezone": ""},
            "instrument": {"ticker": "LEDGER", "tickerid": "TEST:LEDGER", "type": "crypto",
                           "currency": "USDT", "basecurrency": "ETH", "description": "",
                           "volumetype": ""},
            "execution": {"initial_capital": 10000, "point_value": 1, "account_fx": 1,
                          "price_tick": 0.01, "slippage_ticks": 0, "fee_kind": "CashPerExecution",
                          "fee_value": 6, "quantity_grid": None, "close_execution": "NextEligiblePoint",
                          "max_abs_units": None, "max_open_lots": None,
                          "allowed_open_directions": 3, "initial_margin_fraction": None},
        }))
        self.serial = 0
        (self.root / "manifest.json").write_text(json.dumps({
            "row": options.row, "inputs": options.inputs, "mode": options.mode,
            "binaries": {name: {"path": str(path), "sha256": file_hash(path)}
                         for name, path in (("runner", options.runner), ("strategy", options.strategy),
                                            ("fixture_writer", options.fixture_writer))},
            "warmup_sha256": file_hash(self.warmup)}, indent=2) + "\n")

    def bars(self, count):
        for index in range(1, count + 1):
            price = 100 + (index * 37 % 101) / 13
            yield {"type": "bar", "bar": {"ts_open": 1704067200000 + index * 60000,
                "o": price, "h": price + 2, "l": price - 2, "c": price + 0.25, "v": 4}}

    def command(self, ledger, mode=None, feed="-"):
        command = [self.options.runner, "run", "--strategy", self.options.strategy,
            "--warmup", str(self.warmup), "--script-tf", "1", "--symbol", "TEST:LEDGER",
            "--native-config", str(self.native_config),
            "--mode", "bars", "--feed", str(feed), "--ledger", str(ledger),
            "--feed-idle-timeout", "300", "--feed-message-timeout", "300"]
        if mode:
            command += ["--report-jsonl=" + mode]
        return command

    def run(self, command, text=None, expected=0, observed=False):
        self.serial += 1
        name = self.root / f"command-{self.serial}"
        environment = test_environment()
        log = name.with_suffix(".observer.jsonl")
        if observed:
            environment["PINEFORGE_LEDGER_WORK_LOG"] = str(log)
        result = subprocess.run(list(map(str, command)), input="" if text is None else text, text=True,
                                capture_output=True, env=environment)
        name.with_suffix(".command.json").write_text(json.dumps(list(map(str, command))) + "\n")
        name.with_suffix(".stdout").write_text(result.stdout)
        name.with_suffix(".stderr").write_text(result.stderr)
        name.with_suffix(".exit").write_text(str(result.returncode) + "\n")
        assert result.returncode == expected, (command, result.returncode, result.stderr)
        work = [json.loads(line) for line in log.read_text().splitlines()] if observed else []
        return result, work


class Receiver:
    def __init__(self):
        self.hold = threading.Event()
        self.hold.set()
        self.rows = []
        self.errors = []
        self.code = 204
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                signature = "sha256=" + hmac.new(TEST_SECRET.encode(), body, hashlib.sha256).hexdigest()
                if self.headers["X-PineForge-Signature"] != signature:
                    owner.errors.append("receiver signature differs from the independent HMAC")
                owner.rows.append((body.decode(), self.headers["Idempotency-Key"]))
                owner.hold.wait()
                try:
                    self.send_response(owner.code)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.hold.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def delivery_pending_tail_work(f):
    receiver = Receiver()
    try:
        routes = f.root / "routes.json"
        routes.write_text(json.dumps({"schema_version": 1,
            "targets": {name: {"url": f"http://127.0.0.1:{receiver.server.server_port}/",
                               "secret_env": "PINEFORGE_LEDGER_TEST_HMAC"}
                        for name in ("main", "side", "third")},
            "default_target": "main", "rules": [
                {"match": {"order_id": "journal-close"}, "target": None},
                {"match": {"order_id": "side-open"}, "target": "side"},
                {"match": {"order_id": "third-close"}, "target": "third"}],
            "delivery": {"max_in_flight": 1, "transport_retries": 0,
                         "connect_timeout_ms": 2000, "total_timeout_ms": 300000}}))
        measurements = []
        for count in (f.options.inputs, 10 * f.options.inputs):
            ledger = f.root / f"delivery-{count}.sqlite"
            feed = f.root / f"delivery-{count}.jsonl"
            feed.write_text("".join(json.dumps(row) + "\n" for row in f.bars(count)))
            command = f.command(ledger) + ["--allow-insecure-http", "--webhook-routes", str(routes),
                "--ratio-interval", str(count + 1), "--checkpoint-interval", str(count + 1)]
            receiver.hold.clear()
            with (f.root / f"seed-{count}.stdout").open("w") as out, (
                    f.root / f"seed-{count}.stderr").open("w") as err:
                child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=out, stderr=err,
                                         text=True, env=test_environment())
                try:
                    child.stdin.write(feed.read_text())
                    child.stdin.flush()
                    wait_for(lambda: query(ledger, "SELECT COUNT(*) FROM inputs")[0][0] == count, child)
                    # Terminate this owned fixture child while its receiver is
                    # held, leaving real durable actions for the fixture writer.
                    child.kill()
                    child.communicate()
                    assert child.returncode == -signal.SIGKILL
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.communicate()
            receiver.hold.set()
            f.run([f.options.fixture_writer, "--settle-prefix", ledger, "1"])
            source = query(ledger, "SELECT input_index,canonical_json,state_hash FROM inputs ORDER BY input_index")
            actions = query(ledger, "SELECT ordinal,event_id,payload FROM events ORDER BY ordinal")
            tail = query(ledger, "SELECT e.payload,r.delivery_id FROM events e JOIN event_routes r USING(ordinal) "
                "WHERE e.acknowledged=0 AND r.target_id IS NOT NULL AND NOT EXISTS "
                "(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed')")
            assert len(tail) == 1
            assert query(ledger, "SELECT COUNT(DISTINCT event_id) FROM delivery_log "
                         "WHERE phase='completed'")[0][0] > count // 4
            assert query(ledger, "SELECT COUNT(*) FROM event_routes WHERE target_id IS NULL")[0][0] > 0
            receiver.rows.clear()
            status_path = f.root / f"delivery-{count}.status.json"
            before_ms = int(time.time() * 1000)
            result, work = f.run(command + ["--from-input", str(count), "--status-file", status_path], observed=True)
            after_ms = int(time.time() * 1000)
            assert json.loads(result.stdout)["webhooks_delivered"] == 1
            assert receiver.rows == [(tail[0][0], tail[0][1])]
            probes = [row for row in work if row["type"] == "dispatch"]
            assert probes and any(row["type"] == "sqlite" and row["vm_steps"] > 0 for row in work), \
                "observer did not cover actual CLI SQLite calls"
            # Count through the tail and first exhausted seek, excluding later
            # idle polling. With K=1, returned historical rows must be <=K+0;
            # at most K+3 probes and 512 VM steps are allowed.
            through_empty = []
            for probe in probes:
                through_empty.append(probe)
                if probe["rows"] == 0:
                    break
            rows = sum(row["rows"] for row in through_empty)
            steps = sum(row["vm_steps"] for row in through_empty)
            measurements.append({"inputs": count, "rows": rows, "probes": len(through_empty), "vm_steps": steps})
            (f.root / "dispatch-work.json").write_text(json.dumps(measurements, indent=2) + "\n")
            assert query(ledger, "SELECT COUNT(*) FROM events e JOIN event_routes r USING(ordinal) "
                "WHERE e.acknowledged=0 AND r.target_id IS NOT NULL AND NOT EXISTS "
                "(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed')")[0][0] == 0
            service_targets = json.loads(status_path.read_text())["metrics"]["targets"]
            status, _ = f.run([f.options.runner, "status", "--ledger", ledger])
            for target in ("main", "side", "third"):
                pending, oldest = query(ledger,
                    "SELECT COUNT(*),MIN(CASE WHEN e.created_at_ms>0 THEN e.created_at_ms ELSE "
                    "(SELECT MIN(d.started_at) FROM delivery_log d WHERE d.event_id=e.event_id) END) "
                    "FROM events e JOIN event_routes r USING(ordinal) WHERE e.acknowledged=0 AND r.target_id=?",
                    (target,))[0]
                metric = service_targets[target]
                assert metric["pending_count"] == pending
                if oldest is None:
                    assert metric["oldest_age_ms"] is None
                else:
                    assert max(0, before_ms - oldest) <= metric["oldest_age_ms"] <= max(0, after_ms - oldest)
                assert json.loads(status.stdout)["targets"][target]["unsent"] == 0
            assert query(ledger, "SELECT input_index,canonical_json,state_hash FROM inputs ORDER BY input_index") == source
            assert query(ledger, "SELECT ordinal,event_id,payload FROM events ORDER BY ordinal") == actions
            # A completed failure is not automatically retried on restart.
            failed = query(ledger, "SELECT r.target_id,e.ordinal FROM events e JOIN event_routes r USING(ordinal) "
                "WHERE e.acknowledged=0 AND r.target_id IS NOT NULL ORDER BY e.ordinal")
            assert len(failed) == 1
            receiver.rows.clear()
            f.run(command + ["--from-input", str(count)])
            assert receiver.rows == []
            deployment = query(ledger, "SELECT identity FROM metadata")[0][0]
            expected_failed = query(ledger,
                "SELECT e.payload,r.delivery_id FROM events e JOIN event_routes r USING(ordinal) WHERE e.ordinal=?",
                (failed[0][1],))[0]
            f.run([f.options.runner, "redeliver", "--ledger", ledger, "--deployment", deployment,
                   "--target", failed[0][0], "--from", str(failed[0][1]), "--failed-only"])
            assert receiver.rows == [expected_failed]
            assert query(ledger, "SELECT ordinal,event_id,payload FROM events ORDER BY ordinal") == actions
        assert all(row["rows"] <= 1 and row["probes"] <= 4 and row["vm_steps"] <= 512
                   for row in measurements), measurements
        assert measurements[1]["rows"] <= measurements[0]["rows"], measurements
        assert not receiver.errors, receiver.errors
    finally:
        receiver.close()


def ledger_observability_status(f):
    for fraction in (None, "1e-15", "1"):
        ledger = f.root / f"observe-{fraction}.sqlite"
        status = f.root / f"observe-{fraction}.status.json"
        command = f.command(ledger) + ["--status-file", str(status),
                                      "--status-interval", str(f.options.status_interval)]
        if fraction is not None:
            command += ["--ledger-warn-free-fraction", fraction]
        with (f.root / f"observe-{fraction}.stdout").open("w") as out, (
                f.root / f"observe-{fraction}.stderr").open("w") as err:
            started = time.time()
            child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=out, stderr=err,
                                     text=True, env=test_environment())
            try:
                initial = wait_for(lambda: (value if (value := json.loads(status.read_text()))
                    ["metrics"]["committed_input"] == 0 and value["ready"] else None), child)
                initial_bytes = footprint(ledger)
                initial_time = time.time()
                assert initial["metrics"]["ledger_storage"]["used_bytes"] == initial_bytes
                assert initial["metrics"]["ledger_storage"]["bytes_per_input"] is None
                child.stdin.write("".join(json.dumps(row) + "\n" for row in f.bars(24)))
                child.stdin.flush()
                document = wait_for(lambda: (value if (value := json.loads(status.read_text()))
                    ["metrics"]["committed_input"] == 24 and value["ready"] else None), child)
                used = footprint(ledger)
                storage = document["metrics"]["ledger_storage"]
                assert storage["used_bytes"] == used, (storage, used)
                assert math.isclose(storage["bytes_per_input"], used / 24, rel_tol=1e-12)
                effective = 0.8 if fraction is None else float(fraction)
                assert storage["warn_free_fraction"] == effective
                free = free_disk(ledger)
                # Other host activity may consume a few pages between samples.
                tolerance = f.options.disk_sample_tolerance_bytes
                assert abs(storage["free_disk_bytes"] - free) <= tolerance
                expected_warning = used >= effective * free
                assert storage["state"] == ("WARNING" if expected_warning else "OK")
                assert document["ready"] is True and document["readiness"]["storage_below_budget"] is True
                # Independent file extents and host timestamps bound the rate.
                # Sampling precedes publication by at most one configured
                # writer interval; the baseline precedes our first ready read.
                # The additional 50 ms permits timestamp/publication skew.
                publication_skew = f.options.status_interval + 0.05
                elapsed_lower = max(0.001, status.stat().st_mtime - initial_time - publication_skew)
                elapsed_upper = time.time() - started + 0.05
                growth_lower = (used - initial_bytes) / elapsed_upper
                growth_upper = (used - initial_bytes) / elapsed_lower
                assert growth_lower > 0
                assert growth_lower <= storage["growth_bytes_per_second"] <= growth_upper
                assert max(0, free - tolerance) / growth_upper <= storage["seconds_to_disk_exhaustion"] \
                    <= (free + tolerance) / growth_lower
                f.run([f.options.runner, "probe", "--status-file", status, "--max-age", "300", "--ready"])
                offline, _ = f.run([f.options.runner, "status", "--ledger", ledger])
                offline_storage = json.loads(offline.stdout)["ledger_storage"]
                assert offline_storage["used_bytes"] == used
                assert offline_storage["seconds_to_disk_exhaustion"] is None
                stdout, stderr = child.communicate()
                assert child.returncode == 0, (stdout, stderr)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.communicate()
    for invalid in ("0", "-0.1", "1.00001", "nan", "inf", "word", "", "1e999"):
        ledger = f.root / "invalid.sqlite"
        result, _ = f.run(f.command(ledger) + ["--ledger-warn-free-fraction", invalid], expected=1)
        assert not ledger.exists()
        assert "ledger-warn-free-fraction" in result.stderr
    f.run(f.command(f.root / "duplicate.sqlite") +
          ["--ledger-warn-free-fraction", "0.5", "--ledger-warn-free-fraction", "0.5"], expected=1)


def report_default_delta_work(f):
    def measure(count, mode, feed):
        ledger = f.root / f"work-{mode}-{count}.sqlite"
        result, observed = f.run(f.command(ledger, mode, feed), observed=True)
        reports = [json.loads(line) for line in result.stdout.splitlines()]
        assert reports[-1]["inputs_committed"] == count
        assert len(reports) == count + 1
        passes = [row["points"] for row in observed if row["type"] == "ratio"]
        assert passes and passes[0] > 0, "observer missed the real warmup ratio pass"
        assert any(row["type"] == "sqlite" and row["vm_steps"] > 0 for row in observed)
        assert query(ledger, "SELECT COUNT(*) FROM events")[0][0] > count // 2
        final = json.loads(f.run([f.options.runner, "report", "--ledger", ledger])[0].stdout)
        assert len(final["closed_trades"]) > count // 4 and len(final["report"]["equity_curve"]) >= count
        ordinary = passes[1:-1]  # Warmup and finite-feed final work are separate.
        row = {"inputs": count, "mode": mode, "bytes": len(result.stdout.encode()),
               "ratio_calls": len(passes), "ratio_points": sum(passes),
               "warmup_ratio_points": passes[0], "ordinary_ratio_points": sum(ordinary),
               "final_ratio_points": passes[-1],
               "serialized_rows": sum(len(patch["items"]) for report in reports[:-1]
                                      for patch in report["arrays"].values())}
        row["bytes_per_input"] = row["bytes"] / count
        row["serialized_rows_per_input"] = row["serialized_rows"] / count
        print("report-work:", json.dumps(row, sort_keys=True), flush=True)
        return row

    measurements = []
    for count in (f.options.inputs, 10 * f.options.inputs):
        feed = f.root / f"work-{count}.jsonl"
        feed.write_text("".join(json.dumps(row) + "\n" for row in f.bars(count)))
        row = measure(count, f.options.mode, feed)
        if f.options.mode == "delta-v2":
            row["v1_delta"] = measure(count, "delta", feed)
        measurements.append(row)
        (f.root / "report-work.json").write_text(json.dumps(measurements, indent=2) + "\n")
    for key in ("bytes", "serialized_rows"):
        assert measurements[1][key] <= 12 * measurements[0][key] + 4096, (key, measurements)
    if f.options.mode == "delta-v2":
        # At 10,000/100,000 inputs, 20% allows wider cursor/offset/number text.
        # Resending growing history would cost about 10x per input and fail.
        per_input_growth_factor = 1.2
        for key in ("bytes_per_input", "serialized_rows_per_input"):
            assert measurements[1][key] <= per_input_growth_factor * measurements[0][key], \
                (key, per_input_growth_factor, measurements)
        for row in measurements:
            for key in ("ratio_calls", "ratio_points", "ordinary_ratio_points"):
                assert row[key] <= row["v1_delta"][key], ("v2 ratio work exceeds v1 delta", key, row)


def report_jsonl_delta_v2_contract(f):
    count = 31
    feed = f.root / "contract.jsonl"
    feed.write_text("".join(json.dumps(row) + "\n" for row in f.bars(count)))
    full_path = f.root / "full.sqlite"
    full, _ = f.run(f.command(full_path, "full", feed))
    full_rows = [json.loads(line) for line in full.stdout.splitlines()[:-1]]
    for interval, pipe in ((1, False), (1, True), (7, False), (7, True)):
        path = f.root / f"v2-{interval}-{pipe}.sqlite"
        extra = ["--ratio-interval", str(interval)]
        reference = full_rows
        if interval > 1:
            interval_full, _ = f.run(f.command(f.root / f"full-{interval}-{pipe}.sqlite", "full", feed) + extra)
            reference = [json.loads(line) for line in interval_full.stdout.splitlines()[:-1]]
            # v1 interval freshness lives inside the legacy cumulative document.
            # v2 carries that control information only in its envelope.
            for document in reference:
                document.pop("ratios_as_of_input")
                document.pop("ratio_context")
        result, _ = f.run(f.command(path, "delta-v2", "-" if pipe else feed) + extra,
                          text=feed.read_text() if pipe else None)
        lines = result.stdout.splitlines()
        records = [json.loads(line) for line in lines[:-1]]
        terminal = pipe and count % interval != 0
        assert len(records) == count + int(terminal)
        assert json.loads(lines[-1])["inputs_committed"] == count
        base = json.loads(f.run([f.options.runner, "report", "--ledger", path,
            "--at-input", "0", "--delta-base"])[0].stdout)
        assert set(base) == {"input_cursor", "document", "ratios", "schema_version"}
        assert base["schema_version"] == "pineforge-native-report-base/v2"
        document = copy.deepcopy(base["document"])
        stored = dict(query(path, "SELECT input_cursor,payload FROM report_snapshots ORDER BY input_cursor"))
        for index, (line, delta) in enumerate(zip(lines, records)):
            cursor = min(index + 1, count)
            assert line.startswith('{"input_cursor":' + str(cursor) + ',')
            assert list(delta) == ["input_cursor", "arrays", "fields", "kind", "ratios", "schema_version"]
            assert delta["schema_version"] == SCHEMA and delta["input_cursor"] == cursor
            assert delta["kind"] == ("final" if index == count else "input")
            assert set(delta["ratios"]) == {"as_of_input", "chart_timezone", "exact"}
            as_of = delta["ratios"]["as_of_input"]
            assert delta["ratios"] == {"as_of_input": as_of, "chart_timezone": "UTC", "exact": as_of == cursor}
            assert as_of == (cursor if index == count or (not pipe and cursor == count)
                             else cursor // interval * interval)
            assert not (ARRAYS & delta["fields"].keys())
            assert not ((ARRAYS - {"closed_trades"}) & delta["fields"].get("report", {}).keys())
            if index == count:
                assert delta["arrays"] == {}
            else:
                stored_delta = json.loads(stored[cursor])
                assert "input_cursor" not in stored_delta
                assert stored_delta == {key: value for key, value in delta.items() if key != "input_cursor"}
            fold(document, delta, changed_only=True)
            expected = copy.deepcopy(reference[cursor - 1])
            if as_of != cursor:
                ratio_base = reference[as_of - 1] if as_of else base["document"]
                expected["report"]["metrics"]["equity"].update(
                    {key: ratio_base["report"]["metrics"]["equity"][key] for key in RATIOS})
            assert same(document, expected)
        assert same(document, reference[-1])
        assert same(json.loads(f.run([f.options.runner, "report", "--ledger", path])[0].stdout), document)
        # Bootstrap from a stale cursor; export's exact values are not the base.
        stale = json.loads(f.run([f.options.runner, "report", "--ledger", path,
            "--at-input", "8", "--delta-base"])[0].stdout)
        assert stale["ratios"]["as_of_input"] == 8 // interval * interval
        resumed = stale["document"]
        fold(resumed, records[8], changed_only=True)
        rebuilt = copy.deepcopy(base["document"])
        for delta in records[:9]:
            fold(rebuilt, delta)
        assert same(resumed, rebuilt)
        before = query(path, "SELECT input_cursor,payload FROM report_snapshots ORDER BY input_cursor")
        restart, _ = f.run(f.command(path, "delta-v2", feed) + extra)
        assert json.loads(restart.stdout.splitlines()[-1])["inputs_processed"] == 0
        assert query(path, "SELECT input_cursor,payload FROM report_snapshots ORDER BY input_cursor") == before
    # A required-write refusal advances the engine before rolling back SQL.
    # The final patch must reconstruct the committed prefix, never that input.
    stopped = f.root / "storage-stop.sqlite"
    input_count = f.options.storage_stop_inputs
    interval = input_count + 1
    feed_text = "".join(json.dumps(row) + "\n" for row in f.bars(input_count))
    result, _ = f.run(f.command(stopped, "delta-v2") + ["--ratio-interval", str(interval),
        "--max-ledger-bytes", str(f.options.storage_stop_bytes)],
        text=feed_text, expected=3)
    committed = query(stopped, "SELECT COUNT(*) FROM inputs")[0][0]
    assert 8 <= committed < input_count, "storage fixture must refuse a live input after a real prefix"
    assert '"code":"scratch_capacity_exceeded"' in result.stderr
    assert '"args":{"resource":"layout"}' in result.stderr
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert rows[-1]["inputs_committed"] == committed
    records = rows[:-1]
    assert len(records) == committed + 1
    assert records[-1]["kind"] == "final" and records[-1]["input_cursor"] == committed
    assert records[-1]["arrays"] == {} and records[-1]["ratios"]["exact"] is True
    base = json.loads(f.run([f.options.runner, "report", "--ledger", stopped,
        "--at-input", "0", "--delta-base"])[0].stdout)
    document = base["document"]
    for delta in records:
        fold(document, delta, changed_only=True)
    prefix_feed = f.root / "committed-prefix.jsonl"
    prefix_feed.write_text("".join(json.dumps(row) + "\n" for row in f.bars(committed)))
    reference, _ = f.run(f.command(f.root / "committed-full.sqlite", "full", prefix_feed)
                          + ["--ratio-interval", str(interval)])
    expected = json.loads(reference.stdout.splitlines()[-2])
    expected.pop("ratios_as_of_input")
    expected.pop("ratio_context")
    assert same(document, expected)
    assert same(json.loads(f.run([f.options.runner, "report", "--ledger", stopped])[0].stdout), document)
    assert json.loads(query(stopped, "SELECT payload FROM report_snapshots WHERE input_cursor=?",
                           (committed,))[0][0])["ratios"]["as_of_input"] == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("row", choices=["delivery_pending_tail_work", "ledger_observability_status",
        "report_default_delta_work", "report_jsonl_delta_v2_contract"])
    parser.add_argument("--runner", required=True)
    parser.add_argument("--strategy", required=True)
    parser.add_argument("--fixture-writer", required=True)
    parser.add_argument("--inputs", type=int, default=10000)
    parser.add_argument("--disk-sample-tolerance-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--status-interval", type=int, default=1)
    parser.add_argument("--storage-stop-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--storage-stop-inputs", type=int, default=4096)
    parser.add_argument("--mode", choices=["delta-v2", "delta"], default="delta-v2")
    parser.add_argument("--artifacts", required=True)
    options = parser.parse_args()
    Path(options.artifacts).mkdir(parents=True, exist_ok=True)
    globals()[options.row](Fixture(options))


if __name__ == "__main__":
    main()
