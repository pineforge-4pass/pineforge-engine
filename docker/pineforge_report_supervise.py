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
attempt_id, test_only_unisolated_process_fixture=true, and optional grants.
It runs controlled process fixtures without claiming the deployed OS boundary.
It must never be supplied by a billed/production launcher. The cleanup receipt
leaves domain_empty unavailable; a PID or a reaped child is no domain proof.
A delivered test fixture therefore has complete=false, failure=null and
exit_class=resource_refusal, with the runtime-refusal facade exit 1 (4 for the
entrypoint). Only measured-true domain_empty can permit complete=true.

the supervised report channel does not forward the host run-quota marker; it refuses to start when a PF_RUN_QUOTA_ variable is set.
Empty values count as set. This policy refusal follows control-grammar parsing,
including for the TEST adapter, and precedes host attestation and child launch.

Unset and zero R/D/W/P requests impose no limit. A positive host grant is an
optional constraint: omitted/zero requests resolve to it, positive requests
above it are refused without clamping. An absent or zero host grant means
unlimited, not a policy refusal. limits.*.effective is null for an unlimited
dimension. Accounting continues and all polls use at most 50 ms slices.
--post-exit-drain-ms (PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS) bounds the diagnostic
and receipt drains after their owners exit. Its default is 1000 ms; zero means
unlimited. --stop-grace-ms (PINEFORGE_CHANNEL_STOP_GRACE_MS) bounds the
TERM-to-KILL cleanup after a stop request. Its default is also 1000 ms; zero
means unlimited, so S waits for the owned group to exit and sends no KILL after
TERM. Both grace parameters accept only unsigned decimal integers in
0..9007199254740990 and report requested/granted/effective, with effective=null
for unlimited. Their one grant rule is: operator 0 means unlimited; an absent
operator value takes the 1000 ms default; an absent or zero host grant imposes
no host limit, so that default still applies when the operator gave no value; a
positive host grant makes an absent operator value take the grant, refuses a
positive operator value above it, and refuses operator 0 as exceeding it.
A leaked descendant can outlive its parent and keep an output pipe open. Drain
expiry refuses that drain with report_channel_failed and names
--post-exit-drain-ms in terminal diagnostics. Stop-grace expiry names
--stop-grace-ms in diagnostics. Output progress never renews either interval.
The terminal record carries diagnostics as text strings cut to at most 1,024
UTF-8 bytes.

