"""Real CLI ledger contracts; this file is also usable against the frozen CLI.

Keep output artifacts even on failure. --inputs controls the growth population;
the work row always compares N and 10N. --mode delta retains the v1 output-work
checks; delta-v2 also compares observed ratio work with v1 on the same feeds.
"""
import argparse
import copy
from fractions import Fraction
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import shutil
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


def force_write(directory, count):
    """Simulate another process writing the same filesystem between two samples."""
    path = Path(directory) / "forced-write.bin"
    try:
        with path.open("wb") as output:
            remaining = count
            while remaining > 0:
                chunk = os.urandom(min(1 << 20, remaining))
                remaining -= output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise
    return path


def check_free_disk_report(reported, low, high, fragment, capacity):
    values = (f"free_disk_bytes={reported!r}, observed=[{low}, {high}], "
              f"fragment={fragment}, capacity={capacity}")
    assert type(reported) is int, values
    assert reported % fragment == 0, values
    assert 0 < reported <= capacity, values
    assert low <= reported <= high, values


def check_state_report(used, warn_fraction, free, state):
    expected = "WARNING" if Fraction(used) >= Fraction(warn_fraction) * free else "OK"
    assert state == expected, (f"state={state!r}, expected={expected!r}, used_bytes={used!r}, "
                              f"warn_free_fraction={warn_fraction!r}, free_disk_bytes={free!r}")


def check_exhaustion_report(free, growth, seconds):
    values = (f"free_disk_bytes={free!r}, growth_bytes_per_second={growth!r}, "
              f"seconds_to_disk_exhaustion={seconds!r}")
    assert type(free) is int, values
    assert 0 < free <= 2 ** 53, values
    assert growth > 0, values
    expected = float(free) / float(growth)
    assert seconds == expected, f"{values}, expected={expected!r}"


class FreeSpaceEnvelope:
    # Two endpoint samples only contain the runner's value when free space
    # moves in one direction. A temporary file can appear and disappear while
    # the runner samples, leaving both endpoints above its value. Sample
    # throughout the window and keep the smallest and largest values observed,
    # so the report must match a free-space level the filesystem really had.
    def __init__(self, sample):
        self.sample = sample
        self.low = None
        self.high = None
        self.count = 0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._sample_loop, daemon=True)

    def record(self, value):
        self.low = value if self.low is None else min(self.low, value)
        self.high = value if self.high is None else max(self.high, value)
        self.count += 1

    def _sample_loop(self):
        while not self.stop.is_set():
            # statvfs releases the interpreter lock; keep the sample gap short.
            self.record(self.sample())

    def __enter__(self):
        self.record(self.sample())
        self.thread.start()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.stop.set()
        self.thread.join()
        self.record(self.sample())


def bracketed_document(status, child, ledger, between=None):
    with FreeSpaceEnvelope(lambda: free_disk(ledger)) as envelope:
        start = time.time_ns()
        # The runner samples and publishes in the same pulse. The first
        # publication after start might have been sampled just before start;
        # the second publication was sampled inside this window.
        first = wait_for(lambda: (mtime if (mtime := status.stat().st_mtime_ns) > start
                                  else None), child)
        document = wait_for(lambda: json.loads(status.read_text())
                            if status.stat().st_mtime_ns > first else None, child)
        assert document["metrics"]["committed_input"] == 24, document["metrics"]
        if between is not None:
            between()
    return document, envelope.low, envelope.high


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


