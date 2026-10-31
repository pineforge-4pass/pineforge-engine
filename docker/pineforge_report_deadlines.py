"""Opt-in, synchronous channel helpers for R06/R08/R09/R16/R17.

Importing this module installs no signal handlers, starts no threads, opens no
descriptors and creates no files. The default Deadline clock is CLOCK_BOOTTIME
when the platform and kernel provide it (so suspend counts), else monotonic.
An explicit alternative clock must also return monotonic seconds.

Writes require stream sockets: send uses MSG_NOSIGNAL, or SO_NOSIGPIPE where
available, independently of the process SIGPIPE disposition. Non-socket write
endpoints fail with TransportError(ENOTSOCK), since pipe writes have no portable
per-call signal suppression. Reads accept sockets and pipes. Borrowed endpoints
are left nonblocking; ownership and required final close remain with the caller.

These helpers enforce deadlines only while their calling process can run.
The host watchdog, native process-tree termination, concurrent channel service,
terminal packet atomicity/delivery grace (R14) and acceptance policy are outside
this module. In particular, byte-stream completion is not a success receipt.
"""

import ctypes
from collections.abc import Mapping
from dataclasses import dataclass
import errno
import math
import os
import select
import socket
import stat
import time


__all__ = [
    "DeadlineError", "Deadline", "ChannelIOError", "PeerClosed", "TransportError",
    "RelayResult", "bounded_write", "bounded_read", "relay", "close_once",
]

_QUEUE_BYTES = 65536
_POLL_SLICE_MS = 50
_MAX_DURATION_MS = 9007199254740990
_CLOSED_ERRNOS = frozenset((errno.EPIPE, errno.ECONNRESET, errno.ENOTCONN))
_AGAIN_ERRNOS = frozenset((errno.EAGAIN, errno.EWOULDBLOCK))


class DeadlineError(Exception):
    """R16: local wall/stall expiry with trusted stage and successful byte count.

    Does not stop other processes or replace the external host watchdog.
    """

    def __init__(self, kind, stage_hint, bytes_moved):
        if kind not in ("wall", "stall"):
            raise ValueError("deadline kind must be wall or stall")
        self.kind = kind
        self.stage_hint = stage_hint
        self.bytes_moved = bytes_moved
        self.relay_result = None
        super().__init__(kind + " deadline expired at " + stage_hint)


class ChannelIOError(OSError):
    """R17: common local I/O failure base; no host status or process teardown.

    errno, stage_hint and bytes_moved describe the actual transport operation.
    """

    def __init__(self, error, stage_hint, bytes_moved):
        super().__init__(error, os.strerror(error))
        self.stage_hint = stage_hint
        self.bytes_moved = bytes_moved
        self.relay_result = None


class PeerClosed(ChannelIOError):
    """R17: EPIPE/ECONNRESET/ENOTCONN; does not certify host disposition."""


class TransportError(ChannelIOError):
    """R17: zero progress, EBADF or other terminal I/O; no watchdog/teardown."""


