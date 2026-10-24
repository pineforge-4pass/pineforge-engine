"""CR-281: port CR-224's real-native sink-OFF file-jail test to 1.7.0.

CTest builds the unchanged WIDE corpus probe. The jailed children use the real
run_json profile, table setter and arena admission. Only the unjailed reference
and negative control suppress the Python report-sink factory, as that profile
has no sink-OFF CLI switch. No ctypes loader or native export is replaced.
"""
from __future__ import annotations

from contextlib import contextmanager
import errno
import fcntl
from itertools import islice
import json
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import stat
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "corpus/validation/bracket-same-id-exit-replace-01"
FEED = ROOT / "corpus/data/ohlcv_ETH-USDT-USDT_1m.csv"
MODULE_ENV = "PINEFORGE_CR224_STRATEGY_SO"
INPUT_BARS = 3 * 24 * 60
ARENA_FD = 5  # ScratchArena::kInheritedFd, scratch_arena.hpp.
# linux/fcntl.h values, used when the interpreter's fcntl module was built without them.
F_ADD_SEALS = getattr(fcntl, "F_ADD_SEALS", 1033)
F_GET_SEALS = getattr(fcntl, "F_GET_SEALS", 1034)
F_SEAL_SEAL = getattr(fcntl, "F_SEAL_SEAL", 0x0001)
F_SEAL_SHRINK = getattr(fcntl, "F_SEAL_SHRINK", 0x0002)
F_SEAL_GROW = getattr(fcntl, "F_SEAL_GROW", 0x0004)
CHUNK_BYTES = 1024 * 1024
# Use the line's hosting default, not a guessed bound for an unavailable FACTS
# record: test_arena_bound.py:47-49 and docs/design/arena-bound.md:82-87.
ARENA_BYTES = 1 << 30
WINDOW_BYTES = 32 * CHUNK_BYTES  # ScratchArena::kDefaultWindowBytes.
CONSTRAINTS_OK = "CR281 constraints: RLIMIT_FSIZE=0/0 SIGXFSZ=SIG_IGN write=EFBIG pwrite=EFBIG"
ARENA_OK = f"CR281 arena: fd=5 bytes={ARENA_BYTES} seals=GROW|SHRINK|SEAL pwrite=EFBIG"
NO_ARENA_OK = "CR281 arena: fd=5 absent"
TRACE_FILTER = "trace=%file,memfd_create"
TRACE_NAMES = (
    r"openat2|openat|open|creat|memfd_create|mknodat|mknod|"
    r"linkat|link|symlinkat|symlink|renameat2|renameat|rename|mkdirat|mkdir")
TRACE_CALL = re.compile(r"^(" + TRACE_NAMES + r")\(")
TRACE_PREFIX = re.compile(r"^(?:(?:\[pid\s+(\d+)\]|(\d+))\s+)?(.*)$")


def _library():
    configured = os.environ.get(MODULE_ENV)
    # No skip or fallback to a stale corpus module, including outside CTest.
    assert configured and Path(configured).is_file(), (
        f"CR281 requires the ALL-build cr224_nofile_strategy module; "
        f"{MODULE_ENV}={configured!r}")
    return Path(configured).resolve()


def _child_environment(cwd, temporary, *, profile, arena):
    # An allowlist, not a filtered copy: no inherited writable HOME/cache,
    # spill/output path, loader injection, Python path or profiler destination.
    environment = {
        "PATH": os.defpath,
        "HOME": str(cwd),
        "TMPDIR": str(temporary), "TEMP": str(temporary), "TMP": str(temporary),
        "XDG_CACHE_HOME": str(temporary), "XDG_CONFIG_HOME": str(temporary),
        "XDG_DATA_HOME": str(temporary), "XDG_STATE_HOME": str(temporary),
        "PYTHONDONTWRITEBYTECODE": "1", "PYTHONNOUSERSITE": "1",
        "PYTHONCOERCECLOCALE": "0",
        "LC_ALL": "C", "TZ": "UTC",
    }
    if profile == "jailed":
        environment["PF_SCRATCH_ARENA_JAILED"] = "1"
    if arena:
        assert profile == "jailed"
        environment.update(
            PF_SCRATCH_ARENA_FD=str(ARENA_FD),
            PF_SCRATCH_ARENA_BYTES=str(ARENA_BYTES),
            PF_SCRATCH_WINDOW_BYTES=str(WINDOW_BYTES),
        )
    return environment


