"""One real-native CR-224 regression: sink-OFF execution in APP's file jail.

CTest supplies a module built from the unchanged WIDE corpus probe. The jailed
child uses run_json's actual profile/setter path. Only the unjailed reference
and negative control suppress the Python report-sink factory: run_json has no
sink-OFF CLI switch for that profile. No ctypes loader, export, native state,
scratch binding or execution function is replaced.
"""
from __future__ import annotations

import errno
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
CONSTRAINTS_OK = "CR224 constraints: RLIMIT_FSIZE=0/0 SIGXFSZ=SIG_IGN write=EFBIG pwrite=EFBIG"
TRACE_CALL = re.compile(r"^(openat|open|creat|memfd_create)\(")
TRACE_PREFIX = re.compile(r"^(?:(?:\[pid\s+(\d+)\]|(\d+))\s+)?(.*)$")


def _library():
    configured = os.environ.get(MODULE_ENV)
    if configured is not None:
        # A missing ALL-build target in CTest is a failure, never a green skip.
        assert configured and Path(configured).is_file(), (
            f"CTest did not build its CR224 native module: {configured!r}")
        return Path(configured).resolve()
    candidates = [*sorted(ROOT.glob("build*/cr224_nofile/cr224_nofile_strategy.so")),
                  PROBE / "strategy.so", PROBE / "strategy.dylib"]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    import pytest
    reason = ("CR224 native strategy library is not built; build the CTest "
              f"cr224_nofile_strategy target or supply {MODULE_ENV}. "
              f"Searched: {', '.join(map(str, candidates))}")
    print("SKIP " + reason, flush=True)
    pytest.skip(reason)


def _child_environment(cwd, temporary):
    # An allowlist, not a filtered copy: no inherited writable HOME/cache,
    # spill/output path, loader injection, Python path or profiler destination.
    return {
        "PATH": os.defpath,
        "HOME": str(cwd),
        "TMPDIR": str(temporary), "TEMP": str(temporary), "TMP": str(temporary),
        "XDG_CACHE_HOME": str(temporary), "XDG_CONFIG_HOME": str(temporary),
        "XDG_DATA_HOME": str(temporary), "XDG_STATE_HOME": str(temporary),
        "PYTHONDONTWRITEBYTECODE": "1", "PYTHONNOUSERSITE": "1",
        "PYTHONCOERCECLOCALE": "0",
        "LC_ALL": "C", "TZ": "UTC",
    }


def _jail_preexec():
    # Applied before exec, also when the lab runs as root. Match pf-jail.c's
    # SIGXFSZ handling: a failed regular-file write must return EFBIG, not
    # terminate the process and accidentally satisfy the negative control.
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)


def _check_child_constraints(canary_fd):
    assert resource.getrlimit(resource.RLIMIT_FSIZE) == (0, 0)
    assert signal.getsignal(signal.SIGXFSZ) == signal.SIG_IGN
    cwd = Path.cwd()
    temporary = Path(os.environ["TMPDIR"])
    assert cwd != temporary
    assert os.environ == _child_environment(cwd, temporary)
    for directory in (cwd, temporary):
        assert stat.S_IMODE(directory.stat().st_mode) == 0o555
        assert not list(directory.iterdir()), str(directory)
    # The parent opened this empty regular file before the jail, then made
    # its path read-only. Permission checks cannot cause these write failures.
    # A zero size limit forbids every positive-length regular-file write;
    # exercise both write APIs, including the positional API used by scratch.
    assert stat.S_ISREG(os.fstat(canary_fd).st_mode)
    assert os.fstat(canary_fd).st_size == 0
    try:
        for positional, payload in ((False, b"x"), (True, b"scratch-write-canary")):
            try:
                if positional:
                    os.pwrite(canary_fd, payload, 0)
                else:
                    os.write(canary_fd, payload)
            except OSError as error:
                assert error.errno == errno.EFBIG, repr(error)
            else:
                raise AssertionError("a positive-length jailed file write succeeded")
        assert os.fstat(canary_fd).st_size == 0
    finally:
        os.close(canary_fd)  # No writable regular-file descriptor reaches the engine.
    print(CONSTRAINTS_OK + f" euid={os.geteuid()}", file=sys.stderr, flush=True)


