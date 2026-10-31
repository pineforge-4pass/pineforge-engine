"""TEST host for real-process report-channel contract rows.

This fixture deliberately does not install the deployed OS isolation policy.
Its explicitly named test-only attestation exercises the channel core, never
certifies an image/launcher pairing. No product module is imported here.
"""
from __future__ import annotations

import base64
from contextlib import ExitStack, contextmanager
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import time
import uuid


ROOT = Path(__file__).resolve().parents[2]
SUPERVISOR = ROOT / "docker/pineforge_report_supervise.py"
PRODUCER = Path(__file__).with_name("producer.py")


def require_supervisor():
    if not SUPERVISOR.is_file():
        raise AssertionError(
            "report-channel red-first prerequisite: "
            "docker/pineforge_report_supervise.py does not exist on main")


def encoded(data):
    return base64.b64encode(data).decode("ascii")


def clean_environment():
    return {key: value for key, value in os.environ.items()
            if not key.startswith(("PINEFORGE_REPORT_", "PINEFORGE_TERMINAL_",
                                   "PINEFORGE_DIAGNOSTIC_", "PINEFORGE_CHANNEL_"))}


def sink_wire(closes, first_ts=1000):
    """Independent frozen E/T wire fixture, including a real logical digest."""
    columns = {
        "E": (("time_ms", "i64", "unix_ms"), ("equity", "f64?", "account_ccy"),
              ("open_profit", "f64?", "account_ccy")),
        "T": (("n", "i64", ""), ("side", "str", ""),
              ("entry_time", "i64", "unix_ms"), ("exit_time", "i64", "unix_ms"),
              ("entry_price", "f64?", "quote_ccy"), ("exit_price", "f64?", "quote_ccy"),
              ("qty", "f64?", "qty"), ("pnl", "f64?", "account_ccy"),
              ("pnl_pct", "f64?", "percent"), ("max_runup", "f64?", "account_ccy"),
              ("max_drawdown", "f64?", "account_ccy"), ("commission", "f64?", "account_ccy"),
              ("entry_bar_index", "i64", "script_bar"), ("exit_bar_index", "i64", "script_bar"),
              ("open_at_end", "bool", ""), ("entry_incarnation", "u64", ""),
              ("close_cause", "i64", ""), ("entry_id", "str", ""),
              ("exit_id", "str", ""), ("exit_comment", "str", "")),
    }

    def line(value):
        return json.dumps(value, separators=(",", ":"), allow_nan=False).encode() + b"\n"

    header = {"schema": "pf-report-sink", "version": 1, "run_id": "report-channel-fixture",
              "engine": "pineforge", "records": [
                  {"type": tag, "fields": [column[0] for column in values],
                   "types": [column[1] for column in values],
                   "units": [column[2] for column in values]}
                  for tag, values in columns.items()]}
    # The f64? columns require canonical double tokens even for integral inputs.
    prefix = line(header) + b"".join(
        line(["E", first_ts + index * 60000, float(close), 0.0])
        for index, close in enumerate(closes))
    import hashlib
    return prefix + line({"trailer": "pf-report-sink", "version": 1,
                          "counts": {"E": len(closes), "T": 0},
                          "sha256": hashlib.sha256(prefix).hexdigest()})


def sink_receipt(data, *, complete, cap=None, equity_count=0):
    """Finalizer 1.1.0 FILE receipt oracle, independent of product imports/M."""
    import hashlib
    prefix = b"".join(data.splitlines(keepends=True)[:-1])
    value = {
        "complete": complete,
        **{"count_" + tag.lower(): equity_count if tag == "E" else 0
           for tag in "ETOVKXH"},
        **({"reason": "sink_exhausted"} if cap is not None else {}),
        "sink_bytes": len(data),
        **({"cap_bytes": cap} if cap is not None else {}),
        "sha256": hashlib.sha256(prefix).hexdigest() if complete else "",
        "finalizer_version": "1.1.0",
    }
    return json.dumps(value, separators=(",", ":")).encode() + b"\n"


