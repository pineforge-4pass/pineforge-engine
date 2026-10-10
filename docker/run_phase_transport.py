"""Run-only phase transport writer.

Phase transport v1 (selected-window wire v1.3) tells the process that started a
run which phase the run has reached, over a channel the strategy cannot write
to, so that an external deadline is classified from the last phase handed over
and never from anything the strategy printed. One RunPhaseWriter serves one run
on one connection: the inherited descriptor named by --run-phase-fd N (N >= 3),
an AF_UNIX SOCK_STREAM socket the caller owns. stdout stays report-only and
stderr diagnostics-only; this module prints nothing.

The phases, in this order, each at most once, with no restart:

    preflight, execution, result_assembly, results_digest (optional),
    serialization, completed

advance(phase) enters a phase. The caller starts that phase's work only after
advance() returned; an advance() that raised means the phase was not entered
and its work must not start. A boundary is one record: UTF-8 NDJSON, one compact
object per LF, at most 1024 bytes with the LF, at most 6 records per run:

    {"record":"run_phase","version":1,"sequence":1,"scope":"run","trial_id":null,"phase":"preflight","elapsed_ms":0}

sequence counts the connection's records from 1 without a gap. elapsed_ms is the
monotonic time since the first record, in milliseconds, so preflight reports 0.
completed adds execution_ms, result_assembly_ms, results_digest_ms (null when
the digest phase was skipped) and serialization_ms; serialization_ms is measured
at the completed call, after the caller flushed its output. Nothing here is part
of a report's provenance or results digest.

Strict handover. A boundary is exactly one nonblocking os.write of the whole
record: no poll, no sleep, no spin, no retry, no wait. A write that does not take
the whole record (short, zero, EAGAIN, a disconnect or any other error) raises
PhaseTransportError, keeps the part the socket did not take in `unsent` (at most
one record, far inside the 1 MiB unsent bound) and ends the writer for good. A
record handed to the socket is not an acknowledgement: nothing here claims that
the parent received or validated it. The optimizer's trial scope and the bounded
wait of its phase-aware mode belong to another writer; this one
never waits for its reader.

The descriptor is borrowed: it is never closed, duplicated or owned here and no
registry keeps it. RunPhaseWriter(fd) is the check that fails a bad transport in
preflight, so build the writer before any strategy code loads. fd=None still
tracks the phases and their timing, for phase_timing(), but exports nothing. A
writer is not thread safe: one thread drives it.
"""
from __future__ import annotations

import fcntl
import json
import math
import os
import socket
import time
from typing import Callable, Dict, Mapping, Optional

__all__ = [
    "MAX_RECORD_BYTES",
    "MAX_RECORDS",
    "MAX_UNSENT_BYTES",
    "PHASES",
    "PhaseTransportError",
    "RunPhaseWriter",
    "encode_record",
]

PHASES = ("preflight", "execution", "result_assembly", "results_digest",
          "serialization", "completed")
MAX_RECORD_BYTES = 1024         # one record, its LF included
MAX_RECORDS = 6                 # per scope; a run is one scope
MAX_UNSENT_BYTES = 1024 * 1024  # unsent data a writer may hold; this one holds at most a record

# What may follow what: results_digest is the only phase a run may skip, a phase
# is entered once, and nothing follows completed.
_SUCCESSORS = {
    None: ("preflight",),
    "preflight": ("execution",),
    "execution": ("result_assembly",),
    "result_assembly": ("results_digest", "serialization"),
    "results_digest": ("serialization",),
    "serialization": ("completed",),
    "completed": (),
}

_ACCESS_MODE_BITS = os.O_WRONLY | os.O_RDWR  # the access-mode field of F_GETFL flags
_MAX_DESCRIPTOR = 2 ** 31 - 1                # a C int: nothing above it names a descriptor
_EXACT_INTEGER_LIMIT = 2 ** 53               # whole floats below this go on the wire as integers


