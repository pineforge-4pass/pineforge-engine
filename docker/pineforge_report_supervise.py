#!/usr/bin/env python3
"""Opt-in, separate-process JSON report-channel core.

Trusted host invocation::

    python3 docker/pineforge_report_supervise.py \
        --report-channel supervised/v1 --terminal-status-fd M \
        --attempt-id HOST_GENERATED_32_LOWERCASE_HEX \
        [--host-attestation-fd A] [caller ceilings] -- PRODUCER [arguments...]

R is inherited fd 1, D is inherited fd 2, M is an inherited connected AF_UNIX
SOCK_SEQPACKET endpoint. None is passed to the producer. The producer receives
only a dynamically allocated candidate writer, named by the private environment
PINEFORGE_REPORT_CANDIDATE_FD, and the marker PINEFORGE_REPORT_PRODUCER=supervised/v1.
An explicit inherited sink adds a private K writer named
PINEFORGE_REPORT_NATIVE_SINK_FD. U must close its last K writer before C.
Both of its stdio writers refer to one private diagnostic pipe.

This first integration has no deployed namespace/seccomp/reservation adapter.
An ordinary invocation therefore refuses isolation before starting a producer,
including when an arbitrary file purports to attest production isolation.
--test-only-host-attestation-fd is expressly TEST infrastructure: a bounded
pipe containing one pf-report-test-only-host-attestation/v1 object bound to
attempt_id, test_only_unisolated_process_fixture=true, and positive grants.
It runs controlled process fixtures without claiming the deployed OS boundary.
It must never be supplied by a billed/production launcher. The cleanup receipt
leaves domain_empty unavailable; a PID or a reaped child is no domain proof.

S counts/hashes K, relays through one 64 KiB queue to the real file finalizer,
and publishes its private verified inode to N before R. N stays solely in S.
Named sinks and the automatic internal snapshot handoff remain fail-closed.
The synchronous deadline helpers handle attestation reads and stream publication.
Metered ready reads, pipes, packet delivery and concurrent relay steps use
nonblocking adapters with the same absolute Deadline and preserve every chunk.
"""
from __future__ import annotations

import argparse
import codecs
import ctypes
from dataclasses import dataclass
import errno
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import select
import selectors
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

from pineforge_report_deadlines import (
    Deadline, DeadlineError, PeerClosed, TransportError,
    bounded_read, bounded_write, close_once,
)


CONTRACT = "supervised/v1"
MAX_INTEGER = (1 << 53) - 2
INT_MAX = (1 << 31) - 1
TERMINAL_BYTES = 65536
QUEUE_BYTES = 65536
CLAIM_BYTES = 8192
STOP_GRACE_MS = 1000
DEFAULTS = {
    "report_bytes": 67108864, "diagnostic_bytes": 1048576,
    "wall_ms": 600000, "stall_ms": 5000,
}
CEILINGS = {
    "--report-max-bytes": ("PINEFORGE_REPORT_MAX_BYTES", "report_bytes"),
    "--diagnostic-max-bytes": ("PINEFORGE_DIAGNOSTIC_MAX_BYTES", "diagnostic_bytes"),
    "--channel-wall-ms": ("PINEFORGE_CHANNEL_WALL_MS", "wall_ms"),
    "--channel-stall-ms": ("PINEFORGE_CHANNEL_STALL_MS", "stall_ms"),
}
CONTROLS = {
    "--report-channel": "PINEFORGE_REPORT_CHANNEL",
    "--terminal-status-fd": "PINEFORGE_TERMINAL_STATUS_FD",
    "--attempt-id": "PINEFORGE_REPORT_ATTEMPT_ID",
    "--host-attestation-fd": "PINEFORGE_REPORT_HOST_ATTESTATION_FD",
    "--test-only-host-attestation-fd": None,
}
NEW_OPTIONS = frozenset((*CEILINGS, *CONTROLS))
CATALOG = Path(__file__).with_name("run_failure_codes.json")


def wire(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("utf-8") + b"\n"


class UsageError(Exception):
    pass


class ProtocolError(Exception):
    pass


class IncompleteJSON(ProtocolError):
    pass


class TransferError(Exception):
    def __init__(self, channel, reason, *, wall=False):
        self.channel, self.reason, self.wall = channel, reason, wall
        super().__init__(f"{channel}: {reason}")


def decimal(text, *, positive=False, maximum=MAX_INTEGER):
    if not isinstance(text, str) or not text or re.fullmatch(r"[0-9]+", text) is None:
        raise UsageError("expected an unsigned decimal integer")
    canonical = text.lstrip("0") or "0"
    if len(canonical) > 16:
        raise UsageError("integer outside the admitted range")
    value = int(canonical)
    if value > maximum or (positive and value == 0):
        raise UsageError("integer outside the admitted range")
    return value


def descriptor(text):
    value = decimal(text, maximum=INT_MAX)
    if value < 3 or value == 5:
        raise UsageError("invalid or reserved descriptor")
    return value


def direct_arguments(argv):
    """Extract exact new controls without adding options to the legacy parser.

    Existing argument spelling/abbreviation handling stays in run_json. External
    sink/phase options are copied for S's admission and removed from U's argv.
    """
    control, producer = [], []
    index = 0
    admission_options = {"--report-sink", "--report-sink-fd", "--run-phase-fd"}
    while index < len(argv):
        token = argv[index]
        if token == "--":
            producer.extend(argv[index:])
            break
        name, equals, value = token.partition("=")
        if name in NEW_OPTIONS | admission_options:
            control.append(token)
            if not equals:
                index += 1
                if index >= len(argv):
                    # Let S produce the usage refusal through the same boundary.
                    control.append("")
                else:
                    control.append(argv[index])
        else:
            producer.append(token)
        index += 1
    return [*control, "--", sys.executable, "-B",
            str(Path(__file__).with_name("run_json.py")), *producer]


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise UsageError(message)


def parse(argv, environ):
    parser = Parser(add_help=False, allow_abbrev=False)
    for flag, environment in CONTROLS.items():
        parser.add_argument(flag, default=environ.get(environment) if environment else None)
    for flag, (environment, _) in CEILINGS.items():
        parser.add_argument(flag, default=environ.get(environment))
    for flag in ("--report-sink", "--report-sink-fd", "--run-phase-fd"):
        parser.add_argument(flag)
    parser.add_argument("--invocation", choices=("run_json", "entrypoint"), default="run_json")
    # Conflicting duplicates are usage errors, even if argparse would take the last.
    seen = set()
    head = argv[:argv.index("--")] if "--" in argv else argv
    for token in head:
        name = token.partition("=")[0]
        if name in parser._option_string_actions:
            if name in seen:
                raise UsageError("duplicate channel option")
            seen.add(name)
    if "--" not in argv:
        raise UsageError("a producer command is required")
    boundary = argv.index("--")
    options = parser.parse_args(argv[:boundary])
    options.command = argv[boundary + 1:]
    if not options.command:
        raise UsageError("a producer command is required")
    return options


def early_value(argv, flag, environment=None):
    """Recover only host controls needed to deliver a parser refusal."""
    tokens = argv[:argv.index("--")] if "--" in argv else argv
    value = os.environ.get(environment) if environment else None
    for index, token in enumerate(tokens):
        name, equals, suffix = token.partition("=")
        if name == flag:
            value = suffix if equals else (tokens[index + 1] if index + 1 < len(tokens) else None)
    return value


def endpoint_identity(fd):
    info = os.fstat(fd)
    return info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode)


def writable_stream(fd):
    info = os.fstat(fd)
    access = fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE
    if access == os.O_RDONLY:
        raise UsageError("endpoint is not writable")
    if stat.S_ISFIFO(info.st_mode):
        return
    if stat.S_ISSOCK(info.st_mode):
        sock = socket.socket(fileno=os.dup(fd))
        try:
            if sock.family != socket.AF_UNIX or sock.getsockopt(
                    socket.SOL_SOCKET, socket.SO_TYPE) != socket.SOCK_STREAM:
                raise UsageError("data endpoint is not an AF_UNIX stream")
            sock.getpeername()
        finally:
            sock.close()
        return
    raise UsageError("data endpoint is not a pipe or AF_UNIX stream")


def terminal_socket(text):
    fd = descriptor(text)
    copied = os.dup(fd)
    try:
        sock = socket.socket(fileno=copied)
    except BaseException:
        os.close(copied)
        raise
    try:
        if (sock.family != socket.AF_UNIX
                or sock.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) != socket.SOCK_SEQPACKET):
            raise UsageError("terminal endpoint is not AF_UNIX SOCK_SEQPACKET")
        sock.getpeername()
        if endpoint_identity(fd) in (endpoint_identity(1), endpoint_identity(2)):
            raise UsageError("terminal endpoint aliases a data endpoint")
        sock.setblocking(False)
    except BaseException:
        sock.close()
        raise
    return fd, sock


