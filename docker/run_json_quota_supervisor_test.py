"""CLOUD: real non-root quota-supervisor admission and parser checks.

Included in the run_json_*_test.py preflight discovery glob. Real kernel pidfd
support is required, with or without the interpreter's native pidfd functions.
No uid transition or successful same-uid launch is used. Invalid policy must
produce its typed parent-pipe admission refusal even without privilege. ROOT
PROOF lives in pf_quota_run_privileged_proof.py and must be run in full
separately.
Source inventory: 129 CLOUD cases in 15 families; 176 ROOT cases in 37
families. These counts are authoring metadata until real collection/execution.

The inventory row collects that entire file; collection is not execution or
a privileged verdict. No product is imported into pytest. All invocations
list their explicit followed files, refuse sensitive names, use isolated
interpreters, and write no marker, cache, disk fixture or temporary script.
"""

from __future__ import annotations

import ast
from collections import Counter
import ctypes
import errno
import fcntl
import json
import math
import os
from pathlib import Path
import selectors
import signal
import stat
import subprocess
import sys

import pytest


ROOT = Path(__file__).absolute().parents[1]
SUPERVISOR = ROOT / "docker" / "pf_quota_run.py"
CLIENT = ROOT / "docker" / "run_quota.py"
PROOF = ROOT / "docker" / "pf_quota_run_privileged_proof.py"
SCHEMA = "pf-quota-receipt/v1"
LINE = "quota=managed enforcement=cooperative"
COUNTER_MAX = 9007199254740991
LIMITS = (
    ("--scratch-bytes", "scratch_bytes"),
    ("--scratch-files", "scratch_files"),
    ("--output-bytes", "output_bytes"),
    ("--descriptor-limit", "descriptors"),
)
POLICY = ("65536", "64", "65536", "64")
FIELDS = (
    "schema", "quota_line", "ledger_verdict", "wait_status", "exit_code",
    "signal", "oom", "deadline", "launcher_failure",
)
VALID_LIMITS = ("0", "0007", "9007199254740991", "0009007199254740991")
INVALID_LIMITS = (
    ("", "malformed_limit"), ("-1", "malformed_limit"),
    ("+1", "malformed_limit"), (" 1", "malformed_limit"),
    ("1 ", "malformed_limit"), ("1.0", "malformed_limit"),
    ("1e1", "malformed_limit"), ("0x1", "malformed_limit"),
    ("\uff11", "malformed_limit"), ("9007199254740992", "counter_overflow"),
    ("1_0", "malformed_limit"), ("9" * 5001, "counter_overflow"),
)
INVALID_IDS = (
    "empty", "negative", "sign", "leading-space", "trailing-space",
    "fraction", "exponent", "hex", "unicode", "overflow", "underscore", "long-overflow",
)
BAD_DEADLINES = (
    "", "0", "-1", "nan", "inf", "-inf", "1e309", "not-seconds",
    "+1", "-0.5", "1_0", "1.0_0",
    " 1", "1 ", "1 0", "\t1", "1\t", "1\n", "\n1", "1\r\n",
    "1e1", "1E1", ".5", "1.", ".", "0.0", "00.000", "\uff11", "1.\uff11",
    "9" * 400, "0." + "0" * 400 + "1",
)
VALID_DEADLINES = ("20", "00020", "20.5", "00020.50")
BAD_RECEIPTS = ("missing", "closed", "readonly", "negative", "standard", "text")
# Admission-only identifiers, never an account fallback or a success fixture.
ADMISSION_UID = "2147483646"
ADMISSION_GID = "2147483646"