def _native_child(argv):
    canary_fd, profile = int(argv[0]), argv[1]
    if canary_fd >= 0:
        _check_child_constraints(canary_fd)
    sys.path.insert(0, str(ROOT / "docker"))
    import run_json

    sink_requests = []
    if profile == "unjailed":
        # Keep both unjailed runs sink-OFF so their disk requirement is native
        # scratch, rather than Python's report spool. The REAL CDLL and all
        # exports still flow through load_strategy / run_backtest_full.
        def sink_off(lib, state, args):
            assert args.profile == "unjailed"
            sink_requests.append(state)
            return None
        run_json._new_report_sink = sink_off
    status = run_json.main(["--profile", profile, *argv[2:]])
    if profile == "unjailed":
        assert len(sink_requests) == 1, "the real body run did not reach sink-OFF setup"
    return status


def _run_child(tmp_path, name, library, tape, *, profile, restricted, tracer=None):
    directory = (tmp_path / name).resolve()
    directory.mkdir()
    cwd, temporary = directory / "cwd", directory / "tmp"
    cwd.mkdir()
    temporary.mkdir()
    canary_fd = -1
    try:
        if restricted:
            cwd.chmod(0o555)
            temporary.chmod(0o555)
            # Root can create in 0555 directories. The size limit plus the
            # post-run census and (when present) syscall census still apply.
            canary = directory / "preopened-write-canary"
            canary_fd = os.open(canary, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            canary.chmod(0o444)
        command = [sys.executable, "-B", str(Path(__file__).resolve()),
                   "--native-child", str(canary_fd), profile,
                   "--so", str(library), "--ohlcv", str(tape),
                   "--input-tf", "1", "--script-tf", "15"]
        if tracer:
            # mkstemp is a libc function; its open/openat(O_CREAT) is traced.
            # stderr is a PIPE: a strace -o file could not grow under this limit.
            command = [tracer, "-f", "-qq", "-s", "65535", "-e",
                       "trace=openat,open,creat,memfd_create", "--", *command]
        result = subprocess.run(
            command, cwd=cwd, env=_child_environment(cwd, temporary),
            capture_output=True, text=True, check=False,
            preexec_fn=_jail_preexec if restricted else None,
            pass_fds=(canary_fd,) if restricted else (),
        )
        # Census BEFORE restoring permissions; even an empty leftover fails.
        census = {"cwd": sorted(path.name for path in cwd.iterdir()),
                  "TMPDIR": sorted(path.name for path in temporary.iterdir())}
        (directory / "stdout.json").write_text(result.stdout, encoding="utf-8")
        (directory / "stderr.log").write_text(result.stderr, encoding="utf-8")
        (directory / "census.json").write_text(json.dumps(census), encoding="utf-8")
        print(f"CR224 {name}: exit={result.returncode} census={census} logs={directory}",
              flush=True)
        if restricted:
            assert canary.stat().st_size == 0, f"write canary grew: {canary}"
            receipt = re.search(re.escape(CONSTRAINTS_OK) + r" euid=\d+", result.stderr)
            assert receipt is not None, result.stderr
            print(f"CR224 {name}: {receipt.group(0)}", flush=True)
        return result, census
    finally:
        if canary_fd >= 0:
            os.close(canary_fd)
        # Keep artifacts, but allow pytest's ordinary tmp_path cleanup/re-runs.
        cwd.chmod(0o755)
        temporary.chmod(0o755)


def _trace_calls(stderr):
    """Join strace's per-pid unfinished/resumed calls; fail closed on gaps."""
    pending, calls = {}, []
    for line in stderr.splitlines():
        prefix = TRACE_PREFIX.fullmatch(line)
        assert prefix is not None, line
        pid, body = prefix.group(1) or prefix.group(2) or "main", prefix.group(3)
        resumed = re.match(r"^<\.\.\. (openat|open|creat|memfd_create) resumed>(.*)$", body)
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
    assert calls, "strace was present but returned no open/creat/memfd census"
    return calls


def _assert_no_creations(stderr, library, tape):
    calls = _trace_calls(stderr)
    successful = [call for call, descriptor in calls if descriptor >= 0]
    for required_input in (library, tape):
        quoted_path = json.dumps(str(required_input), ensure_ascii=False)
        assert any(quoted_path in call for call in successful), (
            f"strace did not observe the real input being opened: {required_input}")
    creations = [call for call in successful
                 if call.startswith(("creat(", "memfd_create("))
                 or re.search(r"\bO_(?:CREAT|TMPFILE)\b",
                              re.sub(r'"(?:\\.|[^"\\])*"', '""', call))]
    # Stronger than an input allowlist: no successful creation ANYWHERE,
    # including transient/unlinked files, O_TMPFILE and anonymous memfd files.
    assert not creations, "jailed child created files:\n" + "\n".join(creations)
    print(f"CR224 strace: {len(calls)} syscalls; real module/feed opened; creations=0",
          flush=True)


def _reject_nonfinite(word):
    raise AssertionError("non-finite JSON token: " + word)


def _report(result):
    assert result.returncode == 0, result.stdout + "\n" + result.stderr
    document = json.loads(result.stdout, parse_constant=_reject_nonfinite)
    assert document["engine"] == "pineforge" and "error" not in document, document
    assert isinstance(document["trades"], list) and document["trades"], document
    assert document["summary"]["total_trades"] == len(document["trades"]), document
    return document


def test_real_jailed_sink_off_run_without_files(tmp_path):
    library = _library()
    tracer = shutil.which("strace")
    if tracer is None:
        print("CR224 strace: SKIP extra syscall census: strace is absent from PATH; "
              "native execution, EFBIG checks and directory census remain mandatory",
              flush=True)
    inputs = tmp_path / "readonly-inputs"
    inputs.mkdir()
    tape = inputs / "chart-1m.csv"
    # Three days of the distributed REAL feed contain daily entry/timeout
    # opportunities. Native 1m -> 15m aggregation matches the probe's chart.
    # No shared derived feed is read or materialized by this test.
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
        reference, _ = _run_child(tmp_path, "reference", library, tape,
                                  profile="unjailed", restricted=False)
        expected = _report(reference)["trades"]
        print(f"CR224 reference: probe={PROBE.name} native sink-OFF trades={len(expected)}",
              flush=True)
        jailed, census = _run_child(tmp_path, "jailed", library, tape,
                                    profile="jailed", restricted=True, tracer=tracer)
        actual = _report(jailed)["trades"]
        assert census == {"cwd": [], "TMPDIR": []}, census
        if tracer:
            _assert_no_creations(jailed.stderr, library, tape)
        assert len(actual) == len(expected), (actual, expected)
        for index, (got, want) in enumerate(zip(actual, expected)):
            assert got.keys() == want.keys(), (index, got, want)
            for field in want:
                # Exact JSON values, including numeric types and signed zero;
                # every field comes from the live unjailed reference run.
                assert json.dumps(got[field], allow_nan=False) == json.dumps(
                    want[field], allow_nan=False), (index, field, got[field], want[field])
        print(f"CR224 jailed: trades identical field by field ({len(actual)}); "
              "cwd=empty TMPDIR=empty", flush=True)
        control, _ = _run_child(tmp_path, "negative-control", library, tape,
                               profile="unjailed", restricted=True, tracer=tracer)
        assert control.returncode != 0, (
            "CR224 NEGATIVE CONTROL INVALID: the same native disk-scratch probe "
            "succeeded under the jail; this test cannot establish the regression.\n"
            + control.stdout + "\n" + control.stderr)
        failure = json.loads(control.stdout, parse_constant=_reject_nonfinite)
        # Reject unrelated nonzero exits (including Python report-spool errors
        # and SIGXFSZ termination): native scratch must report the actual fault.
        assert failure["code"] == "scratch_io_failed", (failure, control.stderr)
        assert "summary" not in failure and "trades" not in failure, failure
        print(f"CR224 negative-control: native {failure['code']}: {failure['error']}",
              flush=True)
        if tracer:
            scratch_calls = [call for call, _ in _trace_calls(control.stderr)
                             if re.search(r'"[^"]*/pineforge-[^/"]*"', call)]
            assert scratch_calls, "the native scratch failure lacks a traced scratch open"
            print("CR224 negative-control scratch syscalls:\n" + "\n".join(scratch_calls),
                  flush=True)
    finally:
        inputs.chmod(0o755)


if __name__ == "__main__":
    assert sys.argv[1:2] == ["--native-child"], "use pytest for the parent driver"
    raise SystemExit(_native_child(sys.argv[2:]))
