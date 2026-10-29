#!/usr/bin/env python3
"""Trusted, bounded-memory finalizer for pf-report-sink/v1.

Invocation (the launcher and this Python module take the same arguments):
  pineforge-sink-finalize --in-fd N --out - --max-bytes B --receipt-fd M
      [--run-id ID]
  pineforge-sink-finalize --in-fd N --out PATH [--run-id ID]
      [--spool-dir DIR] [--max-bytes B] [--receipt-fd M]
  python3 docker/pineforge_sink_finalize.py <the same arguments>

N is an inherited, blocking FIFO/pipe or AF_UNIX stream read descriptor.
Only that descriptor is read; there is no implicit stdin input or network
access. The wrapper closes every other copy of the stream's write end so
EOF can be observed, and drains stdout and the receipt descriptor concurrently.
No particular cwd is needed; wrappers should pass absolute paths for file mode.

With --out -, binary stdout carries the exact verified lines in input order,
in one pass, with no spool or filesystem writes. This mode supports a jail
with RLIMIT_FSIZE=0 and --no-fork. --spool-dir is ignored in this mode.
--max-bytes B is REQUIRED and must be positive to bound the jailed sink
stream; missing or non-positive B is a usage error (2), with no receipt.
The wrapper supplies its planned whole-sink bound U as B. File mode keeps
--max-bytes optional and accepts a nonnegative B.
PROVISIONAL OUTPUT CONTRACT: stdout bytes are PROVISIONAL until exit 0.
On exit 3/4/5/6/7/8 the consumer MUST discard everything received. The receipt
is written ONLY after the last stdout byte and a successful stdout flush.
A stdout flush failure has no receipt. A broken stdout (EPIPE) is exit 5.
--receipt-fd M must differ from stdout (fd 1); the default receipt fd is a
usage error here. Exit 0 plus complete=true means the whole sink was verified
and delivered.
The root wrapper streams stdout into a file in its own private directory,
renames only on exit 0 plus a complete receipt and a successful producer,
and deletes the provisional file otherwise.

PATH is a dedicated output pathname owned exclusively by this invocation.
File mode runs as a uid permitted to write the output and spool directories,
without RLIMIT_FSIZE=0.
PINEFORGE_RUN_JSON_PROFILE accepts unjailed (default) or jailed. File mode is excluded under jailed before touching
PATH or creating a spool: exit 3, with a text diagnostic naming finalizer_file_output_no_jailed_route on stderr only when --receipt-fd is not 2.
With --receipt-fd 2, the caller sees exit 3 and the ordinary failure receipt only, indistinguishable from a malformed stream.
Stdout mode with a positive --max-bytes remains accepted.
An old entry at PATH is unlinked before receiving input, so a failed run
cannot leave an old success behind. A random mode-0600 spool is created
in DIR (default: PATH's directory), filled, fsynced, closed, and verified
by exhausting pf_sink.SinkReader before os.replace publishes it at PATH.
DIR and PATH must be on the same filesystem: EXDEV is exit 5, with cleanup.
A tmpfs DIR is supported on that basis; its backing pages are filesystem
storage charged to tmpfs/cgroup budgets. Cross-filesystem copies are not used.
Every handled failure removes the spool and any output this run published.
The wrapper must retain directory permissions until cleanup completes;
SIGKILL, power loss, or an externally removed directory cannot be cleaned
up by a process that can no longer run or access it.

Input is read in at most 64 KiB chunks. Both modes use SinkReader's grammar,
SHA-256 fold and seven counts, with no retained records. Stdout mode holds
one wire line (<=1 MiB, with one extra byte to detect overflow), a 64 KiB
input buffer and a temporary read of <=64 KiB; stdout uses unbuffered
write(2). Parsing adds a bounded JSON working set and header, seven counters
and SHA-256 state. File mode first copies the input, then verifies the spool.
RAM is O(MAX_LINE_BYTES), independent of total stream size. This is a bound
in stream length, not a universal numeric Python RSS limit.
The 64 MiB regression test requires peak child RSS below 20 MiB for its
16 KiB records, including the interpreter and imports.

With --max-bytes B, both modes read at most B+1 input bytes. Exactly B
bytes with a valid trailer and EOF succeed. Overflow stops at byte B+1
and exits 8, without forwarding that byte. In stdout mode the reader sees
all first B bytes before the separate one-byte overflow probe, so the
same input and cap give the same stop point and verified prefix counts,
independent of read chunk sizes. File-mode overflow counts are zero,
because spool verification has not started. Earlier grammar, line or I/O
failures can still stop the run before the cap.

One compact JSON line goes to stdout in file mode, or exclusively to M
when supplied (required with --out -).
The receipt shapes in finalizer version 1.1.0 have these keys in order:
Keys (success): complete, count_e, count_t, count_o, count_v, count_k,
count_x, count_h, sink_bytes, sha256, finalizer_version.
Keys (ordinary failure): complete, count_e, count_t, count_o, count_v, count_k,
count_x, count_h, sink_bytes, sha256, finalizer_version.
Keys (exit-8 exhaustion): complete, count_e, count_t, count_o, count_v, count_k,
count_x, count_h, reason, sink_bytes, cap_bytes, sha256, finalizer_version.
Keys (typed refusal with M=2): complete, count_e, count_t, count_o, count_v,
count_k, count_x, count_h, sink_bytes, sha256, finalizer_version,
tool, event, exit_code, detail, code, args.
Counts are the strict reader's recomputed counts (absent types are zero). sink_bytes
includes the header, trailer and LFs; sha256 is the strict reader's logical
digest of all bytes BEFORE the trailer, including the header and LFs.
Failure receipts have complete=false and sha256="", with the byte count
actually read and any counts folded by the reader before refusal. Usage
errors have no receipt. The exit-8 receipt sets reason="sink_exhausted",
sink_bytes=B+1 and cap_bytes=B. The wrapper maps
exit 8 plus this receipt to report_sink_failed, reason=sink_exhausted,
class engine_fault: refunded; COMPUTE re-places with 2x U.
Typed exit-3 refusals append tool, event, exit_code, detail, code and args
only when M=2 reserves stderr exclusively for the receipt and cleanup has
not replaced the typed exit status. With M other than 2, the receipt uses
the ordinary failure shape and stderr carries one compact JSON diagnostic
with those six keys in that order. Other handled failures use the text
line "pineforge-sink-finalize: <text>" on stderr when M is not 2.
With M=2 no separate diagnostic is written. A receipt write failure also
removes PATH and returns 5; the receiver must require exit 0.

Exit codes:
  0  complete and verified; sink delivered/published and receipt written
  2  usage error
  3  malformed stream (JSON/schema/line rule, run-id mismatch, or E/T/O/K order) or a host-profile refusal (text diagnostic naming the reason on stderr only when --receipt-fd is not 2; with --receipt-fd 2, the ordinary failure receipt only, indistinguishable from a malformed stream)
  4  verification failure: trailer grammar, counts, SHA-256, or extra bytes
  5  input/output I/O failure (including receipt/rename)
  6  EOF without a trailer, including empty input/final fragments
  7  internal error
  8  sink_exhausted: stream exceeds --max-bytes at byte B+1

An unterminated final fragment is incomplete (6); a malformed LF-terminated
JSON line is 3. Transport size and line limits are checked first, so an
overlong fragment is 3 even at EOF. Interleaved record types are legal.
E times may repeat but may not decrease; T numbers start at 1, O rows at 0,
both contiguous, and K occurs at most once, as in LineWriter. The strict
reader remains the sole JSON, header, field and trailer parser.
An E-row null equity/open_profit is refused with event=sink_refused,
exit_code=3, code=report_result_invalid and
args={field: equity_curve.<field>, reason: nonfinite} (in the sole receipt
when M=2).
"""
from __future__ import annotations