# Source inventory, not a runtime verdict. Keep in sync with the explicit file.
# Both kernel-cgroup cases require memory.swap.max; neither may be omitted.
ROOT_FAMILY_CASES = {
    "test_supervisor_path_is_present_before_invocation_or_privileged_setup": 1,
    "test_normal_exit_preserves_caller_streams_and_raw_wait": 6,
    "test_normal_exit_without_native_pidfd_preserves_receipt_and_wait": 1,
    "test_actual_receipt_field_presence_and_types": 1,
    "test_receipt_pipe_identity_is_absent_in_child_and_forked_descendant": 1,
    "test_child_forged_t_receipt_is_forwarded_but_never_authoritative": 2,
    "test_real_descendant_client_exceeds_explicit_output_policy": 3,
    "test_real_signal_and_deliberate_crash_retain_raw_wait": 2,
    "test_deadline_actually_sigkills_and_reaps_owned_group": 1,
    "test_deadline_without_native_pidfd_sigkills_and_reaps_owned_group": 1,
    "test_absent_or_non_cgroup_memory_events_path_is_unknown": 3,
    "test_normal_first_child_exit_kills_and_reaps_setsid_double_fork": 1,
    "test_complete_supervisor_reaps_escaped_tree_under_pidfd_signal_filter": 4,
    "test_adopted_cleanup_survives_real_pidfd_failures": 2,
    "test_exec_child_restores_sigxfsz_and_reports_real_signal": 1,
    "test_explicit_policy_grammar_accepts_zero_leading_zero_and_full_range": 16,
    "test_invalid_caller_policy_refuses_before_spawning": 48,
    "test_every_caller_policy_is_mandatory_without_a_default": 4,
    "test_supervisor_rss_stays_flat_over_real_completed_protocol_cycles": 1,
    "test_explicit_pass_fd_preserves_preopened_channel": 1,
    "test_receipt_forwarding_collision_refuses_before_spawn": 2,
    "test_root_child_identity_is_forbidden": 2,
    "test_child_identity_is_mandatory": 2,
    "test_absent_deadline_launches_without_inventing_budget": 1,
    "test_valid_same_uid_nonmember_hello_is_refused": 2,
    "test_full_range_off_by_one_kernel_identity_hello_is_refused": 12,
    "test_native_and_python_endpoints_share_parent_ledger": 1,
    "test_full_range_kernel_identities_use_both_endpoints_and_shared_ledger": 6,
    "test_real_cgroup_v2_oom_has_attributable_kernel_evidence": 1,
    "test_populated_owned_cgroup_memory_events_stays_unknown": 1,
    "test_invalid_deadline_refuses_without_inventing_budget": len(BAD_DEADLINES),
    "test_plain_integer_and_decimal_deadlines_launch_normally": len(VALID_DEADLINES),
    "test_bad_receipt_carrier_fails_without_child_or_stdout_fallback": 6,
    "test_child_receipt_option_cannot_supply_missing_launcher_value": 2,
    "test_child_receipt_options_after_boundary_remain_child_argv": 1,
    "test_spawn_failure_is_parent_owned_and_has_actual_wait": 1,
    "test_stdin_passes_through_to_commanded_child": 1,
}


def _permitted(path):
    path = Path(path).absolute()
    parts = path.parts
    assert not (
        any(p in (".ssh", ".aws", ".codex") or p.endswith(".env") for p in parts)
        or any(parts[i:i + 3] == (".config", "zsh", "keys") for i in range(len(parts)))
        or any(parts[i:i + 2] == (".config", "pineforge") for i in range(len(parts)))
    ), f"REFUSED sensitive path by name: {path}"
    return path


def _files_before_run(*paths):
    """Announce names before following; inspect each symlink target by name."""
    for path in paths:
        pending = _permitted(path)
        print(f"FILES followed by quota test: {pending}", file=sys.stderr, flush=True)
        links = 0
        while True:
            current = Path(pending.anchor)
            for index, part in enumerate(pending.parts[1:], 1):
                current = _permitted(current / part)
                if stat.S_ISLNK(os.lstat(current).st_mode):
                    links += 1
                    assert links <= 40, "REFUSED symlink cycle"
                    target = Path(os.readlink(current))
                    if not target.is_absolute():
                        target = current.parent / target
                    pending = _permitted(target.joinpath(*pending.parts[index + 1:]))
                    pending = _permitted(os.path.normpath(pending))
                    print(f"FILES symlink target followed: {pending}",
                          file=sys.stderr, flush=True)
                    break
            else:
                break


def _require_supervisor():
    try:
        _files_before_run(SUPERVISOR)
    except FileNotFoundError:
        pytest.fail(f"MISSING SUPERVISOR: {SUPERVISOR}")
    assert _permitted(SUPERVISOR).is_file(), f"MISSING SUPERVISOR: {SUPERVISOR}"