@contextmanager
def validator_observer():
    """Build only on the executing CLOUD test host; absence is a failure."""
    with tempfile.TemporaryDirectory(prefix="pf-finalizer-observer-") as directory:
        library = Path(directory) / "observe.so"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-shared", "-fPIC",
             "-Wall", "-Wextra", "-Werror", "-o", str(library),
             str(Path(__file__).with_name("observe_finalizer.c"))],
            capture_output=True, check=True, timeout=30)
        yield library


@dataclass
class Capture:
    report: bytes
    diagnostic: bytes
    sink: bytes
    packets: list
    packet_flags: list
    returncode: int | None
    attempt_id: str
    eof: set
    validator_events: list = field(default_factory=list)
    stopped_validators: set = field(default_factory=set)
    killed_validators: set = field(default_factory=set)

    def finalizer_receipt(self):
        return b"".join(event[1:] for event in self.validator_events if event[:1] == b"R")

    def failure_context(self):
        """Keep the whole terminal record and bounded, loss-tolerant D text."""
        records = []
        for packet in self.packets:
            try:
                records.append(json.loads(packet))
            except (ValueError, UnicodeError):
                records.append({"invalid_terminal_bytes": repr(packet)})
        record = records[0] if len(records) == 1 else records
        verification = record.get("sink_verification") if isinstance(record, dict) else None
        if not isinstance(verification, dict):
            verification = {}
        receipt = self.finalizer_receipt()
        return json.dumps({
            "terminal": record,
            "supervisor_wait_status": self.returncode,
            "packet_flags": self.packet_flags,
            "eof": sorted(self.eof),
            "diagnostic": {
                "length_bytes": len(self.diagnostic),
                "prefix": self.diagnostic[:4096].decode("utf-8", "replace"),
                "truncated": len(self.diagnostic) > 4096,
            },
            "validator": {
                "exit_status": verification.get("exit_status") if verification else None,
                "signal": verification.get("signal") if verification else None,
                "receipt_length_bytes": len(receipt),
                "receipt_bytes": repr(receipt),
            },
        }, separators=(",", ":"), ensure_ascii=True)

    def terminal(self):
        if len(self.packets) != 1:
            raise AssertionError(
                f"expected exactly one terminal packet; {self.failure_context()}")
        if self.packet_flags != [0]:
            raise AssertionError(
                f"truncated/ancillary terminal packet; {self.failure_context()}")
        try:
            return json.loads(self.packets[0])
        except (ValueError, UnicodeError) as error:
            raise AssertionError(
                f"invalid terminal JSON; {self.failure_context()}") from error


