"""TEST host for real-process report-channel contract rows.

This fixture deliberately does not install the deployed OS isolation policy.
Its explicitly named test-only attestation exercises the channel core, never
certifies an image/launcher pairing. No product module is imported here.
"""
from __future__ import annotations

import array
import base64
from contextlib import ExitStack, contextmanager
import ctypes
from dataclasses import dataclass, field
import errno
import fcntl
import json
import os
from pathlib import Path
import select
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import termios
import threading
import time
import uuid


ROOT = Path(__file__).resolve().parents[2]
SUPERVISOR = ROOT / "docker/pineforge_report_supervise.py"
PRODUCER = Path(__file__).with_name("producer.py")
PROCESS_EXIT_POLL_SECONDS = 0.005


def require_supervisor():
    if not SUPERVISOR.is_file():
        raise AssertionError(
            "report-channel red-first prerequisite: "
            "docker/pineforge_report_supervise.py does not exist on main")


def encoded(data):
    return base64.b64encode(data).decode("ascii")


def clean_environment():
    """Build an unmanaged TEST baseline; launch overlays explicit controls."""
    return {key: value for key, value in os.environ.items()
            if key != "PINEFORGE_RUN_JSON_PROFILE"
            and not key.startswith(("PINEFORGE_REPORT_", "PINEFORGE_TERMINAL_",
                                    "PINEFORGE_DIAGNOSTIC_", "PINEFORGE_CHANNEL_",
                                    "PF_RUN_QUOTA_"))}


def main_blob(path):
    """Read the finalizer/catalog contract from main's committed text."""
    # Pinned to main at the merge; when main moves again before this change merges, repoint all three to the new main head (and re-run the re-anchor and the OFF row).
    return subprocess.run(
        ["git", "show", f"1a7593b2f2f8a0b17a90c3cb9503ce4a7c508683:{path}"], cwd=ROOT,
        capture_output=True, check=True, timeout=30).stdout


def supervisor_fixture(directory, *, main_finalizer=False, main_catalog=False, text=None):
    """Materialize a real temporary supervisor closure on the executing host."""
    destination = Path(directory) / "docker"
    destination.mkdir()
    for name in ("pineforge_report_supervise.py", "pineforge_report_deadlines.py",
                 "pineforge_sink_finalize.py", "pf_sink.py", "run_failure_codes.json"):
        source = ROOT / "docker" / name
        data = (main_blob("docker/" + name)
                if (main_finalizer and name in ("pineforge_sink_finalize.py", "pf_sink.py"))
                or (main_catalog and name == "run_failure_codes.json")
                else source.read_bytes())
        if name == "pineforge_report_supervise.py" and text is not None:
            data = text.encode("utf-8")
        (destination / name).write_bytes(data)
    return destination / "pineforge_report_supervise.py"


def process_info(pid):
    """Read the real Linux process table from the external TEST host."""
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rpartition(") ")[2].split()
    except FileNotFoundError:
        return None
    return {"pid": pid, "state": fields[0], "parent": int(fields[1]),
            "group": int(fields[2]), "started": int(fields[19])}


class ProcessExitObservation:
    """Sample the signalled child independently of channel EOF and S's wait."""
    def __init__(self, identity):
        self.identity = identity
        self.result = None
        self.error = None
        self.stop = threading.Event()
        self.last_alive_at = time.monotonic()
        self.thread = threading.Thread(target=self.poll, daemon=True)
        self.thread.start()

    def poll(self):
        last_alive_at = self.last_alive_at
        try:
            while not self.stop.is_set():
                info = process_info(self.identity["pid"])
                observed_at = time.monotonic()
                # A zombie has already stopped executing; reaping may follow.
                # The start tick distinguishes this child from a reused pid.
                if (info is None or info["state"] in ("Z", "X")
                        or info["started"] != self.identity["started"]):
                    self.result = {
                        "pid": self.identity["pid"], "started": self.identity["started"],
                        "poll_interval_seconds": PROCESS_EXIT_POLL_SECONDS,
                        "last_alive_at_monotonic": last_alive_at,
                        "exited_at_monotonic": observed_at, "final_process": info,
                    }
                    return
                last_alive_at = observed_at
                self.stop.wait(PROCESS_EXIT_POLL_SECONDS)
        except Exception as error:
            self.error = repr(error)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)