class PhaseTransportError(Exception):
    """The phase transport failed, or was driven outside its contract. For the
    harness this is a harness fault, never a strategy outcome: `code` is
    harness_internal_error (class engine_fault in run_failure_codes.json).

    `reason` names the cause: invalid_fd, invalid_family, invalid_type,
    not_writable, nonblocking_failed (the descriptor); invalid_phase,
    phase_order, writer_failed (the call); clock_invalid (the clock);
    record_invalid, record_oversize (the record); would_block, short_write,
    disconnected, write_failed (the handover); timing_unavailable; unexpected
    (any other exception that ended an advance). `phase` is the boundary that
    was refused, when there is one."""

    code = "harness_internal_error"

    def __init__(self, text: str, *, reason: str, phase: Optional[str] = None) -> None:
        super().__init__(text)
        self.reason = reason
        self.phase = phase


def encode_record(record: Mapping[str, object]) -> bytes:
    """The framing of one record: compact JSON in UTF-8 and the LF that ends the
    line, at most MAX_RECORD_BYTES bytes in all. A value JSON cannot hold, a
    non-finite number or an oversized record raises PhaseTransportError."""
    try:
        text = json.dumps(record, separators=(",", ":"), allow_nan=False)
    except (TypeError, ValueError) as error:
        raise PhaseTransportError(f"phase record cannot be encoded: {error}",
                                  reason="record_invalid") from error
    line = text.encode("utf-8") + b"\n"
    if len(line) > MAX_RECORD_BYTES:
        raise PhaseTransportError(
            f"phase record is {len(line)} bytes, over the {MAX_RECORD_BYTES} byte limit",
            reason="record_oversize")
    return line


def _wire_number(value: Optional[float]) -> Optional[float]:
    """A millisecond value as it goes on the wire: a whole number is written as an
    integer (elapsed_ms 0, not 0.0, as in the pinned example), any other as the
    float. None stays None (results_digest_ms of a skipped digest)."""
    if value is not None and value.is_integer() and abs(value) < _EXACT_INTEGER_LIMIT:
        return int(value)
    return value


def _milliseconds(start: float, end: float) -> float:
    return (end - start) * 1000.0


def _is_writable_mode(flags: int) -> bool:
    """Whether F_GETFL flags carry write access (O_WRONLY or O_RDWR)."""
    return (flags & _ACCESS_MODE_BITS) in (os.O_WRONLY, os.O_RDWR)


def _borrow_descriptor(fd: object) -> int:
    """Check the descriptor named by --run-phase-fd and make it nonblocking: an int
    that is not a bool, from 3 up, an open AF_UNIX SOCK_STREAM socket with write
    access. The socket is looked at through a temporary wrapper that is detached
    again, never closed, and no descriptor is duplicated: on every path, rejected
    ones included, the descriptor stays open and the caller's. O_NONBLOCK is set
    here, before any write, and left set."""
    if isinstance(fd, bool) or not isinstance(fd, int):
        raise PhaseTransportError(
            f"run phase descriptor must be an int, not {type(fd).__name__}", reason="invalid_fd")
    if fd < 3 or fd > _MAX_DESCRIPTOR:
        raise PhaseTransportError(
            f"run phase descriptor must be between 3 and {_MAX_DESCRIPTOR}, got {fd}",
            reason="invalid_fd")
    try:
        probe = socket.socket(fileno=fd)
    except (OSError, OverflowError, ValueError) as error:
        raise PhaseTransportError(
            f"run phase descriptor {fd} is not an open socket: {error}",
            reason="invalid_fd") from error
    try:
        family, kind = probe.family, probe.type
    finally:
        probe.detach()  # borrowed: let go of the descriptor without closing it
    if family != socket.AF_UNIX:
        raise PhaseTransportError(
            f"run phase descriptor {fd} is not an AF_UNIX socket", reason="invalid_family")
    if kind != socket.SOCK_STREAM:
        raise PhaseTransportError(
            f"run phase descriptor {fd} is not a SOCK_STREAM socket", reason="invalid_type")
    try:
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    except (OSError, OverflowError) as error:
        raise PhaseTransportError(
            f"run phase descriptor {fd} flags cannot be read: {error}",
            reason="invalid_fd") from error
    if not _is_writable_mode(flags):
        raise PhaseTransportError(
            f"run phase descriptor {fd} is not writable", reason="not_writable")
    try:
        os.set_blocking(fd, False)
    except OSError as error:
        raise PhaseTransportError(
            f"run phase descriptor {fd} cannot be made nonblocking: {error}",
            reason="nonblocking_failed") from error
    return fd