def _keep_above_arena_slot(fd):
    # Never overwrite pytest's own fd 5 in the parent. Only the child launch
    # shim installs that slot, before importing run_json or loading the module.
    try:
        return fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 16)
    finally:
        os.close(fd)


def _memfd_create(name, flags):
    # The lab's interpreter (a relocatable CPython) is built without
    # os.memfd_create. glibc 2.27+ and the kernel provide the call itself, so
    # use it directly rather than skipping or weakening the sealed arena.
    if hasattr(os, "memfd_create"):
        return os.memfd_create(name, flags)
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    fd = libc.memfd_create(name.encode(), flags)
    if fd < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), "memfd_create")
    return fd


def _new_arena():
    # MFD_CLOEXEC = 1, MFD_ALLOW_SEALING = 2 (linux/memfd.h).
    fd = _keep_above_arena_slot(_memfd_create("pineforge-cr281-arena", 1 | 2))
    try:
        # All sizing/sealing is parent-side, BEFORE the child's zero limit.
        # The engine writes shared mappings; it never write/pwrites this fd.
        os.ftruncate(fd, ARENA_BYTES)
        seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL
        fcntl.fcntl(fd, F_ADD_SEALS, seals)
        assert fcntl.fcntl(fd, F_GET_SEALS) == seals
        assert os.fstat(fd).st_size == ARENA_BYTES
        return fd
    except BaseException:
        os.close(fd)
        raise


def _install_arena(source_fd):
    if source_fd >= 0:
        assert source_fd > ARENA_FD
        # pass_fds preserves the parent's backing across subprocess's fd
        # cleanup. Remap here, rather than risking a preexec dup being closed
        # by that cleanup. No backing is opened or created in the child.
        os.dup2(source_fd, ARENA_FD)
        os.close(source_fd)
        status = os.fstat(ARENA_FD)
        assert stat.S_ISREG(status.st_mode) and status.st_size == ARENA_BYTES
        seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL
        assert fcntl.fcntl(ARENA_FD, F_GET_SEALS) == seals
        assert fcntl.fcntl(ARENA_FD, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDWR
    else:
        assert source_fd == -1
        try:
            os.fstat(ARENA_FD)
        except OSError as error:
            assert error.errno == errno.EBADF, repr(error)
        else:
            raise AssertionError("the no-arena child inherited fd 5")
        print(NO_ARENA_OK, file=sys.stderr, flush=True)


def _jail_preexec():
    # HOST order from scratch-tables.md: ignore SIGXFSZ before the zero limit.
    # Keep both soft and hard limits zero, including for the admitted memfd.
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))


def _expect_efbig(fd, *, positional):
    try:
        if positional:
            os.pwrite(fd, b"scratch-write-canary", 0)
        else:
            os.write(fd, b"x")
    except OSError as error:
        assert error.errno == errno.EFBIG, repr(error)
    else:
        raise AssertionError("a positive-length jailed syscall write succeeded")