S counts/hashes K and relays through one 64 KiB queue to the real finalizer V.
V receives an explicit environment containing PINEFORGE_RUN_JSON_PROFILE.
An exact producer --profile (either argument form) overrides the environment
default, which is unjailed when absent. Conflicting repeated profiles, missing
values, abbreviations and invalid effective profiles refuse before launch.
Only the explicit TEST adapter additionally admits its observer environment.
Unjailed V uses FILE mode and a private target. Jailed V uses --out - with a
positive --max-bytes B, stdin for the relay, stdout for provisional sink bytes,
and a distinct receipt pipe on fd 2; diagnostics never share binary stdout.
S buffers at most B stdout bytes in memory and publishes only after V exits 0,
receipt completion and independent K/buffer length and hash agreement.
--planned-sink-bytes (PINEFORGE_REPORT_PLANNED_SINK_BYTES), or the attestation's
planned_sink_bytes, supplies B. Unset or zero means no plan. Under jailed, B
otherwise derives from positive effective R; without either, admission refuses
and names --planned-sink-bytes. On a jailed profile the arena is fixed, so an
unlimited sink has no meaning there. Unjailed with unlimited R retains FILE
mode without --max-bytes, including when a sink size was planned.
S publishes the verified sink to N before R. N stays solely in S.
Named sinks and the automatic internal snapshot handoff remain fail-closed.
Until that handoff exists, supervised execution of a sink-capable module needs
an external sink fd; its full projection is unavailable without one.
A failure line may additionally carry cause only on strategy_create_failed:
exactly {"code":"outputs_limit","args":{"max":N,"reason":"too_many_series"}},
where N is an integer >= 1 and both code and args pass the installed catalog.
It remains a producer claim; the parsed claim is re-serialised in the terminal
record, while the report bytes stay relayed intact. A claim exceeding 8 KiB is
omitted from the terminal record under the existing omission rule.
The deadline module has no cancellation callback. S uses its own nonblocking
I/O loops around the same absolute Deadline, checking host cancellation between
poll slices of at most 50 ms. Cancellation stops data publication; M gets one
immediate nonblocking attempt with the latched outcome, without waiting for a
stalled status reader. Metered reads preserve every observed chunk.
"""
from __future__ import annotations

import argparse
import array
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
import termios
import time

from pineforge_report_deadlines import (
    Deadline, DeadlineError, close_once,
)


CONTRACT = "supervised/v1"
RUN_QUOTA_PREFIX = "PF_RUN_QUOTA_"
MAX_INTEGER = (1 << 53) - 2
INT_MAX = (1 << 31) - 1
TERMINAL_BYTES = 65536
QUEUE_BYTES = 65536
CLAIM_BYTES = 8192
DEFAULT_POST_EXIT_DRAIN_MS = 1000
DEFAULT_STOP_GRACE_MILLISECONDS = 1000
DIMENSIONS = ("report_bytes", "diagnostic_bytes", "wall_ms", "stall_ms",
              "post_exit_drain_ms", "stop_grace_ms")
CEILINGS = {
    "--report-max-bytes": ("PINEFORGE_REPORT_MAX_BYTES", "report_bytes"),
    "--diagnostic-max-bytes": ("PINEFORGE_DIAGNOSTIC_MAX_BYTES", "diagnostic_bytes"),
    "--channel-wall-ms": ("PINEFORGE_CHANNEL_WALL_MS", "wall_ms"),
    "--channel-stall-ms": ("PINEFORGE_CHANNEL_STALL_MS", "stall_ms"),
    "--post-exit-drain-ms": ("PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS", "post_exit_drain_ms"),
    "--stop-grace-ms": ("PINEFORGE_CHANNEL_STOP_GRACE_MS", "stop_grace_ms"),
}
GRACE_DEFAULTS = {
    "post_exit_drain_ms": DEFAULT_POST_EXIT_DRAIN_MS,
    "stop_grace_ms": DEFAULT_STOP_GRACE_MILLISECONDS,
}
CONTROLS = {
    "--report-channel": "PINEFORGE_REPORT_CHANNEL",
    "--terminal-status-fd": "PINEFORGE_TERMINAL_STATUS_FD",
    "--attempt-id": "PINEFORGE_REPORT_ATTEMPT_ID",
    "--host-attestation-fd": "PINEFORGE_REPORT_HOST_ATTESTATION_FD",
    "--test-only-host-attestation-fd": None,
    "--planned-sink-bytes": "PINEFORGE_REPORT_PLANNED_SINK_BYTES",
}
NEW_OPTIONS = frozenset((*CEILINGS, *CONTROLS))
CATALOG = Path(__file__).with_name("run_failure_codes.json")


def wire(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("utf-8") + b"\n"


def diagnostic_text(value):
    raw = str(value).encode("utf-8")[:1024]
    return raw.decode("utf-8", "ignore")


class UsageError(Exception):
    pass


class ProtocolError(Exception):
    pass


class IncompleteJSON(ProtocolError):
    pass


class Cancelled(Exception):
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


def effective_profile(command, environ):
    """Resolve the runner's CLI-over-environment profile before any launch."""
    profiles = []
    index = 0
    while index < len(command):
        token = command[index]
        if token == "--":
            break
        name, equals, suffix = token.partition("=")
        if name != "--profile" and name.startswith("--") and "--profile".startswith(name):
            raise UsageError("ambiguous producer --profile abbreviation; use --profile")
        if name == "--profile":
            if not equals:
                index += 1
                if index == len(command):
                    raise UsageError("ambiguous producer --profile: missing value")
            profile = suffix if equals else command[index]
            if profile not in ("unjailed", "jailed"):
                raise UsageError("invalid producer --profile; expected unjailed or jailed")
            profiles.append(profile)
        index += 1
    if len(set(profiles)) > 1:
        raise UsageError("ambiguous producer --profile: conflicting repeated values")
    profile = profiles[-1] if profiles else environ.get("PINEFORGE_RUN_JSON_PROFILE", "unjailed")
    if profile not in ("unjailed", "jailed"):
        raise UsageError("PINEFORGE_RUN_JSON_PROFILE must be unjailed or jailed")
    return profile


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

    def value(self, depth=0, path=()):
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
                child = self.value(depth + 1, path + (key,) if depth < 2 else ())
                if depth <= 1 or path == ("cause", "args"):
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
                    "arena_bytes", "bars", "strategy_started", "cause"}:
                raise ProtocolError("extra failure-object member")
            cause = None
            if "cause" in fields:
                node = fields["cause"]
                if (fields.get("code") != Value("string", "strategy_create_failed")
                        or node.kind != "object" or node.value.keys() != {"code", "args"}):
                    raise ProtocolError("invalid failure cause")
                cause_code, cause_args = node.value["code"], node.value["args"]
                if cause_code.kind != "string" or cause_args.kind != "object":
                    raise ProtocolError("invalid failure cause code or args")
                typed_args(catalog, cause_code.value, cause_args.value)
                values = cause_args.value
                if (cause_code.value != "outputs_limit" or values.keys() != {"max", "reason"}
                        or values["max"].kind != "integer" or values["max"].value is None
                        or values["max"].value < 1
                        or values["reason"] != Value("string", "too_many_series")):
                    raise ProtocolError("invalid operator series-limit cause")
                cause = {"code": cause_code.value,
                         "args": {name: value.value for name, value in values.items()}}
            claim, omitted = None, False
            if "code" in fields:
                code, args = fields["code"], fields["args"]
                if code.kind != "string" or args.kind != "object":
                    raise ProtocolError("invalid failure code or args")
                typed_args(catalog, code.value, args.value)
                omitted = any(value.oversized for value in args.value.values())
                values = {name: value.value for name, value in args.value.items()}
                proposed = {"code": code.value, "args": values,
                            **({"cause": cause} if cause is not None else {}),
                            "authority": "producer"}
                if not omitted:
                    omitted = len(wire(proposed)) - 1 > CLAIM_BYTES
                if not omitted:
                    claim = proposed
            return True, False, True, claim, omitted
        if "code" in fields or "args" in fields or "cause" in fields:
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


def remaining_bytes(limit, used):
    return None if limit is None else limit - used


def read_size(allowance, room=QUEUE_BYTES):
    return room if allowance is None else min(room, allowance + 1)


class GroupStop:
    """Stop owned sessions even after their leaders have been reaped."""
    def __init__(self, grace_ms, on_expire=None):
        self.grace_ms = grace_ms
        self.on_expire = on_expire
        self.ends = {}

    def request(self, process, now):
        if process is None or process in self.ends:
            return
        self.ends[process] = (
            None if self.grace_ms is None else now + self.grace_ms / 1000)
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            self.ends[process] = None

    def advance(self, now):
        for process, end in self.ends.items():
            if end is not None and now >= end:
                if self.on_expire is not None:
                    self.on_expire()
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.ends[process] = None

    def pending(self):
        return any(end is not None for end in self.ends.values())

    def finish(self):
        while self.pending():
            self.advance(time.monotonic())
            if self.pending():
                time.sleep(min(0.05, max(
                    0, min(end for end in self.ends.values() if end is not None)
                    - time.monotonic())))