def group_members(group):
    members = []
    for path in Path("/proc").iterdir():
        if path.name.isdecimal():
            info = process_info(int(path.name))
            if info is not None and info["group"] == group:
                members.append(info)
    return members


@contextmanager
def fixture_subreaper():
    """Let the TEST host reap a fixture orphan and prove its table entry gone."""
    libc = ctypes.CDLL(None, use_errno=True)
    previous = ctypes.c_int()
    if libc.prctl(37, ctypes.byref(previous), 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "cannot read TEST subreaper state")
    if libc.prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "cannot install TEST subreaper")
    try:
        yield
    finally:
        if libc.prctl(36, previous.value, 0, 0, 0) != 0:
            raise OSError(ctypes.get_errno(), "cannot restore TEST subreaper")


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
    flags_before: dict = field(default_factory=dict)
    flags_after: dict = field(default_factory=dict)
    descendant_samples: list = field(default_factory=list)
    elapsed_seconds: float | None = None
    control: dict = field(default_factory=dict)

    def finalizer_receipt(self):
        return b"".join(event[1:] for event in self.validator_events if event[:1] == b"R")

    def finalizer_arguments(self):
        commands = [event[1:].rstrip(b"\0").split(b"\0")
                    for event in self.validator_events if event[:1] == b"A"]
        if len(commands) != 1:
            raise AssertionError(f"expected one observed V argv; {self.failure_context()}")
        return commands[0]

    def finalizer_stdout(self):
        return b"".join(event[1:] for event in self.validator_events if event[:1] == b"O")

    def finalizer_environment_names(self):
        observations = [event[1:].rstrip(b"\0").split(b"\0")
                        for event in self.validator_events if event[:1] == b"E"]
        if len(observations) != 1:
            raise AssertionError(f"expected one observed V environment; {self.failure_context()}")
        return {name.decode("ascii") for name in observations[0]}

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
            "flags_before": self.flags_before, "flags_after": self.flags_after,
            "descendant_samples": self.descendant_samples,
            "elapsed_seconds": self.elapsed_seconds,
            "control": self.control,
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


class HostWatchdogExpired(AssertionError):
    def __init__(self, capture):
        self.capture = capture
        super().__init__(f"TEST host watchdog expired; {capture.failure_context()}")