def launch(request=None, *, options=(), environment=None, status="valid",
           alias=None, sink=False, attested=True, grants=None, command=None,
           direct=False, activate=True, wrong_direction=None, planned_sink_bytes=None,
           observer=None, pause_probe=False, kill_stopped_probe=False):
    """Create R/D/M, launch S, and read every endpoint concurrently to EOF.

    ``status`` tests absent, pipe, stream, reserved, closed and wrong-direction
    endpoints. ``alias`` duplicates real open endpoints, not just fd numbers.
    The process timeout is the TEST host watchdog, independent of S's clock.
    """
    require_supervisor()
    attempt = uuid.uuid4().hex
    with ExitStack() as fixtures, tempfile.TemporaryDirectory(
            prefix="pf-report-channel-") as directory, ExitStack() as stack:
        # Every sink row needs actual receipt bytes in a failure message,
        # including rows that do not otherwise inspect observer events.
        if observer is None and (
                sink or alias in ("report_sink", "diagnostic_sink", "status_sink")):
            observer = fixtures.enter_context(validator_observer())
        request_path = Path(directory) / "request.json"
        request_path.write_text(json.dumps(request or {}), encoding="utf-8")
        all_fds = set()

        def pipe():
            reader, writer = os.pipe()
            all_fds.update((reader, writer))
            stack.callback(os.close, reader)
            stack.callback(os.close, writer)
            return reader, writer

        report_read, report_write = pipe()
        diagnostic_read, diagnostic_write = pipe()
        if wrong_direction == "report":
            report_write = report_read
        elif wrong_direction == "diagnostic":
            diagnostic_write = diagnostic_read
        status_host, status_child = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_SEQPACKET)
        stack.callback(status_host.close)
        stack.callback(status_child.close)
        inherited = [status_child.fileno()]
        terminal_fd = status_child.fileno()
        extra_readers = {}
        if status in ("pipe", "read_end"):
            bad_read, bad_write = pipe()
            terminal_fd = bad_read if status == "read_end" else bad_write
            inherited.append(terminal_fd)
            extra_readers["bad_status"] = bad_read
        elif status == "stream":
            stream_host, stream_child = socket.socketpair()
            stack.callback(stream_host.close)
            stack.callback(stream_child.close)
            terminal_fd = stream_child.fileno()
            inherited.append(terminal_fd)
        elif status == "reserved":
            terminal_fd = 5
        elif status == "closed":
            terminal_fd = 2147483647
        if alias == "report_diagnostic":
            diagnostic_write = report_write
        elif alias in ("report_status", "diagnostic_status"):
            terminal_fd = os.dup(
                report_write if alias == "report_status" else diagnostic_write)
            all_fds.add(terminal_fd)
            stack.callback(os.close, terminal_fd)
            inherited.append(terminal_fd)

        sink_read = sink_write = None
        if sink or alias in ("report_sink", "diagnostic_sink", "status_sink"):
            sink_read, sink_write = pipe()
            if wrong_direction == "sink":
                sink_write = sink_read
            if alias is not None and alias.endswith("_sink"):
                source = {"report_sink": report_write,
                          "diagnostic_sink": diagnostic_write,
                          "status_sink": status_child.fileno()}[alias]
                sink_write = os.dup(source)
                all_fds.add(sink_write)
                stack.callback(os.close, sink_write)
            inherited.append(sink_write)

        policy_read = policy_write = None
        if attested:
            policy_read, policy_write = pipe()
            policy = {
                "schema": "pf-report-test-only-host-attestation",
                "version": 1,
                "attempt_id": attempt,
                "test_only_unisolated_process_fixture": True,
                "grants": grants or {
                    "report_bytes": 67108864, "diagnostic_bytes": 1048576,
                    "wall_ms": 600000, "stall_ms": 5000},
            }
            if planned_sink_bytes is not None:
                policy["planned_sink_bytes"] = planned_sink_bytes
            data = json.dumps(policy, separators=(",", ":")).encode() + b"\n"
            if os.write(policy_write, data) != len(data):
                raise AssertionError("short TEST attestation write")
            inherited.append(policy_read)

        argv = [sys.executable, "-B", str(SUPERVISOR)]
        if direct:
            argv = [sys.executable, "-B", str(ROOT / "docker/run_json.py")]
        if status != "absent":
            argv += ["--terminal-status-fd", str(terminal_fd)]
        argv += ["--attempt-id", attempt]
        if attested:
            argv += ["--test-only-host-attestation-fd", str(policy_read)]
        if sink_write is not None:
            argv += ["--report-sink-fd", str(sink_write)]
        argv += list(options)
        if activate and not any(
                value == "--report-channel" or value.startswith("--report-channel=")
                for value in options):
            argv += ["--report-channel", "supervised/v1"]
        producer_command = command or [
            sys.executable, "-B", str(PRODUCER), str(request_path)]
        if direct:
            argv += producer_command
        else:
            argv += ["--", *producer_command]
        env = clean_environment()
        env.update(environment or {})
        trace = None
        if observer is not None:
            trace = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            trace.bind(str(Path(directory) / "trace"))
            trace.setblocking(False)
            env.update(LD_PRELOAD=str(observer), PF_REPORT_TEST_TRACE=str(Path(directory) / "trace"))
            if pause_probe:
                env["PF_REPORT_TEST_PAUSE_PROBE"] = "1"
        process = subprocess.Popen(
            argv, stdin=subprocess.DEVNULL, stdout=report_write,
            stderr=diagnostic_write, pass_fds=tuple(set(inherited)),
            env=env, cwd=ROOT)
        # Parent copies of all writing endpoints must disappear before waiting
        # for EOF. Detach ExitStack callbacks by replacing it with reader owners.
        owned_readers = {report_read, diagnostic_read}
        if sink_read is not None:
            owned_readers.add(sink_read)
        owned_readers.update(extra_readers.values())
        keep_sockets = {status_host.fileno()}
        stack.pop_all()
        for fd in all_fds | set(inherited):
            if fd not in owned_readers and fd not in keep_sockets:
                os.close(fd)
        # Socket objects no longer own their closed child descriptors.
        status_child.detach()
        if status == "stream":
            stream_child.detach()
            stream_host.close()
        output = {"report": bytearray(), "diagnostic": bytearray(),
                  "sink": bytearray(), "bad_status": bytearray()}
        packets, flags, eof = [], [], set()
        selector = selectors.DefaultSelector()
        for name, fd in (("report", report_read), ("diagnostic", diagnostic_read),
                         ("sink", sink_read), *extra_readers.items()):
            if fd is not None:
                os.set_blocking(fd, False)
                selector.register(fd, selectors.EVENT_READ, name)
        status_host.setblocking(False)
        selector.register(status_host, selectors.EVENT_READ, "status")
        validator_events, stopped_validators, killed_validators = [], set(), set()
        if trace is not None:
            selector.register(trace, selectors.EVENT_READ, "trace")
        deadline = time.monotonic() + 20
        try:
            while selector.get_map():
                if time.monotonic() >= deadline:
                    partial = Capture(
                        bytes(output["report"]), bytes(output["diagnostic"]),
                        bytes(output["sink"]), packets, flags, process.poll(), attempt, eof,
                        validator_events, stopped_validators, killed_validators)
                    raise AssertionError(
                        f"TEST host watchdog expired; {partial.failure_context()}")
                for key, _ in selector.select(0.05):
                    if key.data == "trace":
                        while True:
                            try:
                                data = trace.recv(65537)
                            except BlockingIOError:
                                break
                            validator_events.append(data)
                    elif key.data == "status":
                        data, ancillary, packet_flags, _ = status_host.recvmsg(65537)
                        if data:
                            packets.append(data)
                            flags.append(packet_flags | (socket.MSG_CTRUNC if ancillary else 0))
                        else:
                            eof.add("status")
                            selector.unregister(key.fileobj)
                    else:
                        data = os.read(key.fd, 65536)
                        if data:
                            output[key.data].extend(data)
                        else:
                            eof.add(key.data)
                            selector.unregister(key.fileobj)
                for event in validator_events:
                    if event[:1] != b"P":
                        continue
                    pid = int(event[1:])
                    try:
                        state = Path(f"/proc/{pid}/status").read_text()
                    except FileNotFoundError:
                        continue
                    if any(line.startswith("State:\tT") for line in state.splitlines()):
                        stopped_validators.add(pid)
                        if kill_stopped_probe and pid not in killed_validators:
                            os.kill(pid, signal.SIGKILL)
                            killed_validators.add(pid)
                if (trace is not None and process.poll() is not None
                        and all(key.data == "trace" for key in selector.get_map().values())):
                    while True:
                        try:
                            validator_events.append(trace.recv(65537))
                        except BlockingIOError:
                            break
                    selector.unregister(trace)
            wait = process.wait(timeout=max(0.01, deadline - time.monotonic()))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=2)
            selector.close()
            status_host.close()
            if trace is not None:
                trace.close()
            for fd in owned_readers:
                os.close(fd)
        return Capture(bytes(output["report"]), bytes(output["diagnostic"]),
                       bytes(output["sink"]), packets, flags, wait, attempt, eof,
                       validator_events, stopped_validators, killed_validators)


def reference(request):
    """Run the same input-driven producer directly, without S."""
    with tempfile.TemporaryDirectory(prefix="pf-report-reference-") as directory:
        path = Path(directory) / "request.json"
        path.write_text(json.dumps(request), encoding="utf-8")
        environment = clean_environment()
        reader = writer = None
        if request.get("inherited_sink"):
            reader, writer = os.pipe()
            environment["PINEFORGE_REPORT_REFERENCE_SINK_FD"] = str(writer)
        try:
            result = subprocess.run(
                [sys.executable, "-B", str(PRODUCER), str(path), "--reference"],
                capture_output=True, env=environment, check=False, timeout=10,
                pass_fds=() if writer is None else (writer,))
        finally:
            if writer is not None:
                os.close(writer)
        try:
            result.sink = b"" if reader is None else os.read(reader, 65536)
        finally:
            if reader is not None:
                os.close(reader)
        return result
