"""Exercise service lifecycle through real pipes, SQLite and bounded loopback transports."""
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time

runner, library, gated_library = sys.argv[1:]
children = []
receipts = []
receiver_mode = "ok"
delivery_started = threading.Event()
secret = "service-test-secret-not-public"


def frame(opcode, payload, final=True):
    header = bytes([opcode | (0x80 if final else 0)])
    if len(payload) < 126:
        return header + bytes([len(payload)]) + payload
    return header + bytes([126]) + struct.pack("!H", len(payload)) + payload


class Receiver(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/redelivery":
            receipts.append((self.headers["Idempotency-Key"], body))
        delivery_started.set()
        mode = receiver_mode
        if mode == "hang":
            time.sleep(3)
        try:
            self.send_response(503 if mode == "fail" else 204)
            self.send_header("Content-Length", "0")
            self.end_headers()
        except OSError:
            pass

    def do_GET(self):
        if self.path == "/slow-http":
            time.sleep(3)
            try:
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()
            except OSError:
                pass
            return
        accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] +
            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        try:
            if self.path == "/keepalive":
                for _ in range(2):
                    time.sleep(5)
                    self.wfile.write(frame(10, b"alive"))
                    self.wfile.flush()
                self.wfile.write(frame(1, json.dumps(events[0]).encode()))
                self.wfile.flush()
            elif self.path == "/partial":
                self.wfile.write(frame(1, b"{", False))
                self.wfile.flush()
                for _ in range(20):
                    time.sleep(0.1)
                    self.wfile.write(frame(10, b"alive"))
                    self.wfile.flush()
            else:
                time.sleep(3)
        except OSError:
            pass

    def log_message(self, *args):
        pass


server = ThreadingHTTPServer(("127.0.0.1", 0), Receiver)
server.daemon_threads = True
threading.Thread(target=server.serve_forever, daemon=True).start()
endpoint = f"127.0.0.1:{server.server_port}"