import sys

# Imports must not attempt bytecode-cache writes when run without -B in jail.
sys.dont_write_bytecode = True

import argparse
import errno
import io
import json
import os
from pathlib import Path
import tempfile

if sys.flags.isolated:
    # -I excludes the script directory. Trust only the installed siblings,
    # never the cwd or an environment-provided Python search path.
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from pf_sink import MAX_LINE_BYTES, SinkFormatError, SinkReader


FINALIZER_VERSION = "1.1.0"
CHUNK_BYTES = 64 * 1024
_TAGS = "ETOVKXH"


class _Failure(Exception):
    def __init__(self, code: int, message: str, *, failure_code: str | None = None,
                 failure_args: dict | None = None):
        super().__init__(message)
        self.code = code
        self.failure_code = failure_code
        self.failure_args = {} if failure_args is None else failure_args

    def diagnostic_fields(self) -> dict:
        if self.failure_code is None:
            return {}
        return {"code": self.failure_code, "args": self.failure_args}


def _check_equity(record) -> None:
    # SinkReader pins the E schema: time, equity, open_profit, in that order.
    for name, value in zip(record.fields[1:], record.values[1:]):
        if value is None:
            field = "equity_curve." + name
            raise _Failure(3, f"the report result {field} is not finite",
                           failure_code="report_result_invalid",
                           failure_args={"field": field, "reason": "nonfinite"})