class Deadline:
    """R16: one immutable absolute W and a progress-only stall interval P.

    Default clock: CLOCK_BOOTTIME if usable, otherwise time.monotonic. Helpers
    arm P when an operation becomes pending; idle computation consumes only W.
    expired() checks W; stall_remaining_ms() separately exposes P, including
    its initial interval from construction. None or zero disables either limit;
    the corresponding remaining time is math.inf and polls remain finite.
    This object cannot enforce a deadline while its process is stopped.
    """

    def __init__(self, wall_ms, stall_ms, clock=time.monotonic):
        for name, value in (("wall_ms", wall_ms), ("stall_ms", stall_ms)):
            if value is None:
                continue
            if (isinstance(value, bool) or not isinstance(value, (int, float))
                    or not 0 <= value <= _MAX_DURATION_MS):
                raise ValueError(name + " must be a nonnegative finite duration or None")
        self._clock = clock
        if clock is time.monotonic and hasattr(time, "CLOCK_BOOTTIME"):
            try:
                time.clock_gettime(time.CLOCK_BOOTTIME)
            except (OSError, ValueError):
                pass
            else:
                self._clock = lambda: time.clock_gettime(time.CLOCK_BOOTTIME)
        started = self._now()
        self._wall_at = (math.inf if wall_ms is None or wall_ms == 0
                         else started + wall_ms / 1000)
        self._stall_seconds = (math.inf if stall_ms is None or stall_ms == 0
                               else stall_ms / 1000)
        self._stall_at = started + self._stall_seconds

    def _now(self):
        now = self._clock()
        if not math.isfinite(now):
            raise ValueError("deadline clock must return finite monotonic seconds")
        return now

    def remaining_wall_ms(self):
        """R16: milliseconds to the original W, or math.inf when unlimited."""
        return max(0.0, (self._wall_at - self._now()) * 1000)

    def expired(self):
        """R16: whether absolute W expired; always false for an unlimited W."""
        return self._now() >= self._wall_at

    def note_progress(self, nbytes):
        """R16/R17: positive successful bytes reset only P; no outside action."""
        if isinstance(nbytes, bool) or not isinstance(nbytes, int) or nbytes < 0:
            raise ValueError("progress must be a nonnegative byte count")
        if nbytes:
            self._stall_at = self._now() + self._stall_seconds

    def stall_remaining_ms(self):
        """R16: milliseconds to P, or math.inf when unlimited."""
        return max(0.0, (self._stall_at - self._now()) * 1000)

    def _begin_pending(self):
        # Called once for new pending work, never by a syscall retry.
        self._stall_at = self._now() + self._stall_seconds

    def _timeout_ms(self, stage_hint, bytes_moved, use_stall):
        now = self._now()
        expiry = self._wall_at
        kind = "wall"
        if use_stall and self._stall_at < expiry:
            expiry, kind = self._stall_at, "stall"
        remaining = expiry - now
        if remaining <= 0:
            raise DeadlineError(kind, stage_hint, bytes_moved)
        # Even two unlimited limits use finite slices to recheck signals/clock.
        # Finite slices also recheck BOOTTIME after resume from system suspend.
        return max(1, math.ceil(min(remaining * 1000, _POLL_SLICE_MS)))


@dataclass(frozen=True)
class RelayResult(Mapping):
    """R06/R08/R17: observed/approved/written bytes and actual overflow/EOF.

    Does not assert report validity, terminal delivery or external acceptance.
    """

    observed: int
    accepted: int
    written: int
    overflow: bool
    eof: bool

    def __getitem__(self, key):
        if key not in ("observed", "accepted", "written", "overflow", "eof"):
            raise KeyError(key)
        return getattr(self, key)

    def __iter__(self):
        return iter(("observed", "accepted", "written", "overflow", "eof"))

    def __len__(self):
        return 5


class _RawIO:
    # CPython's socket.send/os.read retry EINTR below the Python deadline loop.
    # Direct libc calls return every interruption to that loop instead.
    def __init__(self):
        library = ctypes.CDLL(None, use_errno=True)
        self.send = library.send
        self.send.argtypes = (ctypes.c_int, ctypes.c_void_p,
                              ctypes.c_size_t, ctypes.c_int)
        self.send.restype = ctypes.c_ssize_t
        self.read = library.read
        self.read.argtypes = (ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t)
        self.read.restype = ctypes.c_ssize_t


def _syscall(function, *args):
    result = function(*args)
    if result < 0:
        error = ctypes.get_errno() or errno.EIO
        raise OSError(error, os.strerror(error))
    return result


def _stage(stage_hint):
    if not isinstance(stage_hint, str):
        raise TypeError("stage_hint must be supplied as a trusted string")


def _io_error(exc, stage_hint, bytes_moved):
    error = exc.errno or errno.EIO
    failure = PeerClosed if error in _CLOSED_ERRNOS else TransportError
    return failure(error, stage_hint, bytes_moved)


def _setup(operation, deadline, stage_hint, use_stall):
    while True:
        deadline._timeout_ms(stage_hint, 0, use_stall)
        try:
            return operation()
        except OSError as exc:
            if exc.errno == errno.EINTR:
                continue
            raise _io_error(exc, stage_hint, 0) from exc