def _kernel_pidfd(pid):
    """Independent host probe/pin; never import the supervisor into pytest."""
    native = getattr(os, "pidfd_open", None)
    if native is not None:
        return native(pid, 0)
    assert (sys.platform == "linux" and ctypes.sizeof(ctypes.c_void_p) == 8
            and os.uname().machine.lower() in ("x86_64", "amd64", "aarch64", "arm64")), (
        "SETUP: no native pidfd_open or supported Linux syscall ABI")
    syscall = ctypes.CDLL(None, use_errno=True).syscall
    syscall.restype = ctypes.c_long
    syscall.argtypes = (ctypes.c_long,)
    result = syscall(434, ctypes.c_int(pid), ctypes.c_uint(0))
    if result == -1:
        number = ctypes.get_errno()
        raise OSError(number, os.strerror(number))
    return result


def _require_kernel_pidfds():
    try:
        descriptor = _kernel_pidfd(os.getpid())
        try:
            native = getattr(signal, "pidfd_send_signal", None)
            if native is not None:
                assert native(descriptor, 0) is None
            else:
                assert (ctypes.sizeof(ctypes.c_void_p) == 8
                        and os.uname().machine.lower() in
                        ("x86_64", "amd64", "aarch64", "arm64")), (
                    "SETUP: no native pidfd_send_signal or supported Linux syscall ABI")
                syscall = ctypes.CDLL(None, use_errno=True).syscall
                syscall.restype = ctypes.c_long
                syscall.argtypes = (ctypes.c_long,)
                result = syscall(424, ctypes.c_int(descriptor), ctypes.c_int(0),
                                 ctypes.c_void_p(), ctypes.c_uint(0))
                if result == -1:
                    number = ctypes.get_errno()
                    raise OSError(number, os.strerror(number))
                assert result == 0, "SETUP: unexpected pidfd_send_signal return"
        finally:
            os.close(descriptor)
    except OSError as error:
        pytest.fail(f"SETUP: real kernel pidfd syscalls are required: {error}")


@pytest.fixture(scope="module", autouse=True)
def cloud_host():
    _require_supervisor()
    assert sys.platform == "linux", "SETUP: CLOUD requires real Linux credentials"
    assert hasattr(os, "getresuid") and hasattr(os, "getresgid")
    uids, gids = os.getresuid(), os.getresgid()
    assert 0 < uids[0] <= 2147483647 and uids == (uids[0],) * 3, (
        "SETUP: CLOUD requires an ordinary non-root uid with no saved privilege", uids)
    assert 0 < gids[0] <= 2147483647 and gids == (gids[0],) * 3, (
        "SETUP: CLOUD requires an ordinary non-root gid with no saved privilege", gids)
    status_path = Path("/proc/self/status")
    _files_before_run(status_path)
    status = status_path.read_text(encoding="ascii")
    for label, value in (("Uid:", uids[0]), ("Gid:", gids[0])):
        fields = next(line for line in status.splitlines() if line.startswith(label))
        assert tuple(map(int, fields.split()[1:])) == (value,) * 4, (
            "SETUP: CLOUD kernel credentials differ from host identity", fields)


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, f"duplicate parent receipt key: {key}"
        result[key] = value
    return result


def _bad_constant(token):
    raise AssertionError(f"non-JSON parent receipt constant: {token}")


def _document(wire, seconds):
    assert wire and wire.endswith(b"\n") and wire.count(b"\n") == 1
    document = json.loads(wire.decode("ascii"), object_pairs_hook=_unique_object,
                          parse_constant=_bad_constant)
    assert type(document) is dict and set(FIELDS) <= document.keys()
    assert document["schema"] == SCHEMA and document["quota_line"] == LINE
    for name in ("wait_status", "exit_code", "signal"):
        assert document[name] is None or type(document[name]) is int
    for name in ("ledger_verdict", "launcher_failure"):
        value = document[name]
        if value is not None:
            assert type(value) is dict
            assert type(value["code"]) is str and value["code"]
            assert type(value["args"]) is dict
    assert type(document["oom"]) is dict
    assert type(document["oom"]["proven"]) is bool
    assert {key: value for key, value in document["oom"].items()
            if key != "diagnostic"} == {
        "proven": False, "before": None, "after": None, "delta": None,
    }, "pre-launch refusal has no attributable OOM observation"
    assert document["oom"].get("diagnostic") is None
    deadline = document["deadline"]
    assert type(deadline) is dict
    assert type(deadline["expired"]) is bool and deadline["expired"] is False
    assert type(deadline["kill_sent"]) is bool and deadline["kill_sent"] is False
    if seconds is None:
        assert deadline["seconds"] is None, "missing/invalid budget became invented seconds"
    else:
        assert type(deadline["seconds"]) in (int, float)
        assert math.isfinite(deadline["seconds"]) and deadline["seconds"] == seconds > 0
    return document