def _check_child_constraints(canary_fd, *, profile, arena):
    assert resource.getrlimit(resource.RLIMIT_FSIZE) == (0, 0)
    assert signal.getsignal(signal.SIGXFSZ) == signal.SIG_IGN
    cwd = Path.cwd()
    temporary = Path(os.environ["TMPDIR"])
    assert cwd != temporary
    assert os.environ == _child_environment(cwd, temporary, profile=profile, arena=arena)
    for directory in (cwd, temporary):
        assert stat.S_IMODE(directory.stat().st_mode) == 0o555
        assert not list(directory.iterdir()), str(directory)
    # The parent opened the empty regular canary before applying the jail and
    # made its path read-only. Thus EFBIG cannot be an open-permission failure.
    assert stat.S_ISREG(os.fstat(canary_fd).st_mode)
    assert os.fstat(canary_fd).st_size == 0
    try:
        _expect_efbig(canary_fd, positional=False)
        _expect_efbig(canary_fd, positional=True)
        assert os.fstat(canary_fd).st_size == 0
    finally:
        os.close(canary_fd)  # No ordinary writable file fd reaches the engine.
    if arena:
        # Even an in-size pwrite to a memfd is forbidden by RLIMIT_FSIZE=0.
        # Successful native execution must use the real arena's MAP_SHARED
        # copies (scratch_arena.cpp), not a relaxed limit or syscall writes.
        _expect_efbig(ARENA_FD, positional=True)
        assert os.fstat(ARENA_FD).st_size == ARENA_BYTES
        print(ARENA_OK, file=sys.stderr, flush=True)
    print(CONSTRAINTS_OK + f" euid={os.geteuid()}", file=sys.stderr, flush=True)


def _native_child(argv):
    canary_fd, source_fd, profile = int(argv[0]), int(argv[1]), argv[2]
    _install_arena(source_fd)
    if canary_fd >= 0:
        _check_child_constraints(canary_fd, profile=profile, arena=source_fd >= 0)
    sys.path.insert(0, str(ROOT / "docker"))
    import run_json

    sink_requests = []
    if profile == "unjailed":
        # Native scratch is the negative control's disk requirement, rather
        # than Python's report sink. All real exports still flow through
        # load_strategy/run_backtest_full. 1.7.0 added keyword-only body.
        def sink_off(lib, state, args, *, body=True):
            assert args.profile == "unjailed" and body
            sink_requests.append(state)
            return None
        run_json._new_report_sink = sink_off
    status = run_json.main(["--profile", profile, *argv[3:]])
    if profile == "unjailed":
        assert len(sink_requests) == 1, "the real body run did not reach sink-OFF setup"
    return status


def _run_child(tmp_path, name, library, tape, *, profile, restricted, arena=False,
               tracer=None):
    assert not arena or (profile == "jailed" and restricted)
    directory = (tmp_path / name).resolve()
    directory.mkdir()
    cwd, temporary = directory / "cwd", directory / "tmp"
    cwd.mkdir()
    temporary.mkdir()
    canary_fd = arena_fd = -1
    try:
        if restricted:
            cwd.chmod(0o555)
            temporary.chmod(0o555)
            # Root can create in 0555 directories. The zero file-size limit,
            # canary, directory census and mandatory strace still apply.
            canary = directory / "preopened-write-canary"
            canary_fd = _keep_above_arena_slot(os.open(
                canary, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600))
            canary.chmod(0o444)
        if arena:
            arena_fd = _new_arena()
        command = [sys.executable, "-B", str(Path(__file__).resolve()),
                   "--native-child", str(canary_fd), str(arena_fd), profile,
                   "--so", str(library), "--ohlcv", str(tape),
                   "--input-tf", "1", "--script-tf", "15"]
        if tracer:
            # The parent creates the canary and memfd outside this census.
            # Trace the entire child, including imports and the fd-5 remap.
            # stderr is a PIPE: a strace -o file could not grow at FSIZE=0.
            command = [tracer, "-f", "-qq", "-s", "65535", "-e",
                       TRACE_FILTER, "--", *command]
        result = subprocess.run(
            command, cwd=cwd,
            env=_child_environment(cwd, temporary, profile=profile, arena=arena),
            capture_output=True, text=True, check=False,
            preexec_fn=_jail_preexec if restricted else None,
            pass_fds=tuple(fd for fd in (canary_fd, arena_fd) if fd >= 0),
        )
        # Census BEFORE restoring permissions; even an empty leftover fails.
        census = {"cwd": sorted(path.name for path in cwd.iterdir()),
                  "TMPDIR": sorted(path.name for path in temporary.iterdir())}
        (directory / "stdout.json").write_text(result.stdout, encoding="utf-8")
        (directory / "stderr.log").write_text(result.stderr, encoding="utf-8")
        (directory / "census.json").write_text(json.dumps(census), encoding="utf-8")
        print(f"CR281 {name}: exit={result.returncode} census={census} logs={directory}",
              flush=True)
        assert (ARENA_OK if arena else NO_ARENA_OK) in result.stderr, result.stderr
        if restricted:
            assert canary.stat().st_size == 0, f"write canary grew: {canary}"
            receipt = re.search(re.escape(CONSTRAINTS_OK) + r" euid=\d+", result.stderr)
            assert receipt is not None, result.stderr
            print(f"CR281 {name}: {receipt.group(0)}", flush=True)
        if arena:
            assert os.fstat(arena_fd).st_size == ARENA_BYTES
            print(ARENA_OK, flush=True)
        return result, census
    finally:
        for fd in (canary_fd, arena_fd):
            if fd >= 0:
                os.close(fd)
        # Keep artifacts, but allow pytest's ordinary tmp_path cleanup/re-runs.
        cwd.chmod(0o755)
        temporary.chmod(0o755)