def wait_for(predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (FileNotFoundError, json.JSONDecodeError, sqlite3.OperationalError):
            pass
        time.sleep(0.02)
    raise AssertionError("service condition timed out")


def invoke(arguments, code=0, stdin=None, env=None):
    result = subprocess.run([runner] + list(map(str, arguments)), input=stdin, text=True,
        capture_output=True, timeout=22, env=env)
    assert result.returncode == code, (arguments, result.returncode, result.stdout, result.stderr)
    assert secret not in result.stdout + result.stderr
    return result.stdout


def query(ledger, sql, arguments=()):
    with sqlite3.connect(ledger) as database:
        return database.execute(sql, arguments).fetchall()


def status(path):
    return json.loads(path.read_text())


def finish(process, code=0, stop=True, signal_number=signal.SIGTERM):
    if stop:
        process.send_signal(signal_number)
    else:
        process.wait(timeout=5)
    output, errors = process.communicate(timeout=5)
    assert process.returncode == code, (process.returncode, output, errors)
    assert secret not in output + errors
    return output


try:
    with tempfile.TemporaryDirectory(prefix="pineforge-service-e2e-") as directory:
        root = Path(directory)
        warmup = root / "warmup.csv"
        warmup.write_text("timestamp,open,high,low,close,volume\n" +
            "".join(f"{index * 60000},100,101,99,100,4\n" for index in range(3)))
        events = [{"type": "bar", "bar": {"ts_open": index * 60000, "o": 100 + index,
            "h": 102 + index, "l": 99 + index, "c": 101 + index, "v": 4, "trade_count": 4}}
            for index in range(3, 27)]
        batch = json.dumps({"type": "batch", "events": events}) + "\n"
        feed = root / "feed.jsonl"
        feed.write_text(batch)

        def base(ledger, module=library, mode="bars"):
            return ["run", "--strategy", module, "--warmup", warmup, "--mode", mode,
                "--script-tf", "3", "--symbol", "TEST:MOCK", "--ledger", ledger]

        def start(name, extra=(), module=library, env=None, mode="bars"):
            ledger, health = root / (name + ".sqlite"), root / (name + ".status.json")
            process = subprocess.Popen([runner] + list(map(str, base(ledger, module, mode) +
                ["--status-file", health, "--feed", "-"] + list(extra))), stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            children.append(process)
            wait_for(lambda: health.exists() and status(health).get("state") == "running")
            assert process.poll() is None
            return process, ledger, health

        def export(ledger):
            return json.loads(invoke(["report", "--ledger", ledger]))

        baseline = root / "baseline.sqlite"
        invoke(base(baseline, gated_library) + ["--feed", feed])
        expected_report = export(baseline)["report"]

        process, ledger, health = start("probes")
        invoke(["probe", "--status-file", health, "--max-age", "2", "--ready"])
        initial = status(health)["written_at_ms"]
        wait_for(lambda: status(health)["written_at_ms"] > initial)
        assert all(status(health)["readiness"].values())
        assert set(status(health)["metrics"]) - {"control_errors"} == {"committed_input", "last_seq", "source_timestamp_ms",
            "source_lag_ms", "queue_bytes", "ledger_bytes", "report_cursor", "targets"}
        duplicate = subprocess.run([runner] + list(map(str, base(ledger) + ["--feed", feed,
            "--status-file", health])), capture_output=True, text=True, timeout=5)
        assert duplicate.returncode == 1 and status(health)["liveness"]["alive"]
        process.stdin.write("{")
        process.stdin.flush()
        finish(process)
        assert query(ledger, "SELECT count(*) FROM inputs") == [(0,)]
        assert status(health)["state"] == "stopped"
        invoke(["probe", "--status-file", health, "--max-age", "2"], 1)
        sample = status(health)
        sample["liveness"] = {"alive": True, "control_loop_heartbeat_ms": 1}
        stale = root / "stale.json"
        stale.write_text(json.dumps(sample))
        invoke(["probe", "--status-file", stale, "--max-age", "2"], 1)
        stale.write_text("{")
        invoke(["probe", "--status-file", stale, "--max-age", "2"], 1)
        invoke(["probe", "--status-file", root / "missing", "--max-age", "2"], 1)
        print("PASS probes, private atomic heartbeat, ledger lock and SIGTERM mid-assembly", flush=True)

        process, ledger, health = start("status-failure")
        process.stdin.write(batch)
        process.stdin.flush()
        wait_for(lambda: query(ledger, "SELECT count(*) FROM inputs") == [(1,)])
        before = invoke(["report", "--ledger", ledger])
        health.unlink()
        health.mkdir()
        wait_for(lambda: process.poll() is not None)
        output, errors = process.communicate(timeout=5)
        assert process.returncode == 1 and "service status publication failed" in errors, (output, errors)
        assert invoke(["report", "--ledger", ledger]) == before
        print("PASS status-publication failure is fatal with committed report intact", flush=True)

        for phase in ("message", "commit"):
            gate = root / (phase + ".gate")
            environment = dict(os.environ, PINEFORGE_TEST_MESSAGE_GATE=str(gate))
            process, ledger, health = start(phase, module=gated_library, env=environment)
            blocker = sqlite3.connect(ledger) if phase == "commit" else None
            if blocker:
                blocker.execute("BEGIN IMMEDIATE")
                Path(str(gate) + ".resume").touch()
            process.stdin.write(batch)
            process.stdin.flush()
            wait_for(gate.exists)
            time.sleep(0.15)
            assert query(ledger, "SELECT count(*) FROM inputs") == [(0,)]
            process.send_signal(signal.SIGTERM)
            if blocker:
                blocker.rollback()
                blocker.close()
            else:
                Path(str(gate) + ".resume").touch()
            finish(process, stop=False)
            assert query(ledger, "SELECT count(*) FROM inputs") == [(1,)]
            assert query(ledger, "SELECT count(*) FROM report_snapshots") == [(2,)]
            assert export(ledger)["report"] == expected_report
            before = invoke(["report", "--ledger", ledger])
            invoke(base(ledger, gated_library) + ["--feed", feed])
            assert invoke(["report", "--ledger", ledger]) == before
        process, ledger, health = start("message", module=gated_library)
        assert not status(health)["readiness"]["verified_source_prefix"]
        invoke(["probe", "--status-file", health, "--max-age", "2"])
        invoke(["probe", "--status-file", health, "--max-age", "2", "--ready"], 1)
        process.stdin.write(batch)
        process.stdin.flush()
        wait_for(lambda: status(health)["ready"])
        finish(process, signal_number=signal.SIGINT)
        process, ledger, health = start("gap", mode="ticks")
        ticks = [{"type": "tick", "ts": 180001, "seq": sequence, "price": 100, "qty": 1}
            for sequence in (1, 3)]
        process.stdin.write("".join(json.dumps(tick) + "\n" for tick in ticks))
        process.stdin.flush()
        finish(process, 1, stop=False)
        assert not status(health)["readiness"]["no_unhealed_input_gap"]
        print("PASS SIGTERM during engine message and SQLite write acquisition; exact restart reports", flush=True)

        routes = root / "routes.json"
        routes.write_text(json.dumps({"schema_version": 1, "default_target": "default",
            "targets": {"default": {"url": f"http://{endpoint}/actions", "secret_env": "SERVICE_TEST_HMAC"}},
            "rules": [], "delivery": {"max_in_flight": 2, "connect_timeout_ms": 100,
                "total_timeout_ms": 500, "transport_retries": 0}}))
        routed = ["--webhook-routes", routes, "--allow-insecure-http"]
        environment = dict(os.environ, SERVICE_TEST_HMAC=secret)
        receiver_mode = "hang"
        delivery_started.clear()
        process, ledger, health = start("delivery", routed, env=environment)
        process.stdin.write(batch)
        process.stdin.flush()
        assert delivery_started.wait(5)
        began = time.monotonic()
        finish(process)
        assert time.monotonic() - began < 3
        assert query(ledger, "SELECT count(*) FROM inputs") == [(1,)]
        assert export(ledger)["report"] == expected_report
        assert query(ledger, "SELECT count(*) FROM events")[0][0] > 0
        assert secret not in health.read_text() and endpoint not in health.read_text()
        print("PASS SIGTERM mid-delivery with bounded unresponsive receiver and durable outbox", flush=True)

        receiver_mode = "fail"
        receipts.clear()
        redelivery_routes = json.loads(routes.read_text())
        redelivery_routes["targets"]["default"]["url"] = f"http://{endpoint}/redelivery"
        redelivery_routes["delivery"].update(connect_timeout_ms=1000, total_timeout_ms=3000)
        routes.write_text(json.dumps(redelivery_routes))
        controls = root / "control"
        process, ledger, health = start("redelivery", routed + ["--control-dir", controls], env=environment)
        process.stdin.write(batch)
        process.stdin.flush()
        wait_for(lambda: query(ledger, "SELECT count(*) FROM delivery_log WHERE phase='completed'") == [(4,)])
        wait_for(lambda: len(receipts) == 4)
        deployment = query(ledger, "SELECT identity FROM metadata")[0][0]
        original = sorted(receipts)
        wait_for(lambda: status(health)["metrics"]["targets"]["default"]["pending_count"] == 4)
        assert status(health)["metrics"]["targets"]["default"]["oldest_age_ms"] >= 0
        invoke(["redeliver", "--ledger", ledger, "--deployment", deployment,
            "--target", "default"], 1, env=environment)
        receiver_mode = "ok"
        response = json.loads(invoke(["redeliver", "--ledger", ledger, "--deployment", deployment,
            "--target", "default", "--failed-only", "--control-dir", controls]))
        identifier = response["request_id"]
        acknowledgment = controls / (identifier + ".ack.json")
        wait_for(acknowledgment.exists)
        assert json.loads(acknowledgment.read_text())["selected"] == 4
        wait_for(lambda: query(ledger, "SELECT count(*) FROM delivery_log WHERE request_id=? AND phase='completed'",
            (identifier,)) == [(4,)])
        wait_for(lambda: len(receipts) == 8)
        assert sorted(receipts[4:]) == original
        duplicate_request = {"schema_version": "pineforge-redelivery-request/v1", "deployment": deployment,
            "request_id": identifier, "target": "default", "from": 1, "failed_only": True}
        acknowledgement_time = acknowledgment.stat().st_mtime_ns
        (controls / (identifier + ".request.json")).write_text(json.dumps(duplicate_request))
        wait_for(lambda: acknowledgment.stat().st_mtime_ns != acknowledgement_time)
        time.sleep(0.15)
        assert len(receipts) == 8
        selected = json.loads(invoke(["redeliver", "--ledger", ledger, "--deployment", deployment,
            "--target", "default", "--from", "4", "--control-dir", controls]))["request_id"]
        wait_for(lambda: query(ledger, "SELECT count(*) FROM delivery_log WHERE request_id=? AND phase='completed'",
            (selected,)) == [(1,)])
        assert receipts[-1] in original
        bad_identifier = "d" * 64
        duplicate_request.update(request_id=bad_identifier, deployment="wrong", from_ignored=1)
        (controls / (bad_identifier + ".request.json")).write_text(json.dumps(duplicate_request))
        wait_for(lambda: (controls / (bad_identifier + ".ack.json")).exists())
        assert not json.loads((controls / (bad_identifier + ".ack.json")).read_text())["accepted"]
        before = invoke(["report", "--ledger", ledger])
        finish(process)
        assert invoke(["report", "--ledger", ledger]) == before
        process, ledger, health = start("redelivery", routed + ["--control-dir", controls], env=environment)
        time.sleep(0.15)
        assert len(receipts) == 9
        finish(process)
        invoke(["redeliver", "--ledger", ledger, "--deployment", deployment, "--target", "default",
            "--from", "4"], env=environment)
        assert len(receipts) == 10
        print("PASS live failed/selected redelivery, stable IDs, dedup, restart, audit and offline lock", flush=True)

        typo = root / "missing-control"
        invoke(["redeliver", "--ledger", ledger, "--deployment", deployment, "--target", "default",
            "--control-dir", typo], 1)
        assert not typo.exists()
        invoke(["redeliver", "--ledger", ledger, "--deployment", deployment, "--target", "default",
            "--control-dir", controls], 1)
        broken = root / "broken-control"
        process, ledger, health = start("control-errors", ["--control-dir", broken])
        (broken / ("1" * 64 + ".request.json")).mkdir()
        outside = root / "outside.json"
        outside.write_text("{}")
        (broken / ("2" * 64 + ".request.json")).symlink_to(outside)
        unreadable = broken / ("3" * 64 + ".request.json")
        unreadable.write_text("{}")
        unreadable.chmod(0)
        (broken / ("4" * 64 + ".request.json")).write_text("x" * 20000)
        (broken / ("5" * 64 + ".request.json")).write_text("{")
        wait_for(lambda: status(health)["metrics"]["control_errors"] >= 5)
        assert process.poll() is None and status(health)["state"] == "running"
        process.stdin.write(batch)
        process.stdin.flush()
        wait_for(lambda: query(ledger, "SELECT count(*) FROM inputs") == [(1,)])
        finish(process)
        process, ledger, health = start("control-errors", ["--control-dir", broken])
        wait_for(lambda: status(health)["metrics"]["control_errors"] >= 5)
        assert process.poll() is None and outside.read_text() == "{}"
        finish(process)
        unreadable.chmod(0o600)
        print("PASS invalid control entries survive intake and restart; missing/offline control submission refused", flush=True)

        pipe_ledger = root / "closed-stdout.sqlite"
        pipe = subprocess.Popen([runner] + list(map(str, base(pipe_ledger) + ["--feed", feed, "--report-jsonl"])),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        children.append(pipe)
        pipe.stdout.close()
        assert pipe.wait(timeout=15) == 1
        assert query(pipe_ledger, "SELECT count(*) FROM inputs") == [(1,)]
        assert "stdout write failed" in pipe.stderr.read().decode()

        for option in ("--feed-idle-timeout", "--feed-message-timeout", "--status-interval"):
            for value in ("0", "301", "-1", "no"):
                invoke(base(root / "invalid.sqlite") + ["--feed", feed, option, value], 1)
        process, ledger, health = start("idle", ["--feed-idle-timeout", "1", "--feed-message-timeout", "3"])
        finish(process, 1, stop=False)
        assert status(health)["state"] == "failed"
        assert status(health)["readiness"]["no_unhealed_input_gap"]
        process, ledger, health = start("assembly", ["--feed-idle-timeout", "3", "--feed-message-timeout", "1"])
        process.stdin.write("{")
        process.stdin.flush()
        finish(process, 1, stop=False)
        for path in ("idle-ws", "partial", "slow-http"):
            scheme = "http" if path == "slow-http" else "ws"
            invoke(base(root / (path + ".sqlite")) + ["--feed-url", f"{scheme}://{endpoint}/{path}",
                "--allow-insecure-http", "--feed-idle-timeout", "1", "--feed-message-timeout", "2"], 1)
        invoke(base(root / "keepalive.sqlite") + ["--feed-url", f"ws://{endpoint}/keepalive",
            "--allow-insecure-http", "--feed-idle-timeout", "8", "--feed-message-timeout", "1",
            "--max-events", "1"])
        print("PASS deadline bounds, stdin/HTTP/WS idle, fragmented assembly and 5-second PONG keepalive", flush=True)

        health = root / "budget.status.json"
        ledger = root / "budget.sqlite"
        invoke(base(ledger) + ["--feed", root / "empty.jsonl"], 1)
        (root / "empty.jsonl").write_text("")
        invoke(base(ledger) + ["--feed", root / "empty.jsonl"])
        budget = ledger.stat().st_size + 65536
        process, ledger, health = start("budget", ["--max-ledger-bytes", str(budget)])
        long_events = [{"type": "bar", "bar": {"ts_open": index * 60000, "o": 100 + index,
            "h": 102 + index, "l": 99 + index, "c": 101 + index, "v": 4, "trade_count": 4}}
            for index in range(3, 303)]
        large_batch = json.dumps({"type": "batch", "events": long_events}) + "\n"
        process.stdin.write(large_batch + json.dumps({"type": "time", "ts": 303 * 60000}) + "\n")
        process.stdin.flush()
        finish(process, 3, stop=False)
        assert query(ledger, "SELECT count(*) FROM inputs") == [(1,)]
        assert export(ledger)["input_cursor"] == 1
        assert status(health)["state"] == "storage_budget" and not status(health)["readiness"]["storage_below_budget"]
        invoke(["probe", "--status-file", health, "--max-age", "2", "--ready"], 1)
        before = invoke(["report", "--ledger", ledger])
        invoke(base(ledger) + ["--feed", feed, "--max-ledger-bytes", "1", "--status-file", health], 3)
        assert invoke(["report", "--ledger", ledger]) == before
        print("PASS storage budget stops after whole message without losing cursor, outbox or report", flush=True)
finally:
    for process in children:
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
    server.shutdown()
    server.server_close()