def _kernel_identity(pid):
    path = Path("/proc") / str(pid) / "stat"
    _files_before_run(path)
    fields = path.read_text(encoding="ascii").rsplit(")", 1)[1].split()
    return int(fields[1]), int(fields[2]), int(fields[3]), int(fields[19])


def _admission(*, policy=POLICY, omit=None, overrides=None,
               collision=None, receipt_kind=None, expected_seconds=None,
               child_receipt_style=None, hide_pidfd=False, unsupported_machine=False):
    """Invoke the real CLI without a default deadline; trust its parent pipe."""
    _require_supervisor()
    assert sys.platform == "linux", "SETUP: CLOUD requires Linux procfs/pidfds"
    _require_kernel_pidfds()
    assert os.geteuid() != int(ADMISSION_UID), "SETUP: admission sentinel uid collision"
    _files_before_run(SUPERVISOR, CLIENT, sys.executable)
    read_fd, low_writer = os.pipe2(os.O_CLOEXEC)
    writer = alias = process = pidfd = None
    try:
        writer = fcntl.fcntl(low_writer, fcntl.F_DUPFD_CLOEXEC, 256)
        os.close(low_writer)
        low_writer = None
        carrier = str(writer)
        passed = [writer]
        if receipt_kind == "closed":
            alias = fcntl.fcntl(writer, fcntl.F_DUPFD_CLOEXEC, writer + 32)
            carrier = str(alias)
            os.close(alias)
            alias = None
        elif receipt_kind == "readonly":
            carrier = str(read_fd)
            passed.append(read_fd)
        elif receipt_kind == "negative":
            carrier = "-1"
        elif receipt_kind == "standard":
            carrier = "2"
        elif receipt_kind == "text":
            carrier = "not-a-descriptor"
        argv = [sys.executable, "-I", "-B", str(SUPERVISOR)]
        if receipt_kind not in ("missing", "missing-value"):
            argv.extend(("--receipt-fd", carrier))
        options = dict(zip((flag for flag, _ in LIMITS), policy))
        options.update({"--child-uid": ADMISSION_UID, "--child-gid": ADMISSION_GID})
        # Only deadline grammar rows supply that option through overrides.
        options.update(overrides or {})
        # --flag=value also delivers negative/text values to argparse as values.
        for flag, value in options.items():
            if flag != omit:
                argv.append(flag + "=" + value)
        if collision is not None:
            forwarded = writer
            if collision == "alias":
                alias = os.dup(writer)
                passed.append(alias)
                forwarded = alias
            argv.extend(("--pass-fd", str(forwarded)))
        if receipt_kind == "missing-value":
            argv.append("--receipt-fd")
        argv.extend(("--", sys.executable, "-I", "-B", "-c",
                     'import os; os.write(1,b"UNEXPECTED CHILD\\n"); raise SystemExit(93)'))
        if child_receipt_style == "separate":
            argv.extend(("--receipt-fd", str(writer)))
        elif child_receipt_style == "equals":
            argv.append("--receipt-fd=" + str(writer))
        if hide_pidfd:
            wrapper = """
import importlib.util, os, signal, sys
for owner, name in ((os, "pidfd_open"), (signal, "pidfd_send_signal")):
    if hasattr(owner, name):
        delattr(owner, name)
spec = importlib.util.spec_from_file_location("_quota_cli", sys.argv[1])
product = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = product
spec.loader.exec_module(product)
"""
            if unsupported_machine:
                # Substitute only the architecture input; execute the real
                # capability check and CLI, without replacing either.
                wrapper += """
assert callable(product._machine)
product._machine = lambda: "unsupported"
"""
            wrapper += "\nsys.exit(product.main(sys.argv[2:]))\n"
            argv = [sys.executable, "-I", "-B", "-c", wrapper, *argv[3:]]
        process = subprocess.Popen(
            argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            pass_fds=tuple(passed), env={"PATH": os.defpath, "LC_ALL": "C"},
            cwd=ROOT, start_new_session=True, bufsize=0,
        )
        os.close(writer)
        writer = None
        if alias is not None:
            os.close(alias)
            alias = None
        pidfd = _kernel_pidfd(process.pid)
        identity = _kernel_identity(process.pid)
        assert identity[0] == os.getpid() and identity[3] > 0
        buffers = {"stdout": bytearray(), "stderr": bytearray(), "receipt": bytearray()}
        with selectors.DefaultSelector() as poller:
            for fd, name in ((process.stdout.fileno(), "stdout"),
                             (process.stderr.fileno(), "stderr"), (read_fd, "receipt")):
                os.set_blocking(fd, False)
                poller.register(fd, selectors.EVENT_READ, name)
            while poller.get_map():
                for key, _ in poller.select():
                    chunk = os.read(key.fd, 4096)
                    if not chunk:
                        poller.unregister(key.fd)
                        continue
                    buffers[key.data].extend(chunk)
                    assert len(buffers[key.data]) <= 65536, "unbounded admission output"
        # Still unreaped: compare the same kernel process generation, then wait.
        assert _kernel_identity(process.pid) == identity
        returncode = process.wait()
        assert returncode != 0, "admission refusal falsely returned success"
        assert not buffers["stdout"], "unexpected child launch or receipt stdout fallback"
        assert not buffers["stderr"], "unexpected launcher stderr"
        if receipt_kind is not None:
            assert not buffers["receipt"], "unusable receipt carrier produced a substitute receipt"
            return None
        document = _document(bytes(buffers["receipt"]), expected_seconds)
        assert all(document[name] is None for name in
                   ("wait_status", "exit_code", "signal", "ledger_verdict"))
        assert document["launcher_failure"] is not None
        return document
    finally:
        if process is not None:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            for stream in (process.stdout, process.stderr):
                stream.close()
        for fd in (read_fd, low_writer, writer, alias, pidfd):
            if fd is not None:
                os.close(fd)