def launch(request=None, *, options=(), environment=None, status="valid",
           alias=None, sink=False, attested=True, grants=None, command=None,
           direct=False, activate=True, wrong_direction=None, planned_sink_bytes=None,
           observer=None, pause_probe=False, pause_validator_until_wall=False,
           endpoint_nonblocking=None, diagnostic_delay_ms=0, stalled_channels=(),
           cancel_channel=None, cancel_signal=signal.SIGTERM, cancel_delay_ms=150,
           saturate_status=False, gate_status=False, watchdog_seconds=20,
           supervisor_path=None, producer_arguments=(), audit_stdout=False, receipt_hold=None,
           ignore_validator_term=False, hold_validator_overflow=False,
           output_after_validator_term=False, release_validator_after_ms=None,
           release_producer_after_ms=None):
    """Create R/D/M, launch S, and read every endpoint concurrently to EOF.

    ``status`` tests absent, pipe, stream, reserved, closed and wrong-direction
    endpoints. ``alias`` duplicates real open endpoints, not just fd numbers.
    Both status stall modes prefill M to EAGAIN and hold S after its real
    failed send. ``gate_status`` drains that backlog before cancellation;
    ``saturate_status`` keeps it unread through S's exit.
    The process timeout is the TEST host watchdog, independent of S's clock.
    """
    require_supervisor()
    status_stall = saturate_status or gate_status
    if saturate_status and gate_status:
        raise AssertionError("choose one TEST status release policy")
    attempt = uuid.uuid4().hex
    with ExitStack() as fixtures, tempfile.TemporaryDirectory(
            prefix="pf-report-channel-") as directory, ExitStack() as stack:
        # Every sink row needs actual receipt bytes in a failure message,
        # including rows that do not otherwise inspect observer events.
        if observer is None and (
                sink or status_stall or alias in ("report_sink", "diagnostic_sink", "status_sink")):
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
        for name, writer in (("report", report_write), ("diagnostic", diagnostic_write)):
            if name in stalled_channels:
                if fcntl.fcntl(writer, fcntl.F_SETPIPE_SZ, 4096) != 4096:
                    raise AssertionError(f"stalled {name} reader needs a 4096-byte pipe")
        if diagnostic_delay_ms:
            if fcntl.fcntl(diagnostic_write, fcntl.F_SETPIPE_SZ, 4096) != 4096:
                raise AssertionError("delayed diagnostic reader needs a 4096-byte pipe")
        if wrong_direction == "report":
            report_write = report_read
        elif wrong_direction == "diagnostic":
            diagnostic_write = diagnostic_read
        status_host, status_child = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_SEQPACKET)
        stack.callback(status_host.close)
        stack.callback(status_child.close)
        status_backlog = []
        status_prefill = {}
        status_probe = None
        if status_stall:
            # Allow a whole terminal packet, then fill the actual granted queue.
            # Linux doubles/clamps these requests; a small request is not a stall.
            status_child.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            status_host.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            status_prefill = {
                "send_buffer_bytes": status_child.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF),
                "receive_buffer_bytes": status_host.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
            }
            if status_prefill["send_buffer_bytes"] <= 65536 + 32:
                raise AssertionError(f"TEST M cannot hold a terminal packet: {status_prefill}")
            status_child.setblocking(False)
            for size in (min(65536, status_prefill["send_buffer_bytes"] // 4), 1):
                filler = b"q" * size
                while True:
                    try:
                        count = status_child.send(filler)
                    except BlockingIOError as error:
                        status_prefill["eagain_errno"] = error.errno
                        break
                    if count != size:
                        raise AssertionError("short TEST status prefill")
                    status_backlog.append(filler)
            if not status_backlog:
                raise AssertionError("TEST status queue was not saturated")
            status_prefill.update(
                packets=len(status_backlog), payload_bytes=sum(map(len, status_backlog)))
            status_probe = fixtures.enter_context(status_child.dup())
        inherited = [status_child.fileno()]
        gate_read = gate_write = None
        if status_stall:
            gate_read, gate_write = os.pipe()
            all_fds.update((gate_read, gate_write))
            stack.callback(os.close, gate_read)
            inherited.append(gate_read)
            fixtures.callback(os.close, gate_write)
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
            if "sink" in stalled_channels:
                if fcntl.fcntl(sink_write, fcntl.F_SETPIPE_SZ, 4096) != 4096:
                    raise AssertionError("stalled sink reader needs a 4096-byte pipe")
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
                "grants": {} if grants is None else grants,
            }
            if planned_sink_bytes is not None:
                policy["planned_sink_bytes"] = planned_sink_bytes
            data = json.dumps(policy, separators=(",", ":")).encode() + b"\n"
            if os.write(policy_write, data) != len(data):
                raise AssertionError("short TEST attestation write")
            inherited.append(policy_read)

        argv = [sys.executable, "-B", str(supervisor_path or SUPERVISOR)]
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
        producer_command = [*producer_command, *producer_arguments]
        if direct:
            argv += producer_command
        else:
            argv += ["--", *producer_command]
        env = clean_environment()
        env.update(environment or {})
        trace = None
        if observer is not None:
            trace = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            fixtures.callback(trace.close)
            trace.bind(str(Path(directory) / "trace"))
            trace.setblocking(False)
            env.update(LD_PRELOAD=str(observer), PF_REPORT_TEST_TRACE=str(Path(directory) / "trace"))
            if pause_probe:
                env["PF_REPORT_TEST_PAUSE_PROBE"] = "1"
            if status_stall:
                env["PF_REPORT_TEST_STATUS_GATE_FD"] = str(gate_read)
            if audit_stdout:
                env["PF_REPORT_TEST_STDOUT_AUDIT"] = "1"
            if receipt_hold is not None:
                env["PF_REPORT_TEST_HOLD_RECEIPT"] = receipt_hold
            if ignore_validator_term:
                env["PF_REPORT_TEST_IGNORE_TERM"] = "1"
            if hold_validator_overflow:
                env["PF_REPORT_TEST_HOLD_OVERFLOW"] = "1"
            if output_after_validator_term:
                env["PF_REPORT_TEST_OUTPUT_AFTER_TERM"] = "1"
        flag_probes, flags_before, flags_after = {}, {}, {}

        def close_flag_probes():
            while flag_probes:
                _, fd = flag_probes.popitem()
                os.close(fd)

        fixtures.callback(close_flag_probes)
        if endpoint_nonblocking is not None:
            for fd, nonblocking in zip((report_write, diagnostic_write), endpoint_nonblocking):
                os.set_blocking(fd, not nonblocking)
            for name, fd in (("report", report_write), ("diagnostic", diagnostic_write)):
                probe = fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 6)
                flag_probes[name] = probe
                flags_before[name] = fcntl.fcntl(probe, fcntl.F_GETFL)
        launched = time.monotonic()
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
        keep_host_fds = {status_host.fileno(), gate_write}
        stack.pop_all()
        for fd in all_fds | set(inherited):
            if fd not in owned_readers and fd not in keep_host_fds:
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
        delayed_diagnostic = diagnostic_read if diagnostic_delay_ms else None
        diagnostic_read_at = launched + diagnostic_delay_ms / 1000
        held_readers = {}
        validator_events, stopped_validators = [], set()
        host_paused_validators = set()
        control = {"stalled_at": {}, "cancelled_at": None, "exited_at": None,
                   "exited_at_monotonic": None,
                   "host_paused_validators": [], "held_until_exit": [],
                   "status_prefill_packets": len(status_backlog), "status_prefill": status_prefill,
                   "supervisor_pid": process.pid}
        descendant_samples = []
        exit_observers = {}

        def unwatch(fd):
            if fd in selector.get_map():
                selector.unregister(fd)

        def pending_bytes(fd):
            pending = array.array("i", [0])
            fcntl.ioctl(fd, termios.FIONREAD, pending, True)
            return pending[0]

        def collect_exit_observations():
            for name, observation in exit_observers.items():
                if observation.result is not None:
                    control[name + "_exit"] = observation.result
                if observation.error is not None:
                    control[name + "_exit_error"] = observation.error

        def watch_child_exit(name, identity):
            observation = ProcessExitObservation(identity)
            exit_observers[name] = observation
            fixtures.callback(observation.close)

        def observe_supervisor_exit(now):
            ended = process.poll() is not None
            if ended:
                if control["exited_at"] is None:
                    control["exited_at"] = now - launched
                    control["exited_at_monotonic"] = now
                for name, fd in tuple(flag_probes.items()):
                    flags_after[name] = fcntl.fcntl(fd, fcntl.F_GETFL)
                    os.close(flag_probes.pop(name))
            return ended

        def current_capture():
            collect_exit_observations()
            return Capture(
                bytes(output["report"]), bytes(output["diagnostic"]),
                bytes(output["sink"]), packets, flags, process.poll(), attempt, eof,
                validator_events, stopped_validators, flags_before, flags_after,
                descendant_samples, time.monotonic() - launched, control)

        def require_precondition(condition, message):
            if not condition:
                raise AssertionError(f"{message}; {current_capture().failure_context()}")

        def status_gate_state():
            observation = control.get("status_pending_send")
            if observation is None:
                return None
            info = process_info(process.pid)
            if info is None or info["state"] != "S":
                return None
            try:
                syscall = Path(f"/proc/{process.pid}/syscall").read_text().strip()
            except FileNotFoundError:
                return None
            fields = syscall.split()
            if (len(fields) < 4 or fields[0] == "running"
                    or int(fields[0], 0) != observation["read_syscall"]
                    or int(fields[1], 0) != gate_read or int(fields[3], 0) != 1):
                return None
            socket_target = os.readlink(f"/proc/{process.pid}/fd/{observation['fd']}")
            gate_target = os.readlink(f"/proc/{process.pid}/fd/{gate_read}")
            require_precondition(
                socket_target == f"socket:[{os.fstat(status_probe.fileno()).st_ino}]"
                and gate_target == f"pipe:[{os.fstat(gate_write).st_ino}]",
                "pending send does not own the host's M socket and gate")
            return {"at": time.monotonic() - launched, "process": info,
                    "syscall": syscall, "socket": socket_target, "gate": gate_target}

        def require_full_status():
            try:
                status_probe.send(b"q", socket.MSG_DONTWAIT)
            except BlockingIOError as error:
                require_precondition(
                    error.errno in (errno.EAGAIN, errno.EWOULDBLOCK),
                    "unexpected TEST status probe error")
                return error.errno
            raise AssertionError(
                f"M accepted a host probe before cancellation; {current_capture().failure_context()}")

        deadline = time.monotonic() + watchdog_seconds
        try:
            for name, fd in (("report", report_read), ("diagnostic", diagnostic_read),
                             ("sink", sink_read), *extra_readers.items()):
                if fd is not None:
                    os.set_blocking(fd, False)
                    if name in stalled_channels:
                        held_readers[name] = fd
                    elif fd != delayed_diagnostic:
                        selector.register(fd, selectors.EVENT_READ, name)
            status_host.setblocking(False)
            if status_stall:
                held_readers["status"] = status_host.fileno()
            else:
                selector.register(status_host, selectors.EVENT_READ, "status")
            if trace is not None:
                selector.register(trace, selectors.EVENT_READ, "trace")
            # Always take the final state sample, even when one wake-up
            # consumes every EOF. Only decide to finish after observations.
            while True:
                now = time.monotonic()
                ended = observe_supervisor_exit(now)
                if ended and status_probe is not None:
                    status_probe.close()
                for name, fd in tuple(held_readers.items()):
                    if ended:
                        selector.register(
                            status_host if name == "status" else fd, selectors.EVENT_READ, name)
                        control["held_until_exit"].append(name)
                        del held_readers[name]
                    elif name != "status" and name not in control["stalled_at"]:
                        if pending_bytes(fd) == 4096:
                            control["stalled_at"][name] = now - launched
                identity_path = (request or {}).get("producer_identity_path")
                term_path = (request or {}).get("producer_term_path")
                if term_path is not None and Path(term_path).is_file():
                    try:
                        term = json.loads(Path(term_path).read_text())
                    except (OSError, ValueError):
                        term = None
                    if term is not None and control.get("producer_term_at") is None:
                        identity = process_info(term["pid"])
                        require_precondition(
                            identity is not None and identity["state"] != "Z",
                            "producer was gone before its TERM observation")
                        control["producer_term_at"] = term["at"]
                        control["producer_term_process"] = identity
                        watch_child_exit("producer", identity)
                    if (release_producer_after_ms is not None
                            and term is not None
                            and control.get("producer_released_at") is None
                            and identity_path is not None
                            and (now - term["at"]) >= release_producer_after_ms / 1000):
                        identity = json.loads(Path(identity_path).read_text())
                        info = process_info(identity["pid"])
                        require_precondition(
                            info is not None and info["state"] != "Z"
                            and info["started"] == control["producer_term_process"]["started"]
                            and process.poll() is None,
                            "producer was not alive before the host release")
                        control["producer_alive_before_release"] = info
                        Path((request or {})["producer_release_path"]).touch()
                        control["producer_released_at"] = now
                status_phase = "status_ready" if gate_status else "status"
                if (status_stall and not ended and {"report", "diagnostic"} <= eof
                        and status_phase not in control["stalled_at"]):
                    state = status_gate_state()
                    if state is not None:
                        control["status_gate_observed"] = state
                        control["status_probe_errno"] = require_full_status()
                        control["stalled_at"][status_phase] = time.monotonic() - launched
                stalled_at = control["stalled_at"].get(cancel_channel)
                if (cancel_channel is not None and not ended
                        and control["cancelled_at"] is None and stalled_at is not None
                        and now - launched >= stalled_at + cancel_delay_ms / 1000):
                    before = {"phase": cancel_channel}
                    if cancel_channel in ("status", "status_ready"):
                        state = status_gate_state()
                        require_precondition(state is not None, "S left its pending M send")
                        before.update(status_gate=state, status_probe_errno=require_full_status())
                        if gate_status:
                            drained = 0
                            while status_backlog:
                                data, ancillary, packet_flags, _ = status_host.recvmsg(65537)
                                require_precondition(
                                    data == status_backlog[0] and not ancillary and not packet_flags,
                                    "TEST status backlog changed before cancellation")
                                status_backlog.pop(0)
                                drained += 1
                            require_precondition(
                                bool(select.select([], [status_probe], [], 0)[1]),
                                "M did not become writable after draining TEST backlog")
                            require_precondition(
                                status_gate_state() is not None,
                                "S left the pending send before M became writable")
                            control["status_released_at"] = time.monotonic() - launched
                            control["status_release_packets"] = drained
                            before["status_writable"] = True
                            selector.register(status_host, selectors.EVENT_READ, "status")
                            del held_readers["status"]
                        status_probe.close()
                    else:
                        require_precondition(
                            cancel_channel in held_readers
                            and pending_bytes(held_readers[cancel_channel]) == 4096,
                            "data pipe was not still full before cancellation")
                        before["pending_bytes"] = 4096
                    require_precondition(not packets, "S sent a terminal packet before cancellation")
                    before["at"] = time.monotonic() - launched
                    control["pre_cancel"] = before
                    os.kill(process.pid, cancel_signal)
                    control["cancelled_at"] = time.monotonic() - launched
                if delayed_diagnostic is not None and time.monotonic() >= diagnostic_read_at:
                    selector.register(delayed_diagnostic, selectors.EVENT_READ, "diagnostic")
                    delayed_diagnostic = None
                child_state = (request or {}).get("diagnostic_child_state")
                if child_state is not None:
                    try:
                        identities = json.loads(Path(child_state).read_text())
                        sample = {
                            "at": now - launched,
                            "leader": process_info(identities["leader"]),
                            "child": process_info(identities["child"]),
                        }
                        if (not descendant_samples or any(
                                descendant_samples[-1][name] != sample[name]
                                for name in ("leader", "child"))):
                            descendant_samples.append(sample)
                    except (FileNotFoundError, ValueError):
                        pass
                if time.monotonic() >= deadline:
                    control["watchdog_pending_bytes"] = {}
                    for name, fd in held_readers.items():
                        if name != "status":
                            pending = array.array("i", [0])
                            fcntl.ioctl(fd, termios.FIONREAD, pending, True)
                            control["watchdog_pending_bytes"][name] = pending[0]
                    identity_path = (request or {}).get("producer_identity_path")
                    if identity_path is not None and Path(identity_path).is_file():
                        identity = json.loads(Path(identity_path).read_text())
                        control["watchdog_producer_group"] = group_members(identity["group"])
                    raise HostWatchdogExpired(current_capture())
                overflow_hold = control.get("validator_overflow_hold")
                if (release_validator_after_ms is not None
                        and overflow_hold is not None
                        and control.get("validator_released_at") is None
                        and now >= overflow_hold["at"] + release_validator_after_ms / 1000):
                    info = process_info(overflow_hold["pid"])
                    require_precondition(
                        info is not None and info["state"] != "Z"
                        and info["started"] == overflow_hold["process"]["started"]
                        and process.poll() is None,
                        "validator was not alive before the host release")
                    control["validator_alive_before_release"] = info
                    os.kill(overflow_hold["pid"], signal.SIGCONT)
                    control["validator_released_at"] = now
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
                        if data and status_backlog:
                            expected = status_backlog.pop(0)
                            if data != expected or ancillary or packet_flags:
                                raise AssertionError("TEST status backlog changed")
                        elif data:
                            packets.append(data)
                            flags.append(packet_flags | (socket.MSG_CTRUNC if ancillary else 0))
                        else:
                            eof.add("status")
                            unwatch(key.fd)
                    else:
                        data = os.read(key.fd, 65536)
                        if data:
                            if key.data in ("report", "sink"):
                                pid = control.get("receipt_hold", {}).get("pid")
                                if pid is not None:
                                    control.setdefault("first_publication", {}).setdefault(
                                        key.data, {"at": time.monotonic() - launched,
                                                   "validator": process_info(pid)})
                            output[key.data].extend(data)
                        else:
                            eof.add(key.data)
                            unwatch(key.fd)
                if (trace is not None and trace.fileno() in selector.get_map()
                        and process.poll() is not None
                        and all(key.data == "trace" for key in selector.get_map().values())):
                    while True:
                        try:
                            validator_events.append(trace.recv(65537))
                        except BlockingIOError:
                            break
                    unwatch(trace.fileno())
                # Include events from the final drain in this dispatch.
                for event in validator_events:
                    if event[:1] == b"G":
                        pid = int(event[1:])
                        info = process_info(pid)
                        if (info is not None and info["state"] == "T"
                                and "receipt_hold" not in control):
                            control["receipt_hold"] = {
                                "pid": pid, "stopped_at": time.monotonic() - launched,
                                "stdio": {str(fd): os.readlink(f"/proc/{pid}/fd/{fd}")
                                          for fd in (0, 1, 2)},
                                "environment_names": [
                                    item.partition(b"=")[0].decode("ascii")
                                    for item in Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
                                    if item],
                                "supervisor_environment_names": [
                                    item.partition(b"=")[0].decode("ascii")
                                    for item in Path(f"/proc/{process.pid}/environ").read_bytes().split(b"\0")
                                    if item],
                            }
                        continue
                    if event[:1] == b"M":
                        observation = json.loads(event[1:])
                        require_precondition(
                            status_stall and observation["pid"] == process.pid
                            and observation["gate_fd"] == gate_read
                            and observation["send_errno"] in (errno.EAGAIN, errno.EWOULDBLOCK)
                            and 0 < observation["packet_bytes"] <= 65536,
                            "observer did not report a real pending terminal send")
                        require_precondition(
                            observation == control.setdefault("status_pending_send", observation),
                            "more than one terminal send entered the TEST gate")
                        continue
                    if event[:1] == b"T":
                        pid = int(event[1:])
                        if control.get("validator_term_at") is None:
                            info = process_info(pid)
                            require_precondition(
                                info is not None and info["state"] != "Z",
                                "validator was gone before its TERM observation")
                            control["validator_term_at"] = time.monotonic()
                            control["validator_term_pid"] = pid
                            control["validator_term_process"] = info
                            watch_child_exit("validator", info)
                        continue
                    if event[:1] == b"S" and pause_validator_until_wall:
                        pid = int(event[1:])
                        if pid not in host_paused_validators:
                            os.kill(pid, signal.SIGSTOP)
                            host_paused_validators.add(pid)
                            control["host_paused_validators"].append(pid)
                    elif event[:1] != b"P":
                        continue
                    else:
                        pid = int(event[1:])
                    if (event[:1] == b"P" and hold_validator_overflow
                            and "validator_overflow_hold" not in control):
                        info = process_info(pid)
                        require_precondition(
                            info is not None and info["state"] != "Z",
                            "validator was gone before its overflow hold observation")
                        control["validator_overflow_hold"] = {
                            "pid": pid, "at": time.monotonic(), "process": info}
                    try:
                        state = Path(f"/proc/{pid}/status").read_text()
                    except FileNotFoundError:
                        continue
                    if any(line.startswith("State:\tT") for line in state.splitlines()):
                        stopped_validators.add(pid)
                        if (pause_validator_until_wall
                                and not control.get("native_released_at")):
                            info = process_info(pid)
                            require_precondition(
                                info is not None and info["state"] == "T",
                                "V was not stopped before releasing native input")
                            control["native_release_precondition"] = {
                                "at": time.monotonic() - launched, "validator": info}
                            Path(request["native_release_path"]).touch()
                            control["native_released_at"] = time.monotonic() - launched
                hold = control.get("receipt_hold")
                if (hold is not None and "released_at" not in hold
                        and time.monotonic() - launched >= hold["stopped_at"] + 0.2):
                    info = process_info(hold["pid"])
                    require_precondition(
                        info is not None and info["state"] == "T",
                        "V was not stopped before releasing its receipt hold")
                    receipt = b"".join(
                        event[1:] for event in validator_events if event[:1] == b"R")
                    report_queued = pending_bytes(report_read)
                    sink_queued = pending_bytes(sink_read)
                    require_precondition(
                        not output["report"] and not output["sink"]
                        and report_queued == 0 and sink_queued == 0,
                        "publication occurred while V was held")
                    require_precondition(
                        not receipt if receipt_hold == "before" else
                        bool(receipt) and json.loads(receipt)["complete"] is True,
                        "V did not reach the requested receipt boundary")
                    hold.update(
                        report_bytes_while_stopped=len(output["report"]),
                        sink_bytes_while_stopped=len(output["sink"]),
                        report_queued_while_stopped=report_queued,
                        sink_queued_while_stopped=sink_queued,
                        receipt_bytes_while_stopped=len(receipt),
                        release_checked_at=time.monotonic() - launched,
                        validator_before_release=info)
                    os.kill(hold["pid"], signal.SIGCONT)
                    hold["released_at"] = time.monotonic() - launched
                collect_exit_observations()
                if (ended and not selector.get_map()
                        and delayed_diagnostic is None and not held_readers):
                    break
            wait = process.wait(timeout=max(0.01, deadline - time.monotonic()))
            observe_supervisor_exit(time.monotonic())
            for name, observation in exit_observers.items():
                observation.thread.join(timeout=max(0.01, deadline - time.monotonic()))
                collect_exit_observations()
                require_precondition(
                    observation.error is None and observation.result is not None,
                    "missing independent " + name + " exit observation")
            if status_backlog:
                raise AssertionError("TEST status backlog did not drain after S exit")
        finally:
            try:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=2)
            finally:
                for fd in tuple(selector.get_map()):
                    unwatch(fd)
                selector.close()
                status_host.close()
                if trace is not None:
                    trace.close()
                close_flag_probes()
                for fd in owned_readers:
                    os.close(fd)
        return Capture(bytes(output["report"]), bytes(output["diagnostic"]),
                       bytes(output["sink"]), packets, flags, wait, attempt, eof,
                       validator_events, stopped_validators,
                       flags_before, flags_after, descendant_samples,
                       time.monotonic() - launched, control)


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
            try:
                result = subprocess.run(
                    [sys.executable, "-B", str(PRODUCER), str(path), "--reference"],
                    capture_output=True, env=environment, check=False, timeout=10,
                    pass_fds=() if writer is None else (writer,))
            finally:
                if writer is not None:
                    os.close(writer)
            result.sink = b"" if reader is None else os.read(reader, 65536)
            return result
        finally:
            if reader is not None:
                os.close(reader)