class _ReaderFailure(SinkFormatError):
    """Classification only; SinkReader still parses and refuses the input."""

    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


class _FinalizerReader(SinkReader):
    """Attach exit categories to the reader's existing refusal points.

    No parser or reader state is replaced. The two EOF/unterminated-line
    diagnostics are textual dependencies, pinned by the truncation tests.
    SinkReader wraps these exceptions as causes and closes its own handle.
    """

    def _readline(self):
        # Drop the previous wire line before allocating the next one.
        self.line = b""
        try:
            value, self.line = super()._readline()
            return value, self.line
        except SinkFormatError as error:
            if str(error) in ("missing header or trailer", "line does not end with LF"):
                raise _ReaderFailure(6, str(error)) from error
            raise

    def _finish(self, value):
        try:
            return super()._finish(value)
        except SinkFormatError as error:
            raise _ReaderFailure(4, str(error)) from error


class _Receipt:
    def __init__(self):
        self.counts = {tag: 0 for tag in _TAGS}
        self.sink_bytes = 0
        self.sha256 = ""
        self.cap_bytes = None

    def document(self, complete: bool) -> dict:
        return {
            "complete": complete,
            **{"count_" + tag.lower(): self.counts.get(tag, 0) for tag in _TAGS},
            **({"reason": "sink_exhausted"} if self.cap_bytes is not None else {}),
            "sink_bytes": self.sink_bytes,
            **({"cap_bytes": self.cap_bytes} if self.cap_bytes is not None else {}),
            "sha256": self.sha256 if complete else "",
            "finalizer_version": FINALIZER_VERSION,
        }


def _write_all(fd: int, data: bytes) -> None:
    remaining = memoryview(data)
    while remaining:
        written = os.write(fd, remaining)
        if written <= 0:
            raise OSError(errno.EIO, "write made no progress")
        remaining = remaining[written:]


def _line_bound(chunk: bytes, line_bytes: int) -> int:
    # Transport framing only; SinkReader owns the record grammar.
    start = 0
    while start < len(chunk):
        end = chunk.find(b"\n", start)
        stop = len(chunk) if end < 0 else end + 1
        line_bytes += stop - start
        if line_bytes > MAX_LINE_BYTES:
            raise _Failure(3, "line exceeds 1 MiB including LF")
        if end >= 0:
            line_bytes = 0
        start = stop
    return line_bytes


class _StreamInput(io.RawIOBase):
    """Borrow the inherited fd; bound reads before SinkReader buffers them."""

    def __init__(self, fd: int, receipt: _Receipt, max_bytes: int | None):
        super().__init__()
        self.fd = fd
        self.receipt = receipt
        self.max_bytes = max_bytes
        self.line_bytes = 0

    def readable(self) -> bool:
        return True

    def readinto(self, buffer) -> int:
        size = min(len(buffer), CHUNK_BYTES)
        if self.max_bytes is not None:
            # Deliver the entire allowed prefix before probing byte B+1.
            # Discarding an overflowing chunk here would make reader counts
            # depend on how the pipe split its reads.
            size = min(size, max(1, self.max_bytes - self.receipt.sink_bytes))
        chunk = os.read(self.fd, size)
        self.receipt.sink_bytes += len(chunk)
        if self.max_bytes is not None and self.receipt.sink_bytes > self.max_bytes:
            raise _Failure(8, "stream exceeds --max-bytes")
        self.line_bytes = _line_bound(chunk, self.line_bytes)
        buffer[:len(chunk)] = chunk
        return len(chunk)


def _receive(in_fd: int, spool_fd: int, receipt: _Receipt, max_bytes: int | None) -> None:
    line_bytes = 0
    while True:
        # Read only one byte beyond a cap, enough to distinguish exact fit
        # followed by EOF from overflow, without spooling past the cap.
        size = CHUNK_BYTES if max_bytes is None else min(
            CHUNK_BYTES, max_bytes - receipt.sink_bytes + 1)
        chunk = os.read(in_fd, size)
        if not chunk:
            return
        receipt.sink_bytes += len(chunk)
        if max_bytes is not None and receipt.sink_bytes > max_bytes:
            raise _Failure(8, "stream exceeds --max-bytes")
        line_bytes = _line_bound(chunk, line_bytes)
        _write_all(spool_fd, chunk)