def _tracer():
    tracer = shutil.which("strace")
    assert tracer is not None, (
        "CR281 requires strace in PATH: the syscall census must detect even "
        "created-but-unwritten or immediately unlinked scratch files")
    try:
        probe = subprocess.run(
            [tracer, "-f", "-qq", "-s", "65535", "-e", TRACE_FILTER, "--",
             sys.executable, "-B", "-c", "pass"],
            capture_output=True, text=True, check=False)
    except OSError as error:
        raise AssertionError("CR281 requires working strace to trace a child") from error
    assert probe.returncode == 0, (
        "CR281 requires working strace with child ptrace permitted:\n" + probe.stderr)
    assert any(result >= 0 for _, result in _trace_calls(probe.stderr)), (
        "CR281 strace did not trace a successful file syscall in a trivial child")
    return tracer


def _trace_calls(stderr):
    """Join strace's per-pid unfinished/resumed calls; fail closed on gaps."""
    pending, calls = {}, []
    for line in stderr.splitlines():
        prefix = TRACE_PREFIX.fullmatch(line)
        assert prefix is not None, line
        pid, body = prefix.group(1) or prefix.group(2) or "main", prefix.group(3)
        resumed = re.match(r"^<\.\.\. (" + TRACE_NAMES + r") resumed>(.*)$", body)
        if resumed:
            assert pid in pending, f"unmatched resumed syscall: {line}"
            beginning = pending.pop(pid)
            assert beginning.startswith(resumed.group(1) + "("), line
            body = beginning + resumed.group(2)
        elif not TRACE_CALL.match(body):
            assert not body.startswith("strace:"), line
            continue  # The child constraint receipt / failure traceback.
        if body.endswith("<unfinished ...>"):
            assert pid not in pending, line
            pending[pid] = body[:-len("<unfinished ...>")]
            continue
        returned = re.search(r"\)\s+=\s+(-?\d+)\b", body)
        assert returned is not None, f"unparsed syscall result: {line}"
        calls.append((body, int(returned.group(1))))
    assert not pending, f"incomplete syscall census: {pending}"
    assert calls, "strace was present but returned no file-creation/writable-open census"
    return calls