def typed_args(catalog, code, args):
    """Use the installed catalog as the sole code/argument vocabulary."""
    if not isinstance(code, str) or code not in catalog or not isinstance(args, dict):
        raise ProtocolError("unknown failure code or non-object args")
    fields = catalog[code]["args"]
    if args.keys() - fields.keys() or any(
            key not in args and not spec.get("optional", False) for key, spec in fields.items()):
        raise ProtocolError("failure arguments do not match the catalog")
    for name, node in args.items():
        spec = fields[name]
        kind = spec["kind"]
        text_kind = kind in ("string", "identifier", "keyword", "pine_source", "symbol", "timeframe")
        if text_kind:
            valid = node.kind == "string"
        elif kind == "nullable_string":
            valid = node.kind in ("string", "null")
        elif kind == "integer":
            valid = node.kind == "integer" and node.value is not None and abs(node.value) <= (1 << 53) - 1
        elif kind == "number":
            valid = node.kind in ("integer", "number") and node.value is not None
        elif kind == "boolean":
            valid = node.kind == "boolean"
        elif kind == "scalar":
            valid = (node.kind in ("string", "integer", "number", "boolean", "null")
                     and (node.kind not in ("integer", "number") or node.value is not None))
        elif kind == "vocab":
            valid = node.kind == "string" and node.value in spec["values"]
        else:
            valid = False
        if not valid:
            raise ProtocolError("failure argument has an invalid type or value")


@dataclass
class Value:
    kind: str
    value: object = None
    oversized: bool = False


class JSONScanner:
    """Chunked JSON validator retaining shape metadata, not report arrays.

    Object key sets enforce duplicates at every depth. String *values* retained
    for claims are bounded independently; large report arrays/objects are walked
    and discarded. The host must reserve parser/key-set overhead as well as R.
    """
    def __init__(self, stream):
        self.stream = stream
        self.decoder = codecs.getincrementaldecoder("utf-8")("strict")
        self.chunk, self.offset, self.ended = "", 0, False

    def peek(self):
        while self.offset == len(self.chunk) and not self.ended:
            raw = self.stream.read(QUEUE_BYTES)
            self.chunk = self.decoder.decode(raw, final=not raw)
            self.offset = 0
            self.ended = not raw
        return self.chunk[self.offset:self.offset + 1]

    def take(self):
        char = self.peek()
        if not char:
            raise IncompleteJSON("unexpected JSON EOF")
        self.offset += 1
        return char

    def expect(self, text):
        for char in text:
            if self.take() != char:
                raise ProtocolError("invalid JSON token")

    def space(self):
        while self.peek() and self.peek() in " \t\r\n":
            self.take()

    def string(self, *, key=False):
        self.expect('"')
        result, length = [], 0
        while True:
            char = self.take()
            if char == '"':
                return Value("string", "".join(result) if key or length <= CLAIM_BYTES else None,
                             not key and length > CLAIM_BYTES)
            if char == "\\":
                escaped = self.take()
                if escaped == "u":
                    digits = "".join(self.take() for _ in range(4))
                    if re.fullmatch(r"[0-9a-fA-F]{4}", digits) is None:
                        raise ProtocolError("invalid Unicode escape")
                    number = int(digits, 16)
                    if 0xD800 <= number <= 0xDBFF:
                        self.expect("\\u")
                        low = "".join(self.take() for _ in range(4))
                        if re.fullmatch(r"[0-9a-fA-F]{4}", low) is None:
                            raise ProtocolError("invalid Unicode pair")
                        low_number = int(low, 16)
                        if not 0xDC00 <= low_number <= 0xDFFF:
                            raise ProtocolError("unpaired Unicode surrogate")
                        number = 0x10000 + ((number - 0xD800) << 10) + low_number - 0xDC00
                    elif 0xDC00 <= number <= 0xDFFF:
                        raise ProtocolError("unpaired Unicode surrogate")
                    char = chr(number)
                else:
                    escapes = {'"': '"', "\\": "\\", "/": "/", "b": "\b",
                               "f": "\f", "n": "\n", "r": "\r", "t": "\t"}
                    if escaped not in escapes:
                        raise ProtocolError("invalid string escape")
                    char = escapes[escaped]
            elif ord(char) < 32:
                raise ProtocolError("unescaped control character")
            length += len(char.encode("utf-8"))
            if key or length <= CLAIM_BYTES:
                result.append(char)

    def value(self, depth=0):
        self.space()
        char = self.peek()
        if not char:
            raise IncompleteJSON("missing JSON value")
        if char == '"':
            return self.string()
        if char == "{":
            self.take()
            seen, retained = set(), {}
            self.space()
            if self.peek() == "}":
                self.take()
                return Value("object", retained)
            while True:
                self.space()
                if self.peek() != '"':
                    if not self.peek():
                        raise IncompleteJSON("missing object key")
                    raise ProtocolError("non-string object key")
                key = self.string(key=True).value
                if key in seen:
                    raise ProtocolError("duplicate JSON key")
                seen.add(key)
                self.space()
                self.expect(":")
                child = self.value(depth + 1)
                if depth <= 1:
                    retained[key] = child
                self.space()
                separator = self.take()
                if separator == "}":
                    return Value("object", retained)
                if separator != ",":
                    raise ProtocolError("invalid object separator")
        if char == "[":
            self.take()
            self.space()
            if self.peek() == "]":
                self.take()
                return Value("array")
            while True:
                self.value(depth + 1)
                self.space()
                separator = self.take()
                if separator == "]":
                    return Value("array")
                if separator != ",":
                    raise ProtocolError("invalid array separator")
        for literal, result in (("true", Value("boolean", True)),
                                ("false", Value("boolean", False)),
                                ("null", Value("null"))):
            if char == literal[0]:
                self.expect(literal)
                return result
        if char in "-0123456789":
            token = []
            while self.peek() and self.peek() in "-+0123456789.eE":
                token.append(self.take())
            text = "".join(token)
            if re.fullmatch(r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?", text) is None:
                raise ProtocolError("invalid JSON number")
            number = float(text)
            if not math.isfinite(number):
                raise ProtocolError("nonfinite JSON number")
            integer = "." not in text and "e" not in text.lower()
            return Value("integer" if integer else "number",
                         int(text) if integer and len(text) <= 17 else
                         (None if integer else number))
        raise ProtocolError("invalid JSON value")


def validate_candidate(stream, catalog, operation):
    """Return validity, EOF-deferred error, failure claim, and omitted flag."""
    stream.seek(0, os.SEEK_END)
    length = stream.tell()
    if not length:
        return False, True, False, None, False
    stream.seek(0)
    try:
        scanner = JSONScanner(stream)
        root = scanner.value()
        # Exactly the object's LF: neither trailing whitespace nor a second LF.
        if scanner.take() != "\n" or scanner.peek():
            raise ProtocolError("extra candidate payload")
        if root.kind != "object":
            raise ProtocolError("candidate is not an object")
        fields = root.value
        if fields.get("engine") != Value("string", "pineforge"):
            raise ProtocolError("wrong report engine")
        if "error" in fields:
            if fields["error"].kind != "string" or ("code" in fields) != ("args" in fields):
                raise ProtocolError("invalid failure object")
            if fields.keys() - {
                    "engine", "error", "code", "args", "reason", "high_water_bytes",
                    "arena_bytes", "bars", "strategy_started"}:
                raise ProtocolError("extra failure-object member")
            claim, omitted = None, False
            if "code" in fields:
                code, args = fields["code"], fields["args"]
                if code.kind != "string" or args.kind != "object":
                    raise ProtocolError("invalid failure code or args")
                typed_args(catalog, code.value, args.value)
                omitted = any(value.oversized for value in args.value.values())
                values = {name: value.value for name, value in args.value.items()}
                if not omitted:
                    omitted = len(wire(values)) - 1 > CLAIM_BYTES
                if not omitted:
                    claim = {"code": code.value, "args": values, "authority": "producer"}
            return True, False, True, claim, omitted
        if "code" in fields or "args" in fields:
            raise ProtocolError("failure metadata without an error")
        if operation == "capabilities":
            required = {"capabilities": "object"}
        elif operation == "validation":
            required = {"validation_only": "boolean", "report_window": "object"}
            if fields.get("validation_only") != Value("boolean", True):
                raise ProtocolError("invalid validation report")
        else:
            required = {name: "object" for name in (
                "input", "summary", "applied_inputs", "applied_overrides",
                "applied_runtime", "diagnostics", "metrics")}
            for name in ("trades", "equity_curve"):
                if name in fields and fields[name].kind != "array":
                    raise ProtocolError("invalid report sequence")
                if operation == "report" and name not in fields:
                    raise ProtocolError("missing full-report sequence")
                if operation == "metrics" and name in fields:
                    raise ProtocolError("unexpected metrics-only sequence")
            if (operation != "metrics"
                    and fields.get("elapsed_seconds", Value("null")).kind not in ("number", "integer")):
                raise ProtocolError("missing report elapsed time")
        if any(name not in fields or fields[name].kind != kind for name, kind in required.items()):
            raise ProtocolError("wrong candidate shape")
        if operation in ("report", "metrics", "fd_summary"):
            def member(parent, name, kinds):
                value = parent.value.get(name)
                if value is None or value.kind not in kinds:
                    raise ProtocolError("missing or wrong-type report field")
                return value

            data = fields["input"]
            member(data, "ohlcv", ("string",))
            bars = member(data, "bars", ("integer",))
            if bars.value is None or bars.value < 0:
                raise ProtocolError("invalid report bar count")
            for name in ("first_ts", "last_ts"):
                member(data, name, ("integer", "null"))
            for name in ("first_time", "last_time"):
                member(data, name, ("string", "null"))
            summary = fields["summary"]
            for name in ("total_trades", "wins", "losses", "bars_processed"):
                value = member(summary, name, ("integer",))
                if value.value is None or value.value < 0:
                    raise ProtocolError("invalid summary count")
            for name in ("win_rate_pct", "net_pnl", "avg_trade", "max_drawdown"):
                member(summary, name, ("integer", "number"))
            for name in ("best_trade", "worst_trade"):
                member(summary, name, ("integer", "number", "null"))
            for name in ("all", "longs", "shorts", "equity"):
                member(fields["metrics"], name, ("object",))
        return True, False, False, None, False
    except IncompleteJSON:
        return False, True, False, None, False
    except (ProtocolError, UnicodeError, ValueError, RecursionError, OverflowError):
        return False, False, False, None, False


def validation_worker(candidate_fd, result_fd, operation):
    """Trusted parser process: no producer, external report or terminal fd."""
    try:
        catalog = json.loads(CATALOG.read_text(encoding="utf-8"))["codes"]
        with os.fdopen(candidate_fd, "rb") as stream:
            result = {"result": validate_candidate(stream, catalog, operation)}
    except Exception:
        result = {"fault": True}
    data = wire(result)
    if len(data) > TERMINAL_BYTES:
        data = wire({"fault": True})
    try:
        remaining = memoryview(data)
        while remaining:
            try:
                written = os.write(result_fd, remaining)
            except InterruptedError:
                continue
            if written <= 0:
                return 1
            remaining = remaining[written:]
    except OSError:
        return 1
    finally:
        os.close(result_fd)
    return 0


class Channel:
    def __init__(self, limit, *, absent=False):
        self.observed = self.accepted = self.written = 0
        self.limit = limit
        self.truncated = False
        self.complete = self.known = absent
        self.accepted_hash = hashlib.sha256()
        self.written_hash = hashlib.sha256()
        self.active = not absent
        self.logical_hash = None

    def accept(self, data):
        self.accepted += len(data)
        self.accepted_hash.update(data)

    def write(self, data):
        self.written += len(data)
        self.written_hash.update(data)

    def record(self, name):
        value = {
            "observed_bytes": self.observed, "accepted_bytes": self.accepted,
            "written_bytes": (self.observed if self.known else None)
            if name == "candidate" else self.written,
            "limit_bytes": self.limit, "truncated": self.truncated,
            "complete": self.complete, "source_total_known": self.known,
            "sha256": (self.accepted_hash if name == "candidate" else self.written_hash).hexdigest(),
        }
        if name == "sink":
            value.update(
                sha256=self.written_hash.hexdigest() if self.written else None,
                accepted_sha256=self.accepted_hash.hexdigest() if self.active else None,
                logical_sha256=self.logical_hash)
        if name == "status":
            value["sha256"] = None
        return value


def resource_cells(state, scope):
    units = {"wall": "ms", "cpu_user": "us", "cpu_system": "us"}
    counts = {"scratch_files", "work_files", "open_fds", "processes"}
    names = ("wall", "cpu_user", "cpu_system", "memory_rss", "memory_private",
             "memory_job", "scratch_logical", "scratch_allocated", "work_logical",
             "work_allocated", "scratch_files", "work_files", "open_fds", "processes",
             "arena_claimed", "arena_capacity", "report_bytes", "sink_bytes", "diagnostic_bytes")
    return {name: {
        "used": None, "requested": None, "limit": None, "peak": None,
        "unit": units.get(name, "count" if name in counts else "bytes"),
        "scope": scope, "source": "none", "state": state,
    } for name in names}


def pipe():
    ends = list(os.pipe())
    for index, fd in enumerate(ends):
        if fd == 5:
            ends[index] = fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 6)
            os.close(fd)
    return tuple(ends)