class RunPhaseWriter:
    """Reports one run's phases over a borrowed descriptor, strictly (see the
    module text).

        writer = RunPhaseWriter(args.run_phase_fd)   # None without the flag
        writer.advance("preflight")
        ...
        writer.advance("serialization")
        timing = writer.phase_timing()               # for the report, then serialize
        writer.advance("completed")                  # after the output is flushed

    `clock` returns monotonic seconds. It is read once per advance() and never
    by the constructor, and a reading that is not a finite number (a bool, a
    string, NaN, infinity) or that is earlier than the last one fails the writer.
    """

    def __init__(self, fd: Optional[int], *,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._fd = None if fd is None else _borrow_descriptor(fd)
        self._clock = clock
        self._phase: Optional[str] = None
        self._sequence = 0
        self._origin: Optional[float] = None         # clock reading of the first record
        self._last_reading: Optional[float] = None
        self._entered: Dict[str, float] = {}         # phase -> clock reading at entry
        self._failure: Optional[str] = None
        self._unsent = b""
        self._last_handed_frame_bytes = 0

    @property
    def phase(self) -> Optional[str]:
        """The last phase entered; None before preflight."""
        return self._phase

    @property
    def sequence(self) -> int:
        """The sequence of the last record handed over: how many phases were entered."""
        return self._sequence

    @property
    def failure(self) -> Optional[str]:
        """The reason that ended the writer; None while it is usable."""
        return self._failure

    @property
    def unsent(self) -> bytes:
        """The part of the failed record the socket did not take; empty when no
        handover failed. At most one record."""
        return self._unsent

    @property
    def export_requested(self) -> bool:
        """Whether this writer exports its records: it holds a descriptor (fd is not
        None). The engine's execution observer reports it in its receipt."""
        return self._fd is not None

    @property
    def last_handed_frame_bytes(self) -> int:
        """The size, LF included, of the one record the last successful advance() handed
        to the socket: 0 before any advance and after an advance of a writer without a
        descriptor. A refused advance leaves it as it was, since nothing was handed over;
        it is read right after the advance() that returned, never after a failure."""
        return self._last_handed_frame_bytes

    def advance(self, phase: str) -> None:
        """Enter `phase`. Returns once its whole record is in the socket (one
        nonblocking os.write; that is not the parent's receipt). Anything else
        raises PhaseTransportError: the phase was NOT entered, the work after it
        must not run, and the writer is finished: every later advance() raises
        writer_failed without touching the descriptor."""
        if self._failure is not None:
            raise PhaseTransportError(
                f"phase writer ended ({self._failure}); a failed writer is never reused",
                reason="writer_failed", phase=phase if type(phase) is str else None)
        try:
            self._enter(phase)
        except PhaseTransportError as error:
            self._failure = error.reason
            raise
        except BaseException:
            self._failure = "unexpected"
            raise

    def phase_timing(self) -> Dict[str, object]:
        """The report's diagnostics.phase_timing: version 1, execution_ms,
        result_assembly_ms and results_digest_ms (None when the digest phase was
        skipped), in milliseconds. Known once serialization began, so only after
        advance("serialization") returned; the serialization duration is not part
        of it, it is in the completed record."""
        if "serialization" not in self._entered:
            raise PhaseTransportError(
                "phase timing is known only once serialization began",
                reason="timing_unavailable")
        timing: Dict[str, object] = {"version": 1}
        timing.update(self._report_durations())
        return timing

    def _enter(self, phase: str) -> None:
        if type(phase) is not str or phase not in PHASES:
            shown = repr(phase) if type(phase) is str else f"<{type(phase).__name__}>"
            raise PhaseTransportError(f"unknown run phase {shown}", reason="invalid_phase")
        if phase not in _SUCCESSORS[self._phase]:
            raise PhaseTransportError(
                f"run phase {phase} cannot follow {self._phase or 'the start'}",
                reason="phase_order", phase=phase)
        reading = self._read_clock()
        origin = reading if self._origin is None else self._origin
        elapsed_ms = _milliseconds(origin, reading)
        if not math.isfinite(elapsed_ms):
            raise PhaseTransportError(
                "phase clock span is not finite", reason="clock_invalid", phase=phase)
        handed = 0
        if self._fd is not None:
            frame = encode_record(self._record(phase, elapsed_ms, reading))
            self._hand_off(frame, phase)
            handed = len(frame)
        # The whole record is in the socket (or nothing is exported): now the phase is entered.
        self._phase = phase
        self._sequence += 1
        self._origin = origin
        self._last_reading = reading
        self._entered[phase] = reading
        self._last_handed_frame_bytes = handed

    def _read_clock(self) -> float:
        """One reading, validated: a finite number that is not a bool and not
        earlier than the last one."""
        try:
            reading = self._clock()
        except Exception as error:
            raise PhaseTransportError(
                f"phase clock failed: {type(error).__name__}: {error}",
                reason="clock_invalid") from error
        if isinstance(reading, bool) or not isinstance(reading, (int, float)):
            raise PhaseTransportError(
                f"phase clock returned {type(reading).__name__}, not a number",
                reason="clock_invalid")
        try:
            seconds = float(reading)
        except OverflowError as error:
            raise PhaseTransportError(
                "phase clock reading is too large", reason="clock_invalid") from error
        if not math.isfinite(seconds):
            raise PhaseTransportError(
                "phase clock reading is not finite", reason="clock_invalid")
        if self._last_reading is not None and seconds < self._last_reading:
            raise PhaseTransportError("phase clock went backward", reason="clock_invalid")
        return seconds

    def _report_durations(self) -> Dict[str, Optional[float]]:
        """execution_ms, result_assembly_ms and results_digest_ms, known once
        serialization began. Each runs from its phase's entry to the next phase's
        entry; result_assembly ends at serialization when the digest was skipped."""
        entered = self._entered
        digest_at = entered.get("results_digest")
        assembly_ends = entered["serialization"] if digest_at is None else digest_at
        return {
            "execution_ms": _milliseconds(entered["execution"], entered["result_assembly"]),
            "result_assembly_ms": _milliseconds(entered["result_assembly"], assembly_ends),
            "results_digest_ms": (None if digest_at is None
                                  else _milliseconds(digest_at, entered["serialization"])),
        }

    def _record(self, phase: str, elapsed_ms: float, reading: float) -> Dict[str, object]:
        record: Dict[str, object] = {
            "record": "run_phase",
            "version": 1,
            "sequence": self._sequence + 1,
            "scope": "run",
            "trial_id": None,
            "phase": phase,
            "elapsed_ms": _wire_number(elapsed_ms),
        }
        if phase == "completed":
            durations = self._report_durations()
            durations["serialization_ms"] = _milliseconds(self._entered["serialization"], reading)
            record.update({name: _wire_number(value) for name, value in durations.items()})
        return record

    def _hand_off(self, line: bytes, phase: str) -> None:
        """The boundary's one os.write. Only a write that took the whole record
        returns; there is no second attempt for the rest."""
        try:
            written = os.write(self._fd, line)
        except BlockingIOError as error:
            self._unsent = line
            raise PhaseTransportError(
                f"socket full: the record for phase {phase} was not handed over",
                reason="would_block", phase=phase) from error
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError) as error:
            self._unsent = line
            raise PhaseTransportError(
                f"parent disconnected: the record for phase {phase} was not handed over: {error}",
                reason="disconnected", phase=phase) from error
        except OSError as error:
            self._unsent = line
            raise PhaseTransportError(
                f"write failed: the record for phase {phase} was not handed over: {error}",
                reason="write_failed", phase=phase) from error
        if written < len(line):
            self._unsent = line[written:]
            raise PhaseTransportError(
                f"short write: {written} of {len(line)} bytes of the record for phase {phase}",
                reason="short_write", phase=phase)