def _policy_failure(document, reason, setting):
    assert document["launcher_failure"] == {
        "code": "run_quota_configuration_invalid",
        "args": {"stage": "admission", "reason": reason, "setting": setting},
    }


def _launcher_failure(document, reason):
    failure = document["launcher_failure"]
    assert failure["code"] == "quota_launcher_error"
    assert failure["args"]["stage"] == "admission"
    assert failure["args"]["reason"] == reason


PIDFD_CASES = (
    "open-lifecycle", "open-reaped", "open-invalid-flags", "send-lifecycle",
    "send-nonpidfd", "native-comparison", "cli-admission", "unsupported-architecture",
)


def _pidfd_contract_program():
    return """
import array, errno, fcntl, importlib.util, os, select, signal, socket, sys, traceback
native_open = getattr(os, "pidfd_open", None)
native_send = getattr(signal, "pidfd_send_signal", None)
for owner, name in ((os, "pidfd_open"), (signal, "pidfd_send_signal")):
    if hasattr(owner, name):
        delattr(owner, name)
spec = importlib.util.spec_from_file_location("_quota_pidfd", sys.argv[1])
product = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = product
spec.loader.exec_module(product)
open_fd, send_signal = product._pidfd_open, product._pidfd_send_signal

def error_of(function, *arguments):
    try:
        function(*arguments)
    except OSError as error:
        return type(error), error.errno, error.strerror
    raise AssertionError("kernel error was lost")

def expected_error(kind, number):
    return kind, number, os.strerror(number)

def finish_child(pid):
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)

def open_lifecycle(open_fd, send_signal):
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    pid = os.fork()
    if pid == 0:
        parent.close()
        try:
            # The target opens its OWN pid. Check CLOEXEC before SCM_RIGHTS,
            # whose receiver independently requests CLOEXEC on its new copy.
            descriptor = open_fd(os.getpid(), 0)
            assert type(descriptor) is int and descriptor >= 0
            assert fcntl.fcntl(descriptor, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
            assert os.get_inheritable(descriptor) is False
            assert child.sendmsg([b"p"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                         array.array("i", [descriptor]))]) == 1
            os.close(descriptor)
            assert child.recv(1) == b"."
        except BaseException:
            traceback.print_exc()
            os._exit(1)
        os._exit(0)
    child.close()
    descriptor = None
    reaped = False
    try:
        size = array.array("i").itemsize
        payload, controls, flags, _ = parent.recvmsg(
            1, socket.CMSG_SPACE(size), socket.MSG_CMSG_CLOEXEC)
        assert payload == b"p" and not flags & (socket.MSG_CTRUNC | socket.MSG_TRUNC)
        assert len(controls) == 1
        level, kind, raw = controls[0]
        assert (level, kind) == (socket.SOL_SOCKET, socket.SCM_RIGHTS)
        descriptors = array.array("i")
        descriptors.frombytes(raw)
        assert len(descriptors) == 1
        descriptor = descriptors[0]
        assert os.get_inheritable(descriptor) is False
        poller = select.poll()
        poller.register(descriptor, select.POLLIN)
        assert poller.poll(0) == [], "pidfd readable while target waits at barrier"
        assert parent.send(b".") == 1
        waited, status = os.waitpid(pid, 0)
        reaped = True
        assert waited == pid and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
        # Wait on the actual target before checking readiness, so a wrong
        # descriptor fails instead of blocking forever on an unrelated pid.
        events = poller.poll(0)
        assert len(events) == 1 and events[0][0] == descriptor
        assert events[0][1] & select.POLLIN
        return type(descriptor), os.get_inheritable(descriptor), status
    finally:
        parent.close()
        if not reaped:
            finish_child(pid)
        if descriptor is not None:
            os.close(descriptor)

def open_reaped(open_fd, send_signal):
    pid = os.fork()
    if pid == 0:
        os._exit(0)
    waited, status = os.waitpid(pid, 0)
    assert waited == pid and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
    result = error_of(open_fd, pid, 0)
    assert result == expected_error(ProcessLookupError, errno.ESRCH)
    return result

def open_invalid_flags(open_fd, send_signal):
    result = error_of(open_fd, os.getpid(), 1)
    assert result == expected_error(OSError, errno.EINVAL)
    return result

def send_lifecycle(open_fd, send_signal):
    reader, writer = os.pipe2(os.O_CLOEXEC)
    pid = os.fork()
    if pid == 0:
        os.close(writer)
        os.read(reader, 1)
        os._exit(0)
    os.close(reader)
    descriptor = None
    reaped = False
    try:
        descriptor = open_fd(pid, 0)
        poller = select.poll()
        poller.register(descriptor, select.POLLIN)
        assert poller.poll(0) == []
        live_return = send_signal(descriptor, 0)
        assert live_return is None
        assert poller.poll(0) == [], "signal zero killed the live child"
        invalid_signal = error_of(send_signal, descriptor, signal.NSIG)
        assert invalid_signal == expected_error(OSError, errno.EINVAL)
        kill_return = send_signal(descriptor, signal.SIGKILL)
        assert kill_return is None
        # EOF lets a broken, non-signaling implementation finish too; its
        # normal wait status must then fail the real SIGKILL assertion.
        os.close(writer)
        writer = None
        waited, status = os.waitpid(pid, 0)
        reaped = True
        assert waited == pid and os.WIFSIGNALED(status)
        assert os.WTERMSIG(status) == signal.SIGKILL
        dead_zero = error_of(send_signal, descriptor, 0)
        dead_kill = error_of(send_signal, descriptor, signal.SIGKILL)
        assert dead_zero == dead_kill == expected_error(ProcessLookupError, errno.ESRCH)
        return live_return, invalid_signal, kill_return, status, dead_zero, dead_kill
    finally:
        if writer is not None:
            os.close(writer)
        if not reaped:
            finish_child(pid)
        if descriptor is not None:
            os.close(descriptor)

def send_nonpidfd(open_fd, send_signal):
    reader, writer = os.pipe2(os.O_CLOEXEC)
    try:
        result = error_of(send_signal, reader, 0)
        assert result == expected_error(OSError, errno.EBADF)
    finally:
        os.close(reader)
        os.close(writer)
    closed = error_of(send_signal, reader, 0)
    assert closed == result
    return result, closed

exercises = {
    "open-lifecycle": open_lifecycle,
    "open-reaped": open_reaped,
    "open-invalid-flags": open_invalid_flags,
    "send-lifecycle": send_lifecycle,
    "send-nonpidfd": send_nonpidfd,
}
case = sys.argv[2]
if case == "native-comparison":
    observed = {name: exercise(open_fd, send_signal)
                for name, exercise in exercises.items()}
    if native_open is not None and native_send is not None:
        native = {name: exercise(native_open, native_send)
                  for name, exercise in exercises.items()}
        assert observed == native, (observed, native)
    else:
        # Only the native COMPARISON is unavailable. Every kernel assertion
        # above still executed; this row is never omitted on older Python.
        assert set(observed) == set(exercises)
        print("native comparison unavailable; all kernel checks completed")
else:
    exercises[case](open_fd, send_signal)
"""