def _assert_no_scratch_opens(stderr, library, tape):
    calls = _trace_calls(stderr)
    successful = [call for call, descriptor in calls if descriptor >= 0]
    opened = [call for call in successful
              if call.startswith(("open(", "openat(", "openat2("))]
    for required_input in (library, tape):
        quoted_path = json.dumps(str(required_input), ensure_ascii=False)
        assert any(quoted_path in call for call in opened), (
            f"strace did not observe the real input being opened: {required_input}")
    forbidden = [call for call in successful
                 if call.startswith(("creat(", "memfd_create(", "mknod(", "mknodat(",
                                     "link(", "linkat(", "symlink(", "symlinkat(",
                                     "rename(", "renameat(", "renameat2(", "mkdir(", "mkdirat("))
                 or re.search(r"\bO_(?:CREAT|TMPFILE|WRONLY|RDWR)\b",
                              re.sub(r'"(?:\\.|[^"\\])*"', '""', call))]
    # No successful creation or writable open ANYWHERE, including transient/
    # unlinked files, O_TMPFILE, child-created memfds and paths made by
    # mknod/link/rename/mkdir. Read-only imports, the module and feed are inputs.
    assert not forbidden, "jailed child created/opened scratch files:\n" + "\n".join(forbidden)
    print(f"CR281 strace: {len(calls)} syscalls; real module/feed opened; "
          "creations=0 writable-opens=0", flush=True)


def _reject_nonfinite(word):
    raise AssertionError("non-finite JSON token: " + word)


def _report(result):
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    document = json.loads(result.stdout, parse_constant=_reject_nonfinite)
    assert document["engine"] == "pineforge" and "error" not in document, document
    assert isinstance(document["trades"], list) and document["trades"], document
    assert document["summary"]["total_trades"] == len(document["trades"]), document
    return document


def _failure(result, code, arguments):
    # Exactly one failure object, with no report body or partial/trailing JSON.
    # Status 1 excludes argparse (2), signals and a successful report.
    assert result.returncode == 1, result.stdout + "\n" + result.stderr
    document = json.loads(result.stdout, parse_constant=_reject_nonfinite)
    expected_keys = {"engine", "error", "code", "args"}
    if code == "report_sink_failed":
        expected_keys.add("reason")  # failure_line's top-level mirror.
        assert document["reason"] == arguments["reason"], document
    assert set(document) == expected_keys, document
    assert document["engine"] == "pineforge", document
    assert isinstance(document["error"], str) and document["error"], document
    assert (document["code"], document["args"]) == (code, arguments), document
    assert "summary" not in document and "trades" not in document, document
    return document


@contextmanager
def _inputs(tmp_path):
    library = _library()
    inputs = tmp_path / "readonly-inputs"
    inputs.mkdir()
    tape = inputs / "chart-1m.csv"
    # Preserve CR-224's three days of the distributed REAL feed and native
    # 1m -> 15m aggregation. No derived feed is read or materialized.
    with FEED.open("rb") as source, tape.open("wb") as target:
        header = next(source)
        assert header.strip() == b"timestamp,open,high,low,close,volume", (
            f"the corpus feed is missing/unhydrated: {FEED}")
        target.write(header)
        count = 0
        for row in islice(source, INPUT_BARS):
            target.write(row)
            count += 1
    assert count == INPUT_BARS, "the corpus feed lacks the probe's three input days"
    tape.chmod(0o444)
    inputs.chmod(0o555)
    try:
        yield library, tape
    finally:
        inputs.chmod(0o755)


def test_a_requires_syscall_census():
    _tracer()  # Fails on absent strace, ptrace denial or an empty census.


