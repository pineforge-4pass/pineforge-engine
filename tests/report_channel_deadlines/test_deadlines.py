"""Real-process tests for R06/R08/R16/R17 and T18/T19/T20/T21/T30.

Every test fails in setUp on main: the required module is absent. No module
functions or clocks are replaced. The separate Linux interposer records real
syscall returns and supplies faults the kernel cannot reliably schedule.
"""

import contextlib
import errno
import hashlib
import json
import math
import os
from pathlib import Path
import select
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MODULE = ROOT / "docker/pineforge_report_deadlines.py"
FIXTURE = HERE / "process_fixture.py"
TIMING_SLACK_MS = 250
EARLY_SLACK_MS = 40
_injector_dir = None
_injector_library = None


def reference(length, seed):
    return bytes((((index % 256) * 37) + seed) & 255 for index in range(length))


def build_injector():
    global _injector_dir, _injector_library
    if _injector_library is None:
        _injector_dir = tempfile.TemporaryDirectory(prefix="report-deadline-injector-")
        candidate = Path(_injector_dir.name) / "syscall_faults.so"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
            str(HERE / "syscall_faults.c"), "-o", str(candidate),
        ]
        built = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if built.returncode:
            raise AssertionError("external fault injector did not build:\n" +
                                 built.stdout + built.stderr)
        _injector_library = candidate
    return _injector_library


def tearDownModule():
    if _injector_dir is not None:
        _injector_dir.cleanup()