def _run_pidfd_program(case):
    _require_supervisor()
    _files_before_run(SUPERVISOR, sys.executable)
    process = subprocess.Popen(
        [sys.executable, "-I", "-B", "-c", _pidfd_contract_program(),
         str(SUPERVISOR), case],
        cwd=ROOT, env={"PATH": os.defpath, "LC_ALL": "C"},
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True, bufsize=0,
    )
    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    try:
        with selectors.DefaultSelector() as poller:
            for stream, name in ((process.stdout, "stdout"), (process.stderr, "stderr")):
                os.set_blocking(stream.fileno(), False)
                poller.register(stream, selectors.EVENT_READ, name)
            while poller.get_map():
                for key, _ in poller.select():
                    chunk = os.read(key.fd, 4096)
                    if not chunk:
                        poller.unregister(key.fd)
                        continue
                    buffers[key.data].extend(chunk)
                    assert len(buffers[key.data]) <= 65536, "unbounded pidfd proof output"
        assert process.wait() == 0, bytes(buffers["stderr"]).decode(
            "ascii", "backslashreplace")
        assert not buffers["stderr"]
        expected = (
            b"native comparison unavailable; all kernel checks completed\n"
            if case == "native-comparison" and not (
                hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"))
            else b""
        )
        assert bytes(buffers["stdout"]) == expected
    finally:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        process.stdout.close()
        process.stderr.close()


@pytest.mark.parametrize("case", PIDFD_CASES)
def test_pidfd_fallback_matches_real_kernel(case):
    if case == "cli-admission":
        _launcher_failure(_admission(hide_pidfd=True), "privileged_launcher_required")
    elif case == "unsupported-architecture":
        _launcher_failure(
            _admission(hide_pidfd=True, unsupported_machine=True),
            "linux_containment_unavailable")
    else:
        _run_pidfd_program(case)


def test_supervisor_path_is_present_before_invocation_or_privileged_setup():
    _require_supervisor()


@pytest.mark.parametrize("index", range(4))
@pytest.mark.parametrize("text,reason", INVALID_LIMITS, ids=INVALID_IDS)
def test_invalid_caller_policy_refuses_before_spawning(index, text, reason):
    policy = list(POLICY)
    policy[index] = text
    _policy_failure(_admission(policy=policy), reason, LIMITS[index][1])


@pytest.mark.parametrize("flag,setting", LIMITS)
def test_every_caller_policy_is_mandatory_without_a_default(flag, setting):
    _policy_failure(_admission(omit=flag), "missing_limit", setting)


def test_actual_parent_refusal_receipt_has_all_fields_and_types():
    # One actual document observation, not nine claimed independent mutations.
    _policy_failure(_admission(omit="--scratch-bytes"), "missing_limit", "scratch_bytes")


@pytest.mark.parametrize("flag", ("--child-uid", "--child-gid"))
def test_child_identity_is_mandatory(flag):
    _launcher_failure(_admission(omit=flag), "invalid_integer")


def test_absent_deadline_reaches_privileged_launcher_admission_without_budget():
    document = _admission(omit="--deadline-seconds", expected_seconds=None)
    _launcher_failure(document, "privileged_launcher_required")


@pytest.mark.parametrize("flag", ("--child-uid", "--child-gid"))
def test_root_child_identity_is_forbidden(flag):
    _launcher_failure(_admission(overrides={flag: "0"}), "child_identity_not_separate")


@pytest.mark.parametrize("text", BAD_DEADLINES)
def test_invalid_deadline_refuses_without_inventing_budget(text):
    document = _admission(overrides={"--deadline-seconds": text}, expected_seconds=None)
    assert document["launcher_failure"] == {
        "code": "quota_launcher_error",
        "args": {"stage": "admission", "reason": "invalid_deadline"},
    }


@pytest.mark.parametrize("text", VALID_DEADLINES)
def test_plain_integer_and_decimal_deadlines_reach_policy_admission(text):
    # The real CLI must accept the complete spelling before the deliberately
    # missing policy refuses. No root facility or successful child is needed.
    # This refusal precedes the CLI's first expiry check, regardless of delay.
    document = _admission(
        omit="--scratch-bytes", overrides={"--deadline-seconds": text},
        expected_seconds=float(text))
    _policy_failure(document, "missing_limit", "scratch_bytes")


@pytest.mark.parametrize("kind", ("exact", "alias"))
def test_receipt_forwarding_collision_refuses_before_spawn(kind):
    _launcher_failure(_admission(collision=kind), "receipt_fd_collision")


@pytest.mark.parametrize("kind", BAD_RECEIPTS)
def test_bad_receipt_carrier_fails_without_child_or_stdout_fallback(kind):
    _admission(receipt_kind=kind)


@pytest.mark.parametrize("style", ("separate", "equals"))
def test_child_receipt_option_cannot_supply_missing_launcher_value(style):
    # The inherited writer is usable, but appears ONLY in child argv after
    # the launcher's missing value and `--`. It must receive no receipt.
    _admission(receipt_kind="missing-value", child_receipt_style=style)


@pytest.mark.parametrize("index", range(4))
@pytest.mark.parametrize("text", VALID_LIMITS)
def test_real_shipped_parser_retains_zero_leading_zero_and_full_range(index, text):
    # This proves parser semantics only. ROOT PROOF separately observes all
    # four limits from the actual supervisor ledger through its shipped client.
    _require_supervisor()
    _files_before_run(CLIENT, sys.executable)
    policy = list(POLICY)
    policy[index] = text
    code = f"""
import json, os, sys
sys.path.insert(0, {str(CLIENT.parent)!r})
import run_quota as q
assert os.path.abspath(q.__file__) == {str(CLIENT)!r}, "unrelated quota module imported"
values = q.parse_limits(dict(zip(q.LIMIT_ENV, {tuple(policy)!r}))).values()
print(json.dumps(values, separators=(",", ":")))
"""
    result = subprocess.run(
        [sys.executable, "-I", "-B", "-c", code], cwd=ROOT,
        env={"PATH": os.defpath, "LC_ALL": "C"}, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr.decode("ascii", "backslashreplace")
    assert not result.stderr
    observed = json.loads(result.stdout.decode("ascii"))
    assert all(type(value) is int for value in observed)
    assert observed == [int(value) for value in policy]


def test_complete_root_proof_collection_inventory_is_not_execution():
    _require_supervisor()
    _files_before_run(PROOF, CLIENT, SUPERVISOR, sys.executable)
    source = PROOF.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(PROOF))
    for node in ast.walk(tree):
        if isinstance(node, ast.Attribute):
            assert node.attr not in ("skip", "skipif", "xfail", "importorskip")
    result = subprocess.run(
        [sys.executable, "-I", "-B", "-m", "pytest", "--noconftest",
         "-p", "no:cacheprovider", "--collect-only", "-q", "-o", "addopts=",
         str(PROOF)],
        cwd=ROOT, env={"PATH": os.defpath, "LC_ALL": "C",
                       "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1", "PYTHONDONTWRITEBYTECODE": "1"},
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, (result.stdout + result.stderr).decode("ascii", "backslashreplace")
    nodes = [line for line in result.stdout.decode("ascii").splitlines()
             if "::test_" in line]
    assert len(nodes) == len(set(nodes)) == sum(ROOT_FAMILY_CASES.values())
    families = Counter(line.rsplit("::", 1)[1].split("[", 1)[0] for line in nodes)
    assert dict(families) == ROOT_FAMILY_CASES, "incomplete/changed ROOT PROOF inventory"
    print("ROOT PROOF collection only; no execution verdict: " + str(dict(families)))