def test_census_flags_synthetic_file_calls():
    import pytest

    library, tape = Path("/inputs/probe.so"), Path("/inputs/chart.csv")
    inputs = (
        f'openat(AT_FDCWD, "{library}", O_RDONLY|O_CLOEXEC) = 3\n'
        f'openat(AT_FDCWD, "{tape}", O_RDONLY|O_CLOEXEC) = 4\n')
    forbidden = (
        'openat(AT_FDCWD, "/tmp/unwritten", O_RDONLY|O_CREAT, 0600) = 5',
        'openat(AT_FDCWD, "/tmp", O_RDWR|O_TMPFILE, 0600) = 5',
        'memfd_create("scratch", MFD_CLOEXEC) = 5',
        'openat(AT_FDCWD, "/tmp/existing-unwritten", O_WRONLY) = 5',
        'mknodat(AT_FDCWD, "/tmp/x", S_IFREG|0600, 0) = 0',
        'openat2(AT_FDCWD, "/tmp/x", {flags=O_WRONLY|O_CREAT, mode=0600, resolve=0}, 24) = 3',
        'mknod("/tmp/x", S_IFREG|0600, 0) = 0',
        'link("/inputs/old", "/tmp/x") = 0',
        'linkat(AT_FDCWD, "/inputs/old", AT_FDCWD, "/tmp/x", 0) = 0',
        'rename("/inputs/old", "/tmp/x") = 0',
        'renameat(AT_FDCWD, "/inputs/old", AT_FDCWD, "/tmp/x") = 0',
        'renameat2(AT_FDCWD, "/inputs/old", AT_FDCWD, "/tmp/x", RENAME_NOREPLACE) = 0',
        'symlink("/inputs/old", "/tmp/x") = 0',
        'symlinkat("/inputs/old", AT_FDCWD, "/tmp/x") = 0',
        'mkdir("/tmp/x", 0700) = 0',
        'mkdirat(AT_FDCWD, "/tmp/x", 0700) = 0',
    )
    for call in forbidden:
        with pytest.raises(AssertionError, match=re.escape(call)):
            _assert_no_scratch_opens(inputs + call, library, tape)
    # Neither a failed creation nor the read-only module/tape opens is scratch.
    _assert_no_scratch_opens(
        inputs + 'openat(AT_FDCWD, "/tmp/denied", O_WRONLY|O_CREAT, 0600) = -1 EACCES (Permission denied)',
        library, tape)


def test_census_flags_unlinked_scratch(tmp_path):
    import pytest

    tracer = _tracer()
    with _inputs(tmp_path) as (library, tape):
        reference, census = _run_child(tmp_path, "census-reference", library, tape,
                                      profile="unjailed", restricted=False, tracer=tracer)
        document = _report(reference)
        assert document["diagnostics"]["scratch_arena_bytes"] == 0
        assert census == {"cwd": [], "TMPDIR": []}, census
        scratch_calls = [call for call, result in _trace_calls(reference.stderr)
                         if result >= 0 and call.startswith(("open(", "openat(", "openat2("))
                         and re.search(r'"[^"]*/pineforge-scratch-[^/"]*"', call)
                         and re.search(r"\bO_CREAT\b", call)]
        assert scratch_calls, "unrestricted reference lacks a successful O_CREAT pineforge-scratch-* open"
        # Pass the complete real trace, never a fabricated creation witness.
        with pytest.raises(AssertionError, match=re.escape(scratch_calls[0])):
            _assert_no_scratch_opens(reference.stderr, library, tape)
        print("CR281 census control: detected successful, unlinked scratch:\n"
              + "\n".join(scratch_calls), flush=True)