class Process:
    def __init__(self, role, config, fds=(), env=None, cwd=None):
        self.process = subprocess.Popen(
            [sys.executable, "-I", "-B", "-u", str(FIXTURE), role,
             json.dumps(config)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            pass_fds=tuple(fds), env=env, cwd=cwd,
        )
        self.buffer = b""

    def __enter__(self):
        try:
            self.ready()
        except BaseException:
            self.__exit__()
            raise
        return self

    def __exit__(self, *exc):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()

    def line(self, timeout):
        expiry = time.monotonic() + timeout
        poller = select.poll()
        poller.register(self.process.stdout, select.POLLIN)
        while b"\n" not in self.buffer:
            remaining = expiry - time.monotonic()
            if remaining <= 0:
                raise AssertionError("fixture exceeded its external wall bound")
            if not poller.poll(max(1, math.ceil(remaining * 1000))):
                continue
            part = os.read(self.process.stdout.fileno(), 65536)
            if not part:
                self.process.wait(timeout=2)
                detail = self.process.stderr.read().decode(errors="replace")
                raise AssertionError(
                    "fixture exited before its outcome, returncode=" +
                    str(self.process.returncode) + "\n" + detail)
            self.buffer += part
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def ready(self):
        if self.line(5) != {"ready": True}:
            raise AssertionError("fixture readiness protocol failed")

    def command(self, text):
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()

    def go(self):
        self.command("go")

    def result(self, timeout):
        value = self.line(timeout)
        self.finish()
        return value

    def finish(self):
        self.process.wait(timeout=2)
        if self.process.returncode != 0:
            raise AssertionError(self.process.stderr.read().decode(errors="replace"))


def trace_events(stream):
    stream.seek(0)
    events = []
    for line in stream.read().decode().splitlines():
        name, fd, requested, result, error, flags, tick = line.split()
        events.append({
            "name": name, "fd": int(fd), "requested": int(requested),
            "result": int(result), "errno": int(error), "flags": int(flags),
            "tick": int(tick),
        })
    return events


def positive_total(events, name):
    return sum(event["result"] for event in events
               if event["name"] == name and event["result"] > 0)


def socket_pair(tcp=False):
    if tcp:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            left = socket.create_connection(listener.getsockname())
            right, _ = listener.accept()
    else:
        left, right = socket.socketpair()
    for endpoint in (left, right):
        endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
        endpoint.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
    return left, right


class DeadlineProcesses(unittest.TestCase):
    def setUp(self):
        self.assertTrue(
            MODULE.is_file(),
            "main has no docker/pineforge_report_deadlines.py; "
            "every report deadline test must fail before implementation")

    def environment(self, trace, config, faults):
        environment = os.environ.copy()
        environment["LD_PRELOAD"] = str(build_injector())
        environment["REPORT_TEST_TARGET_FD"] = str(config.get("fd", -1))
        read_fd = config.get("src_fd", -1)
        if config["operation"] in ("read", "read_many"):
            read_fd = config["fd"]
        environment["REPORT_TEST_SOURCE_FD"] = str(read_fd)
        environment["REPORT_TEST_TRACE_FD"] = str(trace.fileno())
        for key, value in faults.items():
            environment["REPORT_TEST_" + key] = str(value)
        return environment

    def assert_timing(self, elapsed, expected_ms):
        self.assertGreaterEqual(elapsed * 1000, expected_ms - EARLY_SLACK_MS)
        self.assertLessEqual(elapsed * 1000, expected_ms + TIMING_SLACK_MS)

    def assert_failure(self, outcome, name, kind=None, moved=None, stage="publish"):
        self.assertFalse(outcome["ok"], outcome)
        self.assertEqual(outcome["exception"], name)
        self.assertEqual(outcome["kind"], kind)
        self.assertEqual(outcome["stage_hint"], stage)
        if moved is not None:
            self.assertEqual(outcome["bytes_moved"], moved)

    def assert_prefix(self, peer, size, seed):
        self.assertEqual(peer["count"], size)
        self.assertEqual(peer["sha256"],
                         hashlib.sha256(reference(size, seed)).hexdigest())

    def assert_finite_polls(self, events, maximum_ms):
        polls = [event for event in events if event["name"] == "poll"]
        self.assertTrue(polls, "fixture must actually encounter backpressure")
        for event in polls:
            self.assertGreaterEqual(event["requested"], 0)
            self.assertLessEqual(event["requested"], maximum_ms)

    def pause(self, process):
        os.kill(process.process.pid, signal.SIGSTOP)
        expiry = time.monotonic() + 1
        while time.monotonic() < expiry:
            pid, status = os.waitpid(process.process.pid,
                                     os.WNOHANG | os.WUNTRACED)
            if pid:
                self.assertTrue(os.WIFSTOPPED(status),
                                "reader must really be stopped before publication")
                return
            time.sleep(0.003)
        self.fail("reader did not enter the kernel stopped state")

    def single(self, config, fds=(), faults=None, bound_ms=1000):
        with tempfile.TemporaryFile() as trace:
            environment = self.environment(trace, config, faults or {})
            with Process("worker", config, (*fds, trace.fileno()),
                         env=environment) as worker:
                started = time.monotonic()
                worker.go()
                outcome = worker.result((bound_ms + TIMING_SLACK_MS) / 1000)
                elapsed = time.monotonic() - started
            return outcome, elapsed, trace_events(trace)

    def write_case(self, peer_options=None, options=None, faults=None,
                   bound_ms=1000, paused=False, storm=False, tcp=False):
        config = {"operation": "write", "size": 262144, "seed": 93,
                  "wall_ms": 1000, "stall_ms": 500, "stage_hint": "publish"}
        config.update(options or {})
        with contextlib.ExitStack() as stack:
            left, right = socket_pair(tcp)
            stack.enter_context(left)
            stack.enter_context(right)
            config["fd"] = left.fileno()
            peer_config = {"fd": right.fileno()}
            peer_config.update(peer_options or {})
            peer = stack.enter_context(Process("reader", peer_config,
                                               (right.fileno(),)))
            trace = stack.enter_context(tempfile.TemporaryFile())
            environment = self.environment(trace, config, faults or {})
            worker = stack.enter_context(Process(
                "worker", config, (left.fileno(), trace.fileno()), env=environment))
            left.close()
            right.close()
            peer.go()
            if paused:
                self.pause(peer)
            early_peer = None
            if peer_config.get("close_after") == 0:
                early_peer = peer.result(1)
            storm_process = None
            if storm:
                storm_process = stack.enter_context(Process("storm", {
                    "pid": worker.process.pid, "interval_ms": 2,
                    "duration_ms": config["wall_ms"] + 1000,
                }))
                storm_process.go()
            started = time.monotonic()
            worker.go()
            if peer_config.get("mode") == "after_backpressure":
                # pread leaves the writer's shared trace offset untouched.
                while True:
                    entries = []
                    for line in os.pread(trace.fileno(), 1048576, 0).decode().splitlines():
                        fields = line.split()
                        if len(fields) == 7:
                            entries.append(fields)
                    if any(fields[0] == "send" and
                           int(fields[4]) == errno.EAGAIN
                           for fields in entries):
                        peer.command("drain")
                        break
                    if time.monotonic() - started > bound_ms / 1000:
                        self.fail("real socket never reached EAGAIN before its bound")
                    time.sleep(0.003)
            outcome = worker.line((bound_ms + TIMING_SLACK_MS) / 1000)
            elapsed = time.monotonic() - started
            if storm_process is not None:
                storm_process.command("stop")
                self.assertGreater(storm_process.result(2)["sent"], 10)
                worker.command("release")
            worker.finish()
            if paused:
                os.kill(peer.process.pid, signal.SIGCONT)
            if peer_config.get("mode") == "never":
                peer.command("drain")
            peer_result = early_peer if early_peer is not None else peer.result(3)
            return outcome, peer_result, elapsed, trace_events(trace)

    def read_case(self, writer_options=None, options=None, faults=None,
                  bound_ms=1000):
        config = {"operation": "read", "nmax": 31, "wall_ms": 1000,
                  "stall_ms": 400, "stage_hint": "finalize"}
        config.update(options or {})
        with contextlib.ExitStack() as stack:
            incoming, outgoing = socket_pair()
            stack.enter_context(incoming)
            stack.enter_context(outgoing)
            config["fd"] = incoming.fileno()
            writer_config = {"fd": outgoing.fileno(), "size": 97, "seed": 19}
            writer_config.update(writer_options or {})
            writer = stack.enter_context(Process("writer", writer_config,
                                                 (outgoing.fileno(),)))
            trace = stack.enter_context(tempfile.TemporaryFile())
            worker = stack.enter_context(Process(
                "worker", config, (incoming.fileno(), trace.fileno()),
                env=self.environment(trace, config, faults or {})))
            incoming.close()
            outgoing.close()
            writer.go()
            started = time.monotonic()
            worker.go()
            outcome = worker.result((bound_ms + TIMING_SLACK_MS) / 1000)
            elapsed = time.monotonic() - started
            peer = writer.result(2)
            return outcome, peer, elapsed, trace_events(trace)

    def relay_case(self, size, allowance, chunk=127, paused=False,
                   wall_ms=1000, stall_ms=500, writer_options=None):
        with contextlib.ExitStack() as stack:
            source, producer = socket_pair()
            destination, consumer = socket_pair()
            for endpoint in (source, producer, destination, consumer):
                stack.enter_context(endpoint)
            config = {
                "operation": "relay", "src_fd": source.fileno(),
                "fd": destination.fileno(), "allowance": allowance,
                "wall_ms": wall_ms, "stall_ms": stall_ms,
                "stage_hint": "run", "chunk": chunk,
            }
            writer_config = {"fd": producer.fileno(), "size": size, "seed": 71}
            writer_config.update(writer_options or {})
            writer = stack.enter_context(Process("writer", writer_config,
                                                 (producer.fileno(),)))
            reader = stack.enter_context(Process("reader", {"fd": consumer.fileno()},
                                                 (consumer.fileno(),)))
            trace = stack.enter_context(tempfile.TemporaryFile())
            worker = stack.enter_context(Process(
                "worker", config,
                (source.fileno(), destination.fileno(), trace.fileno()),
                env=self.environment(trace, config, {})))
            for endpoint in (source, producer, destination, consumer):
                endpoint.close()
            reader.go()
            if paused:
                self.pause(reader)
            writer.go()
            started = time.monotonic()
            worker.go()
            outcome = worker.result((wall_ms + TIMING_SLACK_MS) / 1000)
            elapsed = time.monotonic() - started
            if paused:
                os.kill(reader.process.pid, signal.SIGCONT)
            peer = reader.result(2)
            sent = writer.result(2)
            return outcome, peer, sent, elapsed, trace_events(trace)

    def test_import_has_no_signal_thread_file_or_descriptor_side_effects(self):
        """R16/R17: import alone cannot activate channel enforcement."""
        with tempfile.TemporaryDirectory() as directory:
            with Process("import", {}, cwd=directory) as probe:
                probe.go()
                result = probe.result(1)
        self.assertEqual(result["before"], result["after"])
        self.assertEqual(result["after"]["files"], [])
        self.assertEqual(len(result["after"]["tasks"]), 1)
        self.assertEqual(result["after"]["sigpipe"], int(signal.SIG_DFL))

    def test_deadline_progress_changes_only_stall(self):
        """R16: positive progress resets P; zero progress resets neither."""
        wall_ms, stall_ms, delay_ms = 360, 130, 60
        outcome, elapsed, _ = self.single({
            "operation": "deadline", "wall_ms": wall_ms, "stall_ms": stall_ms,
            "progress_after_ms": delay_ms, "progress_bytes": 11,
        }, bound_ms=wall_ms)
        self.assertTrue(outcome["ok"], outcome)
        result = outcome["result"]
        self.assertLessEqual(result["wall_after"], result["wall_before"])
        self.assertLessEqual(result["stall_zero"], result["stall_before"])
        self.assertGreater(result["stall_positive"] - result["stall_zero"],
                           delay_ms - EARLY_SLACK_MS)
        self.assertEqual(result["remaining_at_expiry"], 0)
        self.assert_timing(elapsed, wall_ms)

    def test_idle_computation_does_not_consume_pending_publication_stall(self):
        """R16: P begins when bytes become pending, after idle computation."""
        size, seed = 83, 41
        outcome, peer, _, events = self.write_case(options={
            "operation": "idle_write", "idle_ms": 190, "stall_ms": 70,
            "wall_ms": 700, "size": size, "seed": seed,
        }, bound_ms=700)
        self.assertTrue(outcome["ok"], outcome)
        self.assertFalse(outcome["result"]["idle_expired"])
        self.assertEqual(outcome["result"]["written"], size)
        self.assert_prefix(peer, size, seed)
        self.assertEqual(positive_total(events, "send"), size)

    def test_idle_computation_cannot_renew_wall(self):
        """R16: a late call cannot start a fresh W."""
        outcome, peer, _, events = self.write_case(options={
            "operation": "idle_write", "idle_ms": 180,
            "wall_ms": 100, "stall_ms": 500,
        }, bound_ms=250)
        self.assert_failure(outcome, "DeadlineError", "wall", 0)
        self.assert_prefix(peer, 0, 93)
        self.assertEqual(positive_total(events, "send"), 0)

    def test_nonreading_peer_stalls_with_exact_prefix(self):
        """T18/T20, R16/R17: open reader, no reads, P plus 120 ms slack."""
        stall_ms = 150
        outcome, peer, elapsed, events = self.write_case(
            {"mode": "never"}, {"wall_ms": 1400, "stall_ms": stall_ms},
            bound_ms=stall_ms)
        written = positive_total(events, "send")
        self.assertGreater(written, 0)
        self.assert_failure(outcome, "DeadlineError", "stall", written)
        self.assert_prefix(peer, written, 93)
        self.assert_timing(elapsed, stall_ms)
        self.assert_finite_polls(events, stall_ms)

    def test_one_byte_reader_cannot_renew_absolute_wall(self):
        """T18/T30, R16: ongoing one-byte progress still ends at fixed W."""
        wall_ms, stall_ms = 620, 240
        outcome, peer, elapsed, events = self.write_case(
            {"mode": "bytewise", "interval_ms": 20},
            {"wall_ms": wall_ms, "stall_ms": stall_ms},
            {"SEND_LIMIT": 1}, bound_ms=wall_ms)
        written = positive_total(events, "send")
        self.assert_failure(outcome, "DeadlineError", "wall", written)
        self.assert_prefix(peer, written, 93)
        self.assert_timing(elapsed, wall_ms)
        positive = [event for event in events
                    if event["name"] == "send" and event["result"] > 0]
        self.assertTrue(all(event["result"] == 1 for event in positive))
        self.assertGreater((positive[-1]["tick"] - positive[0]["tick"]) / 1e6,
                           wall_ms / 2)
        self.assertGreater(len(peer["read_times"]), 10)
        self.assert_finite_polls(events, stall_ms)

    def test_real_short_writes_and_eagain_deliver_exact_payload(self):
        """T30, R17: tiny real buffers force short writes and EAGAIN."""
        size, seed = 524305, 137
        outcome, peer, _, events = self.write_case(
            {"mode": "after_backpressure"},
            options={"size": size, "seed": seed, "wall_ms": 2500,
                     "stall_ms": 800}, bound_ms=2500)
        self.assertTrue(outcome["ok"], outcome)
        self.assertTrue(outcome["fd_live"])
        self.assertEqual(outcome["result"]["written"], size)
        self.assert_prefix(peer, size, seed)
        self.assertEqual(positive_total(events, "send"), size)
        self.assertTrue(any(event["name"] == "send" and
                            0 < event["result"] < event["requested"]
                            for event in events))
        self.assertTrue(any(event["name"] == "send" and
                            event["errno"] == errno.EAGAIN for event in events))
        self.assert_finite_polls(events, 800)

    def test_external_recoverable_write_faults_do_not_duplicate_counts(self):
        """T30, R17: actual short sends plus externally injected EINTR/EAGAIN."""
        size, seed = 9029, 53
        outcome, peer, _, events = self.write_case(
            options={"size": size, "seed": seed},
            faults={"SEND_LIMIT": 173, "SEND_FAULTS": "real,eintr,eagain"})
        self.assertTrue(outcome["ok"], outcome)
        self.assertEqual(outcome["result"]["written"], size)
        self.assert_prefix(peer, size, seed)
        self.assertEqual(positive_total(events, "send"), size)
        errors = {event["errno"] for event in events if event["name"] == "send"}
        self.assertTrue({errno.EINTR, errno.EAGAIN}.issubset(errors))

    def test_interrupted_poll_retries_with_finite_timeouts(self):
        """T30, R16/R17: poll EINTR is recovered without an infinite wait."""
        outcome, peer, _, events = self.write_case(
            options={"size": 3011},
            faults={"SEND_FAULTS": "eagain", "POLL_FAULTS": "eintr,eintr"})
        self.assertTrue(outcome["ok"], outcome)
        self.assert_prefix(peer, 3011, 93)
        self.assertEqual(positive_total(events, "send"), 3011)
        self.assertEqual(sum(event["name"] == "poll" and
                             event["errno"] == errno.EINTR for event in events), 2)
        self.assert_finite_polls(events, 500)

    def test_zero_write_after_real_prefix_is_terminal_io(self):
        """T30, R17: a zero result is neither success nor another retry."""
        prefix = 113
        outcome, peer, _, events = self.write_case(
            faults={"SEND_LIMIT": prefix, "SEND_FAULTS": "real,zero"})
        self.assert_failure(outcome, "TransportError", moved=prefix)
        self.assertEqual(outcome["errno"], errno.EIO)
        self.assert_prefix(peer, prefix, 93)
        self.assertEqual(positive_total(events, "send"), prefix)
        self.assertTrue(any(event["name"] == "send" and event["result"] == 0
                            for event in events))

    def test_invalid_write_descriptor_is_typed_io(self):
        """T30, R17: a genuinely invalid descriptor yields EBADF."""
        outcome, _, _ = self.single({
            "operation": "write", "fd": -1, "size": 193,
            "stage_hint": "publish",
        })
        self.assert_failure(outcome, "TransportError", moved=0)
        self.assertEqual(outcome["errno"], errno.EBADF)

    def test_unrecoverable_io_after_real_prefix_is_terminal(self):
        """T30, R17: EIO/EBADF preserve successful bytes before the fault."""
        for fault, expected in (("eio", errno.EIO), ("ebadf", errno.EBADF)):
            with self.subTest(fault=fault):
                outcome, peer, _, events = self.write_case(
                    faults={"SEND_LIMIT": 71, "SEND_FAULTS": "real," + fault})
                self.assert_failure(outcome, "TransportError", moved=71)
                self.assertEqual(outcome["errno"], expected)
                self.assert_prefix(peer, 71, 93)
                self.assertEqual(positive_total(events, "send"), 71)
                self.assertTrue(any(event["name"] == "send" and
                                    event["errno"] == expected for event in events))

    def test_closed_peer_survives_default_sigpipe_disposition(self):
        """T19/T20, R17: a real closed peer cannot terminate the publisher."""
        outcome, peer, _, events = self.write_case({"close_after": 0})
        self.assert_failure(outcome, "PeerClosed", moved=0)
        self.assertEqual(outcome["errno"], errno.EPIPE)
        self.assertEqual(outcome["sigpipe"], int(signal.SIG_DFL))
        self.assertTrue(outcome["fd_live"])
        self.assert_prefix(peer, 0, 93)
        sends = [event for event in events if event["name"] == "send"]
        self.assertTrue(sends)
        self.assertTrue(all(event["flags"] & socket.MSG_NOSIGNAL for event in sends))

    def test_peer_closes_midstream_with_exact_syscall_count(self):
        """T19/T20, R17: received bytes and queued bytes are distinct facts."""
        cut = 257
        outcome, peer, _, events = self.write_case({"close_after": cut})
        written = positive_total(events, "send")
        self.assert_failure(outcome, "PeerClosed", moved=written)
        self.assertGreaterEqual(written, cut)
        self.assert_prefix(peer, cut, 93)

    def test_real_tcp_reset_is_typed_peer_failure(self):
        """T19/T20, R17: SO_LINGER produces a real TCP reset."""
        cut = 129
        outcome, peer, _, events = self.write_case(
            {"close_after": cut, "reset": True},
            {"size": 1048609, "wall_ms": 2000}, tcp=True, bound_ms=2000)
        self.assert_failure(outcome, "PeerClosed",
                            moved=positive_total(events, "send"))
        self.assertIn(outcome["errno"], (errno.ECONNRESET, errno.EPIPE))
        self.assert_prefix(peer, cut, 93)

    def test_reset_and_not_connected_faults_are_peer_failures(self):
        """T30, R17: closed-peer errno classification is exhaustive."""
        for fault, expected in (("reset", errno.ECONNRESET),
                                ("notconn", errno.ENOTCONN)):
            with self.subTest(fault=fault):
                outcome, peer, _, events = self.write_case(
                    faults={"SEND_FAULTS": fault})
                self.assert_failure(outcome, "PeerClosed", moved=0)
                self.assertEqual(outcome["errno"], expected)
                self.assert_prefix(peer, 0, 93)
                self.assertTrue(any(event["name"] == "send" and
                                    event["errno"] == expected for event in events))

    def test_repeated_send_eintr_cannot_renew_wall(self):
        """T30, R16/R17: repeated explicit send EINTR reaches absolute W."""
        wall_ms = 330
        outcome, peer, elapsed, events = self.write_case(
            options={"wall_ms": wall_ms, "stall_ms": 1000},
            faults={"REPEAT_SEND_EINTR": 1}, bound_ms=wall_ms)
        self.assert_failure(outcome, "DeadlineError", "wall", 0)
        self.assert_prefix(peer, 0, 93)
        self.assert_timing(elapsed, wall_ms)
        self.assertGreater(sum(event["errno"] == errno.EINTR for event in events), 20)

    def test_repeated_poll_eintr_cannot_renew_wall(self):
        """T30, R16/R17: repeated explicit poll EINTR reaches absolute W."""
        wall_ms = 330
        outcome, peer, elapsed, events = self.write_case(
            {"mode": "never"}, {"wall_ms": wall_ms, "stall_ms": 1000},
            {"REPEAT_POLL_EINTR": 1}, bound_ms=wall_ms)
        written = positive_total(events, "send")
        self.assert_failure(outcome, "DeadlineError", "wall", written)
        self.assert_prefix(peer, written, 93)
        self.assert_timing(elapsed, wall_ms)
        self.assertGreater(sum(event["name"] == "poll" and
                               event["errno"] == errno.EINTR for event in events), 20)
        self.assert_finite_polls(events, wall_ms)

    def test_external_signal_storm_cannot_renew_either_deadline(self):
        """T30, R16: real external signals continue through W or P expiry."""
        for kind, wall_ms, stall_ms in (("wall", 430, 1300),
                                        ("stall", 1300, 170)):
            with self.subTest(kind=kind):
                bound = wall_ms if kind == "wall" else stall_ms
                outcome, peer, elapsed, events = self.write_case(
                    {"mode": "never"}, {"wall_ms": wall_ms, "stall_ms": stall_ms,
                                        "signal_handler": True},
                    bound_ms=bound, storm=True)
                written = positive_total(events, "send")
                self.assert_failure(outcome, "DeadlineError", kind, written)
                self.assert_prefix(peer, written, 93)
                self.assertGreater(outcome["signals_seen"], 10)
                self.assert_timing(elapsed, bound)
                self.assert_finite_polls(events, bound)

    def test_stopped_then_resumed_reader_reaches_wall(self):
        """T20/T21 mechanism, R16: SIGSTOP is bounded by W, then bytes drain."""
        wall_ms = 390
        outcome, peer, elapsed, events = self.write_case(
            options={"wall_ms": wall_ms, "stall_ms": 1500},
            bound_ms=wall_ms, paused=True)
        written = positive_total(events, "send")
        self.assert_failure(outcome, "DeadlineError", "wall", written)
        self.assertGreater(written, 0)
        self.assert_prefix(peer, written, 93)
        self.assert_timing(elapsed, wall_ms)

    def test_read_obeys_nmax_and_reports_real_eof(self):
        """R17: positive reads are bounded; an actual EOF is distinguished."""
        outcome, peer, _, events = self.read_case(options={"nmax": 31})
        self.assertTrue(outcome["ok"], outcome)
        actual = bytes.fromhex(outcome["result"]["data_hex"])
        self.assertGreater(len(actual), 0)
        self.assertLessEqual(len(actual), 31)
        self.assertEqual(actual, reference(97, 19)[:len(actual)])
        self.assertGreaterEqual(peer["count"], len(actual))
        self.assertEqual(positive_total(events, "read"), len(actual))
        eof, peer, _, events = self.read_case({"size": 0})
        self.assertTrue(eof["ok"], eof)
        self.assertEqual(eof["result"], {"data_hex": "", "eof": True})
        self.assertEqual(peer["count"], 0)
        self.assertTrue(any(event["name"] == "read" and event["result"] == 0
                            for event in events))

    def test_read_retries_external_eintr_and_eagain(self):
        """T30, R17: actual reads recover after externally supplied faults."""
        size = 29
        outcome, peer, _, events = self.read_case(
            {"size": size}, {"operation": "read_many", "nmax": 11},
            {"READ_FAULTS": "eintr,eagain", "POLL_FAULTS": "eintr"})
        self.assertTrue(outcome["ok"], outcome)
        self.assertEqual(bytes.fromhex(outcome["result"]["data_hex"]),
                         reference(size, 19))
        self.assertTrue(outcome["result"]["eof"])
        self.assertEqual(peer["count"], size)
        self.assertEqual(positive_total(events, "read"), size)
        read_errors = {event["errno"] for event in events if event["name"] == "read"}
        self.assertTrue({errno.EINTR, errno.EAGAIN}.issubset(read_errors))
        self.assertTrue(any(event["name"] == "poll" and
                            event["errno"] == errno.EINTR for event in events))

    def test_pending_read_stall_is_typed(self):
        """R16/R17: a source that stays open without bytes ends at P."""
        stall_ms = 150
        outcome, _, elapsed, events = self.read_case(
            {"size": 2, "step": 1, "interval_ms": 500},
            {"operation": "read_many", "nmax": 1,
             "wall_ms": 1300, "stall_ms": stall_ms},
            bound_ms=stall_ms)
        self.assert_failure(outcome, "DeadlineError", "stall", 0, stage="finalize")
        self.assertEqual(bytes.fromhex(outcome["data_hex"]), reference(1, 19))
        self.assertEqual(positive_total(events, "read"), 1)
        self.assert_timing(elapsed, stall_ms)

    def test_trickle_writer_cannot_renew_read_wall(self):
        """T18/T30 mechanism, R16: repeated small reads keep the original W."""
        wall_ms = 450
        outcome, peer, elapsed, events = self.read_case(
            {"size": 100, "step": 1, "interval_ms": 50},
            {"operation": "read_many", "nmax": 1,
             "wall_ms": wall_ms, "stall_ms": 150}, bound_ms=wall_ms)
        self.assert_failure(outcome, "DeadlineError", "wall", 0, stage="finalize")
        actual = bytes.fromhex(outcome["data_hex"])
        self.assertGreater(len(actual), 7)
        self.assertEqual(actual, reference(len(actual), 19))
        self.assertGreaterEqual(peer["count"], len(actual))
        self.assertEqual(positive_total(events, "read"), len(actual))
        positive = [event for event in events
                    if event["name"] == "read" and event["result"] > 0]
        self.assertGreater((positive[-1]["tick"] - positive[0]["tick"]) / 1e6,
                           250)
        self.assert_timing(elapsed, wall_ms)
        self.assert_finite_polls(events, 150)

    def test_invalid_read_descriptor_is_typed_io(self):
        """T30, R17: read EBADF is terminal, not EOF."""
        outcome, _, _ = self.single({
            "operation": "read", "fd": -1, "nmax": 19, "stage_hint": "finalize",
        })
        self.assert_failure(outcome, "TransportError", moved=0, stage="finalize")
        self.assertEqual(outcome["errno"], errno.EBADF)

    def test_repeated_read_eintr_cannot_renew_wall(self):
        """T30, R16/R17: raw read interruptions return to the absolute timer."""
        wall_ms = 340
        outcome, peer, elapsed, events = self.read_case(
            {"size": 37}, {"wall_ms": wall_ms, "stall_ms": 1300},
            {"REPEAT_READ_EINTR": 1}, bound_ms=wall_ms)
        self.assert_failure(outcome, "DeadlineError", "wall", 0, stage="finalize")
        self.assertEqual(peer["count"], 37)
        self.assertEqual(positive_total(events, "read"), 0)
        self.assertGreater(sum(event["name"] == "read" and
                               event["errno"] == errno.EINTR for event in events), 20)
        self.assert_timing(elapsed, wall_ms)

    def test_relay_exact_fit_waits_for_eof_without_overflow(self):
        """R06/R08: exact allowance plus real EOF is a complete fit."""
        size = 1043
        outcome, peer, sent, _, events = self.relay_case(size, size)
        self.assertTrue(outcome["ok"], outcome)
        self.assertEqual(outcome["result"], {
            "observed": size, "accepted": size, "written": size,
            "overflow": False, "eof": True,
        })
        self.assert_prefix(peer, size, 71)
        self.assertEqual(sent["count"], size)
        self.assertEqual(positive_total(events, "read"), size)
        self.assertEqual(positive_total(events, "send"), size)
        self.assertTrue(any(event["name"] == "read" and
                            event["requested"] == 1 and event["result"] == 0
                            for event in events))

    def test_relay_observes_one_over_without_forwarding_probe(self):
        """R06/R08: observation stops at allowance+1; the probe never arrives."""
        allowance = 1043
        for excess in (1, 83):
            with self.subTest(excess=excess):
                outcome, peer, sent, _, events = self.relay_case(
                    allowance + excess, allowance, chunk=131)
                self.assertTrue(outcome["ok"], outcome)
                self.assertEqual(outcome["result"], {
                    "observed": allowance + 1, "accepted": allowance,
                    "written": allowance, "overflow": True, "eof": False,
                })
                self.assertGreaterEqual(sent["count"], allowance + 1)
                self.assert_prefix(peer, allowance, 71)
                self.assertEqual(positive_total(events, "read"), allowance + 1)
                self.assertEqual(positive_total(events, "send"), allowance)

    def test_zero_relay_allowance_still_probes_without_forwarding(self):
        """R06/R08: the helper's numeric zero allowance is an actual byte cap."""
        for size in (0, 17):
            with self.subTest(size=size):
                outcome, peer, _, _, events = self.relay_case(size, 0)
                self.assertTrue(outcome["ok"], outcome)
                self.assertEqual(outcome["result"], {
                    "observed": min(size, 1), "accepted": 0, "written": 0,
                    "overflow": size > 0, "eof": size == 0,
                })
                self.assert_prefix(peer, 0, 71)
                self.assertEqual(positive_total(events, "read"), min(size, 1))
                self.assertEqual(positive_total(events, "send"), 0)

    def test_relay_chunk_cannot_expand_fixed_queue(self):
        """R09/R16: a huge requested chunk cannot grow the 65536-byte queue."""
        size = 196631
        outcome, peer, _, _, events = self.relay_case(
            size, size, chunk=1 << 30, wall_ms=2000, stall_ms=700)
        self.assertTrue(outcome["ok"], outcome)
        self.assert_prefix(peer, size, 71)
        self.assertEqual(outcome["result"]["observed"], size)
        reads = [event for event in events if event["name"] == "read"]
        self.assertTrue(reads)
        self.assertLessEqual(max(event["requested"] for event in reads), 65536)
        self.assertEqual(positive_total(events, "read"), size)
        self.assertEqual(positive_total(events, "send"), size)

    def test_relay_stopped_reader_preserves_independent_prefix_counters(self):
        """T21 mechanism, R06/R08/R16: paused destination ends at W with facts."""
        size, chunk, wall_ms = 1048619, 4096, 410
        outcome, peer, sent, elapsed, events = self.relay_case(
            size, size + 31, chunk=chunk, paused=True,
            wall_ms=wall_ms, stall_ms=1500)
        written = positive_total(events, "send")
        observed = positive_total(events, "read")
        self.assert_failure(outcome, "DeadlineError", "wall", written, stage="run")
        self.assertTrue(outcome["fd_live"])
        self.assertTrue(outcome["src_live"])
        self.assertGreater(written, 0)
        self.assert_prefix(peer, written, 71)
        self.assertGreaterEqual(sent["count"], observed)
        self.assertEqual(outcome["relay_result"], {
            "observed": observed, "accepted": observed, "written": written,
            "overflow": False, "eof": False,
        })
        self.assertGreater(observed, written)
        self.assertLessEqual(observed - written, chunk)
        self.assertLessEqual(observed, size + 32)
        self.assert_timing(elapsed, wall_ms)
        self.assert_finite_polls(events, wall_ms)

    def test_relay_empty_queue_waits_for_source_under_wall_only(self):
        """R16: an idle capture source has no pending publication stall."""
        outcome, peer, sent, _, events = self.relay_case(
            3, 3, wall_ms=1200, stall_ms=70,
            writer_options={"step": 1, "interval_ms": 180})
        self.assertTrue(outcome["ok"], outcome)
        self.assert_prefix(peer, 3, 71)
        self.assertEqual(sent["count"], 3)
        self.assertTrue(outcome["result"]["eof"])
        self.assertEqual(positive_total(events, "read"), 3)
        self.assertEqual(positive_total(events, "send"), 3)

    def test_close_success_is_one_real_attempt(self):
        """R17: successful close invalidates the owned descriptor once."""
        endpoint, peer = socket.socketpair()
        with endpoint, peer:
            outcome, _, events = self.single({
                "operation": "close", "fd": endpoint.fileno(),
            }, (endpoint.fileno(),))
        self.assertTrue(outcome["ok"], outcome)
        self.assertEqual(outcome["result"], {
            "success": True, "live": False, "fd_errno": errno.EBADF,
        })
        closes = [event for event in events if event["name"] == "close"]
        self.assertEqual(len(closes), 1)
        self.assertEqual(closes[0]["result"], 0)

    def test_close_eintr_never_retries_reused_descriptor_number(self):
        """T30, R17: external EINTR closes and reuses the fd before return."""
        left, right = socket.socketpair()
        with left, right:
            outcome, _, events = self.single({
                "operation": "close", "fd": left.fileno(),
            }, (left.fileno(),), faults={"CLOSE_FAULTS": "eintr"})
        self.assertTrue(outcome["ok"], outcome)
        self.assertEqual(outcome["result"], {
            "success": False, "live": True, "fd_errno": None,
        })
        closes = [event for event in events if event["name"] == "close"]
        self.assertEqual(len(closes), 1)
        self.assertEqual(closes[0]["errno"], errno.EINTR)

    def test_close_invalid_descriptor_reports_failure_once(self):
        """R17: close on a genuinely invalid fd is false and is not retried."""
        outcome, _, events = self.single({"operation": "close", "fd": -1})
        self.assertTrue(outcome["ok"], outcome)
        self.assertFalse(outcome["result"]["success"])
        closes = [event for event in events if event["name"] == "close"]
        self.assertEqual(len(closes), 1)
        self.assertEqual(closes[0]["errno"], errno.EBADF)

    def test_pipe_write_fails_closed_without_default_sigpipe(self):
        """R17: unsupported non-socket writes fail before signal-prone I/O."""
        read_fd, write_fd = os.pipe()
        try:
            os.close(read_fd)
            outcome, _, _ = self.single({
                "operation": "write", "fd": write_fd, "size": 37,
                "stage_hint": "publish",
            }, (write_fd,))
        finally:
            os.close(write_fd)
        self.assert_failure(outcome, "TransportError", moved=0)
        self.assertEqual(outcome["errno"], errno.ENOTSOCK)


if __name__ == "__main__":
    unittest.main()