def _writer(fd, deadline, stage_hint):
    metadata = _setup(lambda: os.fstat(fd), deadline, stage_hint, True)
    if not stat.S_ISSOCK(metadata.st_mode):
        raise TransportError(errno.ENOTSOCK, stage_hint, 0)
    endpoint = _setup(lambda: socket.socket(fileno=fd),
                      deadline, stage_hint, True)
    try:
        socket_type = _setup(
            lambda: endpoint.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE),
            deadline, stage_hint, True)
        if socket_type != socket.SOCK_STREAM:
            raise TransportError(errno.EPROTOTYPE, stage_hint, 0)
        _setup(lambda: endpoint.setblocking(False), deadline, stage_hint, True)
        flags = getattr(socket, "MSG_DONTWAIT", 0)
        if hasattr(socket, "MSG_NOSIGNAL"):
            flags |= socket.MSG_NOSIGNAL
        elif hasattr(socket, "SO_NOSIGPIPE"):
            _setup(lambda: endpoint.setsockopt(socket.SOL_SOCKET,
                                               socket.SO_NOSIGPIPE, 1),
                   deadline, stage_hint, True)
        else:
            raise TransportError(errno.EOPNOTSUPP, stage_hint, 0)
        return endpoint, flags
    except BaseException:
        endpoint.detach()
        raise


def _reader(fd, deadline, stage_hint, use_stall):
    metadata = _setup(lambda: os.fstat(fd), deadline, stage_hint, use_stall)
    if not (stat.S_ISSOCK(metadata.st_mode) or stat.S_ISFIFO(metadata.st_mode)):
        # Disk/device reads can block despite O_NONBLOCK and need a worker.
        raise TransportError(errno.EOPNOTSUPP, stage_hint, 0)
    _setup(lambda: os.set_blocking(fd, False), deadline, stage_hint, use_stall)


def _wait(fd, event, deadline, stage_hint, bytes_moved, use_stall):
    poller = select.poll()
    poller.register(fd, event)
    while True:
        timeout = deadline._timeout_ms(stage_hint, bytes_moved, use_stall)
        try:
            ready = poller.poll(timeout)
        except OSError as exc:
            if exc.errno == errno.EINTR or exc.errno in _AGAIN_ERRNOS:
                continue
            raise _io_error(exc, stage_hint, bytes_moved) from exc
        for ready_fd, mask in ready:
            if ready_fd == fd:
                if mask & select.POLLNVAL:
                    raise TransportError(errno.EBADF, stage_hint, bytes_moved)
                # HUP/ERR must reach read/send for EOF or the real socket errno.
                return


def _write_all(endpoint, flags, data, deadline, stage_hint, raw):
    moved = 0
    while moved < len(data):
        deadline._timeout_ms(stage_hint, moved, True)
        piece = data[moved:moved + _QUEUE_BYTES]
        # A bounded syscall scratch buffer also accepts read-only input views.
        buffer = (ctypes.c_char * len(piece)).from_buffer_copy(piece)
        try:
            deadline._timeout_ms(stage_hint, moved, True)
            count = _syscall(raw.send, endpoint.fileno(), buffer, len(piece), flags)
        except OSError as exc:
            if exc.errno == errno.EINTR:
                continue
            if exc.errno in _AGAIN_ERRNOS:
                _wait(endpoint.fileno(), select.POLLOUT, deadline, stage_hint,
                      moved, True)
                continue
            raise _io_error(exc, stage_hint, moved) from exc
        finally:
            del buffer
        if count <= 0:
            raise TransportError(errno.EIO, stage_hint, moved)
        moved += count
        deadline.note_progress(count)
    deadline._timeout_ms(stage_hint, moved, True)
    return moved


def _read_some(fd, nmax, deadline, stage_hint, use_stall, raw):
    deadline._timeout_ms(stage_hint, 0, use_stall)
    buffer = ctypes.create_string_buffer(nmax)
    while True:
        deadline._timeout_ms(stage_hint, 0, use_stall)
        try:
            count = _syscall(raw.read, fd, buffer, nmax)
            return buffer[:count]
        except OSError as exc:
            if exc.errno == errno.EINTR:
                continue
            if exc.errno in _AGAIN_ERRNOS:
                _wait(fd, select.POLLIN, deadline, stage_hint, 0, use_stall)
                continue
            raise _io_error(exc, stage_hint, 0) from exc