def stream_pair():
    ends = [endpoint.detach() for endpoint in socket.socketpair()]
    for index, fd in enumerate(ends):
        if fd == 5:
            ends[index] = fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 6)
            close_once(fd)
    return tuple(ends)


class Supervisor:
    def __init__(self, argv):
        self.argv, self.started = list(argv), time.monotonic()
        self.catalog = json.loads(CATALOG.read_text(encoding="utf-8"))["codes"]
        self.options = None
        self.failure = None
        self.facade = 0
        self.invocation = (
            "entrypoint" if early_value(argv, "--invocation") == "entrypoint" else "run_json")
        self.attempt = early_value(argv, "--attempt-id", "PINEFORGE_REPORT_ATTEMPT_ID")
        self.limits = {name: {"requested": None, "granted": default, "effective": default}
                       for name, default in DEFAULTS.items()}
        self.channels = {
            "candidate": Channel(DEFAULTS["report_bytes"]),
            "report": Channel(DEFAULTS["report_bytes"]),
            "sink": Channel(None, absent=True),
            "diagnostic": Channel(DEFAULTS["diagnostic_bytes"], absent=True),
            "status": Channel(TERMINAL_BYTES),
        }
        self.status_socket = None
        self.status_fd = None
        self.sink_fd = None
        self.planned_sink_bytes = None
        self.sink_bound = None
        self.validator = None
        self.sink_directory = None
        self.sink_target = None
        self.sink_verification = None
        self.sink_deferred = False
        self.sink_overflow = False
        self.native_eof = False
        self.publication_started = False
        self.process = None
        self.spool = None
        self.charged = 0
        self.run_started = self.run_finished = self.publish_started = None
        self.finalize_started = self.finalize_finished = None
        self.publish_finished = None
        self.producer_claim = None
        self.claim_omitted = False
        self.cancelled = False
        self.external_closed = set()
        self.artifacts_removed = None
        self.operation = "report"
        self.wall_end = self.started + DEFAULTS["wall_ms"] / 1000
        self.stall_seconds = DEFAULTS["stall_ms"] / 1000
        self.deadline = Deadline(DEFAULTS["wall_ms"], DEFAULTS["stall_ms"])
        self.diagnostic_by_stage = {stage: 0 for stage in ("run", "finalize")}
        # A single raw syscall returns EINTR to S's deadline loop. Python's
        # convenience writes can retry it below that loop.
        self.raw_io = ctypes.CDLL(None, use_errno=True)
        self.raw_io.write.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
        self.raw_io.write.restype = ctypes.c_ssize_t
        self.raw_io.read.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
        self.raw_io.read.restype = ctypes.c_ssize_t
        self.raw_io.send.argtypes = (
            ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int)
        self.raw_io.send.restype = ctypes.c_ssize_t
        for fd in (1, 2):
            try:
                os.set_blocking(fd, False)
            except OSError:
                pass

    def latch(self, code, args, exit_class=None, *, usage=False):
        if self.failure is not None:
            return
        # A misspelt code or malformed trusted argument is an S fault, never a
        # silently extended vocabulary.
        typed_args(self.catalog, code, {
            name: Value("boolean" if isinstance(value, bool) else
                        "integer" if isinstance(value, int) else "string", value)
            for name, value in args.items()})
        if exit_class is None:
            exit_class = (
                "input_refusal" if usage or code == "report_mode_unsupported" else
                "resource_refusal" if code in ("report_budget_exceeded",
                                              "diagnostic_budget_exceeded") else
                "producer_failure" if code == "run_producer_failed" else
                "harness_fault" if code == "harness_internal_error" else
                "aborted" if code == "run_aborted" else
                "transport_failure" if code in ("report_channel_failed", "report_result_invalid",
                                               "report_sink_failed") else "resource_refusal")
        self.failure = {"code": code, "args": args, "authority": "supervisor"}
        self.exit_class = exit_class
        self.facade = 4 if self.invocation == "entrypoint" else (2 if usage else 1)

    def io_failure(self, error, stage="publish"):
        if error.wall:
            self.latch("run_deadline_exceeded", {"stage": stage})
        elif error.channel == "sink" and error.reason != "deadline":
            self.latch("report_sink_failed", {
                "reason": "sink_peer_closed" if error.reason == "peer_closed" else "sink_io"})
        else:
            self.latch("report_channel_failed", {"channel": error.channel, "reason": error.reason})

    def stage(self):
        return "finalize" if (self.native_eof or (
            self.sink_bound is None and self.process is not None
            and self.process.poll() is not None)) else "run"

    def transfer_error(self, error, channel):
        if isinstance(error, DeadlineError):
            return TransferError(channel, "deadline", wall=error.kind == "wall")
        return TransferError(channel, "peer_closed" if isinstance(error, PeerClosed) else "io")

    def _attestation(self, text):
        if text is None:
            self.latch("run_isolation_unavailable", {"reason": "policy"})
            return None
        try:
            fd = descriptor(text)
            if endpoint_identity(fd) in {
                    endpoint_identity(item) for item in (1, 2, self.status_fd)}:
                raise UsageError("attestation aliases a public endpoint")
            if not stat.S_ISFIFO(os.fstat(fd).st_mode):
                raise UsageError("TEST attestation requires a private pipe")
            if fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE != os.O_RDONLY:
                raise UsageError("TEST attestation must be read-only")
            os.set_blocking(fd, False)
            data = bytearray()
            while True:
                chunk, eof = bounded_read(
                    fd, min(QUEUE_BYTES, TERMINAL_BYTES + 1 - len(data)),
                    self.deadline, "prepare")
                if eof:
                    break
                data.extend(chunk)
                if len(data) > TERMINAL_BYTES:
                    raise UsageError("oversized attestation")
            document = json.loads(data.decode("utf-8"))
            if (document.get("schema") != "pf-report-test-only-host-attestation"
                    or type(document.get("version")) is not int or document["version"] != 1
                    or document.get("attempt_id") != self.attempt
                    or document.get("test_only_unisolated_process_fixture") is not True):
                raise UsageError("invalid TEST attestation")
            grants = document["grants"]
            if set(grants) != set(DEFAULTS) or any(
                    type(value) is not int or not 1 <= value <= MAX_INTEGER
                    for value in grants.values()):
                for name in DEFAULTS:
                    value = grants.get(name)
                    known = type(value) is int and 0 <= value <= MAX_INTEGER
                    self.limits[name].update(granted=value if known else None, effective=None)
                report_grant = self.limits["report_bytes"]["granted"] or 0
                for name in ("candidate", "report"):
                    self.channels[name].limit = report_grant
                self.channels["diagnostic"].limit = (
                    self.limits["diagnostic_bytes"]["granted"] or 0)
                raise UsageError("invalid host grant")
            planned = document.get("planned_sink_bytes")
            if planned is not None and (type(planned) is not int or not 1 <= planned <= MAX_INTEGER):
                raise UsageError("invalid planned sink allowance")
            self.planned_sink_bytes = planned
            return grants
        except (OSError, DeadlineError, ValueError, UnicodeError, KeyError, TypeError, UsageError):
            self.latch("run_isolation_unavailable", {"reason": "policy"})
            return None

    def admit(self):
        try:
            text = early_value(self.argv, "--terminal-status-fd", "PINEFORGE_TERMINAL_STATUS_FD")
            self.status_fd, self.status_socket = terminal_socket(text)
        except (OSError, ValueError, TypeError, UsageError):
            self.latch("run_request_invalid", {"option": "arguments"}, usage=True)
            return
        try:
            self.options = parse(self.argv, os.environ)
            options = self.options
            self.invocation = options.invocation
            if options.report_channel != CONTRACT:
                raise UsageError("report channel must be explicitly supervised/v1")
            if not isinstance(self.attempt, str) or re.fullmatch(r"[0-9a-f]{32}", self.attempt) is None:
                # Without a host-bound identity there is no valid record to send.
                self.status_socket.close()
                self.status_socket = None
                raise UsageError("host attempt identity is required")
            writable_stream(1)
            writable_stream(2)
            identities = [endpoint_identity(fd) for fd in (1, 2, self.status_fd)]
            if options.report_sink_fd is not None:
                sink_fd = descriptor(options.report_sink_fd)
                self.sink_fd = sink_fd
                writable_stream(sink_fd)
                identities.append(endpoint_identity(sink_fd))
            if options.report_sink is not None and options.report_sink_fd is not None:
                raise UsageError("conflicting sink routes")
            if len(identities) != len(set(identities)):
                raise UsageError("aliased endpoints")
            if options.run_phase_fd is not None:
                raise UsageError("external phase transport is excluded")
            if (options.host_attestation_fd is not None
                    and options.test_only_host_attestation_fd is not None):
                raise UsageError("conflicting host attestation routes")
            for flag, (_, name) in CEILINGS.items():
                text = getattr(options, flag[2:].replace("-", "_"))
                self.limits[name]["requested"] = None if text is None else decimal(text)
            tokens = options.command
            names = {token.partition("=")[0] for token in tokens}
            if names & {"--help", "-h", "--run-phase-fd"} or "csv2bin" in tokens:
                raise UsageError("excluded producer protocol")
            for index, token in enumerate(tokens):
                name, equals, suffix = token.partition("=")
                if name == "--report-mode":
                    mode = suffix if equals else (tokens[index + 1] if index + 1 < len(tokens) else "")
                    if mode in ("SUMMARY_ONLY", "STREAMED"):
                        self.latch("report_mode_unsupported",
                                   {"mode": mode, "contract": "pineforge-report/v1"})
                        return
            if "--capabilities-json" in names:
                self.operation = "capabilities"
            elif "--validate-window-only" in names:
                self.operation = "validation"
            elif "--metrics-only" in names:
                self.operation = "metrics"
            if options.report_sink_fd is not None:
                self.operation = "fd_summary"
            grants = self._attestation(options.test_only_host_attestation_fd)
            if grants is None:
                return
            for name, grant in grants.items():
                cell = self.limits[name]
                requested = cell["requested"]
                cell["granted"] = grant
                cell["effective"] = grant if requested == 0 else (
                    DEFAULTS[name] if requested is None else requested)
            above_grant = False
            for cell in self.limits.values():
                if cell["effective"] > cell["granted"]:
                    cell["effective"] = None
                    above_grant = True
            # A rejected request is not silently recorded as an accepted clamp.
            # Its failure publication must nevertheless obey the host grant.
            report_limit = (self.limits["report_bytes"]["effective"]
                            if self.limits["report_bytes"]["effective"] is not None
                            else self.limits["report_bytes"]["granted"])
            for name in ("candidate", "report"):
                self.channels[name].limit = report_limit
            self.channels["diagnostic"].limit = (
                self.limits["diagnostic_bytes"]["effective"]
                if self.limits["diagnostic_bytes"]["effective"] is not None
                else self.limits["diagnostic_bytes"]["granted"])
            if above_grant:
                raise UsageError("positive ceiling exceeds host grant")
            if self.planned_sink_bytes is not None and self.planned_sink_bytes > report_limit:
                self.latch("report_budget_exceeded", {
                    "channel": "aggregate", "cap_bytes": report_limit,
                    "used_bytes": 0, "requested_bytes": self.planned_sink_bytes})
                return
            self.wall_end = self.started + self.limits["wall_ms"]["effective"] / 1000
            self.stall_seconds = self.limits["stall_ms"]["effective"] / 1000
            for name in ("candidate", "report"):
                self.channels[name].limit = self.limits["report_bytes"]["effective"]
            self.channels["diagnostic"].limit = self.limits["diagnostic_bytes"]["effective"]
            # A named target and the internal snapshot protocol need their own
            # handoff/publication adapters. Never let U open either target.
            if (options.report_sink is not None
                    or names & {"--report-sink", "--report-sink-fd", "--report-replay-sink"}):
                self.latch("run_isolation_unavailable", {"reason": "descriptor"})
                return
            if time.monotonic() >= self.wall_end:
                self.latch("run_deadline_exceeded", {"stage": "prepare"})
                return
            # Resolve W once from the original start, before any covered work.
            # No data phase or successful write constructs a fresh W.
            self.deadline = Deadline(
                (self.wall_end - time.monotonic()) * 1000,
                self.limits["stall_ms"]["effective"])
            if self.sink_fd is not None:
                self.sink_bound = self.planned_sink_bytes or report_limit
                self.channels["sink"] = Channel(self.sink_bound)
                self.sink_verification = {
                    "exit_status": None, "signal": None, "complete": False,
                    "counts": {tag: None for tag in "ETOVKXH"},
                    "sha256": None, "finalizer_version": None}
            os.set_blocking(1, False)
            os.set_blocking(2, False)
        except (OSError, UsageError, ValueError):
            self.latch("run_request_invalid", {"option": "arguments"}, usage=True)

    def send_ready(self, fd, data, *, socket_endpoint):
        """One raw nonblocking syscall, including EINTR, with bounded scratch."""
        length = len(data)
        if length > QUEUE_BYTES:
            raise ValueError("oversized pending channel step")
        buffer = (ctypes.c_char * length).from_buffer_copy(data)
        if socket_endpoint:
            count = self.raw_io.send(
                fd, buffer, length, getattr(socket, "MSG_DONTWAIT", 0)
                | getattr(socket, "MSG_NOSIGNAL", 0))
        else:
            # main() suppresses SIGPIPE before any pipe write. The companion
            # helper deliberately accepts stream sockets only.
            count = self.raw_io.write(fd, buffer, length)
        if count < 0:
            error = ctypes.get_errno() or errno.EIO
            raise OSError(error, os.strerror(error))
        return count

    def read_ready(self, fd, size, stage):
        """Return a ready chunk before the caller checks W after observation.

        bounded_read can raise after consuming a chunk without returning its
        bytes. Metered inputs need the payload for independent prefix hashes.
        These owned pipes are nonblocking; no wait/retry happens in this step.
        """
        if self.deadline.expired():
            raise DeadlineError("wall", stage, 0)
        if not 0 < size <= QUEUE_BYTES:
            raise ValueError("invalid ready read size")
        buffer = ctypes.create_string_buffer(size)
        count = self.raw_io.read(fd, buffer, size)
        if count < 0:
            error = ctypes.get_errno() or errno.EIO
            raise OSError(error, os.strerror(error))
        return buffer.raw[:count]

    def write_ready(self, fd, data):
        """Caller owns W, per-channel P, the queue and nonblocking fd setup."""
        return self.send_ready(
            fd, data, socket_endpoint=stat.S_ISSOCK(os.fstat(fd).st_mode))

    def bounded_write(self, fd, data, channel, *, stage="publish"):
        if stat.S_ISSOCK(os.fstat(fd).st_mode):
            try:
                count = bounded_write(fd, data, self.deadline, stage)
            except (DeadlineError, PeerClosed, TransportError) as error:
                self.channels[channel].write(memoryview(data)[:error.bytes_moved])
                self.publication_started |= channel in ("report", "sink") and error.bytes_moved > 0
                raise self.transfer_error(error, channel) from error
            self.channels[channel].write(memoryview(data)[:count])
            self.publication_started |= channel in ("report", "sink") and count > 0
            return
        # Pipe adapter: the descriptor is nonblocking, W never changes, and
        # only a positive syscall return renews this pending operation's P.
        end = time.monotonic() + self.stall_seconds
        remaining = memoryview(data)
        with selectors.DefaultSelector() as selector:
            selector.register(fd, selectors.EVENT_WRITE)
            while remaining:
                now = time.monotonic()
                if self.deadline.expired():
                    raise TransferError(channel, "deadline", wall=True)
                if now >= end:
                    raise TransferError(channel, "deadline")
                try:
                    count = self.write_ready(fd, remaining[:QUEUE_BYTES])
                except (BlockingIOError, InterruptedError):
                    selector.select(min(0.05, self.deadline.remaining_wall_ms() / 1000,
                                        max(0, end - now)))
                    continue
                except OSError as error:
                    reason = "peer_closed" if error.errno in (
                        errno.EPIPE, errno.ECONNRESET, errno.ENOTCONN) else "io"
                    raise TransferError(channel, reason) from error
                if count <= 0:
                    raise TransferError(channel, "io")
                self.channels[channel].write(remaining[:count])
                self.publication_started |= channel in ("report", "sink") and count > 0
                remaining = remaining[count:]
                end = time.monotonic() + self.stall_seconds
        if self.deadline.expired():
            raise TransferError(channel, "deadline", wall=True)

    def finish_sink(self, data, receipt_eof):
        """Interpret V only after its actual wait and receipt EOF are known."""
        status = self.validator.returncode
        verification = self.sink_verification
        verification["exit_status"] = status if status >= 0 else None
        verification["signal"] = -status if status < 0 else None
        try:
            if not receipt_eof or not data.endswith(b"\n") or data.count(b"\n") != 1:
                raise ProtocolError("missing or incomplete finalizer receipt")
            receipt = json.loads(data)
            base = {"complete", "sink_bytes", "sha256", "finalizer_version",
                    *("count_" + tag.lower() for tag in "ETOVKXH")}
            extra = ({"reason", "cap_bytes"} if "reason" in receipt else
                     {"tool", "event", "exit_code", "detail", "code", "args"}
                     if "code" in receipt else set())
            if (set(receipt) != base | extra or type(receipt["complete"]) is not bool
                    or type(receipt["sink_bytes"]) is not int
                    or not 0 <= receipt["sink_bytes"] <= self.sink_bound + 1
                    or not isinstance(receipt["finalizer_version"], str)
                    or receipt["finalizer_version"] != "1.1.0"):
                raise ProtocolError("invalid finalizer receipt")
            counts = {tag: receipt["count_" + tag.lower()] for tag in "ETOVKXH"}
            if any(type(value) is not int or not 0 <= value <= MAX_INTEGER
                   for value in counts.values()):
                raise ProtocolError("invalid finalizer counts")
            digest = receipt["sha256"]
            if ((receipt["complete"] and (not isinstance(digest, str)
                                         or re.fullmatch(r"[0-9a-f]{64}", digest) is None))
                    or (not receipt["complete"] and digest != "")):
                raise ProtocolError("invalid finalizer digest")
            if "reason" in receipt and (
                    receipt["reason"] != "sink_exhausted"
                    or receipt["cap_bytes"] != self.sink_bound
                    or receipt["sink_bytes"] != self.sink_bound + 1):
                raise ProtocolError("invalid exhaustion receipt")
            verification.update(counts=counts, sha256=digest or None,
                                finalizer_version=receipt["finalizer_version"])
            if (receipt["complete"] != (status == 0)
                    or (status == 8) != ("reason" in receipt)
                    or ("code" in receipt and status != 3)):
                raise ProtocolError("finalizer receipt disagrees with actual wait")
            sink = self.channels["sink"]
            if status == 0:
                if (not self.native_eof or sink.truncated
                        or receipt["sink_bytes"] != sink.accepted
                        or os.stat(self.sink_target).st_size != sink.accepted):
                    raise ProtocolError("finalizer completion differs from K")
                verification["complete"] = True
                sink.logical_hash = digest
            elif status == 6:
                # A missing trailer can result from a producer exit. R19
                # defers this EOF evidence until U's real wait/claim is known.
                self.sink_deferred = True
            elif status == 8:
                if not self.sink_overflow:
                    raise ProtocolError("finalizer overflow without K evidence")
            elif status == 5:
                self.latch("report_sink_failed", {"reason": "sink_io"})
            elif status == 7:
                self.latch("harness_internal_error", {})
            elif status == 3 and extra:
                if (receipt["tool"] != "pineforge-sink-finalize"
                        or receipt["event"] != "sink_refused" or receipt["exit_code"] != 3
                        or receipt["code"] != "report_result_invalid"):
                    raise ProtocolError("invalid typed finalizer refusal")
                self.latch(receipt["code"], receipt["args"])
            else:
                self.latch("report_channel_failed", {"channel": "sink", "reason": "protocol"})
        except (OSError, ValueError, TypeError, KeyError, ProtocolError):
            self.latch("report_channel_failed", {"channel": "sink", "reason": "protocol"})

    def capture(self):
        candidate_read, candidate_write = pipe()
        diagnostic_read, diagnostic_write = pipe()
        owned = {candidate_read, candidate_write, diagnostic_read, diagnostic_write}
        readers = {candidate_read: "candidate", diagnostic_read: "diagnostic"}
        native_read = native_write = relay_fd = validator_read = receipt_read = None
        environment = dict(os.environ)
        for name in tuple(environment):
            if name.startswith(("PINEFORGE_REPORT_", "PINEFORGE_TERMINAL_",
                                "PINEFORGE_DIAGNOSTIC_", "PINEFORGE_CHANNEL_")):
                environment.pop(name)
        environment["PINEFORGE_REPORT_PRODUCER"] = CONTRACT
        environment["PINEFORGE_REPORT_CANDIDATE_FD"] = str(candidate_write)
        self.spool = tempfile.TemporaryFile(mode="w+b")
        self.artifacts_removed = False
        diagnostic_pending, relay_pending, receipt = bytearray(), bytearray(), bytearray()
        diagnostic_since = None
        candidate_waiting = False
        receipt_eof = validator_done = False
        stopping = {}

        def release(fd):
            if fd in owned:
                owned.remove(fd)
                if not close_once(fd):
                    name = readers.get(fd, "sink")
                    if name == "receipt":
                        name = "sink"
                    self.latch("report_channel_failed", {"channel": name, "reason": "io"})

        def stop(process, now):
            if process is None or process.poll() is not None:
                return
            if process not in stopping:
                process.terminate()
                stopping[process] = now
            elif now - stopping[process] >= STOP_GRACE_MS / 1000:
                process.kill()

        try:
            if self.sink_fd is not None:
                native_read, native_write = pipe()
                relay_fd, validator_read = stream_pair()
                receipt_read, receipt_write = pipe()
                owned.update((native_read, native_write, relay_fd, validator_read,
                              receipt_read, receipt_write))
                readers.update({native_read: "sink", receipt_read: "receipt"})
                environment["PINEFORGE_REPORT_NATIVE_SINK_FD"] = str(native_write)
                self.sink_directory = tempfile.TemporaryDirectory(prefix="pf-supervised-sink-")
                self.sink_target = str(Path(self.sink_directory.name) / "verified.sink")
                self.validator = subprocess.Popen(
                    [sys.executable, "-B", str(Path(__file__).with_name("pineforge_sink_finalize.py")),
                     "--in-fd", str(validator_read), "--out", self.sink_target,
                     "--max-bytes", str(self.sink_bound), "--receipt-fd", "2"],
                    stdin=subprocess.DEVNULL, stdout=diagnostic_write, stderr=receipt_write,
                    close_fds=True, pass_fds=(validator_read,), start_new_session=True)
                release(validator_read)
                release(receipt_write)
                os.set_blocking(relay_fd, False)
            launch_started = time.monotonic()
            self.process = subprocess.Popen(
                self.options.command, stdin=subprocess.DEVNULL,
                stdout=diagnostic_write, stderr=diagnostic_write, close_fds=True,
                pass_fds=(candidate_write,) if native_write is None else
                (candidate_write, native_write), env=environment, start_new_session=True)
            self.run_started = launch_started
            diagnostic = self.channels["diagnostic"]
            diagnostic.known = diagnostic.complete = False
            release(candidate_write)
            release(diagnostic_write)
            if native_write is not None:
                release(native_write)

            with selectors.DefaultSelector() as selector:
                def watch(fd, events, name):
                    if fd not in selector.get_map():
                        selector.register(fd, events, name)

                def unwatch(fd):
                    if fd in selector.get_map():
                        selector.unregister(fd)

                def end_read(fd):
                    unwatch(fd)
                    release(fd)
                    readers.pop(fd, None)

                def read(fd, size, name):
                    try:
                        return self.read_ready(fd, size, self.stage())
                    except (BlockingIOError, InterruptedError):
                        return None
                    except DeadlineError as error:
                        if error.kind == "wall":
                            self.latch("run_deadline_exceeded", {"stage": self.stage()})
                        else:
                            self.latch("report_channel_failed", {"channel": name, "reason": "deadline"})
                    except OSError:
                        self.latch("report_channel_failed", {"channel": name, "reason": "io"})
                    return None

                for fd in readers:
                    os.set_blocking(fd, False)
                while readers or self.process.poll() is None or diagnostic_pending or (
                        self.validator is not None and self.validator.poll() is None):
                    for fd, name in readers.items():
                        paused = (
                            (name == "sink" and bool(relay_pending))
                            or (name == "diagnostic" and len(diagnostic_pending) == QUEUE_BYTES)
                            or (name == "candidate" and candidate_waiting))
                        if paused:
                            unwatch(fd)
                        else:
                            watch(fd, selectors.EVENT_READ, name)
                    if relay_fd in owned and relay_pending:
                        watch(relay_fd, selectors.EVENT_WRITE, "relay")
                    elif relay_fd is not None:
                        unwatch(relay_fd)
                    if diagnostic_pending:
                        watch(2, selectors.EVENT_WRITE, "diagnostic_output")
                    else:
                        unwatch(2)
                    events = selector.select(min(0.02, self.deadline.remaining_wall_ms() / 1000))
                    ready = {key.fd for key, _ in events}
                    if self.cancelled:
                        self.latch("run_aborted", {})

                    # R19: diagnostic budget, native budget, candidate budget,
                    # independent validator/transport evidence, then W.
                    if diagnostic_read in ready and diagnostic_read in readers:
                        allowance = diagnostic.limit - diagnostic.accepted
                        room = QUEUE_BYTES - len(diagnostic_pending)
                        data = read(diagnostic_read, min(room, allowance + 1), "diagnostic")
                        if data == b"":
                            diagnostic.known = True
                            end_read(diagnostic_read)
                        elif data:
                            diagnostic.observed += len(data)
                            accepted = data[:allowance]
                            diagnostic.accept(accepted)
                            self.diagnostic_by_stage[self.stage()] += len(accepted)
                            if not diagnostic_pending and accepted:
                                diagnostic_since = time.monotonic()
                            diagnostic_pending.extend(accepted)
                            if len(data) > allowance:
                                diagnostic.truncated = True
                                self.latch("diagnostic_budget_exceeded", {
                                    "cap_bytes": diagnostic.limit, "used_bytes": diagnostic.accepted,
                                    "requested_bytes": 1})
                                end_read(diagnostic_read)

                    if candidate_read in ready:
                        candidate_waiting = True
                        unwatch(candidate_read)
                    if candidate_waiting and native_read in readers and not relay_pending:
                        # A readiness batch can precede a just-queued K EOF.
                        # Check K again before interpreting any C readiness.
                        ready.update(select.select([native_read], [], [], 0)[0])
                    native_progress = False
                    if (native_read in ready and native_read in readers
                            and not relay_pending and self.failure is None):
                        sink = self.channels["sink"]
                        allowance = sink.limit - sink.accepted
                        native_progress = True
                        data = read(native_read, min(QUEUE_BYTES, allowance + 1), "sink")
                        if data == b"":
                            sink.known = self.native_eof = True
                            self.finalize_started = time.monotonic()
                            self.channels["candidate"].limit = self.channels["report"].limit - sink.accepted
                            end_read(native_read)
                        elif data:
                            native_progress = True
                            sink.observed += len(data)
                            accepted = data[:allowance]
                            sink.accept(accepted)
                            self.charged += len(accepted)
                            self.channels["candidate"].limit = self.channels["report"].limit - sink.accepted
                            # Unlike deadlines.relay(), V must receive the one
                            # probe too. It is queued, never approved/charged.
                            relay_pending.extend(data)
                            if len(data) > allowance:
                                sink.truncated = self.sink_overflow = True
                                self.latch("report_sink_failed", {
                                    "reason": "sink_exhausted", "sink_bytes": sink.observed,
                                    "cap_bytes": sink.limit})
                                end_read(native_read)

                    if receipt_read in ready and receipt_read in readers:
                        data = read(receipt_read, min(QUEUE_BYTES, TERMINAL_BYTES + 1 - len(receipt)),
                                    "sink")
                        if data == b"":
                            receipt_eof = True
                            end_read(receipt_read)
                        elif data:
                            receipt.extend(data)
                            if len(receipt) > TERMINAL_BYTES:
                                self.latch("report_channel_failed", {"channel": "sink", "reason": "protocol"})
                                end_read(receipt_read)

                    if candidate_waiting and candidate_read in readers and self.failure is None:
                        candidate = self.channels["candidate"]
                        if native_read in readers:
                            if not relay_pending and not native_progress:
                                # K readiness was serviced first. With no K
                                # bytes/EOF queued, C data proves an open K.
                                data = read(candidate_read, 1, "candidate")
                                if data == b"":
                                    candidate.known = True
                                    end_read(candidate_read)
                                elif data:
                                    candidate.observed += len(data)
                                    candidate.truncated = True
                                    self.latch("report_channel_failed", {
                                        "channel": "candidate", "reason": "protocol"})
                        elif self.validator is None or validator_done:
                            allowance = candidate.limit - candidate.accepted
                            data = read(candidate_read, min(QUEUE_BYTES, allowance + 1), "candidate")
                            candidate_waiting = False
                            if data == b"":
                                candidate.known = True
                                end_read(candidate_read)
                            elif data:
                                candidate.observed += len(data)
                                accepted = data[:allowance]
                                candidate.accept(accepted)
                                if self.spool.write(accepted) != len(accepted):
                                    raise OSError("short candidate spool write")
                                self.charged += len(accepted)
                                if len(data) > allowance:
                                    candidate.truncated = True
                                    self.latch("report_budget_exceeded", {
                                        "channel": "aggregate" if self.channels["sink"].accepted else "json",
                                        "cap_bytes": self.channels["report"].limit,
                                        "used_bytes": self.charged, "requested_bytes": 1})

                    # Each output direction owns one fixed-size pending queue.
                    # Never wait in a synchronous write here: D and C must stay
                    # serviceable when V stops. K waits consume W, not P.
                    for fd, pending, name in (
                            (relay_fd, relay_pending, "sink"),
                            (2, diagnostic_pending, "diagnostic")):
                        if not pending or fd not in ready or self.deadline.expired():
                            continue
                        try:
                            count = self.write_ready(fd, pending)
                            if count <= 0:
                                raise OSError(errno.EIO, "zero channel write")
                            if name == "diagnostic":
                                diagnostic.write(memoryview(pending)[:count])
                                diagnostic_since = time.monotonic()
                            del pending[:count]
                        except (BlockingIOError, InterruptedError):
                            pass
                        except OSError as error:
                            reason = "peer_closed" if error.errno in (
                                errno.EPIPE, errno.ECONNRESET, errno.ENOTCONN) else "io"
                            self.io_failure(TransferError(name, reason))
                            self.channels[name].truncated = True
                            pending.clear()
                    if relay_fd in owned and native_read not in readers and not relay_pending:
                        unwatch(relay_fd)
                        release(relay_fd)
                    if (self.validator is not None and not validator_done
                            and self.validator.poll() is not None and receipt_read not in readers):
                        self.finish_sink(bytes(receipt), receipt_eof)
                        validator_done = True

                    now = time.monotonic()
                    if self.deadline.expired():
                        self.latch("run_deadline_exceeded", {"stage": self.stage()})
                        for fd in tuple(readers):
                            end_read(fd)
                        diagnostic.truncated |= bool(diagnostic_pending)
                        diagnostic_pending.clear()
                        relay_pending.clear()
                        if self.validator is not None and self.validator.poll() is None:
                            self.validator.kill()
                    elif (diagnostic_pending and diagnostic_since is not None
                          and now - diagnostic_since >= self.stall_seconds):
                        diagnostic.truncated = True
                        diagnostic_pending.clear()
                        self.io_failure(TransferError("diagnostic", "deadline"))
                    if self.failure is not None:
                        for fd in (candidate_read, native_read):
                            if fd in readers:
                                channel = self.channels[readers[fd]]
                                channel.truncated |= bool(channel.accepted and not channel.known)
                                end_read(fd)
                        stop(self.process, now)
                        if not self.sink_overflow:
                            relay_pending.clear()
                            if relay_fd in owned:
                                unwatch(relay_fd)
                                release(relay_fd)
                            stop(self.validator, now)
                    if self.process.poll() is not None and self.run_finished is None:
                        self.run_finished = now
                        if self.finalize_started is None and self.sink_bound is None:
                            self.finalize_started = now
                self.process.wait(timeout=STOP_GRACE_MS / 1000)
                if self.validator is not None:
                    self.validator.wait(timeout=STOP_GRACE_MS / 1000)
                    if not validator_done:
                        self.finish_sink(bytes(receipt), receipt_eof)
        finally:
            for fd in tuple(owned):
                release(fd)
        diagnostic = self.channels["diagnostic"]
        diagnostic.complete = (diagnostic.known and not diagnostic.truncated
                               and diagnostic.accepted == diagnostic.written)

    def validate(self):
        """Watch the trusted parser externally, including its diagnostic bytes."""
        self.spool.flush()
        self.spool.seek(0)
        result_read, result_write = pipe()
        diagnostic_read, diagnostic_write = pipe()
        worker = None
        data = bytearray()
        readers = {result_read: "result", diagnostic_read: "diagnostic"}
        diagnostic_eof = False
        try:
            launch_started = time.monotonic()
            worker = subprocess.Popen(
                [sys.executable, "-B", str(Path(__file__).resolve()),
                 "--_validate-candidate", str(self.spool.fileno()), str(result_write), self.operation],
                stdin=subprocess.DEVNULL, stdout=diagnostic_write, stderr=diagnostic_write,
                pass_fds=(self.spool.fileno(), result_write), close_fds=True)
            if self.finalize_started is None:
                self.finalize_started = launch_started
        except BaseException:
            os.close(result_read)
            os.close(diagnostic_read)
            raise
        finally:
            os.close(result_write)
            os.close(diagnostic_write)
        try:
            with selectors.DefaultSelector() as selector:
                for fd, name in readers.items():
                    os.set_blocking(fd, False)
                    selector.register(fd, selectors.EVENT_READ, name)
                while readers or worker.poll() is None:
                    events = selector.select(0.02)
                    events.sort(key=lambda item: item[0].data != "diagnostic")
                    for key, _ in events:
                        channel = self.channels["diagnostic"]
                        allowance = (channel.limit - channel.accepted if key.data == "diagnostic"
                                     else TERMINAL_BYTES - len(data))
                        try:
                            chunk = self.read_ready(
                                key.fd, min(QUEUE_BYTES, allowance + 1), "finalize")
                        except (BlockingIOError, InterruptedError):
                            continue
                        except DeadlineError as error:
                            if error.kind == "wall":
                                self.latch("run_deadline_exceeded", {"stage": "finalize"})
                            else:
                                self.latch("report_channel_failed", {
                                    "channel": "diagnostic" if key.data == "diagnostic" else "candidate",
                                    "reason": "deadline"})
                            break
                        except OSError:
                            self.latch("report_channel_failed", {
                                "channel": "diagnostic" if key.data == "diagnostic" else "candidate",
                                "reason": "io"})
                            break
                        if not chunk:
                            if key.data == "diagnostic":
                                diagnostic_eof = True
                            selector.unregister(key.fd)
                            readers.pop(key.fd)
                            os.close(key.fd)
                            continue
                        if key.data == "diagnostic":
                            channel.observed += len(chunk)
                            accepted = chunk[:allowance]
                            channel.accept(accepted)
                            self.diagnostic_by_stage["finalize"] += len(accepted)
                            if len(chunk) > allowance:
                                channel.truncated = True
                                self.latch("diagnostic_budget_exceeded", {
                                    "cap_bytes": channel.limit, "used_bytes": channel.accepted,
                                    "requested_bytes": 1})
                            try:
                                self.bounded_write(2, accepted, "diagnostic", stage="finalize")
                            except TransferError as error:
                                channel.truncated = True
                                self.io_failure(error, "finalize")
                        else:
                            data.extend(chunk)
                            if len(data) > TERMINAL_BYTES:
                                self.latch("harness_internal_error", {})
                    if self.cancelled:
                        self.latch("run_aborted", {})
                    if self.deadline.expired():
                        self.latch("run_deadline_exceeded", {"stage": "finalize"})
                    if self.failure is not None:
                        if worker.poll() is None:
                            worker.kill()
                        break
                worker.wait(timeout=STOP_GRACE_MS / 1000)
            self.finalize_finished = time.monotonic()
        finally:
            for fd in readers:
                os.close(fd)
            if worker.poll() is None:
                worker.kill()
                worker.wait(timeout=STOP_GRACE_MS / 1000)
            if not diagnostic_eof:
                self.channels["diagnostic"].known = self.channels["diagnostic"].complete = False
        if self.failure is not None:
            return False, True, False, None, False
        self.channels["diagnostic"].complete = (
            self.channels["diagnostic"].known and not self.channels["diagnostic"].truncated
            and self.channels["diagnostic"].accepted == self.channels["diagnostic"].written)
        if worker.returncode != 0:
            self.latch("harness_internal_error", {})
            return False, True, False, None, False
        try:
            message = json.loads(data)
            result = message["result"]
            if (len(result) != 5 or any(type(result[index]) is not bool for index in (0, 1, 2, 4))
                    or (result[3] is not None and not isinstance(result[3], dict))):
                raise ValueError("invalid trusted parser result")
            return tuple(result)
        except (ValueError, TypeError, KeyError):
            self.latch("harness_internal_error", {})
            return False, True, False, None, False

    def choose_report(self):
        if self.process is None:
            return None
        if self.failure is not None:
            return None
        valid, deferred, failed, claim, omitted = self.validate()
        self.producer_claim, self.claim_omitted = claim, omitted
        candidate = self.channels["candidate"]
        candidate.complete = valid and candidate.known and not candidate.truncated
        # Complete malformed candidates are independent protocol evidence. EOF
        # fragments are consequences until the producer wait status is known.
        if not valid and not deferred:
            self.latch("report_channel_failed", {"channel": "candidate", "reason": "protocol"})
        status = self.process.returncode
        if valid and failed:
            self.latch("run_producer_failed", {"reason": "claim"})
        elif status is not None and status < 0:
            self.latch("run_producer_failed", {"reason": "signal", "signal": -status})
        elif status:
            self.latch("run_producer_failed", {"reason": "exit", "exit_status": status})
        if self.sink_deferred:
            self.latch("report_channel_failed", {"channel": "sink", "reason": "protocol"})
        if not valid:
            self.latch("report_channel_failed", {"channel": "candidate", "reason": "protocol"})
        if valid and (self.failure is None or (
                failed and self.failure["code"] == "run_producer_failed"
                and self.failure["args"]["reason"] == "claim")):
            self.spool.seek(0)
            return self.spool
        return None

    def failure_document(self):
        if self.failure is None or self.publication_started:
            return None
        code, args = self.failure["code"], self.failure["args"]
        text = self.catalog[code]["english"][0]
        value = {"engine": "pineforge", "error": text, "code": code, "args": args}
        allowance = self.channels["report"].limit - self.charged
        if allowance < 1:
            return None
        pieces, length = [], 1  # The final LF is part of R.
        encoder = json.JSONEncoder(separators=(",", ":"), ensure_ascii=True, allow_nan=False)
        for text in encoder.iterencode(value):
            piece = text.encode("utf-8")
            if len(piece) > allowance - length:
                return None
            pieces.append(piece)
            length += len(piece)
        pieces.append(b"\n")
        data = b"".join(pieces)
        self.charged += length
        return data

    def publish(self, selected):
        if selected is None:
            selected = self.failure_document()
        if selected is None:
            return
        self.publish_started = time.monotonic()
        sink = self.channels["sink"]
        if self.sink_fd is not None and self.failure is None:
            try:
                os.set_blocking(self.sink_fd, False)
                with open(self.sink_target, "rb") as source:
                    while data := source.read(QUEUE_BYTES):
                        self.bounded_write(self.sink_fd, data, "sink")
                sink.complete = (
                    self.sink_verification["complete"] and sink.known
                    and sink.written == sink.accepted and not sink.truncated)
                if not sink.complete:
                    raise TransferError("sink", "io")
            except TransferError as error:
                sink.truncated = sink.written < sink.accepted
                self.io_failure(error)
                self.publish_finished = time.monotonic()
                return
            except OSError:
                sink.truncated = sink.written < sink.accepted
                self.latch("report_sink_failed", {"reason": "sink_io"})
                self.publish_finished = time.monotonic()
                return
        report = self.channels["report"]
        length = len(selected) if isinstance(selected, bytes) else self.channels["candidate"].accepted
        report.observed = report.accepted = length
        report.known = True
        try:
            if isinstance(selected, bytes):
                self.bounded_write(1, selected, "report")
            else:
                while data := selected.read(QUEUE_BYTES):
                    self.bounded_write(1, data, "report")
            report.complete = True
        except TransferError as error:
            report.truncated = True
            self.io_failure(error)
        except Exception:
            report.truncated = report.written < report.accepted
            self.latch("harness_internal_error", {})
        self.publish_finished = time.monotonic()

    def close_data(self):
        endpoints = [(1, "report"), (2, "diagnostic")]
        if self.sink_fd is not None:
            endpoints.append((self.sink_fd, "sink"))
        for fd, name in endpoints:
            if fd in self.external_closed:
                continue
            self.external_closed.add(fd)
            if not close_once(fd):
                self.channels[name].complete = False
                if name == "sink":
                    self.latch("report_sink_failed", {"reason": "sink_io"})
                else:
                    self.latch("report_channel_failed", {"channel": name, "reason": "io"})
        if self.spool is not None:
            try:
                self.spool.close()
                self.artifacts_removed = True
            except OSError:
                self.artifacts_removed = False
                self.latch("report_channel_failed", {"channel": "candidate", "reason": "io"})
        if self.sink_directory is not None:
            try:
                self.sink_directory.cleanup()
            except OSError:
                self.artifacts_removed = False
                self.latch("report_sink_failed", {"reason": "sink_io"})

    def usage(self):
        stages = {stage: {"state": "not_started",
                          "resources": resource_cells("not_started", "stage_process")}
                  for stage in ("prepare", "compile", "run", "finalize", "publish")}
        job = {"state": "unavailable", "resources": resource_cells("unavailable", "job_domain")}

        def measured(resources, name, value, source, *, limit=None, requested=None):
            if type(value) is not int or not 0 <= value <= (1 << 53) - 1:
                return
            resources[name].update(used=value, source=source, state="measured",
                                   limit=limit, requested=requested)

        now = time.monotonic()
        measured(job["resources"], "wall", int((now - self.started) * 1000), "clock",
                 limit=self.limits["wall_ms"]["effective"],
                 requested=self.limits["wall_ms"]["requested"])
        for stage, start, end in (
                ("run", self.run_started, self.run_finished),
                ("finalize", self.finalize_started, self.finalize_finished),
                ("publish", self.publish_started, self.publish_finished)):
            if start is not None:
                stages[stage] = {"state": "unavailable",
                                 "resources": resource_cells("unavailable", "stage_process")}
                measured(stages[stage]["resources"], "wall",
                         int(((end or now) - start) * 1000), "clock")
        for name, value, limit_name in (
                ("report_bytes", self.charged, "report_bytes"),
                ("sink_bytes", self.channels["sink"].accepted, None),
                ("diagnostic_bytes", self.channels["diagnostic"].accepted, "diagnostic_bytes")):
            limit = self.limits.get(limit_name, {})
            measured(job["resources"], name, value, "channel",
                     limit=self.sink_bound if name == "sink_bytes" else limit.get("effective"),
                     requested=self.planned_sink_bytes if name == "sink_bytes"
                     else limit.get("requested"))
        if self.run_started is not None:
            measured(stages["run"]["resources"], "report_bytes",
                     self.channels["sink"].accepted +
                     (0 if self.sink_bound is not None else self.channels["candidate"].accepted),
                     "channel", limit=self.channels["report"].limit,
                     requested=self.limits["report_bytes"]["requested"])
            measured(stages["run"]["resources"], "sink_bytes",
                     self.channels["sink"].accepted, "channel", limit=self.sink_bound,
                     requested=self.planned_sink_bytes)
            measured(stages["run"]["resources"], "diagnostic_bytes",
                     self.diagnostic_by_stage["run"], "channel",
                     limit=self.channels["diagnostic"].limit,
                     requested=self.limits["diagnostic_bytes"]["requested"])
        if self.finalize_started is not None:
            measured(stages["finalize"]["resources"], "report_bytes",
                     self.channels["candidate"].accepted if self.sink_bound is not None else 0,
                     "channel", limit=self.channels["candidate"].limit)
            measured(stages["finalize"]["resources"], "sink_bytes", 0, "channel",
                     limit=self.sink_bound, requested=self.planned_sink_bytes)
            measured(stages["finalize"]["resources"], "diagnostic_bytes",
                     self.diagnostic_by_stage["finalize"], "channel",
                     limit=self.channels["diagnostic"].limit,
                     requested=self.limits["diagnostic_bytes"]["requested"])
        if self.publish_started is not None:
            measured(stages["publish"]["resources"], "report_bytes",
                     self.channels["report"].written, "channel",
                     limit=self.channels["report"].limit,
                     requested=self.limits["report_bytes"]["requested"])
            measured(stages["publish"]["resources"], "sink_bytes",
                     self.channels["sink"].written, "channel", limit=self.sink_bound,
                     requested=self.planned_sink_bytes)
            measured(stages["publish"]["resources"], "diagnostic_bytes", 0, "channel",
                     limit=self.channels["diagnostic"].limit,
                     requested=self.limits["diagnostic_bytes"]["requested"])
        return {"version": 1, "stages": stages, "job": job}

    def record(self):
        status = self.process.returncode if self.process is not None else None
        cleanup = lambda value: {
            "value": value, "measurement_state": "unavailable" if value is None else "measured"}
        return {
            "schema": "pf-run-terminal", "version": 1, "contract": CONTRACT,
            "attempt_id": self.attempt, "invocation": self.invocation,
            "complete": (self.failure is None and self.channels["report"].complete
                         and self.channels["candidate"].complete
                         and self.channels["diagnostic"].complete
                         and self.channels["sink"].complete),
            "exit_class": self.exit_class if self.failure is not None else "success",
            "failure": self.failure, "exit_status": self.facade,
            "producer": {
                "stage": None if self.process is None else "run",
                "exit_status": status if status is not None and status >= 0 else None,
                "signal": -status if status is not None and status < 0 else None,
                "failure": self.producer_claim, "failure_claim_omitted": self.claim_omitted,
            },
            "limits": {**self.limits, "sink_bytes": self.sink_bound, "terminal_bytes": TERMINAL_BYTES,
                       "stop_grace_ms": STOP_GRACE_MS, "relay_queue_bytes": QUEUE_BYTES,
                       "producer_claim_bytes": CLAIM_BYTES},
            "report_charged_bytes": self.charged,
            "channels": {name: channel.record(name) for name, channel in self.channels.items()},
            "sink_verification": self.sink_verification, "usage": self.usage(),
            "cleanup": {
                "producer_reaped": cleanup(None if self.process is None else status is not None),
                "domain_empty": cleanup(None),
                "artifacts_removed": cleanup(self.artifacts_removed),
            },
        }

    def send_terminal(self):
        if self.status_socket is None:
            return
        record = self.record()
        status = record["channels"]["status"]
        status.update(complete=True, source_total_known=True, truncated=False)
        previous = -1
        while True:
            packet = wire(record)
            size = len(packet)
            if size == previous:
                break
            previous = size
            for key in ("observed_bytes", "accepted_bytes", "written_bytes"):
                status[key] = size
        if size > TERMINAL_BYTES:
            # Required fields never become an unbounded "emergency" packet.
            self.facade = 4 if self.invocation == "entrypoint" else 1
            return
        # The helper's stream writer cannot send a packet atomically. This
        # adapter attempts exactly one packet within the reserved terminal P.
        grace = Deadline(self.stall_seconds * 1000, self.stall_seconds * 1000)
        with selectors.DefaultSelector() as selector:
            selector.register(self.status_socket, selectors.EVENT_WRITE)
            while True:
                if grace.expired():
                    self.facade = 4 if self.invocation == "entrypoint" else 1
                    return
                try:
                    count = self.send_ready(self.status_socket.fileno(), packet, socket_endpoint=True)
                except (BlockingIOError, InterruptedError):
                    selector.select(min(0.05, grace.remaining_wall_ms() / 1000))
                    continue
                except OSError:
                    self.facade = 4 if self.invocation == "entrypoint" else 1
                    return
                if count != size:
                    self.facade = 4 if self.invocation == "entrypoint" else 1
                return

    def run(self):
        selected = None
        try:
            self.admit()
            if self.failure is None:
                self.capture()
                selected = self.choose_report()
        except Exception:
            self.latch("harness_internal_error", {})
            if self.process is not None and self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=STOP_GRACE_MS / 1000)
            if self.validator is not None and self.validator.poll() is None:
                self.validator.kill()
                self.validator.wait(timeout=STOP_GRACE_MS / 1000)
            if self.validator is not None:
                status = self.validator.returncode
                self.sink_verification["exit_status"] = status if status >= 0 else None
                self.sink_verification["signal"] = -status if status < 0 else None
        try:
            self.publish(selected)
        except Exception:
            self.latch("harness_internal_error", {})
        self.close_data()
        self.send_terminal()
        if self.status_socket is not None:
            try:
                self.status_socket.close()
                if self.status_fd not in self.external_closed:
                    os.close(self.status_fd)
            except OSError:
                self.facade = 4 if self.invocation == "entrypoint" else 1
        return self.facade


def main(argv=None):
    supervisor = Supervisor(sys.argv[1:] if argv is None else argv)
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)

    def cancel(signum, frame):
        supervisor.cancelled = True

    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGINT, cancel)
    return supervisor.run()


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--_validate-candidate":
        sys.exit(validation_worker(int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]))
    sys.exit(main())