class Supervisor:
    def __init__(self, argv):
        self.argv, self.started = list(argv), time.monotonic()
        self.catalog = json.loads(CATALOG.read_text(encoding="utf-8"))["codes"]
        self.options = None
        self.failure = None
        self.diagnostics = []
        self.facade = 0
        self.invocation = (
            "entrypoint" if early_value(argv, "--invocation") == "entrypoint" else "run_json")
        self.attempt = early_value(argv, "--attempt-id", "PINEFORGE_REPORT_ATTEMPT_ID")
        self.limits = {name: {"requested": None, "granted": None, "effective": None}
                       for name in DIMENSIONS}
        self.channels = {
            "candidate": Channel(None),
            "report": Channel(None),
            "sink": Channel(None, absent=True),
            "diagnostic": Channel(None, absent=True),
            "status": Channel(TERMINAL_BYTES),
        }
        self.status_socket = None
        self.status_fd = None
        self.sink_fd = None
        self.planned_sink_bytes = None
        self.planned_sink_request = None
        self.sink_bound = None
        self.profile = None
        self.test_adapter_admitted = False
        self.sink_buffer = None
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
        self.domain_empty = None
        self.operation = "report"
        self.stall_seconds = math.inf
        self.stall_ms = None
        self.deadline = Deadline(None, None)
        self.borrowed_flags = {}
        self.diagnostic_by_stage = {stage: 0 for stage in ("run", "finalize")}
        self.stop_grace_diagnostic_emitted = False
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

    def nonblocking(self, fd):
        if fd not in self.borrowed_flags:
            self.borrowed_flags[fd] = fcntl.fcntl(fd, fcntl.F_GETFL)
        os.set_blocking(fd, False)

    def restore_flags(self, fd):
        if fd not in self.borrowed_flags:
            return
        flags = self.borrowed_flags[fd]
        fcntl.fcntl(fd, fcntl.F_SETFL, flags)
        del self.borrowed_flags[fd]

    def set_deadline(self, wall_ms, stall_ms):
        # Admission may add a host constraint, but never a fresh wall interval.
        remaining = None if not wall_ms else wall_ms - (time.monotonic() - self.started) * 1000
        if remaining is not None and remaining <= 0:
            self.latch("run_deadline_exceeded", {"stage": "prepare"})
            return False
        self.stall_seconds = stall_ms / 1000 if stall_ms else math.inf
        self.stall_ms = stall_ms
        self.deadline = Deadline(remaining, stall_ms)
        return True

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

    def stop_grace_expired(self, context="TERM-to-KILL cleanup"):
        if not self.stop_grace_diagnostic_emitted:
            self.diagnostics.append(diagnostic_text(
                "--stop-grace-ms (PINEFORGE_CHANNEL_STOP_GRACE_MS) expired during "
                + context + "."))
            self.stop_grace_diagnostic_emitted = True

    def wait_after_stop(self, process, name):
        if process is None:
            return
        grace_ms = self.limits["stop_grace_ms"]["effective"]
        try:
            if grace_ms is None:
                process.wait()
            else:
                process.wait(timeout=grace_ms / 1000)
        except subprocess.TimeoutExpired:
            self.stop_grace_expired("the " + name + " wait")
            self.diagnostics.append(diagnostic_text(
                "the " + name + " survived SIGKILL and the stop grace"))
            self.latch("harness_internal_error", {})

    def check_cancelled(self):
        if self.cancelled:
            self.latch("run_aborted", {})
            raise Cancelled()

    def stage(self):
        return "finalize" if (self.native_eof or (
            self.sink_fd is None and self.process is not None
            and self.process.poll() is not None)) else "run"

    def attestation_read(self, fd, size):
        # bounded_read has no cancel callback and can keep polling internally
        # forever with unlimited W/P. Keep that pending interval in S instead.
        pending_read = Deadline(None, self.stall_ms)
        with selectors.DefaultSelector() as selector:
            selector.register(fd, selectors.EVENT_READ)
            while True:
                self.check_cancelled()
                if self.deadline.expired():
                    raise DeadlineError("wall", "prepare", 0)
                if pending_read.stall_remaining_ms() <= 0:
                    raise DeadlineError("stall", "prepare", 0)
                try:
                    return self.read_ready(fd, size, "prepare")
                except (BlockingIOError, InterruptedError):
                    selector.select(min(0.05, self.deadline.remaining_wall_ms() / 1000,
                                        pending_read.stall_remaining_ms() / 1000))

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
                chunk = self.attestation_read(
                    fd, min(QUEUE_BYTES, TERMINAL_BYTES + 1 - len(data)))
                if not chunk:
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
            grants = document.get("grants", {})
            if not isinstance(grants, dict) or grants.keys() - set(DIMENSIONS) or any(
                    type(value) is not int or not 0 <= value <= MAX_INTEGER
                    for value in grants.values()):
                raise UsageError("invalid host grant")
            planned = document.get("planned_sink_bytes")
            if planned is not None and (type(planned) is not int or not 0 <= planned <= MAX_INTEGER):
                raise UsageError("invalid planned sink allowance")
            self.planned_sink_bytes = planned or None
            self.test_adapter_admitted = True
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
                self.limits[name]["effective"] = (
                    GRACE_DEFAULTS[name]
                    if name in GRACE_DEFAULTS and text is None
                    else self.limits[name]["requested"] or None)
            if options.planned_sink_bytes is not None:
                self.planned_sink_request = decimal(options.planned_sink_bytes)
            for name in ("candidate", "report"):
                self.channels[name].limit = self.limits["report_bytes"]["effective"]
            self.channels["diagnostic"].limit = self.limits["diagnostic_bytes"]["effective"]
            if not self.set_deadline(self.limits["wall_ms"]["effective"],
                                     self.limits["stall_ms"]["effective"]):
                return
            tokens = options.command
            self.profile = effective_profile(tokens, os.environ)
            names = {token.partition("=")[0] for token in tokens}
            if names & {"--help", "-h", "--run-phase-fd"} or "csv2bin" in tokens:
                raise UsageError("excluded producer protocol")
            quota_markers = sorted(name for name in os.environ
                                   if name.startswith(RUN_QUOTA_PREFIX))
            if quota_markers:
                for name in quota_markers:
                    self.diagnostics.append(diagnostic_text(
                        "the supervised report channel does not forward the host run-quota marker: "
                        + name + " is set (unsupported combination)"))
                self.latch("run_isolation_unavailable", {"reason": "policy"})
                return
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
            above_grant = set()
            for name in DIMENSIONS:
                grant = grants.get(name)
                cell = self.limits[name]
                requested = cell["requested"]
                cell["granted"] = grant
                if name in GRACE_DEFAULTS:
                    if grant and requested == 0:
                        cell["effective"] = None
                        above_grant.add(name)
                    elif grant and requested is not None and requested > grant:
                        cell["effective"] = None
                        above_grant.add(name)
                    elif requested is None:
                        cell["effective"] = grant or GRACE_DEFAULTS[name]
                    else:
                        cell["effective"] = requested or None
                else:
                    cell["effective"] = requested or grant or None
            # A rejected request is not silently recorded as an accepted clamp.
            # Its failure publication must nevertheless obey the host grant.
            report_limit = self.limits["report_bytes"][
                "granted" if "report_bytes" in above_grant else "effective"]
            for name in ("candidate", "report"):
                self.channels[name].limit = report_limit
            self.channels["diagnostic"].limit = self.limits["diagnostic_bytes"][
                "granted" if "diagnostic_bytes" in above_grant else "effective"]
            if above_grant:
                raise UsageError("positive ceiling exceeds host grant")
            if self.planned_sink_request:
                if (self.planned_sink_bytes is not None
                        and self.planned_sink_bytes != self.planned_sink_request):
                    raise UsageError("--planned-sink-bytes conflicts with attested planned_sink_bytes")
                self.planned_sink_bytes = self.planned_sink_request
            if (report_limit is not None and self.planned_sink_bytes is not None
                    and self.planned_sink_bytes > report_limit):
                self.latch("report_budget_exceeded", {
                    "channel": "aggregate", "cap_bytes": report_limit,
                    "used_bytes": 0, "requested_bytes": self.planned_sink_bytes})
                return
            # A named target and the internal snapshot protocol need their own
            # handoff/publication adapters. Never let U open either target.
            if (options.report_sink is not None
                    or names & {"--report-sink", "--report-sink-fd", "--report-replay-sink"}):
                self.latch("run_isolation_unavailable", {"reason": "descriptor"})
                return
            if not self.set_deadline(self.limits["wall_ms"]["effective"],
                                     self.limits["stall_ms"]["effective"]):
                return
            if self.sink_fd is not None:
                if self.profile == "jailed":
                    self.sink_bound = self.planned_sink_bytes or report_limit
                else:
                    self.sink_bound = (None if report_limit is None
                                       else self.planned_sink_bytes or report_limit)
                if self.profile == "jailed" and self.sink_bound is None:
                    raise UsageError(
                        "jailed sink requires --planned-sink-bytes "
                        "(PINEFORGE_REPORT_PLANNED_SINK_BYTES or attested planned_sink_bytes), "
                        "or positive --report-max-bytes")
                self.channels["sink"] = Channel(self.sink_bound)
                self.sink_verification = {
                    "exit_status": None, "signal": None, "complete": False,
                    "counts": {tag: None for tag in "ETOVKXH"},
                    "sha256": None, "finalizer_version": None}
        except (OSError, UsageError, ValueError) as error:
            if isinstance(error, UsageError):
                self.diagnostics.append(diagnostic_text(error))
            self.latch("run_request_invalid", {"option": "arguments"}, usage=True)

    def validator_environment(self):
        environment = {"PINEFORGE_RUN_JSON_PROFILE": self.profile}
        if self.test_adapter_admitted:
            # Only the explicitly unisolated TEST adapter admits instrumentation.
            # No production environment, path search or Python path is inherited.
            for name in ("LD_PRELOAD", "PF_REPORT_TEST_TRACE", "PF_REPORT_TEST_PAUSE_PROBE",
                         "PF_REPORT_TEST_STDOUT_AUDIT", "PF_REPORT_TEST_HOLD_RECEIPT",
                         "PF_REPORT_TEST_FAIL_AFTER_RECEIPT", "PF_REPORT_TEST_CORRUPT_STDOUT",
                         "PF_REPORT_TEST_EXCESS_STDOUT", "PF_REPORT_TEST_IGNORE_TERM",
                         "PF_REPORT_TEST_HOLD_OVERFLOW", "PF_REPORT_TEST_OUTPUT_AFTER_TERM"):
                if name in os.environ:
                    environment[name] = os.environ[name]
        return environment

    def drain_expired(self, channel):
        self.diagnostics.append(diagnostic_text(
            "--post-exit-drain-ms (PINEFORGE_CHANNEL_POST_EXIT_DRAIN_MS) ended the "
            + ("diagnostic" if channel == "diagnostic" else "validator output/receipt")
            + " drain: a descendant can outlive its parent and keep an output pipe open."))
        self.latch("report_channel_failed", {
            "channel": channel, "reason": "io" if channel == "diagnostic" else "protocol"})

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
        # The helper's synchronous socket loop has no cancel callback. Use
        # the same raw step for pipes and streams, with cancellation in S.
        # The descriptor is nonblocking, W never changes, and
        # only a positive syscall return renews this pending operation's P.
        pending = Deadline(None, self.stall_ms)
        remaining = memoryview(data)
        with selectors.DefaultSelector() as selector:
            selector.register(fd, selectors.EVENT_WRITE)
            while remaining:
                self.check_cancelled()
                if self.deadline.expired():
                    raise TransferError(channel, "deadline", wall=True)
                if pending.stall_remaining_ms() <= 0:
                    raise TransferError(channel, "deadline")
                try:
                    count = self.write_ready(fd, remaining[:QUEUE_BYTES])
                except (BlockingIOError, InterruptedError):
                    selector.select(min(0.05, self.deadline.remaining_wall_ms() / 1000,
                                        pending.stall_remaining_ms() / 1000))
                    continue
                except OSError as error:
                    self.check_cancelled()
                    reason = "peer_closed" if error.errno in (
                        errno.EPIPE, errno.ECONNRESET, errno.ENOTCONN) else "io"
                    raise TransferError(channel, reason) from error
                if count <= 0:
                    self.check_cancelled()
                    raise TransferError(channel, "io")
                self.channels[channel].write(remaining[:count])
                self.publication_started |= channel in ("report", "sink") and count > 0
                remaining = remaining[count:]
                pending.note_progress(count)
        self.check_cancelled()
        if self.deadline.expired():
            raise TransferError(channel, "deadline", wall=True)

    def sink_hashes(self):
        """Hash the retained wire independently of V's receipt."""
        whole, logical = hashlib.sha256(), hashlib.sha256()
        if self.sink_buffer is not None:
            length = len(self.sink_buffer)
            if not self.sink_buffer.endswith(b"\n"):
                raise ProtocolError("incomplete buffered sink")
            prefix_end = self.sink_buffer.rfind(b"\n", 0, length - 1) + 1
            view = memoryview(self.sink_buffer)
            for offset in range(0, length, QUEUE_BYTES):
                self.check_cancelled()
                if self.deadline.expired():
                    self.latch("run_deadline_exceeded", {"stage": "finalize"})
                    raise ProtocolError("wall expired during buffered-sink verification")
                chunk = view[offset:offset + QUEUE_BYTES]
                whole.update(chunk)
                logical.update(chunk[:max(0, min(len(chunk), prefix_end - offset))])
            return length, whole.hexdigest(), logical.hexdigest()
        with open(self.sink_target, "rb") as source:
            length = os.fstat(source.fileno()).st_size
            source.seek(max(0, length - 1))
            if source.read(1) != b"\n":
                raise ProtocolError("incomplete retained sink")
            position, prefix_end = max(0, length - 1), 0
            while position:
                self.check_cancelled()
                start = max(0, position - QUEUE_BYTES)
                source.seek(start)
                previous = source.read(position - start).rfind(b"\n")
                if previous >= 0:
                    prefix_end = start + previous + 1
                    break
                position = start
            source.seek(0)
            offset = 0
            while chunk := source.read(QUEUE_BYTES):
                self.check_cancelled()
                if self.deadline.expired():
                    self.latch("run_deadline_exceeded", {"stage": "finalize"})
                    raise ProtocolError("wall expired during retained-sink verification")
                whole.update(chunk)
                logical.update(chunk[:max(0, min(len(chunk), prefix_end - offset))])
                offset += len(chunk)
            return length, whole.hexdigest(), logical.hexdigest()

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
                    or receipt["sink_bytes"] < 0
                    or (self.sink_bound is not None
                        and receipt["sink_bytes"] > self.sink_bound + 1)
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
                    self.sink_bound is None or receipt["reason"] != "sink_exhausted"
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
                length, wire_hash, logical_hash = self.sink_hashes()
                if (not self.native_eof or sink.truncated
                        or receipt["sink_bytes"] != sink.accepted
                        or length != sink.accepted
                        or wire_hash != sink.accepted_hash.hexdigest()
                        or logical_hash != digest):
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
        finally:
            if status != 0 or self.failure is not None:
                self.sink_buffer = None

    def capture(self):
        candidate_read, candidate_write = pipe()
        diagnostic_read, diagnostic_write = pipe()
        owned = {candidate_read, candidate_write, diagnostic_read, diagnostic_write}
        readers = {candidate_read: "candidate", diagnostic_read: "diagnostic"}
        native_read = native_write = relay_fd = validator_read = receipt_read = None
        sink_output_read = None
        environment = dict(os.environ)
        for name in tuple(environment):
            if name.startswith(("PINEFORGE_REPORT_", "PINEFORGE_TERMINAL_",
                                "PINEFORGE_DIAGNOSTIC_", "PINEFORGE_CHANNEL_")):
                environment.pop(name)
        environment["PINEFORGE_REPORT_PRODUCER"] = CONTRACT
        environment["PINEFORGE_REPORT_CANDIDATE_FD"] = str(candidate_write)
        environment["PINEFORGE_RUN_JSON_PROFILE"] = self.profile
        self.spool = tempfile.TemporaryFile(mode="w+b")
        self.artifacts_removed = False
        diagnostic_pending, relay_pending, receipt = bytearray(), bytearray(), bytearray()
        diagnostic_since = None
        candidate_waiting = False
        receipt_eof = validator_done = False
        stop_grace_ms = self.limits["stop_grace_ms"]["effective"]
        stopping = GroupStop(stop_grace_ms, self.stop_grace_expired)
        diagnostic_drain_end = receipt_drain_end = overflow_finish_end = None
        diagnostic_tail_remaining = None
        drain_ms = self.limits["post_exit_drain_ms"]["effective"]
        drain_seconds = drain_ms / 1000 if drain_ms else math.inf

        def release(fd):
            if fd in owned:
                owned.remove(fd)
                if not close_once(fd):
                    name = readers.get(fd, "sink")
                    if name in ("receipt", "sink_output"):
                        name = "sink"
                    self.latch("report_channel_failed", {"channel": name, "reason": "io"})

        try:
            if self.sink_fd is not None:
                native_read, native_write = pipe()
                receipt_read, receipt_write = pipe()
                jailed = self.profile == "jailed"
                if jailed:
                    validator_read, relay_fd = pipe()
                    sink_output_read, sink_output_write = pipe()
                    owned.update((sink_output_read, sink_output_write))
                    readers[sink_output_read] = "sink_output"
                    self.sink_buffer = bytearray()
                else:
                    relay_fd, validator_read = stream_pair()
                    self.sink_directory = tempfile.TemporaryDirectory(prefix="pf-supervised-sink-")
                    self.sink_target = str(Path(self.sink_directory.name) / "verified.sink")
                owned.update((native_read, native_write, relay_fd, validator_read,
                              receipt_read, receipt_write))
                readers.update({native_read: "sink", receipt_read: "receipt"})
                environment["PINEFORGE_REPORT_NATIVE_SINK_FD"] = str(native_write)
                validator_command = [
                    sys.executable, "-B", str(Path(__file__).with_name("pineforge_sink_finalize.py")),
                    "--in-fd", "0" if jailed else str(validator_read),
                    "--out", "-" if jailed else self.sink_target,
                    "--receipt-fd", "2"]
                if self.sink_bound is not None:
                    validator_command += ["--max-bytes", str(self.sink_bound)]
                self.validator = subprocess.Popen(
                    validator_command,
                    stdin=validator_read if jailed else subprocess.DEVNULL,
                    stdout=sink_output_write if jailed else diagnostic_write, stderr=receipt_write,
                    close_fds=True, pass_fds=() if jailed else (validator_read,),
                    env=self.validator_environment(), start_new_session=True)
                release(validator_read)
                release(receipt_write)
                if jailed:
                    release(sink_output_write)
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
                        self.validator is not None and self.validator.poll() is None) or stopping.pending():
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
                    wall_remaining = self.deadline.remaining_wall_ms() / 1000
                    events = selector.select(min(0.05, wall_remaining) if wall_remaining > 0 else 0.05)
                    ready = {key.fd for key, _ in events}
                    if self.cancelled:
                        self.latch("run_aborted", {})

                    # R19: diagnostic budget, native budget, candidate budget,
                    # independent validator/transport evidence, then W.
                    if diagnostic_read in ready and diagnostic_read in readers:
                        allowance = remaining_bytes(diagnostic.limit, diagnostic.accepted)
                        room = QUEUE_BYTES - len(diagnostic_pending)
                        if (diagnostic_drain_end is not None
                                and time.monotonic() >= diagnostic_drain_end
                                and diagnostic_tail_remaining):
                            room = min(room, diagnostic_tail_remaining)
                        data = read(diagnostic_read, read_size(allowance, room), "diagnostic")
                        if data == b"":
                            diagnostic.known = True
                            end_read(diagnostic_read)
                        elif data:
                            if diagnostic_tail_remaining is not None:
                                diagnostic_tail_remaining = max(
                                    0, diagnostic_tail_remaining - len(data))
                            diagnostic.observed += len(data)
                            accepted = data[:allowance]
                            diagnostic.accept(accepted)
                            self.diagnostic_by_stage[self.stage()] += len(accepted)
                            if not diagnostic_pending and accepted:
                                diagnostic_since = time.monotonic()
                            diagnostic_pending.extend(accepted)
                            if allowance is not None and len(data) > allowance:
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
                        allowance = remaining_bytes(sink.limit, sink.accepted)
                        native_progress = True
                        data = read(native_read, read_size(allowance), "sink")
                        if data == b"":
                            sink.known = self.native_eof = True
                            self.finalize_started = time.monotonic()
                            self.channels["candidate"].limit = remaining_bytes(
                                self.channels["report"].limit, sink.accepted)
                            end_read(native_read)
                        elif data:
                            native_progress = True
                            sink.observed += len(data)
                            accepted = data[:allowance]
                            sink.accept(accepted)
                            self.charged += len(accepted)
                            self.channels["candidate"].limit = remaining_bytes(
                                self.channels["report"].limit, sink.accepted)
                            # Unlike deadlines.relay(), V must receive the one
                            # probe too. It is queued, never approved/charged.
                            relay_pending.extend(data)
                            if allowance is not None and len(data) > allowance:
                                sink.truncated = self.sink_overflow = True
                                overflow_finish_end = (
                                    None if stop_grace_ms is None
                                    else time.monotonic() + stop_grace_ms / 1000)
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

                    if sink_output_read in ready and sink_output_read in readers:
                        room = self.sink_bound - len(self.sink_buffer)
                        data = read(sink_output_read, min(QUEUE_BYTES, room + 1), "sink")
                        if data == b"":
                            end_read(sink_output_read)
                        elif data:
                            if len(data) > room:
                                # The probe is never retained beyond the certified B.
                                self.latch("report_channel_failed", {
                                    "channel": "sink", "reason": "protocol"})
                                self.sink_buffer.clear()
                                end_read(sink_output_read)
                            else:
                                self.sink_buffer.extend(data)

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
                            allowance = remaining_bytes(candidate.limit, candidate.accepted)
                            data = read(candidate_read, read_size(allowance), "candidate")
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
                                if allowance is not None and len(data) > allowance:
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
                            and self.validator.poll() is not None and receipt_read not in readers
                            and sink_output_read not in readers):
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
                        stopping.request(self.process, now)
                        if not self.sink_overflow:
                            relay_pending.clear()
                            if relay_fd in owned:
                                unwatch(relay_fd)
                                release(relay_fd)
                            stopping.request(self.validator, now)
                        elif self.deadline.expired() or (
                                overflow_finish_end is not None
                                and now >= overflow_finish_end):
                            # Let V emit its existing overflow receipt first,
                            # then stop its whole session under the same grace.
                            relay_pending.clear()
                            if relay_fd in owned:
                                unwatch(relay_fd)
                                release(relay_fd)
                            stopping.request(self.validator, now)
                    if self.process.poll() is not None and self.run_finished is None:
                        self.run_finished = now
                        stopping.request(self.process, now)
                        if self.finalize_started is None and self.sink_fd is None:
                            self.finalize_started = now
                    validator_gone = self.validator is None or self.validator.poll() is not None
                    if self.validator is not None and validator_gone:
                        stopping.request(self.validator, now)
                        if receipt_drain_end is None:
                            receipt_drain_end = now + drain_seconds
                        if now >= receipt_drain_end and (
                                receipt_read in readers or sink_output_read in readers):
                            for fd in (receipt_read, sink_output_read):
                                if fd in readers:
                                    end_read(fd)
                            self.drain_expired("sink")
                    if self.run_finished is not None and (self.failure is not None or validator_gone):
                        if diagnostic_drain_end is None:
                            diagnostic_drain_end = now + drain_seconds
                            if diagnostic_read in readers:
                                queued = array.array("i", [0])
                                fcntl.ioctl(diagnostic_read, termios.FIONREAD, queued, True)
                                diagnostic_tail_remaining = queued[0]
                        if now >= diagnostic_drain_end:
                            # Bytes already queued at owner exit keep their P.
                            # A continuing descendant cannot renew the grace.
                            if diagnostic_read in readers and (
                                    self.failure is not None or not diagnostic_tail_remaining):
                                tail = read(diagnostic_read, 1, "diagnostic")
                                if tail == b"":
                                    diagnostic.known = True
                                else:
                                    diagnostic.observed += len(tail or b"")
                                    diagnostic.truncated = True
                                    self.drain_expired("diagnostic")
                                end_read(diagnostic_read)
                            if self.failure is not None and diagnostic_pending:
                                diagnostic.truncated = True
                                diagnostic_pending.clear()
                    stopping.advance(now)
                self.wait_after_stop(self.process, "producer")
                if self.validator is not None:
                    self.wait_after_stop(self.validator, "validator")
                    if self.validator.returncode is not None and not validator_done:
                        self.finish_sink(bytes(receipt), receipt_eof)
        finally:
            if self.failure is not None:
                self.sink_buffer = None
            for process in (self.process, self.validator):
                stopping.request(process, time.monotonic())
            stopping.finish()
            for process in (self.process, self.validator):
                self.wait_after_stop(process, "cleanup")
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
                        allowance = (remaining_bytes(channel.limit, channel.accepted)
                                     if key.data == "diagnostic"
                                     else TERMINAL_BYTES - len(data))
                        try:
                            chunk = self.read_ready(
                                key.fd, read_size(allowance), "finalize")
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
                            if allowance is not None and len(chunk) > allowance:
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
                self.wait_after_stop(worker, "trusted parser")
            self.finalize_finished = time.monotonic()
        finally:
            for fd in readers:
                os.close(fd)
            if worker.poll() is None:
                worker.kill()
                self.wait_after_stop(worker, "trusted parser cleanup")
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
        allowance = remaining_bytes(self.channels["report"].limit, self.charged)
        if allowance is not None and allowance < 1:
            return None
        pieces, length = [], 1  # The final LF is part of R.
        encoder = json.JSONEncoder(separators=(",", ":"), ensure_ascii=True, allow_nan=False)
        for text in encoder.iterencode(value):
            piece = text.encode("utf-8")
            if allowance is not None and len(piece) > allowance - length:
                return None
            pieces.append(piece)
            length += len(piece)
        pieces.append(b"\n")
        data = b"".join(pieces)
        self.charged += length
        return data

    def publish(self, selected):
        self.check_cancelled()
        if selected is None:
            selected = self.failure_document()
        if selected is None:
            return
        self.publish_started = time.monotonic()
        sink = self.channels["sink"]
        if self.sink_fd is not None and self.failure is None:
            try:
                self.nonblocking(self.sink_fd)
                if self.sink_buffer is not None:
                    view = memoryview(self.sink_buffer)
                    for offset in range(0, len(view), QUEUE_BYTES):
                        self.bounded_write(self.sink_fd, view[offset:offset + QUEUE_BYTES], "sink")
                else:
                    with open(self.sink_target, "rb") as source:
                        while data := source.read(QUEUE_BYTES):
                            self.bounded_write(self.sink_fd, data, "sink")
                sink.complete = (
                    self.sink_verification["complete"] and sink.known
                    and sink.written == sink.accepted and not sink.truncated)
                if not sink.complete:
                    raise TransferError("sink", "io")
            except Cancelled:
                sink.truncated = sink.written < sink.accepted
                self.publish_finished = time.monotonic()
                raise
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
        except Cancelled:
            report.truncated = report.written < report.accepted
            raise
        except TransferError as error:
            report.truncated = True
            self.io_failure(error)
        except Exception:
            report.truncated = report.written < report.accepted
            self.latch("harness_internal_error", {})
        finally:
            self.publish_finished = time.monotonic()

    def close_data(self):
        self.sink_buffer = None
        endpoints = [(1, "report"), (2, "diagnostic")]
        if self.sink_fd is not None:
            endpoints.append((self.sink_fd, "sink"))
        for fd, name in endpoints:
            if fd in self.external_closed:
                continue
            self.external_closed.add(fd)
            try:
                self.restore_flags(fd)
            except OSError:
                self.latch("report_channel_failed", {"channel": name, "reason": "io"})
                self.borrowed_flags.pop(fd, None)
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
                     (0 if self.sink_fd is not None else self.channels["candidate"].accepted),
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
                     self.channels["candidate"].accepted if self.sink_fd is not None else 0,
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
            "complete": (self.domain_empty is True and self.failure is None
                         and self.channels["report"].complete
                         and self.channels["candidate"].complete
                         and self.channels["diagnostic"].complete
                         and self.channels["sink"].complete),
            "exit_class": (self.exit_class if self.failure is not None else
                           "success" if self.domain_empty is True else "resource_refusal"),
            "failure": self.failure, "exit_status": self.facade,
            "diagnostics": self.diagnostics,
            "producer": {
                "stage": None if self.process is None else "run",
                "exit_status": status if status is not None and status >= 0 else None,
                "signal": -status if status is not None and status < 0 else None,
                "failure": self.producer_claim, "failure_claim_omitted": self.claim_omitted,
            },
            "limits": {**self.limits, "sink_bytes": self.sink_bound, "terminal_bytes": TERMINAL_BYTES,
                       "relay_queue_bytes": QUEUE_BYTES, "producer_claim_bytes": CLAIM_BYTES},
            "report_charged_bytes": self.charged,
            "channels": {name: channel.record(name) for name, channel in self.channels.items()},
            "sink_verification": self.sink_verification, "usage": self.usage(),
            "cleanup": {
                "producer_reaped": cleanup(None if self.process is None else status is not None),
                "domain_empty": cleanup(self.domain_empty),
                "artifacts_removed": cleanup(self.artifacts_removed),
            },
        }

    def terminal_packet(self):
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
        return packet, size

    def send_terminal(self):
        if self.status_socket is None:
            return
        cancelled_record = self.cancelled
        if cancelled_record:
            self.latch("run_aborted", {})
        packet, size = self.terminal_packet()
        if size > TERMINAL_BYTES:
            # Required fields never become an unbounded "emergency" packet.
            self.facade = 4 if self.invocation == "entrypoint" else 1
            return
        # The helper's stream writer cannot send a packet atomically. This
        # adapter attempts exactly one packet within the reserved terminal P.
        stall_ms = self.limits["stall_ms"]["effective"]
        grace = Deadline(stall_ms, stall_ms)
        with selectors.DefaultSelector() as selector:
            selector.register(self.status_socket, selectors.EVENT_WRITE)
            while True:
                if self.cancelled and not cancelled_record:
                    self.latch("run_aborted", {})
                    # No packet has been sent. Rebuild its length fixed point
                    # so an available M names cancellation, never stale success.
                    packet, size = self.terminal_packet()
                    cancelled_record = True
                    if size > TERMINAL_BYTES:
                        self.facade = 4 if self.invocation == "entrypoint" else 1
                        return
                if not self.cancelled and grace.expired():
                    self.facade = 4 if self.invocation == "entrypoint" else 1
                    return
                try:
                    count = self.send_ready(self.status_socket.fileno(), packet, socket_endpoint=True)
                except (BlockingIOError, InterruptedError):
                    if self.cancelled:
                        if not cancelled_record:
                            continue
                        self.facade = 4 if self.invocation == "entrypoint" else 1
                        return
                    selector.select(min(0.05, grace.remaining_wall_ms() / 1000))
                    continue
                except OSError:
                    self.facade = 4 if self.invocation == "entrypoint" else 1
                    return
                if count != size:
                    self.facade = 4 if self.invocation == "entrypoint" else 1
                return

    def run(self):
        try:
            # Save both descriptions before changing either: admission can
            # reject fd 1 and fd 2 aliases of the same open file description.
            for fd in (1, 2):
                try:
                    self.borrowed_flags[fd] = fcntl.fcntl(fd, fcntl.F_GETFL)
                except OSError:
                    pass  # Admission still diagnoses a closed data endpoint.
            for fd in self.borrowed_flags:
                try:
                    os.set_blocking(fd, False)
                except OSError:
                    self.latch("report_channel_failed", {
                        "channel": "report" if fd == 1 else "diagnostic", "reason": "setup"})
            self.run_channels()
        finally:
            for fd in tuple(self.borrowed_flags):
                try:
                    self.restore_flags(fd)
                except OSError:
                    self.latch("report_channel_failed", {
                        "channel": "report" if fd == 1 else
                        "diagnostic" if fd == 2 else "sink", "reason": "io"})
        return self.facade

    def run_channels(self):
        selected = None
        try:
            self.check_cancelled()
            self.admit()
            self.check_cancelled()
            if self.failure is None:
                self.capture()
                self.check_cancelled()
                selected = self.choose_report()
        except Cancelled:
            pass
        except Exception:
            self.latch("harness_internal_error", {})
            stopping = GroupStop(
                self.limits["stop_grace_ms"]["effective"], self.stop_grace_expired)
            for process in (self.process, self.validator):
                stopping.request(process, time.monotonic())
            stopping.finish()
            for process in (self.process, self.validator):
                self.wait_after_stop(process, "exception cleanup")
            if self.validator is not None:
                status = self.validator.returncode
                if status is not None:
                    self.sink_verification["exit_status"] = status if status >= 0 else None
                    self.sink_verification["signal"] = -status if status < 0 else None
        try:
            self.publish(selected)
        except Cancelled:
            pass
        except Exception:
            self.latch("harness_internal_error", {})
        self.close_data()
        if self.failure is None and self.domain_empty is not True:
            self.facade = 4 if self.invocation == "entrypoint" else 1
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