def _reader_code(error: SinkFormatError) -> int:
    code = 3
    cause: BaseException | None = error
    while cause is not None:
        if isinstance(cause, OSError):
            return 5
        if isinstance(cause, _ReaderFailure):
            code = cause.code
        cause = cause.__cause__
    return code


def _verify(path: str | None, run_id: str | None, receipt: _Receipt, *,
            stream=None, out_fd: int | None = None) -> None:
    try:
        with _FinalizerReader(path, stream=stream) as reader:
            if run_id is not None and reader.header["run_id"] != run_id:
                raise _Failure(3, "header run_id differs from --run-id")
            if out_fd is not None:
                _write_all(out_fd, reader.line)
            reader.line = b""
            last_equity_time = None
            next_trade, next_output, constants = 1, 0, 0
            try:
                for record in reader:
                    # Canonical-value and order checks on parsed Record values,
                    # matching the path writer/run_json, not a second grammar.
                    if record.tag == "E":
                        _check_equity(record)
                        timestamp = record.values[0]
                        if last_equity_time is not None and timestamp < last_equity_time:
                            raise _Failure(3, "equity times are out of order")
                        last_equity_time = timestamp
                    elif record.tag == "T":
                        if record.values[0] != next_trade:
                            raise _Failure(3, "trade rows are out of order")
                        next_trade += 1
                    elif record.tag == "O":
                        if record.values[0] != next_output:
                            raise _Failure(3, "output rows are out of order")
                        next_output += 1
                    elif record.tag == "K":
                        constants += 1
                        if constants > 1:
                            raise _Failure(3, "constants record is repeated")
                    if out_fd is not None:
                        _write_all(out_fd, reader.line)
                    reader.line = b""
                    del record
            finally:
                receipt.counts = reader.counts
            if not reader.complete:
                raise _Failure(4, "strict reader did not verify completion")
            if out_fd is not None:
                _write_all(out_fd, reader.line)
            receipt.sha256 = reader.sha256
    except SinkFormatError as error:
        raise _Failure(_reader_code(error), str(error)) from error


def _nonnegative(value: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a nonnegative integer") from error
    if number < 0:
        raise argparse.ArgumentTypeError("expected a nonnegative integer")
    return number


def _descriptor(value: str) -> int:
    number = _nonnegative(value)
    if number > (1 << 31) - 1:
        raise argparse.ArgumentTypeError("descriptor exceeds the signed int range")
    return number


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="pineforge-sink-finalize", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False)
    parser.add_argument("--in-fd", type=_descriptor, required=True, metavar="N",
                        help="inherited blocking stream read descriptor")
    parser.add_argument("--out", type=Path, required=True, metavar="PATH|-",
                        help="final sink path, or - for provisional binary stdout (requires --receipt-fd and positive --max-bytes)")
    parser.add_argument("--run-id", metavar="ID", help="require this exact header run_id")
    parser.add_argument("--spool-dir", type=Path, metavar="DIR",
                        help="private spool directory (default: output directory; ignored with --out -)")
    parser.add_argument("--max-bytes", type=int, metavar="B",
                        help="hard input byte cap (required and positive with --out -; optional and nonnegative in file mode)")
    parser.add_argument("--receipt-fd", type=_descriptor, default=1, metavar="M",
                        help="receipt write descriptor (default: stdout, fd 1)")
    return parser


def _unlink(path: str) -> None:
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def _diagnostic(message: str, receipt_fd: int, *, document: dict | None = None) -> None:
    if receipt_fd == 2:
        return
    try:
        if document:
            line = json.dumps(document, separators=(",", ":"), allow_nan=False).encode("ascii") + b"\n"
        else:
            line = f"pineforge-sink-finalize: {message}\n".encode("utf-8", "backslashreplace")
        _write_all(2, line)
    except OSError:
        pass