def ledger_observability_check_selftest():
    def accept(name, check, *args):
        check(*args)
        print("CHECK", name, "accepted", flush=True)

    def reject(name, check, *args):
        try:
            check(*args)
        except AssertionError:
            print("CHECK", name, "rejected", flush=True)
        else:
            raise AssertionError(f"{name}: helper accepted the invalid values {args!r}")

    fragment = 4096
    capacity = 4 * 1024 ** 4
    low = 10 ** 12
    high = low + 4 * fragment
    reported = low + 2 * fragment
    accept("free-constant", check_free_disk_report, low, low, low, fragment, capacity)
    accept("free-interior", check_free_disk_report, reported, low, high, fragment, capacity)
    accept("free-low-edge", check_free_disk_report, low, low, high, fragment, capacity)
    accept("free-high-edge", check_free_disk_report, high, low, high, fragment, capacity)
    accept("free-write-decrease", check_free_disk_report,
           low - 4 * fragment, low - 8 * fragment, low, fragment, capacity)
    accept("free-delete-increase", check_free_disk_report,
           low + 4 * fragment, low, low + 8 * fragment, fragment, capacity)
    accept("free-temp-dip", check_free_disk_report,
           low - 8192, low - 8192, low, fragment, capacity)
    accept("state-ok", check_state_report, fragment, 0.5, low, "OK")
    accept("state-warning", check_state_report, fragment, 1e-15, low, "WARNING")
    growth = 1234.5
    expected = float(low) / float(growth)
    accept("exhaustion-exact", check_exhaustion_report, low, growth, expected)

    reject("free-above-high", check_free_disk_report,
           high + fragment, low, high, fragment, capacity)
    reject("free-below-low", check_free_disk_report,
           low - fragment, low, high, fragment, capacity)
    reject("free-outside-point", check_free_disk_report,
           low + fragment, low, low, fragment, capacity)
    reject("free-wrong-units", check_free_disk_report,
           reported // 1024, low, high, fragment, capacity)
    reject("free-capacity-outside", check_free_disk_report,
           capacity, low, high, fragment, capacity)
    reject("free-other-filesystem", check_free_disk_report,
           high + 10 * 1024 ** 3, low, high, fragment, capacity)
    for name, value in (("float", float(reported)), ("str", str(reported)), ("bool", True)):
        reject(f"free-type-{name}", check_free_disk_report, value, low, high, fragment, capacity)
    reject("free-zero", check_free_disk_report, 0, 0, high, fragment, capacity)
    reject("free-above-capacity", check_free_disk_report,
           capacity + fragment, capacity, capacity + fragment, fragment, capacity)
    reject("exhaustion-ulp-up", check_exhaustion_report,
           low, growth, math.nextafter(expected, math.inf))
    reject("exhaustion-ulp-down", check_exhaustion_report,
           low, growth, math.nextafter(expected, -math.inf))
    reject("state-flipped-warning", check_state_report, fragment, 0.5, low, "WARNING")
    reject("state-flipped-ok", check_state_report, fragment, 1e-15, low, "OK")
    too_large = 2 ** 53 + 1
    reject("exhaustion-free-too-large", check_exhaustion_report,
           too_large, growth, float(too_large) / float(growth))
    reject("exhaustion-zero-growth", check_exhaustion_report, low, 0, expected)
    reject("exhaustion-negative-growth", check_exhaustion_report, low, -1, expected)
    for name, value in (("float", float(low)), ("str", str(low)), ("bool", True)):
        reject(f"exhaustion-free-type-{name}", check_exhaustion_report,
               value, growth, float(value) / float(growth))
    reject("exhaustion-zero-free", check_exhaustion_report, 0, growth, 0.0)

    sequences = (
        ("envelope-constant", (low, low, low), low, low),
        ("envelope-decrease", (high, reported, low), low, high),
        ("envelope-increase", (low, reported, high), low, high),
        ("envelope-temp-dip", (low, low - 8192, low), low - 8192, low),
        ("envelope-first-only", (low,), low, low),
    )
    for name, values, expected_low, expected_high in sequences:
        envelope = FreeSpaceEnvelope(lambda: low)
        for value in values:
            envelope.record(value)
        assert envelope.low == expected_low, (name, values, envelope.low, expected_low)
        assert envelope.high == expected_high, (name, values, envelope.high, expected_high)
        assert envelope.count == len(values), (name, envelope.count, len(values))
        print("CHECK", name, "recorded", flush=True)

    with FreeSpaceEnvelope(lambda: low) as envelope:
        assert envelope.low == envelope.high == low, (envelope.low, envelope.high, low)
    assert envelope.count >= 2, envelope.count
    assert envelope.low == envelope.high == low, (envelope.low, envelope.high, low)
    assert not envelope.thread.is_alive(), "free-space sampler still running after exit"
    print("CHECK envelope-context stopped", flush=True)

    failure = RuntimeError("envelope body failure")
    try:
        with FreeSpaceEnvelope(lambda: low) as envelope:
            raise failure
    except RuntimeError as error:
        assert error is failure, error
    else:
        raise AssertionError("free-space envelope suppressed the body exception")
    assert envelope.count >= 2, envelope.count
    assert envelope.low == envelope.high == low, (envelope.low, envelope.high, low)
    assert not envelope.thread.is_alive(), "free-space sampler still running after body exception"
    print("CHECK envelope-exception stopped", flush=True)


def ledger_observability_status(f):
    shared = Path(tempfile.mkdtemp(prefix="ledger-observe-", dir="/dev/shm"))
    try:
        for variant, fraction in (("disk", None), ("disk", "1e-15"), ("disk", "1"),
                                  ("tmpfs", None)):
            name = f"observe-{variant}-{fraction}"
            ledger_directory = (f.root if variant == "disk" else shared) / name
            ledger_directory.mkdir()
            output_directory = shared if variant == "disk" else f.root
            ledger = ledger_directory / "ledger.sqlite"
            status = output_directory / f"{name}.status.json"
            assert os.stat(ledger.parent).st_dev != os.stat(status.parent).st_dev, \
                "/dev/shm must be a different filesystem than the artifacts directory; this row must not skip"
            filesystem = os.statvfs(ledger.parent)
            fragment = filesystem.f_frsize
            capacity = filesystem.f_blocks * filesystem.f_frsize
            command = f.command(ledger) + ["--status-file", str(status),
                                          "--status-interval", str(f.options.status_interval)]
            if fraction is not None:
                command += ["--ledger-warn-free-fraction", fraction]
            with (output_directory / f"{name}.stdout").open("w") as out, (
                    output_directory / f"{name}.stderr").open("w") as err:
                started = time.time()
                child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=out, stderr=err,
                                         text=True, env=test_environment())
                forced = None
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
                    check_state_report(used, storage["warn_free_fraction"],
                                       storage["free_disk_bytes"], storage["state"])
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
                    check_exhaustion_report(storage["free_disk_bytes"], storage["growth_bytes_per_second"],
                                            storage["seconds_to_disk_exhaustion"])
                    between = None
                    if variant == "disk" and f.options.force_write_between_samples_bytes > 0:
                        count = f.options.force_write_between_samples_bytes

                        def between():
                            nonlocal forced
                            forced = force_write(ledger.parent, count)
                            written = forced.stat()
                            assert written.st_size == count, ("forced write size", written.st_size, count)
                            assert written.st_blocks * 512 >= count, \
                                ("forced write allocated bytes", written.st_blocks * 512, count)

                    bracket, low, high = bracketed_document(status, child, ledger, between=between)
                    bracket_storage = bracket["metrics"]["ledger_storage"]
                    check_free_disk_report(bracket_storage["free_disk_bytes"], low, high, fragment, capacity)
                    check_state_report(used, bracket_storage["warn_free_fraction"],
                                       bracket_storage["free_disk_bytes"], bracket_storage["state"])
                    check_exhaustion_report(bracket_storage["free_disk_bytes"],
                                            bracket_storage["growth_bytes_per_second"],
                                            bracket_storage["seconds_to_disk_exhaustion"])
                    f.run([f.options.runner, "probe", "--status-file", status, "--max-age", "300", "--ready"])
                    offline, _ = f.run([f.options.runner, "status", "--ledger", ledger])
                    offline_storage = json.loads(offline.stdout)["ledger_storage"]
                    assert offline_storage["used_bytes"] == used
                    assert offline_storage["seconds_to_disk_exhaustion"] is None
                    stdout, stderr = child.communicate()
                    assert child.returncode == 0, (stdout, stderr)
                finally:
                    if forced is not None:
                        forced.unlink(missing_ok=True)
                    if child.poll() is None:
                        child.kill()
                        child.communicate()
    finally:
        shutil.rmtree(shared, ignore_errors=True)
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
        "ledger_observability_check_selftest", "report_default_delta_work",
        "report_jsonl_delta_v2_contract"])
    parser.add_argument("--runner")
    parser.add_argument("--strategy")
    parser.add_argument("--fixture-writer")
    parser.add_argument("--inputs", type=int, default=10000)
    parser.add_argument("--force-write-between-samples-bytes", type=int, default=0)
    parser.add_argument("--status-interval", type=int, default=1)
    parser.add_argument("--storage-stop-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--storage-stop-inputs", type=int, default=4096)
    parser.add_argument("--mode", choices=["delta-v2", "delta"], default="delta-v2")
    parser.add_argument("--artifacts")
    options = parser.parse_args()
    if options.row == "ledger_observability_check_selftest":
        ledger_observability_check_selftest()
        return
    missing = ["--" + name.replace("_", "-")
               for name in ("runner", "strategy", "fixture_writer", "artifacts")
               if getattr(options, name) is None]
    if missing:
        parser.error("the following arguments are required for this row: " + ", ".join(missing))
    Path(options.artifacts).mkdir(parents=True, exist_ok=True)
    globals()[options.row](Fixture(options))


if __name__ == "__main__":
    main()