def test_a_real_jailed_sink_off_with_admitted_arena_creates_no_scratch_file(tmp_path):
    tracer = _tracer()
    with _inputs(tmp_path) as (library, tape):
        reference, _ = _run_child(tmp_path, "reference", library, tape,
                                  profile="unjailed", restricted=False)
        reference_report = _report(reference)
        assert reference_report["diagnostics"]["scratch_arena_bytes"] == 0
        expected = reference_report["trades"]
        print(f"CR281 reference: probe={PROBE.name} native sink-OFF trades={len(expected)}",
              flush=True)
        jailed, census = _run_child(tmp_path, "jailed-with-arena", library, tape,
                                    profile="jailed", restricted=True, arena=True,
                                    tracer=tracer)
        assert census == {"cwd": [], "TMPDIR": []}, census
        _assert_no_scratch_opens(jailed.stderr, library, tape)
        document = _report(jailed)
        diagnostics = document["diagnostics"]
        # An environment variable or exit 0 alone is not arena admission.
        assert diagnostics["scratch_arena_bytes"] == ARENA_BYTES, diagnostics
        assert 0 < diagnostics["scratch_arena_high_water_bytes"] <= ARENA_BYTES, diagnostics
        actual = document["trades"]
        assert len(actual) == len(expected), (actual, expected)
        for index, (got, want) in enumerate(zip(actual, expected)):
            assert got.keys() == want.keys(), (index, got, want)
            for field in want:
                # Exact JSON values, including numeric types and signed zero;
                # every field comes from the live unjailed reference run.
                assert json.dumps(got[field], allow_nan=False) == json.dumps(
                    want[field], allow_nan=False), (index, field, got[field], want[field])
        excluded = {"elapsed_seconds", "diagnostics"}
        actual_report = {key: value for key, value in document.items() if key not in excluded}
        expected_report = {key: value for key, value in reference_report.items() if key not in excluded}
        assert json.dumps(actual_report, sort_keys=True, allow_nan=False) == json.dumps(
            expected_report, sort_keys=True, allow_nan=False), (
                "jailed/reference reports differ beyond elapsed_seconds and diagnostics")
        print(f"CR281 CHECK (a) PASS: arena_bytes={ARENA_BYTES} "
              f"high_water={diagnostics['scratch_arena_high_water_bytes']}; "
              f"trades identical field by field ({len(actual)}); full report identical "
              "except elapsed_seconds/diagnostics; cwd=empty TMPDIR=empty",
              flush=True)


def test_b_real_jailed_without_arena_refuses_before_execution(tmp_path):
    tracer = _tracer()
    with _inputs(tmp_path) as (library, tape):
        refused, census = _run_child(tmp_path, "jailed-no-arena", library, tape,
                                     profile="jailed", restricted=True, tracer=tracer)
        failure = _failure(refused, "report_sink_failed", {"reason": "arena_backing"})
        assert census == {"cwd": [], "TMPDIR": []}, census
        _assert_no_scratch_opens(refused.stderr, library, tape)
        print(f"CR281 CHECK (b) PASS: fd 5 absent; exit=1 code={failure['code']} "
              f"args={failure['args']} reason={failure['reason']}; no report body/trades",
              flush=True)


def test_c_real_unjailed_disk_scratch_negative_control_fails_under_same_limits(tmp_path):
    tracer = _tracer()
    with _inputs(tmp_path) as (library, tape):
        control, census = _run_child(tmp_path, "unjailed-negative-control", library, tape,
                                     profile="unjailed", restricted=True, tracer=tracer)
        assert control.returncode != 0, (
            "CR281 NEGATIVE CONTROL INVALID: native disk scratch succeeded under "
            "the same jail limits; this test cannot establish the regression.\n"
            + control.stdout + "\n" + control.stderr)
        # Reject unrelated failures, including a Python spool error or SIGXFSZ.
        failure = _failure(control, "scratch_io_failed", {})
        assert census == {"cwd": [], "TMPDIR": []}, census
        scratch_calls = [call for call, _ in _trace_calls(control.stderr)
                         if re.search(r'"[^"]*/pineforge-[^/"]*"', call)]
        assert scratch_calls, "the native scratch failure lacks a traced scratch open"
        print("CR281 negative-control scratch syscalls:\n" + "\n".join(scratch_calls),
              flush=True)
        print(f"CR281 CHECK (c) PASS: exit=1 native {failure['code']} "
              f"args={failure['args']}; no report body/trades", flush=True)


if __name__ == "__main__":
    assert sys.argv[1:2] == ["--native-child"], "use pytest for the parent driver"
    raise SystemExit(_native_child(sys.argv[2:]))