def main(argv: list[str] | None = None) -> int:
    profile = os.environ.get("PINEFORGE_RUN_JSON_PROFILE", "unjailed")
    parser = _parser()
    args = parser.parse_args(argv)
    if args.in_fd == args.receipt_fd:
        parser.error("--in-fd and --receipt-fd must be different descriptors")
    stdout_mode = args.out == Path("-")
    if stdout_mode and (args.max_bytes is None or args.max_bytes <= 0):
        parser.error("--out - requires a positive --max-bytes to bound the jailed sink stream")
    if stdout_mode and args.receipt_fd == 1:
        parser.error("--out - requires --receipt-fd different from stdout (fd 1)")
    if args.max_bytes is not None and args.max_bytes < 0:
        parser.error("--max-bytes must be a nonnegative integer")
    out = None if stdout_mode else os.path.abspath(args.out)
    receipt = _Receipt()
    spool = None
    owns_output = False
    status = 0
    failure_diagnostic = {}
    try:
        if profile not in ("unjailed", "jailed"):
            raise _Failure(3, "PINEFORGE_RUN_JSON_PROFILE must be unjailed or jailed")
        if stdout_mode:
            with io.BufferedReader(_StreamInput(args.in_fd, receipt, args.max_bytes),
                                   buffer_size=CHUNK_BYTES) as stream:
                _verify(None, args.run_id, receipt, stream=stream, out_fd=1)
        else:
            if profile == "jailed":
                raise _Failure(
                    3, "jailed profile excludes pineforge-sink-finalize file-output mode: "
                    "finalizer_file_output_no_jailed_route")
            directory = (os.path.abspath(args.spool_dir) if args.spool_dir is not None
                         else os.path.dirname(out))
            _unlink(out)
            owns_output = True
            spool_fd, spool = tempfile.mkstemp(prefix=".pineforge-sink-", suffix=".spool", dir=directory)
            try:
                # mkstemp is 0600 or stricter under umask; keep the promised mode.
                os.fchmod(spool_fd, 0o600)
                _receive(args.in_fd, spool_fd, receipt, args.max_bytes)
                os.fsync(spool_fd)
            finally:
                os.close(spool_fd)
            _verify(spool, args.run_id, receipt)
            os.replace(spool, out)
            spool = None
    except _Failure as error:
        status = error.code
        fields = error.diagnostic_fields()
        if fields:
            failure_diagnostic = {
                "tool": "pineforge-sink-finalize", "event": "sink_refused",
                "exit_code": status, "detail": str(error), **fields,
            }
        _diagnostic(str(error), args.receipt_fd, document=failure_diagnostic)
    except OSError as error:
        status = 5
        _diagnostic(str(error), args.receipt_fd)
    except (Exception, KeyboardInterrupt) as error:
        status = 7
        _diagnostic(f"internal error: {type(error).__name__}: {error}", args.receipt_fd)
    finally:
        if spool is not None:
            try:
                _unlink(spool)
            except OSError as error:
                status = 5
                _diagnostic(f"spool cleanup failed: {error}", args.receipt_fd)
        if status and owns_output:
            try:
                _unlink(out)
            except OSError as error:
                status = 5
                _diagnostic(f"output cleanup failed: {error}", args.receipt_fd)
    if stdout_mode:
        # Sink writes use write(2). Flush after output stops, on failure too,
        # and require flush success before constructing any receipt.
        try:
            sys.stdout.flush()
        except OSError as error:
            _diagnostic(f"stdout flush failed: {error}", args.receipt_fd)
            return 5
        except (Exception, KeyboardInterrupt) as error:
            _diagnostic(f"stdout flush internal error: {error}", args.receipt_fd)
            return 7
    try:
        # Cleanup can replace an overflow with an I/O failure. Only the
        # final exit-8 outcome gets the exhaustion-specific receipt keys.
        receipt.cap_bytes = args.max_bytes if status == 8 else None
        document = receipt.document(status == 0)
        if args.receipt_fd == 2 and status == failure_diagnostic.get("exit_code"):
            document.update(failure_diagnostic)
        line = json.dumps(document, separators=(",", ":"), allow_nan=False)
        _write_all(args.receipt_fd, line.encode("ascii") + b"\n")
    except OSError as error:
        status = 5
        _diagnostic(f"receipt write failed: {error}", args.receipt_fd)
    except (Exception, KeyboardInterrupt) as error:
        status = 7
        _diagnostic(f"receipt internal error: {type(error).__name__}: {error}", args.receipt_fd)
    else:
        return status
    if owns_output:
        try:
            _unlink(out)
        except OSError as error:
            status = 5
            _diagnostic(f"output cleanup failed: {error}", args.receipt_fd)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