def bounded_write(fd, data, deadline, stage_hint):
    """R16/R17: write all bytes to a nonblocking stream socket, returning int.

    Positive returns alone count; EINTR/EAGAIN/short sends keep the original W.
    DeadlineError or ChannelIOError carries successful bytes_moved. No process
    teardown, host watchdog or terminal packet delivery is performed.
    """
    _stage(stage_hint)
    view = memoryview(data).cast("B")
    deadline._timeout_ms(stage_hint, 0, False)
    if not view:
        return 0
    deadline._begin_pending()
    endpoint, flags = _writer(fd, deadline, stage_hint)
    try:
        return _write_all(endpoint, flags, view, deadline, stage_hint, _RawIO())
    finally:
        endpoint.detach()


def bounded_read(fd, nmax, deadline, stage_hint):
    """R16/R17: read at most nmax socket/pipe bytes, returning (bytes, eof).

    A pending read observes W/P; a zero nmax returns (b"", False) without I/O.
    EINTR/EAGAIN never renew W/P. No host watchdog or process teardown runs.
    """
    _stage(stage_hint)
    if isinstance(nmax, bool) or not isinstance(nmax, int) or nmax < 0:
        raise ValueError("nmax must be a nonnegative integer")
    deadline._timeout_ms(stage_hint, 0, False)
    if nmax == 0:
        return b"", False
    deadline._begin_pending()
    _reader(fd, deadline, stage_hint, True)
    data = _read_some(fd, nmax, deadline, stage_hint, True, _RawIO())
    deadline.note_progress(len(data))
    deadline._timeout_ms(stage_hint, len(data), True)
    return data, not data


def relay(src_fd, dst_fd, allowance_bytes, deadline, stage_hint, chunk=65536):
    """R06/R08/R09/R16/R17: relay at most allowance bytes with one probe.

    One queue of at most min(chunk, 65536) bytes plus bounded syscall scratch;
    the observed overflow byte is never forwarded. Zero is an actual allowance
    here, not caller admission.
    Empty-queue source waits use W only; pending writes use W/P. Exceptions
    carry total written bytes_moved and an immutable relay_result snapshot.
    Does not terminate producers, validate sink grammar or deliver host status.
    """
    _stage(stage_hint)
    if (isinstance(allowance_bytes, bool) or not isinstance(allowance_bytes, int)
            or allowance_bytes < 0):
        raise ValueError("allowance_bytes must be a nonnegative integer")
    if isinstance(chunk, bool) or not isinstance(chunk, int) or chunk <= 0:
        raise ValueError("chunk must be a positive integer")
    window = min(chunk, _QUEUE_BYTES)
    observed = accepted = written = 0
    overflow = eof = False
    endpoint = None
    writing = False
    try:
        _reader(src_fd, deadline, stage_hint, False)
        raw = _RawIO()
        while True:
            # With no pending output, source computation cannot expire P.
            data = _read_some(src_fd, min(window, allowance_bytes - observed + 1),
                              deadline, stage_hint, False, raw)
            if not data:
                eof = True
                deadline._timeout_ms(stage_hint, written, False)
                return RelayResult(observed, accepted, written, overflow, eof)
            observed += len(data)
            approved = min(len(data), allowance_bytes - accepted)
            accepted += approved
            overflow = observed > allowance_bytes
            deadline._timeout_ms(stage_hint, written, False)
            if approved:
                deadline._begin_pending()
                if endpoint is None:
                    endpoint, flags = _writer(dst_fd, deadline, stage_hint)
                writing = True
                moved = _write_all(endpoint, flags, memoryview(data)[:approved],
                                   deadline, stage_hint, raw)
                writing = False
                written += moved
            if overflow:
                return RelayResult(observed, accepted, written, True, False)
            data = None
    except (DeadlineError, ChannelIOError) as exc:
        # A chunk failure counts only successful sends from that chunk.
        if writing:
            written += exc.bytes_moved
        exc.bytes_moved = written
        exc.relay_result = RelayResult(observed, accepted, written, overflow, eof)
        raise
    finally:
        if endpoint is not None:
            endpoint.detach()


def close_once(fd):
    """R17: attempt one owned-fd close, returning success; never retry EINTR.

    No cross-call ownership registry, host watchdog or process teardown exists.
    The caller must relinquish this fd even when the attempted close fails.
    """
    try:
        os.close(fd)
        return True
    except OSError:
        return False
