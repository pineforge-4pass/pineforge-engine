"""ROOT PRIVILEGED PROOF: invoke this COMPLETE file explicitly on the root box.

Outside the run_json_*_test.py discovery glob by design. All original families
are retained here; the non-root sibling run_json_quota_supervisor_test.py also
runs genuine admission rows.
No filtering or runtime facility omission yields a privileged verdict.
Source inventory: 176 cases in 37 families, all required; collection is UNRUN
until the complete file is actually collected on the validation host.
Requirements: real 64-bit Linux root, procfs, pidfds, explicit non-root
PF_QUOTA_TEST_CHILD_UID/GID, and a sealed exclusively reserved root-owned
PF_QUOTA_TEST_CGROUP_ROOT on cgroup v2 with memory already enabled and kernel
swap accounting (`memory.swap.max`). The full-range rows require real uid/gid
transitions through 2^32-2. Cleanup fault coverage requires native x86-64 or
aarch64 seccomp filters; SIGXFSZ coverage directly execs `/bin/sh`. Missing
facilities fail SETUP. No SUT/ledger/service is mocked or imported into pytest.

Each cgroup row creates and retains its own subgroup below the supplied root;
it never moves the harness or supervisor, edits ancestors, or deletes groups.
Only that subgroup receives memory.max=64 MiB and memory.swap.max=0. The OOM
positive gates allocation on kernel membership; the populated-group negative
keeps a separately owned sleeper there and requires unknown OOM attribution.

RSS uses 8,192 warmup and 65,536/131,072/262,144 completed real cycles, each
producing 16 bytes and exercising RESERVE/COMMIT/CANCEL. The outside observer
drains stdout into a fixed counter, cross-checks ledger/cycle/byte totals, and
samples the pinned supervisor generation. Every checkpoint requires exactly
the owned actor as the supervisor's only child across ALL supervisor threads.
RSS growth above warm baseline is bounded by 1 MiB (drops are allowed); three
paused samples must have <=256 KiB noise. The memory row has no supervisor
deadline or outer harness time limit; it waits for completed protocol events.
Finite RSS evidence does not replace source review or the independent
post-green deliberate mutation.

All policies are test inputs, not new caps/defaults. Only deadline-feature
rows supply a supervisor deadline. No disk scratch or exit/DONE marker is
written. Pytest/JUnit assertions are the verdict. The SIGABRT row is a
deliberate real crash control, with core files disabled.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import ctypes
import errno
import fcntl
import importlib.util
import json
import math
import os
from pathlib import Path
import resource
import select
import selectors
import signal
import socket
import stat
import struct
import subprocess
import sys
import time

import pytest


ROOT = Path(__file__).absolute().parents[1]
SUPERVISOR = ROOT / "docker" / "pf_quota_run.py"
CLIENT = ROOT / "docker" / "run_quota.py"
COUNTER_MAX = 9007199254740991
RECEIPT_FD_MIN = 256
LINE = "quota=managed enforcement=cooperative"
SCHEMA = "pf-quota-receipt/v1"
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
CHECKPOINTS = (8192, 65536, 131072, 262144)
RSS_TOLERANCE = 1024 * 1024
RSS_NOISE = 256 * 1024
CYCLE_BYTES = 16
CLOUD = ROOT / "docker" / "run_json_quota_supervisor_test.py"
BAD_DEADLINES = (
    "", "0", "-1", "nan", "inf", "-inf", "1e309", "not-seconds",
    "+1", "-0.5", "1_0", "1.0_0",
    " 1", "1 ", "1 0", "\t1", "1\t", "1\n", "\n1", "1\r\n",
    "1e1", "1E1", ".5", "1.", ".", "0.0", "00.000", "\uff11", "1.\uff11",
    "9" * 400, "0." + "0" * 400 + "1",
)
VALID_DEADLINES = ("20", "00020", "20.5", "00020.50")
HIGH_IDENTITIES = ((1 << 31), (1 << 31) + 1, (1 << 32) - 2)


def _permitted(path):
    """Refuse sensitive names before resolving or following a supplied path."""
    path = Path(path).absolute()
    parts = path.parts
    forbidden = (
        any(part in (".ssh", ".aws", ".codex") for part in parts)
        or any(part.endswith(".env") for part in parts)
        or any(parts[i:i + 3] == (".config", "zsh", "keys")
               for i in range(len(parts)))
        or any(parts[i:i + 2] == (".config", "pineforge")
               for i in range(len(parts)))
    )
    assert not forbidden, f"REFUSED sensitive path by name: {path}"
    return path


def _files_before_run(*paths):
    for path in paths:
        pending = _permitted(path)
        print(f"FILES followed by quota supervisor test: {pending}",
              file=sys.stderr, flush=True)
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
    # Do not move Linux/root/uid setup, invocation, or a product import above
    # this assertion: the initial CLOUD RED must name the missing product.
    path = _permitted(SUPERVISOR)
    try:
        _files_before_run(path)
    except FileNotFoundError:
        pytest.fail(f"MISSING SUPERVISOR: {path}")
    assert path.is_file(), f"MISSING SUPERVISOR: {path}"


def test_supervisor_path_is_present_before_invocation_or_privileged_setup():
    _require_supervisor()


@dataclass(frozen=True)
class Lab:
    uid: int
    gid: int


def _credentials(lab, host_expression="os.getppid()", *, announce=True):
    """Executed in a real isolated fixture child, not in the supervisor."""
    code = f"""
import ctypes, errno, os, resource, select, sys
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert os.getresuid() == ({lab.uid}, {lab.uid}, {lab.uid}), "child uid separation"
assert os.getresgid() == ({lab.gid}, {lab.gid}, {lab.gid}), "child gid separation"
assert os.getgroups() == [], "child retained supplementary groups"
for _restore in (os.setuid, os.setgid):
    try:
        _restore(0)
    except PermissionError:
        pass
    else:
        raise AssertionError("child regained root privilege")
_host_pid = {host_expression}
print("FILES fixture follows: /proc/self/status", file=sys.stderr, flush=True)
with open("/proc/self/status", encoding="ascii") as _status_file:
    _no_new_privs = None
    for _status_line in _status_file:
        if _status_line.split(":", 1)[0] in ("CapInh", "CapPrm", "CapEff", "CapBnd", "CapAmb"):
            assert int(_status_line.split()[1], 16) == 0, "retained child capability"
        if _status_line.startswith("NoNewPrivs:"):
            _no_new_privs = int(_status_line.split()[1])
assert _no_new_privs == 1, "child can regain privilege across exec"
try:
    os.kill(_host_pid, 0)
except PermissionError:
    pass
else:
    raise AssertionError("child can signal its protected supervisor")
_libc = ctypes.CDLL(None, use_errno=True)
_libc.ptrace.restype = ctypes.c_long
_libc.ptrace.argtypes = (ctypes.c_uint, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
ctypes.set_errno(0)
_trace_result = _libc.ptrace(0x4206, _host_pid, None, None)  # PTRACE_SEIZE
assert _trace_result == -1 and ctypes.get_errno() == errno.EPERM, "child can ptrace supervisor"
def _await_observer():
    assert os.read(0, 1) == b"."
"""
    if announce:
        code += """
os.write(2, ("owned %d %d\\n" % (os.getpid(), _host_pid)).encode("ascii"))
_await_observer()
"""
    return code


@pytest.fixture(scope="module", autouse=True)
def lab():
    _require_supervisor()
    if sys.platform != "linux" or struct.calcsize("P") != 8:
        pytest.fail("SETUP: requires a 64-bit Linux uid-isolation lab")
    if (os.geteuid() != 0 or not hasattr(os, "pidfd_open")
            or not hasattr(signal, "pidfd_send_signal")):
        pytest.fail("SETUP: root uid-change capability and pidfds are required")
    try:
        descriptor = os.pidfd_open(os.getpid(), 0)
        try:
            assert signal.pidfd_send_signal(descriptor, 0) is None
        finally:
            os.close(descriptor)
    except OSError as error:
        pytest.fail(f"SETUP: real kernel pidfd syscalls are required: {error}")
    values = []
    for name in ("PF_QUOTA_TEST_CHILD_UID", "PF_QUOTA_TEST_CHILD_GID"):
        text = os.environ.get(name)
        if not text or not text.isascii() or not text.isdecimal():
            pytest.fail(f"SETUP: explicit ASCII decimal {name} is required")
        value = int(text)
        if not 0 < value < (1 << 32) - 1:
            pytest.fail(f"SETUP: {name} must select an explicit non-root identity")
        values.append(value)
    config = Lab(*values)
    soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft != resource.RLIM_INFINITY and soft <= RECEIPT_FD_MIN + 16:
        pytest.fail("SETUP: insufficient lab nofile for a high receipt descriptor")
    _files_before_run(SUPERVISOR, CLIENT, sys.executable)
    # Popen's user/group transition is only a lab traversal/uid probe. Bounding
    # capabilities/no_new_privs are checked in every SUT child, not invented
    # by the test launcher.
    probe = f"""
import os
assert os.getresuid() == ({config.uid},) * 3
assert os.getresgid() == ({config.gid},) * 3 and os.getgroups() == []
assert os.access({str(CLIENT)!r}, os.R_OK), "SETUP: dropped uid cannot read shipped client"
"""
    try:
        result = subprocess.run(
            [sys.executable, "-I", "-B", "-c", probe],
            user=config.uid, group=config.gid, extra_groups=(),
            env={"PATH": os.defpath, "LC_ALL": "C"}, cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    except OSError as error:
        pytest.fail(f"SETUP: actual uid-change probe failed: {error}")
    if result.returncode != 0:
        pytest.fail(f"SETUP: uid/isolation probe exit={result.returncode}: "
                    f"{result.stderr.decode('ascii', 'backslashreplace')}")
    return config


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, f"duplicate receipt key: {key}"
        result[key] = value
    return result


def _bad_json_constant(token):
    raise AssertionError(f"non-JSON receipt constant: {token}")


def _document(wire):
    # Every caller supplies bytes read from the real parent-only pipe. There
    # are no synthetic malformed-document tests of a test-only validator.
    assert wire, "missing receipt: empty EOF is a launcher failure"
    text = wire.decode("ascii")
    assert text.endswith("\n") and text.count("\n") == 1, (
        "receipt must be exactly one newline-terminated ASCII document"
    )
    document = json.loads(text, object_pairs_hook=_unique_object,
                          parse_constant=_bad_json_constant)
    assert type(document) is dict
    for field in FIELDS:
        assert field in document, f"missing actual receipt field: {field}"
    assert document["schema"] == SCHEMA
    assert document["quota_line"] == LINE
    for field in ("wait_status", "exit_code", "signal"):
        assert document[field] is None or type(document[field]) is int, field
    for field in ("ledger_verdict", "launcher_failure"):
        value = document[field]
        if value is not None:
            assert type(value) is dict
            assert type(value["code"]) is str and value["code"]
            assert type(value["args"]) is dict
    oom = document["oom"]
    assert type(oom) is dict
    assert type(oom["proven"]) is bool
    for field in ("before", "after", "delta"):
        assert oom[field] is None or (type(oom[field]) is int and oom[field] >= 0)
    if oom["proven"]:
        assert all(type(oom[field]) is int for field in ("before", "after", "delta"))
        assert oom["delta"] == oom["after"] - oom["before"] > 0
    diagnostic = oom.get("diagnostic")
    if diagnostic is not None:
        assert type(diagnostic) is dict
        assert type(diagnostic["code"]) is str and diagnostic["code"]
        assert type(diagnostic["args"]) is dict
    deadline = document["deadline"]
    assert type(deadline) is dict
    if deadline["seconds"] is None:
        assert deadline["expired"] is False and deadline["kill_sent"] is False
    else:
        assert type(deadline["seconds"]) in (int, float)
        assert math.isfinite(deadline["seconds"]) and deadline["seconds"] > 0
    assert type(deadline["expired"]) is bool
    assert type(deadline["kill_sent"]) is bool
    return document


def _normal(document, exit_code):
    assert document["wait_status"] == exit_code << 8
    assert os.WIFEXITED(document["wait_status"])
    assert os.WEXITSTATUS(document["wait_status"]) == exit_code
    assert document["exit_code"] == exit_code
    assert document["signal"] is None
    assert document["launcher_failure"] is None
    assert document["deadline"]["expired"] is False
    assert document["deadline"]["kill_sent"] is False


def _unknown_oom(document):
    assert {key: value for key, value in document["oom"].items()
            if key != "diagnostic"} == {
        "proven": False, "before": None, "after": None, "delta": None,
    }
    assert document["oom"].get("diagnostic") is None


def _quota_refusal(document, used, requested, limit, peak):
    verdict = document["ledger_verdict"]
    assert type(verdict) is dict, "no parent-ledger refusal"
    assert verdict["code"] == "run_resource_quota_exceeded"
    args = verdict["args"]
    assert args["stage"] == "execution"
    assert args["resource"] == "output_bytes"
    assert args["artifact"] == "stdout"
    for name, expected in (("used", used), ("requested", requested),
                           ("limit", limit), ("peak", peak)):
        assert type(args[name]) is int and 0 <= args[name] <= COUNTER_MAX
        assert args[name] == expected, (name, args)


def _proc(pid, leaf):
    assert type(pid) is int and pid > 0
    assert leaf in ("stat", "status", "statm", "cgroup")
    path = _permitted(Path("/proc") / str(pid) / leaf)
    print(f"FILES observer follows: {path}", file=sys.stderr, flush=True)
    return path.read_text(encoding="ascii")


def _process_identity(pid):
    tail = _proc(pid, "stat").rsplit(")", 1)[1].split()
    return int(tail[1]), int(tail[2]), int(tail[19])


def _only_direct_child(supervisor_pid, expected):
    # A fork can belong to any thread, not just the thread-group leader.
    directory = _permitted(Path("/proc") / str(supervisor_pid) / "task")
    _files_before_run(directory)
    found = 0
    for tid in os.listdir(directory):
        assert tid.isascii() and tid.isdecimal()
        path = _permitted(directory / tid / "children")
        _files_before_run(path)
        with path.open("rb") as source:
            record = source.read(65)
        assert len(record) <= 64, "unexpected helper population in procfs children"
        for child in record.split():
            found += 1
            assert expected is not None and int(child) == expected and found == 1, (
                "hidden helper or unrelated commanded child", supervisor_pid, child)
    assert found == (0 if expected is None else 1), "owned child absent from kernel tree"


def _pidfd_signal_filter(number, *, indent=0):
    """Real native seccomp fixture, installed before the supervisor's exec."""
    assert number in (errno.EPERM, errno.ENOSYS)
    code = f"""
import ctypes, errno, os, signal
_architecture = os.uname().machine
assert _architecture in ("x86_64", "aarch64"), "SETUP: native seccomp ABI required"
_audit_arch = 0xc000003e if _architecture == "x86_64" else 0xc00000b7
class _Filter(ctypes.Structure):
    _fields_ = (("code", ctypes.c_ushort), ("jt", ctypes.c_ubyte),
                ("jf", ctypes.c_ubyte), ("k", ctypes.c_uint32))
class _Program(ctypes.Structure):
    _fields_ = (("len", ctypes.c_ushort), ("filter", ctypes.POINTER(_Filter)))
# Native syscall 424 is pidfd_send_signal on both supported Linux ABIs.
# Validate seccomp_data.arch before reading seccomp_data.nr.
_filter = (_Filter * 7)(
    _Filter(0x20, 0, 0, 4),
    _Filter(0x15, 1, 0, _audit_arch),
    _Filter(0x06, 0, 0, 0x80000000),
    _Filter(0x20, 0, 0, 0),
    _Filter(0x15, 0, 1, 424),
    _Filter(0x06, 0, 0, 0x00050000 | {number}),
    _Filter(0x06, 0, 0, 0x7fff0000),
)
_program = _Program(len(_filter), _filter)
_filter_libc = ctypes.CDLL(None, use_errno=True)
_filter_libc.prctl.argtypes = (
    ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
)
_filter_libc.prctl.restype = ctypes.c_int
_filter_probe = os.pidfd_open(os.getpid(), 0)
try:
    # A host-wide denial cannot stand in for this fixture's real filter.
    signal.pidfd_send_signal(_filter_probe, 0)
    assert _filter_libc.prctl(38, 1, 0, 0, 0) == 0, "SETUP: no_new_privs failed"
    assert _filter_libc.prctl(39, 0, 0, 0, 0) == 1
    assert _filter_libc.prctl(22, 2, ctypes.addressof(_program), 0, 0) == 0, (
        "SETUP: kernel seccomp installation failed", ctypes.get_errno())
    try:
        signal.pidfd_send_signal(_filter_probe, 0)
    except OSError as _error:
        assert _error.errno == {number}, ("actual kernel filter errno", _error)
    else:
        raise AssertionError("kernel seccomp did not refuse pidfd_send_signal")
    os.kill(os.getpid(), 0)  # Numeric signaling remains a real allowed syscall.
finally:
    os.close(_filter_probe)
"""
    return "\n".join(" " * indent + line if line else "" for line in code.splitlines())


class _Run:
    """Observe without a harness time limit or an implicit caller deadline."""

    def __init__(self, lab, code, *, policy=POLICY, seconds=None,
                 memory_events=None, omit=None,
                 pass_fds=(), command=None, child_expected=True, stdin_data=None,
                 child_args=(), pidfd_signal_errno=None, hide_pidfd=False):
        self.lab = lab
        self.code = code
        self.policy = policy
        self.seconds = seconds
        self.memory_events = memory_events
        self.omit = omit
        self.pass_fds = tuple(pass_fds)
        self.command = command
        self.child_args = tuple(child_args)
        self.pidfd_signal_errno = pidfd_signal_errno
        self.hide_pidfd = hide_pidfd
        self.child_expected = child_expected
        self.stdin_data = stdin_data
        self.startup = bytearray()
        self.startup_phase = 0
        self.supervisor_identity = None
        self.supervisor_pidfd = None
        self.started = None
        self.owned = [None, None, None]  # Fixed leader/intermediate/sleeper slots.
        self.process = None
        self.read_fd = None
        self.writer = None
        self.receipt_identity = None
        self.stdout = bytearray()
        self.stderr = bytearray()
        self.receipt = bytearray()
        self.partial = bytearray()

    def __enter__(self):
        _require_supervisor()
        _files_before_run(SUPERVISOR, CLIENT, sys.executable)
        read_fd, write_fd = os.pipe2(os.O_CLOEXEC)
        self.read_fd = read_fd
        try:
            self.writer = fcntl.fcntl(write_fd, fcntl.F_DUPFD_CLOEXEC,
                                      RECEIPT_FD_MIN)
        finally:
            os.close(write_fd)
        metadata = os.fstat(self.writer)
        self.receipt_identity = (metadata.st_dev, metadata.st_ino)
        code = self.code(self) if callable(self.code) else self.code
        argv = [sys.executable, "-I", "-B", str(SUPERVISOR),
                "--receipt-fd", str(self.writer)]
        for (flag, _), value in zip(LIMITS, self.policy):
            if flag != self.omit:
                argv.append(flag + "=" + value)
        for flag, value in (("--deadline-seconds", self.seconds),
                            ("--child-uid", self.lab.uid), ("--child-gid", self.lab.gid)):
            if flag != self.omit and value is not None:
                argv.append(flag + "=" + str(value))
        for fd in self.pass_fds:
            argv.extend(("--pass-fd", str(fd)))
        if self.memory_events is not None:
            _permitted(self.memory_events)
            print(f"FILES supervisor follows: {self.memory_events}",
                  file=sys.stderr, flush=True)
            argv.extend(("--memory-events", str(self.memory_events)))
        if self.command is None:
            argv.extend(("--", sys.executable, "-I", "-B", "-c", code, *self.child_args))
        else:
            assert not self.child_args, "fixture command already contains its argv"
            argv.extend(("--", *self.command))
        if self.hide_pidfd:
            wrapper = """
import os, runpy, signal, sys
for owner, name in ((os, "pidfd_open"), (signal, "pidfd_send_signal")):
    if hasattr(owner, name):
        delattr(owner, name)
sys.argv = sys.argv[1:]
runpy.run_path(sys.argv[0], run_name="__main__")
"""
            argv = [sys.executable, "-I", "-B", "-c", wrapper, *argv[3:]]
        if self.pidfd_signal_errno is not None:
            # No product import or copied cleanup: replace this same process
            # with the complete actual CLI under the inherited kernel filter.
            wrapper = _pidfd_signal_filter(self.pidfd_signal_errno) + """
import sys
os.execv(sys.executable, (sys.executable, "-I", "-B", *sys.argv[1:]))
"""
            argv = [sys.executable, "-I", "-B", "-c", wrapper, *argv[3:]]
        try:
            self.started = time.monotonic()
            self.process = subprocess.Popen(
                argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, pass_fds=(self.writer,) + self.pass_fds,
                env={"PATH": os.defpath, "LC_ALL": "C"},
                cwd=ROOT, start_new_session=True, bufsize=0,
            )
        except BaseException:
            os.close(self.read_fd)
            self.read_fd = None
            raise
        finally:
            os.close(self.writer)
            self.writer = None
        try:
            self.supervisor_pidfd = os.pidfd_open(self.process.pid, 0)
            self.supervisor_identity = _process_identity(self.process.pid)
            assert self.supervisor_identity[0] == os.getpid()
            assert self.supervisor_identity[2] > 0
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise
        return self

    def acknowledge(self):
        assert self.process.stdin.write(b".") == 1
        self.process.stdin.flush()

    def remember(self, slot, pid, parent, *, expected_group=None, expected_session=None):
        assert _process_identity(self.process.pid) == self.supervisor_identity
        ppid, group, generation = _process_identity(pid)
        assert ppid == parent and generation > 0, "fixture pid is not an owned child"
        status = _proc(pid, "status")
        uid_line = next(row for row in status.splitlines() if row.startswith("Uid:"))
        assert tuple(map(int, uid_line.split()[1:])) == (self.lab.uid,) * 4
        gid_line = next(row for row in status.splitlines() if row.startswith("Gid:"))
        assert tuple(map(int, gid_line.split()[1:])) == (self.lab.gid,) * 4
        groups_line = next(row for row in status.splitlines() if row.startswith("Groups:"))
        assert not groups_line.split()[1:], "kernel reports retained supplementary groups"
        if expected_group is None:
            expected_group = pid if slot == 0 else self.owned[0][1]
        assert group == expected_group, "actor escaped the owned group"
        fields = _proc(pid, "stat").rsplit(")", 1)[1].split()
        if expected_session is None:
            expected_session = expected_group
        assert int(fields[3]) == expected_session, "actor escaped the expected session"
        if self.owned[slot] is not None:
            assert self.owned[slot][:3] == (pid, group, generation)
            assert not select.select((self.owned[slot][3],), (), (), 0)[0]
            return
        descriptor = os.pidfd_open(pid, 0)
        assert _process_identity(pid) == (ppid, group, generation)
        self.owned[slot] = (pid, group, generation, descriptor)

    def startup_stderr(self, chunk):
        self.startup.extend(chunk)
        while b"\n" in self.startup and self.startup_phase < 2:
            line, _, remaining = self.startup.partition(b"\n")
            self.startup[:] = remaining
            if self.startup_phase == 0:
                assert line == b"FILES fixture follows: /proc/self/status", (
                    "unexpected parent/child stderr before identity", bytes(line)
                )
            else:
                kind, child, parent = line.decode("ascii").split()
                assert kind == "owned" and int(parent) == self.process.pid
                self.remember(0, int(child), self.process.pid)
                self.acknowledge()
                if self.stdin_data is not None:
                    assert type(self.stdin_data) is bytes
                    assert self.process.stdin.write(self.stdin_data) == len(self.stdin_data)
                    self.process.stdin.close()
            self.startup_phase += 1
        assert len(self.startup) <= 4096
        if self.startup_phase == 2:
            remaining = bytes(self.startup)
            self.startup.clear()
            return remaining
        return b""

    def collect(self, observer=None, *, progress_stream="stdout", output_sink=None):
        with selectors.DefaultSelector() as poller:
            for stream, name in ((self.process.stdout, "stdout"),
                                 (self.process.stderr, "stderr"),
                                 (self.read_fd, "receipt")):
                fd = stream if type(stream) is int else stream.fileno()
                os.set_blocking(fd, False)
                poller.register(fd, selectors.EVENT_READ, name)
            while poller.get_map():
                for key, _ in poller.select():
                    try:
                        chunk = os.read(key.fd, 4096)
                    except BlockingIOError:
                        # An observer may drain a stdout readiness event while
                        # handling the corresponding stderr checkpoint.
                        continue
                    if not chunk:
                        poller.unregister(key.fd)
                        continue
                    if key.data == "stderr" and self.child_expected and self.startup_phase < 2:
                        chunk = self.startup_stderr(chunk)
                        if not chunk:
                            continue
                    if key.data == "stdout" and output_sink is not None:
                        output_sink(chunk)
                    elif key.data == progress_stream and observer is not None:
                        self.partial.extend(chunk)
                        while b"\n" in self.partial:
                            line, _, rest = self.partial.partition(b"\n")
                            self.partial[:] = rest
                            observer(self, bytes(line))
                        assert len(self.partial) <= 4096, "invalid fixture progress line"
                    else:
                        buffer = getattr(self, key.data)
                        buffer.extend(chunk)
                        # A harness diagnostic bound only: overflow fails the
                        # row; it never produces or substitutes a quota verdict.
                        assert len(buffer) <= 1024 * 1024, "unexpected fixture stream volume"
        assert not self.partial, "truncated fixture progress"
        assert not self.startup, "truncated identity handshake"
        if self.child_expected:
            assert self.startup_phase == 2 and self.owned[0] is not None, (
                "commanded child was never pinned to the actual supervisor"
            )
        assert _process_identity(self.process.pid) == self.supervisor_identity
        returncode = self.process.wait()
        document = _document(bytes(self.receipt))
        expected_seconds = (
            None if self.omit == "--deadline-seconds" or self.seconds is None
            else float(self.seconds)
        )
        assert document["deadline"]["seconds"] == expected_seconds
        return returncode, document

    def assert_dead_and_reaped(self, slot):
        pid, _, generation, descriptor = self.owned[slot]
        assert select.select((descriptor,), (), (), 0)[0], "pidfd reports live child"
        try:
            _, _, now = _process_identity(pid)
        except FileNotFoundError:
            return
        assert now != generation, "owned child still exists, including as a zombie"

    def __exit__(self, exc_type, exc, traceback):
        # Cleanup cannot turn a failed observation into a successful row.
        for owned in self.owned:
            if owned is not None:
                descriptor = owned[3]
                if not select.select((descriptor,), (), (), 0)[0]:
                    try:
                        signal.pidfd_send_signal(descriptor, signal.SIGKILL)
                    except ProcessLookupError:
                        pass  # Death raced cleanup; never used as a row verdict.
                os.close(descriptor)
        if self.process is not None:
            if self.process.poll() is None:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.process.wait()
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                stream.close()
        if self.read_fd is not None:
            os.close(self.read_fd)
        if self.supervisor_pidfd is not None:
            os.close(self.supervisor_pidfd)
        if self.writer is not None:
            os.close(self.writer)


PAYLOADS = (
    (b"", b""),
    (b'{"caller":"ordinary","ok":true}\n', b"caller diagnostic\n"),
    (b"\x00\xffcaller\nsecond line\n", b"\x00\xfeerror without newline"),
)


@pytest.mark.parametrize("exit_code", (0, 17))
@pytest.mark.parametrize("stdout,stderr", PAYLOADS)
def test_normal_exit_preserves_caller_streams_and_raw_wait(lab, exit_code, stdout, stderr):
    code = _credentials(lab) + f"""
os.write(1, {stdout!r})
os.write(2, {stderr!r})
sys.exit({exit_code})
"""
    with _Run(lab, code) as run:
        returncode, receipt = run.collect()
        assert returncode == exit_code
        _normal(receipt, exit_code)
        assert receipt["ledger_verdict"] is None
        assert bytes(run.stdout) == stdout
        # Only the exact fixture identity handshake was removed by collect().
        assert bytes(run.stderr) == stderr, "supervisor added success stderr noise"
        run.assert_dead_and_reaped(0)


def test_normal_exit_without_native_pidfd_preserves_receipt_and_wait(lab):
    stdout, stderr = PAYLOADS[1]
    code = _credentials(lab) + f"""
os.write(1, {stdout!r})
os.write(2, {stderr!r})
sys.exit(17)
"""
    with _Run(lab, code, hide_pidfd=True) as run:
        returncode, receipt = run.collect()
        assert returncode == 17
        assert set(FIELDS) <= receipt.keys()
        _normal(receipt, 17)
        _unknown_oom(receipt)
        assert receipt["ledger_verdict"] is None
        assert receipt["deadline"] == {"seconds": None, "expired": False, "kill_sent": False}
        assert bytes(run.stdout) == stdout and bytes(run.stderr) == stderr
        run.assert_dead_and_reaped(0)


def test_actual_receipt_field_presence_and_types(lab):
    with _Run(lab, _credentials(lab) + "\nsys.exit(0)\n") as run:
        returncode, receipt = run.collect()
        assert returncode == 0
        assert set(FIELDS) <= receipt.keys()
        _normal(receipt, 0)
        assert receipt["ledger_verdict"] is None
        _unknown_oom(receipt)
        assert not run.stdout and not run.stderr
        run.assert_dead_and_reaped(0)


def test_receipt_pipe_identity_is_absent_in_child_and_forked_descendant(lab):
    def code(run):
        return _credentials(lab) + f"""
import socket, stat
_receipt_identity = {run.receipt_identity!r}
_high_number = {run.writer}
assert _high_number >= {RECEIPT_FD_MIN}
def probe():
    assert os.environ["PF_RUN_QUOTA_MODE"] == "managed"
    _identities = []
    _expected_fds = {{0, 1, 2}}
    for _name in ("PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD"):
        _fd = int(os.environ[_name])
        assert _fd >= 3 and _fd not in _expected_fds
        _expected_fds.add(_fd)
        _metadata = os.fstat(_fd)
        assert stat.S_ISSOCK(_metadata.st_mode)
        _identities.append((_metadata.st_dev, _metadata.st_ino))
        with socket.socket(fileno=os.dup(_fd)) as _channel:
            assert _channel.family == socket.AF_UNIX
            assert _channel.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == socket.SOCK_SEQPACKET
            _channel.getpeername()
    assert _identities[0] != _identities[1]
    print("FILES fixture follows: /proc/self/fd (fstat only)", file=sys.stderr, flush=True)
    _reachable = 0
    _actual_fds = set()
    for _entry in os.listdir("/proc/self/fd"):
        try:
            _metadata = os.fstat(int(_entry))
        except OSError as _error:
            assert _error.errno == errno.EBADF  # scandir's closed fd is incidental.
            continue
        _actual_fds.add(int(_entry))
        if stat.S_ISSOCK(_metadata.st_mode):
            assert (_metadata.st_dev, _metadata.st_ino) in _identities, (
                "parent ledger endpoint or other socket leaked to child/descendant")
        if (_metadata.st_dev, _metadata.st_ino) == _receipt_identity:
            _reachable += 1
            try:
                os.set_blocking(int(_entry), False)
                os.write(int(_entry), b'{{"escaped_receipt_pipe":true}}\\n')
            except OSError:
                pass  # Reachable read-only ends are failures too.
    assert _reachable == 0, "actual receipt pipe device/inode reached untrusted process"
    assert _actual_fds == _expected_fds, (
        "parent control fd leaked to child/descendant", _actual_fds, _expected_fds)
    os.write(1, b"identity probe completed\\n")
probe()
_descendant = os.fork()
if _descendant == 0:
    os.write(2, ("fd-descendant %d\\n" % os.getpid()).encode("ascii"))
    _await_observer()
    probe()
    os._exit(0)
_waited, _status = os.waitpid(_descendant, 0)
assert _waited == _descendant and _status == 0
"""
    descendants = 0

    def observe(run, line):
        nonlocal descendants
        if line == b"FILES fixture follows: /proc/self/fd (fstat only)":
            return
        kind, pid = line.decode("ascii").split()
        assert kind == "fd-descendant" and descendants == 0
        run.remember(1, int(pid), run.owned[0][0])
        descendants += 1
        run.acknowledge()

    with _Run(lab, code) as run:
        returncode, receipt = run.collect(observe, progress_stream="stderr")
        assert returncode == 0
        assert descendants == 1
        _normal(receipt, 0)
        assert receipt["ledger_verdict"] is None
        assert bytes(run.stdout) == b"identity probe completed\n" * 2
        run.assert_dead_and_reaped(0)
        run.assert_dead_and_reaped(1)


@pytest.mark.parametrize("stream", (1, 2), ids=("stdout", "stderr"))
def test_child_forged_t_receipt_is_forwarded_but_never_authoritative(lab, stream):
    forged = {
        "schema": SCHEMA, "quota_line": LINE,
        "ledger_verdict": {"code": "run_resource_quota_exceeded", "args": {
            "stage": "execution", "resource": "output_bytes", "artifact": "stdout",
            "used": 9, "requested": 1, "limit": 9, "peak": 9}},
        "wait_status": 0, "exit_code": 0, "signal": None,
        "oom": {
            "proven": True, "before": 0, "after": 1, "delta": 1,
            "diagnostic": None,
        },
        "deadline": {"seconds": None, "expired": False, "kill_sent": False},
        "launcher_failure": None,
    }
    wire = (json.dumps(forged, ensure_ascii=True, separators=(",", ":")) + "\n").encode("ascii")
    with _Run(lab, _credentials(lab) + f"\nos.write({stream}, {wire!r})\nsys.exit(17)\n") as run:
        returncode, receipt = run.collect()
        assert returncode == 17
        _normal(receipt, 17)
        assert bytes(run.stdout) == (wire if stream == 1 else b"")
        assert bytes(run.stderr) == (wire if stream == 2 else b"")
        assert receipt["ledger_verdict"] is None, "child T code became parent authority"
        _unknown_oom(receipt)
        run.assert_dead_and_reaped(0)


def _attach_client():
    return f"""
print("FILES fixture imports: {str(CLIENT)}", file=sys.stderr, flush=True)
sys.path.insert(0, {str(CLIENT.parent)!r})
import run_quota as q
assert os.path.abspath(q.__file__) == {str(CLIENT)!r}, "unrelated quota module imported"
assert q.mode() == "managed"
client = q.QuotaClient.from_environment()
"""


@pytest.mark.parametrize("cap", (0, 7, 41))
def test_real_descendant_client_exceeds_explicit_output_policy(lab, cap):
    # The bridge never attaches or sends on either quota socket. It transfers
    # its pair to exactly one runtime actor, closes its copies, and waits
    # WITHOUT exec. Kernel actor authentication must discover that descendant.
    actor = _credentials(lab, "int(sys.argv[1])", announce=False) + """
os.write(2, ("actor %d\\n" % os.getpid()).encode("ascii"))
_await_observer()
""" + _attach_client() + f"""
ticket = client.reserve(q.Amounts(output_bytes={cap}), artifact=q.Artifact.STDOUT)
assert os.write(1, b"x" * {cap}) == {cap}
ticket.commit(q.Amounts(output_bytes={cap}), 0)
ticket.cancel()
usage = client.snapshot(q.Resource.OUTPUT_BYTES)
assert (usage.committed, usage.reserved, usage.used) == ({cap}, 0, {cap})
try:
    client.reserve(q.Amounts(output_bytes=1), artifact=q.Artifact.STDOUT)
except q.QuotaError:
    sys.exit(17)
raise AssertionError("real output overrun was admitted")
"""
    bridge = _credentials(lab) + f"""
import subprocess
os.write(2, ("bridge %d\\n" % os.getpid()).encode("ascii"))
_await_observer()
_pair = tuple(int(os.environ[_name]) for _name in
              ("PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD"))
_actor = subprocess.Popen([sys.executable, "-I", "-B", "-c", {actor!r}, str(_host_pid)],
                          pass_fds=_pair)
for _fd in _pair:
    os.close(_fd)
_actor_exit = _actor.wait()
sys.exit(_actor_exit)
"""
    phase = 0
    produced = 0

    def count_output(chunk):
        nonlocal produced
        assert chunk == b"x" * len(chunk), "caller output was altered"
        produced += len(chunk)

    def observe(run, line):
        nonlocal phase
        if line in (b"FILES fixture follows: /proc/self/status",
                    f"FILES fixture imports: {CLIENT}".encode("ascii")):
            return
        kind, number = line.decode("ascii").split()
        pid = int(number)
        if phase == 0:
            assert kind == "bridge"
            run.remember(0, pid, run.process.pid)
        else:
            assert phase == 1 and kind == "actor"
            assert pid != run.owned[0][0], "Popen.pid was confused with runtime actor"
            run.remember(1, pid, run.owned[0][0])
        phase += 1
        run.acknowledge()

    policy = (POLICY[0], POLICY[1], str(cap), POLICY[3])
    with _Run(lab, bridge, policy=policy) as run:
        returncode, receipt = run.collect(
            observe, progress_stream="stderr", output_sink=count_output)
        assert phase == 2 and returncode == 17
        assert produced == cap
        _normal(receipt, 17)
        _quota_refusal(receipt, cap, 1, cap, cap)
        run.assert_dead_and_reaped(0)
        run.assert_dead_and_reaped(1)


@pytest.mark.parametrize("termination", ("SIGTERM", "SIGABRT"))
def test_real_signal_and_deliberate_crash_retain_raw_wait(lab, termination):
    code = _credentials(lab) + """
import signal
os.write(1, ("ready %d\\n" % os.getpid()).encode("ascii"))
_await_observer()
"""
    if termination == "SIGABRT":
        code += "\nos.abort()  # Deliberate crash control; core output is disabled.\n"
    else:
        code += "\nos.kill(os.getpid(), signal.SIGTERM)\n"

    def observe(run, line):
        kind, number = line.decode("ascii").split()
        assert kind == "ready"
        run.remember(0, int(number), run.process.pid)
        run.acknowledge()

    with _Run(lab, code) as run:
        returncode, receipt = run.collect(observe)
        expected = getattr(signal, termination)
        assert returncode == 128 + expected
        assert type(receipt["wait_status"]) is int
        assert os.WIFSIGNALED(receipt["wait_status"])
        assert os.WTERMSIG(receipt["wait_status"]) == expected
        assert receipt["signal"] == expected and receipt["exit_code"] is None
        assert receipt["launcher_failure"] is None
        assert receipt["ledger_verdict"] is None
        assert receipt["deadline"] == {"seconds": None, "expired": False, "kill_sent": False}
        run.assert_dead_and_reaped(0)


def test_exec_child_restores_sigxfsz_and_reports_real_signal(lab):
    # A new Python interpreter would ignore SIGXFSZ again during startup.
    # Exec a real shell directly, observe its kernel disposition, then signal.
    shell = _permitted("/bin/sh")
    _files_before_run(shell, sys.executable)
    # Prove this shell preserves an ignored disposition. Otherwise a shell
    # startup reset could conceal a missing reset in the supervisor's child.
    control = f"""
import os, resource, signal
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert signal.getsignal(signal.SIGXFSZ) == signal.SIG_IGN
os.execv({str(shell)!r}, ({str(shell)!r}, "-c", 'kill -XFSZ "$$"; exit 94'))
"""
    result = subprocess.run(
        [sys.executable, "-I", "-B", "-c", control],
        user=lab.uid, group=lab.gid, extra_groups=(),
        cwd=ROOT, env={"PATH": os.defpath, "LC_ALL": "C"},
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert result.returncode == 94 and not result.stdout and not result.stderr, (
        "SETUP: shell must preserve inherited SIGXFSZ ignore", result.returncode, result.stderr)
    script = """
ulimit -c 0 || exit 95
printf 'xfsz-ready %s\\n' "$$"
IFS= read -r _ack || exit 96
kill -XFSZ "$$"
exit 94
"""
    observed = 0

    def observe(run, line):
        nonlocal observed
        kind, number = line.decode("ascii").split()
        assert observed == 0 and kind == "xfsz-ready"
        pid = int(number)
        run.remember(0, pid, run.process.pid)
        rows = _proc(pid, "status").splitlines()
        ignored = next(row for row in rows if row.startswith("SigIgn:"))
        assert not int(ignored.split()[1], 16) & (1 << (signal.SIGXFSZ - 1))
        observed += 1
        assert run.process.stdin.write(b".\n") == 2
        run.process.stdin.flush()

    with _Run(lab, "", command=(str(shell), "-c", script), child_expected=False) as run:
        returncode, receipt = run.collect(observe)
        assert observed == 1 and returncode == 128 + signal.SIGXFSZ
        assert os.WIFSIGNALED(receipt["wait_status"])
        assert os.WTERMSIG(receipt["wait_status"]) == signal.SIGXFSZ
        assert receipt["signal"] == signal.SIGXFSZ and receipt["exit_code"] is None
        assert receipt["launcher_failure"] is None and receipt["ledger_verdict"] is None
        assert receipt["deadline"] == {"seconds": None, "expired": False, "kill_sent": False}
        _unknown_oom(receipt)
        assert not run.stdout and not run.stderr
        run.assert_dead_and_reaped(0)


def _deadline_group_proof(lab, *, hide_pidfd=False):
    # The CLI starts this policy during admission, before either child barrier;
    # slow setup can expire it before both real processes have been observed.
    code = _credentials(lab) + """
import signal
signal.signal(signal.SIGTERM, signal.SIG_IGN)
os.write(1, ("leader %d\\n" % os.getpid()).encode("ascii"))
_await_observer()
_descendant = os.fork()
if _descendant == 0:
    os.write(1, ("descendant %d\\n" % os.getpid()).encode("ascii"))
    while True:
        signal.pause()
while True:
    signal.pause()
"""
    phase = 0

    def observe(run, line):
        nonlocal phase
        kind, number = line.decode("ascii").split()
        if phase == 0:
            assert kind == "leader"
            run.remember(0, int(number), run.process.pid)
            run.acknowledge()
        else:
            assert phase == 1 and kind == "descendant"
            run.remember(1, int(number), run.owned[0][0])
            assert run.owned[1][1] == run.owned[0][1]
        phase += 1

    with _Run(lab, code, seconds=3, hide_pidfd=hide_pidfd) as run:
        returncode, receipt = run.collect(observe)
        assert phase == 2, "deadline fixture did not reach both real processes"
        assert returncode == 128 + signal.SIGKILL
        assert os.WIFSIGNALED(receipt["wait_status"])
        assert os.WTERMSIG(receipt["wait_status"]) == signal.SIGKILL
        assert receipt["signal"] == signal.SIGKILL
        assert receipt["exit_code"] is None and receipt["launcher_failure"] is None
        assert receipt["deadline"] == {"seconds": 3, "expired": True, "kill_sent": True}
        elapsed = time.monotonic() - run.started
        assert elapsed >= 3, f"deadline fired before caller policy: {elapsed}"
        run.assert_dead_and_reaped(0)
        run.assert_dead_and_reaped(1)


def test_deadline_actually_sigkills_and_reaps_owned_group(lab):
    _deadline_group_proof(lab)


def test_deadline_without_native_pidfd_sigkills_and_reaps_owned_group(lab):
    _deadline_group_proof(lab, hide_pidfd=True)


def test_normal_first_child_exit_kills_and_reaps_setsid_double_fork(lab):
    # The intermediate owns a NEW session before its second fork. Both
    # generations are pinned while their original parents are still alive.
    # The sleeper survives the leader's original group kill by construction.
    code = _credentials(lab) + """
import signal
_ready_read, _ready_write = os.pipe2(os.O_CLOEXEC)
_intermediate = os.fork()
if _intermediate == 0:
    os.close(_ready_read)
    os.setsid()
    os.write(1, ("new-session %d\\n" % os.getpid()).encode("ascii"))
    _await_observer()
    _release_read, _release_write = os.pipe2(os.O_CLOEXEC)
    _sleeper = os.fork()
    if _sleeper == 0:
        os.close(_release_read)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        os.write(1, ("escaped-sleeper %d\\n" % os.getpid()).encode("ascii"))
        _await_observer()
        assert os.write(_release_write, b".") == 1
        os.close(_release_write)
        assert os.write(_ready_write, b".") == 1
        os.close(_ready_write)
        while True:
            signal.pause()
    os.close(_release_write)
    assert os.read(_release_read, 1) == b"."
    os.close(_release_read)
    os.close(_ready_write)
    os._exit(0)
os.close(_ready_write)
assert os.read(_ready_read, 1) == b"."
os.close(_ready_read)
assert os.waitpid(_intermediate, 0) == (_intermediate, 0)
os.write(1, b"first-child-exit 0\\n")
sys.exit(0)
"""
    phase = 0

    def observe(run, line):
        nonlocal phase
        kind, number = line.decode("ascii").split()
        pid = int(number)
        if phase == 0:
            assert kind == "new-session"
            assert pid != run.owned[0][0]
            run.remember(1, pid, run.owned[0][0],
                         expected_group=pid, expected_session=pid)
            run.acknowledge()
        elif phase == 1:
            assert kind == "escaped-sleeper"
            intermediate = run.owned[1]
            assert not select.select((intermediate[3],), (), (), 0)[0]
            assert _process_identity(intermediate[0]) == (
                run.owned[0][0], intermediate[1], intermediate[2])
            run.remember(2, pid, intermediate[0],
                         expected_group=intermediate[0], expected_session=intermediate[0])
            assert run.owned[2][1] != run.owned[0][1]
            status = _proc(pid, "status").splitlines()
            ignored = next(row for row in status if row.startswith("SigIgn:"))
            assert int(ignored.split()[1], 16) & (1 << (signal.SIGTERM - 1))
            run.acknowledge()
        else:
            assert phase == 2 and (kind, pid) == ("first-child-exit", 0)
        phase += 1

    with _Run(lab, code) as run:
        returncode, receipt = run.collect(observe)
        assert phase == 3 and returncode == 0
        _normal(receipt, 0)
        assert receipt["ledger_verdict"] is None
        _unknown_oom(receipt)
        assert not run.stdout and not run.stderr
        for slot in (0, 1, 2):
            run.assert_dead_and_reaped(slot)


@pytest.mark.parametrize("fault_errno", (errno.EPERM, errno.ENOSYS), ids=("eperm", "enosys"))
@pytest.mark.parametrize("finish", ("normal", "deadline"))
def test_complete_supervisor_reaps_escaped_tree_under_pidfd_signal_filter(lab, finish, fault_errno):
    # Exec the full supervisor with a real inherited kernel denial. Independently
    # observe the supervisor and all three child generations before cleanup.
    code = _credentials(lab) + f"""
import signal
_self_pin = os.pidfd_open(os.getpid(), 0)
try:
    try:
        signal.pidfd_send_signal(_self_pin, 0)
    except OSError as _error:
        assert _error.errno == {fault_errno}, ("inherited kernel filter errno", _error)
    else:
        raise AssertionError("pidfd signaling filter did not survive supervisor/child exec")
    os.kill(os.getpid(), 0)
finally:
    os.close(_self_pin)
os.write(1, ("filter-active %d %d\\n" % (os.getpid(), {fault_errno})).encode("ascii"))
_await_observer()
_ready_read, _ready_write = os.pipe2(os.O_CLOEXEC)
_intermediate = os.fork()
if _intermediate == 0:
    os.close(_ready_read)
    os.setsid()
    os.write(1, ("new-session %d\\n" % os.getpid()).encode("ascii"))
    _await_observer()
    _release_read, _release_write = os.pipe2(os.O_CLOEXEC)
    _sleeper = os.fork()
    if _sleeper == 0:
        os.close(_release_read)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        os.write(1, ("escaped-sleeper %d\\n" % os.getpid()).encode("ascii"))
        _await_observer()
        assert os.write(_release_write, b".") == 1
        os.close(_release_write)
        assert os.write(_ready_write, b".") == 1
        os.close(_ready_write)
        while True:
            signal.pause()
    os.close(_release_write)
    assert os.read(_release_read, 1) == b"."
    os.close(_release_read)
    os.close(_ready_write)
    os._exit(0)
os.close(_ready_write)
assert os.read(_ready_read, 1) == b"."
os.close(_ready_read)
assert os.waitpid(_intermediate, 0) == (_intermediate, 0)
os.write(1, ("tree-ready %d\\n" % os.getpid()).encode("ascii"))
_await_observer()
"""
    if finish == "normal":
        code += "\nsys.exit(17)\n"
    else:
        code += "\nwhile True:\n    signal.pause()\n"
    phase = 0

    def observe(run, line):
        nonlocal phase
        fields = line.decode("ascii").split()
        if phase == 0:
            assert fields == ["filter-active", str(run.owned[0][0]), str(fault_errno)]
            run.remember(0, run.owned[0][0], run.process.pid)
            for pid in (run.process.pid, run.owned[0][0]):
                rows = _proc(pid, "status").splitlines()
                for key, value in (("NoNewPrivs:", 1), ("Seccomp:", 2)):
                    row = next(row for row in rows if row.startswith(key))
                    assert int(row.split()[1]) == value, ("kernel filter absent", pid, row)
            run.acknowledge()
        elif phase == 1:
            assert len(fields) == 2 and fields[0] == "new-session"
            pid = int(fields[1])
            assert pid != run.owned[0][0]
            run.remember(1, pid, run.owned[0][0], expected_group=pid, expected_session=pid)
            run.acknowledge()
        elif phase == 2:
            assert len(fields) == 2 and fields[0] == "escaped-sleeper"
            intermediate = run.owned[1]
            assert not select.select((intermediate[3],), (), (), 0)[0]
            assert _process_identity(intermediate[0]) == (
                run.owned[0][0], intermediate[1], intermediate[2])
            run.remember(2, int(fields[1]), intermediate[0],
                         expected_group=intermediate[0], expected_session=intermediate[0])
            assert run.owned[2][1] != run.owned[0][1]
            rows = _proc(run.owned[2][0], "status").splitlines()
            ignored = next(row for row in rows if row.startswith("SigIgn:"))
            assert int(ignored.split()[1], 16) & (1 << (signal.SIGTERM - 1))
            run.acknowledge()
        else:
            assert phase == 3 and fields == ["tree-ready", str(run.owned[0][0])]
            run.assert_dead_and_reaped(1)
            sleeper = run.owned[2]
            assert not select.select((sleeper[3],), (), (), 0)[0]
            assert _process_identity(sleeper[0]) == (
                run.process.pid, sleeper[1], sleeper[2]), "escaped orphan not adopted by supervisor"
            run.remember(0, run.owned[0][0], run.process.pid)
            run.acknowledge()
        phase += 1

    # The deadline arm covers the live tree observations too: the CLI arms
    # its clock during admission, before those child/observer barriers.
    seconds = 5 if finish == "deadline" else None
    with _Run(lab, code, seconds=seconds,
              pidfd_signal_errno=fault_errno) as run:
        returncode, receipt = run.collect(observe)
        assert phase == 4, ("incomplete real escaped-tree observation", phase, receipt)
        assert receipt["launcher_failure"] is None, receipt
        assert receipt["ledger_verdict"] is None, receipt
        _unknown_oom(receipt)
        if finish == "normal":
            assert returncode == 17, receipt
            _normal(receipt, 17)
            assert receipt["deadline"] == {
                "seconds": None, "expired": False, "kill_sent": False,
            }, receipt
        else:
            assert returncode == 128 + signal.SIGKILL, receipt
            assert os.WIFSIGNALED(receipt["wait_status"]), receipt
            assert os.WTERMSIG(receipt["wait_status"]) == signal.SIGKILL, receipt
            assert receipt["signal"] == signal.SIGKILL and receipt["exit_code"] is None, receipt
            assert receipt["deadline"] == {
                "seconds": 5, "expired": True, "kill_sent": True,
            }, receipt
            elapsed = time.monotonic() - run.started
            assert elapsed >= seconds, ("deadline fired before caller policy", elapsed, receipt)
        assert not run.stdout and not run.stderr
        for slot in (0, 1, 2):
            run.assert_dead_and_reaped(slot)


@contextmanager
def _non_cgroup_evidence(kind):
    if kind == "absent":
        yield None, None
        return
    if not hasattr(os, "memfd_create"):
        pytest.fail("SETUP: Linux memfd required for non-cgroup path fixture")
    descriptor = os.memfd_create("quota-not-a-cgroup", os.MFD_CLOEXEC)
    try:
        initial = b"oom 0\noom_kill 0\n" if kind == "regular-file-wrong-basename" else b"not counters\n"
        os.write(descriptor, initial)
        path = Path("/proc") / str(os.getpid()) / "fd" / str(descriptor)
        # This path is not named memory.events. It tests early path refusal,
        # not counter parsing, cgroup population, or kernel OOM attribution.
        _permitted(path)
        print(f"FILES non-cgroup path fixture (UNPROVEN): {path}",
              file=sys.stderr, flush=True)
        yield path, descriptor
    finally:
        os.close(descriptor)


@pytest.mark.parametrize("fault", ("pidfd-open-emfile", "pidfd-send-eperm"))
def test_adopted_cleanup_survives_real_pidfd_failures(lab, fault):
    # Exercise the actual cleanup code in an isolated process with one real
    # dropped child. No monkeypatch, fake pidfd, or synthetic errno is used.
    # One row leaves zero free descriptors after trusted setup. The other uses
    # a real seccomp filter to deny pidfd_send_signal while allowing kill.
    # Cleanup must kill and wait-reap after each actual kernel refusal.
    _files_before_run(SUPERVISOR, sys.executable, "/dev/null")
    code = f"""
import ctypes, errno, fcntl, importlib.util, os, resource, select, signal, sys
sys.dont_write_bytecode = True
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
_spec = importlib.util.spec_from_file_location("_actual_quota_cleanup", {str(SUPERVISOR)!r})
_product = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_product)
assert os.path.abspath(_product.__file__) == {str(SUPERVISOR)!r}
_parent = os.getpid()
for _path in (f"/proc/{{_parent}}/stat", f"/proc/self/task/{{_parent}}/children"):
    print("FILES fallback worker follows: " + _path, file=sys.stderr, flush=True)
_linux = _product._Linux()
assert not os.get_inheritable(_linux.children_fd)
assert fcntl.fcntl(_linux.children_fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
_read, _write = os.pipe2(os.O_CLOEXEC)
_child = os.fork()
if _child == 0:
    os.close(_read)
    try:
        _linux.close()
        os.setsid()
        _linux.drop({lab.uid}, {lab.gid}, _parent)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        assert os.write(_write, b".") == 1
        os.close(_write)
        while True:
            signal.pause()
    except BaseException:
        os._exit(97)
os.close(_write)
_pin = None
_held = []
_reaped = False
_original = resource.getrlimit(resource.RLIMIT_NOFILE)
try:
    assert os.read(_read, 1) == b"."
    os.close(_read)
    _read = None
    _pin = os.pidfd_open(_child, 0)
    print(f"FILES fallback worker follows: /proc/{{_child}}/stat", file=sys.stderr, flush=True)
    _generation = _product._proc_stat(_child)[3]
    os.write(1, ("fallback-owned %d %d\\n" % (_child, _generation)).encode("ascii"))
    assert os.read(0, 1) == b"."
    assert os.waitid(os.P_PID, _child, os.WEXITED | os.WNOHANG | os.WNOWAIT) is None
    # Leave the pre-opened stream at EOF; cleanup must seek back to offset zero.
    assert tuple(_product._direct_children(_linux.children_fd)) == (_child,)
    if {fault!r} == "pidfd-open-emfile":
        resource.setrlimit(resource.RLIMIT_NOFILE, (64, _original[1]))
        while True:
            try:
                _held.append(os.open("/dev/null", os.O_RDONLY | os.O_CLOEXEC))
            except OSError as _error:
                assert _error.errno == errno.EMFILE
                break
        try:
            _unexpected = os.pidfd_open(_child, 0)
        except OSError as _error:
            assert _error.errno == errno.EMFILE
        else:
            os.close(_unexpected)
            raise AssertionError("fixture did not exhaust real pidfd allocation")
        try:
            _unexpected = os.open("/dev/null", os.O_RDONLY | os.O_CLOEXEC)
        except OSError as _error:
            assert _error.errno == errno.EMFILE
        else:
            os.close(_unexpected)
            raise AssertionError("fixture retained spare descriptor headroom")
        assert _held
    else:
{_pidfd_signal_filter(errno.EPERM, indent=8)}
        try:
            signal.pidfd_send_signal(_pin, 0)
        except OSError as _error:
            assert _error.errno == errno.EPERM
        else:
            raise AssertionError("kernel seccomp did not deny pidfd_send_signal")
    assert not select.select((_pin,), (), (), 0)[0], "fault control killed the child"
    _product._kill_adopted(_linux.children_fd)
    assert select.select((_pin,), (), ())[0], "pidfd failure abandoned the live child"
    _waited, _raw = os.waitpid(_child, 0)
    _reaped = True
    assert _waited == _child and os.WIFSIGNALED(_raw)
    assert os.WTERMSIG(_raw) == signal.SIGKILL
    assert tuple(_product._direct_children(_linux.children_fd)) == ()
    os.write(1, ("fallback-result %d %d\\n" % (_waited, _raw)).encode("ascii"))
finally:
    for _fd in _held:
        os.close(_fd)
    resource.setrlimit(resource.RLIMIT_NOFILE, _original)
    if _read is not None:
        os.close(_read)
    if not _reaped:
        try:
            # Our direct child has never been wait-reaped; its pid is reserved.
            # This also works while the worker's seccomp filter is installed.
            os.kill(_child, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(_child, 0)
    if _pin is not None:
        os.close(_pin)
    _linux.close()
"""
    worker = worker_pin = child_pin = None
    try:
        worker = subprocess.Popen(
            [sys.executable, "-I", "-B", "-c", code],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            cwd=ROOT, env={"PATH": os.defpath, "LC_ALL": "C"},
            start_new_session=True, bufsize=0,
        )
        worker_pin = os.pidfd_open(worker.pid, 0)
        worker_identity = _process_identity(worker.pid)
        assert worker_identity[0] == os.getpid() and worker_identity[2] > 0
        kind, child_text, generation_text = _bounded_line(worker.stdout).decode("ascii").split()
        assert kind == "fallback-owned"
        child, generation = int(child_text), int(generation_text)
        child_pin = os.pidfd_open(child, 0)
        assert _process_identity(child) == (worker.pid, child, generation)
        assert not select.select((child_pin,), (), (), 0)[0]
        rows = _proc(child, "status").splitlines()
        for key, value in (("Uid:", lab.uid), ("Gid:", lab.gid)):
            row = next(line for line in rows if line.startswith(key))
            assert tuple(map(int, row.split()[1:])) == (value,) * 4
        assert not next(line for line in rows if line.startswith("Groups:")).split()[1:]
        _only_direct_child(worker.pid, child)
        stdout, stderr = worker.communicate(b".")
        assert worker.returncode == 0, stderr.decode("ascii", "backslashreplace")
        kind, waited_text, raw_text = stdout.decode("ascii").split()
        assert kind == "fallback-result" and int(waited_text) == child
        raw = int(raw_text)
        assert os.WIFSIGNALED(raw) and os.WTERMSIG(raw) == signal.SIGKILL
        assert stderr == (
            f"FILES fallback worker follows: /proc/{worker.pid}/stat\n"
            f"FILES fallback worker follows: /proc/self/task/{worker.pid}/children\n"
            f"FILES fallback worker follows: /proc/{child}/stat\n"
        ).encode("ascii")
        assert select.select((child_pin,), (), (), 0)[0]
        try:
            _, _, now = _process_identity(child)
        except FileNotFoundError:
            pass
        else:
            assert now != generation, "fallback killed but failed to reap its child"
    finally:
        if child_pin is not None:
            if not select.select((child_pin,), (), (), 0)[0]:
                try:
                    signal.pidfd_send_signal(child_pin, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            os.close(child_pin)
        if worker is not None:
            if worker.poll() is None:
                if not worker.stdin.closed:
                    worker.stdin.close()  # Let the worker's own finally reap.
                worker.wait()
            for stream in (worker.stdin, worker.stdout, worker.stderr):
                stream.close()
        if worker_pin is not None:
            os.close(worker_pin)


@pytest.mark.parametrize("evidence", (
    "absent", "regular-file-wrong-basename", "malformed-file-wrong-basename",
))
def test_absent_or_non_cgroup_memory_events_path_is_unknown(lab, evidence):
    code = _credentials(lab) + """
os.write(1, b"ready\\n")
_await_observer()
sys.exit(17)
"""
    with _non_cgroup_evidence(evidence) as (path, descriptor):
        observed = 0

        def observe(run, line):
            nonlocal observed
            assert observed == 0 and line == b"ready"
            observed += 1
            if evidence == "regular-file-wrong-basename":
                # Changing caller bytes cannot turn this path into cgroupfs.
                os.pwrite(descriptor, b"oom 9\noom_kill 7\n", 0)
            run.acknowledge()

        with _Run(lab, code, memory_events=path) as run:
            returncode, receipt = run.collect(observe)
            assert observed == 1 and returncode == 17
            _normal(receipt, 17)
            _unknown_oom(receipt)
            assert receipt["ledger_verdict"] is None
            assert not run.stderr
            run.assert_dead_and_reaped(0)


@pytest.mark.parametrize("index", range(4))
@pytest.mark.parametrize("text", ("0", "0007", "9007199254740991", "0009007199254740991"))
def test_explicit_policy_grammar_accepts_zero_leading_zero_and_full_range(lab, index, text):
    policy = list(POLICY)
    policy[index] = text
    expected = tuple(int(value) for value in policy)
    code = _credentials(lab) + _attach_client() + f"""
_limits = tuple(client.snapshot(_resource).limit for _resource in q.Resource)
assert _limits == {expected!r}, ("actual ledger retained different policy", _limits)
os.write(1, ("limits %d %d %d %d\\n" % _limits).encode("ascii"))
if {int(text)} == 0:
    _amounts = [0, 0, 0, 0]
    _amounts[{index}] = 1
    _fields = {{"artifact": q.Artifact.STDOUT}}
    if {index} in (0, 1):
        _fields.update(object_id=1, generation=1, new_extent=_amounts[0])
        _amounts[1] = 1
    try:
        client.reserve(q.Amounts(*_amounts), **_fields)
    except q.QuotaError as _error:
        assert _error.code == {("run_descriptor_limit_exceeded" if index == 3 else "run_resource_quota_exceeded")!r}
        assert _error.code_args["limit"] == 0
        sys.exit(17)
    raise AssertionError("literal zero was treated as unlimited")
client.finish()
client.close()
sys.exit(0)
"""
    with _Run(lab, code, policy=policy) as run:
        returncode, receipt = run.collect()
        assert bytes(run.stdout) == ("limits %d %d %d %d\n" % expected).encode("ascii")
        assert bytes(run.stderr) == f"FILES fixture imports: {CLIENT}\n".encode("ascii")
        if int(text) == 0:
            assert returncode == 17, "zero must lead to actual finite-cap refusal"
            _normal(receipt, 17)
            verdict = receipt["ledger_verdict"]
            assert verdict is not None
            args = verdict["args"]
            for name in ("used", "requested", "limit", "peak"):
                assert type(args[name]) is int and 0 <= args[name] <= COUNTER_MAX
            assert verdict["code"] == (
                "run_descriptor_limit_exceeded" if index == 3 else "run_resource_quota_exceeded")
            assert args["stage"] == "execution"
            assert (args["used"], args["requested"], args["limit"], args["peak"]) == (0, 1, 0, 0)
            if index == 3:
                assert args["process_role"] == "worker"
                assert type(args["soft_nofile"]) is int and args["soft_nofile"] > 0
            else:
                assert args["resource"] == LIMITS[index][1] and args["artifact"] == "stdout"
        else:
            assert returncode == 0, "a valid full-range caller policy was narrowed"
            _normal(receipt, 0)
            assert receipt["ledger_verdict"] is None
        run.assert_dead_and_reaped(0)


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


@pytest.mark.parametrize("index", range(4))
@pytest.mark.parametrize("text,reason", INVALID_LIMITS, ids=INVALID_IDS)
def test_invalid_caller_policy_refuses_before_spawning(lab, index, text, reason):
    policy = list(POLICY)
    policy[index] = text
    with _Run(lab, 'raise AssertionError("invalid policy spawned child")\n',
              policy=policy, child_expected=False) as run:
        returncode, receipt = run.collect()
        assert returncode != 0
        assert receipt["wait_status"] is None
        assert receipt["exit_code"] is None and receipt["signal"] is None
        assert receipt["ledger_verdict"] is None
        failure = receipt["launcher_failure"]
        assert failure["code"] == "run_quota_configuration_invalid"
        assert failure["args"] == {
            "stage": "admission", "reason": reason, "setting": LIMITS[index][1],
        }
        assert bytes(run.stdout) == b""
        assert not run.stderr


@pytest.mark.parametrize("flag,setting", LIMITS)
def test_every_caller_policy_is_mandatory_without_a_default(lab, flag, setting):
    with _Run(lab, 'raise AssertionError("missing policy spawned child")\n',
              omit=flag, child_expected=False) as run:
        returncode, receipt = run.collect()
        assert returncode != 0
        assert receipt["wait_status"] is None
        assert receipt["exit_code"] is None and receipt["signal"] is None
        assert receipt["ledger_verdict"] is None
        failure = receipt["launcher_failure"]
        assert failure["code"] == "run_quota_configuration_invalid"
        assert failure["args"] == {
            "stage": "admission", "reason": "missing_limit", "setting": setting,
        }
        assert bytes(run.stdout) == b""
        assert not run.stderr


def _rss(run):
    # The Popen PID here is the supervisor itself, never its runtime actor.
    assert run.process.poll() is None, "supervisor exited before RSS observation"
    values = [0, 0, 0]
    for index in range(3):
        try:
            fields = _proc(run.process.pid, "statm").split()
            resident = int(fields[1])
            page_size = os.sysconf("SC_PAGE_SIZE")
        except (OSError, ValueError, IndexError) as error:
            pytest.fail(f"SETUP: actual supervisor RSS unavailable: {error}")
        if resident <= 0 or page_size <= 0:
            pytest.fail("SETUP: missing/zero actual supervisor RSS")
        values[index] = resident * page_size
        time.sleep(0.02)
    if max(values) - min(values) > RSS_NOISE:
        pytest.fail(f"SETUP: paused-checkpoint RSS noise exceeds {RSS_NOISE}: {values}")
    return sorted(values)[1]


def test_supervisor_rss_stays_flat_over_real_completed_protocol_cycles(lab):
    total = CHECKPOINTS[-1]
    produced_total = total * CYCLE_BYTES
    limit = produced_total + 1
    code = _credentials(lab) + _attach_client() + f"""
_checkpoints = {CHECKPOINTS!r}
_checkpoint_index = 0
for _completed in range(1, {total} + 1):
    _ticket = client.reserve(q.Amounts(output_bytes={CYCLE_BYTES + 1}), artifact=q.Artifact.STDOUT)
    assert os.write(1, b"x" * {CYCLE_BYTES}) == {CYCLE_BYTES}
    _ticket.commit(q.Amounts(output_bytes={CYCLE_BYTES}), 0)
    _ticket.cancel()
    if _completed == _checkpoints[_checkpoint_index]:
        _usage = client.snapshot(q.Resource.OUTPUT_BYTES)
        assert (_usage.committed, _usage.reserved, _usage.used, _usage.peak, _usage.limit) == (
            _completed * {CYCLE_BYTES}, 0, _completed * {CYCLE_BYTES},
            _completed * {CYCLE_BYTES} + 1, {limit})
        os.write(2, ("checkpoint %d %d %d %d %d %d\\n" % (
            os.getpid(), _completed, _usage.committed, _usage.reserved,
            _usage.peak, _usage.limit)).encode("ascii"))
        _await_observer()
        _checkpoint_index += 1
assert _checkpoint_index == len(_checkpoints)
try:
    client.reserve(q.Amounts(output_bytes=2), artifact=q.Artifact.STDOUT)
except q.QuotaError:
    sys.exit(17)
raise AssertionError("cumulative produced output was refunded or cap was ignored")
"""
    samples = [None, None, None, None]
    completed_checkpoints = 0
    actor_pid = None
    supervisor_generation = None
    produced = 0

    def count_output(chunk):
        nonlocal produced
        assert chunk == b"x" * len(chunk), "caller output was altered"
        produced += len(chunk)

    def observe(run, line):
        nonlocal completed_checkpoints, actor_pid, supervisor_generation
        if line in (b"FILES fixture follows: /proc/self/status",
                    f"FILES fixture imports: {CLIENT}".encode("ascii")):
            return
        assert completed_checkpoints < len(CHECKPOINTS), "unexpected progress history"
        kind, pid, completed, committed, reserved, peak, actual_limit = line.decode("ascii").split()
        assert kind == "checkpoint"
        pid, completed, committed, reserved = map(int, (pid, completed, committed, reserved))
        assert completed == CHECKPOINTS[completed_checkpoints]
        expected_bytes = completed * CYCLE_BYTES
        assert committed == expected_bytes and reserved == 0
        assert int(peak) == expected_bytes + 1 and int(actual_limit) == limit
        if completed_checkpoints == 0:
            actor_pid = pid
            run.remember(0, pid, run.process.pid)
            supervisor_generation = _process_identity(run.process.pid)[2]
        assert pid == actor_pid and pid != run.process.pid
        assert _process_identity(run.process.pid)[2] == supervisor_generation
        run.remember(0, pid, run.process.pid)
        _only_direct_child(run.process.pid, actor_pid)
        _only_direct_child(actor_pid, None)
        # Drain actual forwarded payload at this checkpoint, independently
        # of the fixture's claimed count. The child is paused, not producing.
        while produced < expected_bytes:
            select.select((run.process.stdout,), (), ())
            chunk = os.read(run.process.stdout.fileno(), 4096)
            assert chunk, "stdout EOF before completed output"
            count_output(chunk)
        assert produced == expected_bytes
        samples[completed_checkpoints] = _rss(run)
        _only_direct_child(run.process.pid, actor_pid)
        _only_direct_child(actor_pid, None)
        completed_checkpoints += 1
        run.acknowledge()

    policy = (POLICY[0], POLICY[1], str(limit), POLICY[3])
    with _Run(lab, code, policy=policy) as run:
        returncode, receipt = run.collect(
            observe, progress_stream="stderr", output_sink=count_output)
        assert completed_checkpoints == 4 and all(type(value) is int for value in samples)
        assert produced == produced_total
        assert returncode == 17, "long legitimate cycle population was not completed"
        _normal(receipt, 17)
        _quota_refusal(receipt, produced_total, 2, limit, limit)
        for value in samples[1:]:
            assert value - samples[0] <= RSS_TOLERANCE, (
                f"supervisor RSS grew beyond warm noise budget: checkpoints={CHECKPOINTS}, "
                f"RSS={samples}, tolerance={RSS_TOLERANCE}"
            )
        run.assert_dead_and_reaped(0)
        print(f"actual supervisor RSS bytes: checkpoints={CHECKPOINTS}, samples={samples}, "
              f"tolerance={RSS_TOLERANCE}, within_checkpoint_noise={RSS_NOISE}")


def _cloud_support():
    """Load only the owned trusted sibling by filename, also under -I."""
    _files_before_run(CLOUD)
    spec = importlib.util.spec_from_file_location("_quota_cloud_admission", str(CLOUD))
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    try:
        sys.dont_write_bytecode = True
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


def test_explicit_pass_fd_preserves_preopened_channel(lab):
    assert hasattr(os, "memfd_create"), "SETUP: memfd is required for pass-fd proof"
    descriptor = os.memfd_create("quota-pass-fd-sentinel", os.MFD_CLOEXEC)
    sentinel = b"arena-channel:\x00\xff"
    answer = b"owned-child-write:\x00\xfe"
    try:
        assert os.write(descriptor, sentinel) == len(sentinel)
        item = os.fstat(descriptor)
        identity = (item.st_dev, item.st_ino)
        code = _credentials(lab) + f"""
_fd = {descriptor}
_item = os.fstat(_fd)
assert (_item.st_dev, _item.st_ino) == {identity!r}
assert os.pread(_fd, {len(sentinel)}, 0) == {sentinel!r}
assert os.pwrite(_fd, {answer!r}, {len(sentinel)}) == {len(answer)}
os.write(1, b"explicit pass-fd preserved\\n")
"""
        with _Run(lab, code, pass_fds=(descriptor,)) as run:
            returncode, receipt = run.collect()
            assert returncode == 0
            _normal(receipt, 0)
            assert receipt["ledger_verdict"] is None
            assert bytes(run.stdout) == b"explicit pass-fd preserved\n" and not run.stderr
            assert os.pread(descriptor, len(answer), len(sentinel)) == answer
            run.assert_dead_and_reaped(0)
    finally:
        os.close(descriptor)


@pytest.mark.parametrize("kind", ("exact", "alias"))
def test_receipt_forwarding_collision_refuses_before_spawn(lab, kind):
    support = _cloud_support()
    support._launcher_failure(support._admission(collision=kind), "receipt_fd_collision")


@pytest.mark.parametrize("flag", ("--child-uid", "--child-gid"))
def test_root_child_identity_is_forbidden(lab, flag):
    support = _cloud_support()
    support._launcher_failure(
        support._admission(overrides={flag: "0"}), "child_identity_not_separate")


@pytest.mark.parametrize("flag", ("--child-uid", "--child-gid"))
def test_child_identity_is_mandatory(lab, flag):
    support = _cloud_support()
    support._launcher_failure(support._admission(omit=flag), "invalid_integer")


def test_absent_deadline_launches_without_inventing_budget(lab):
    code = _credentials(lab) + """
os.write(1, b"deadline omitted\\n")
sys.exit(17)
"""
    with _Run(lab, code, seconds=None) as run:
        returncode, receipt = run.collect()
        assert returncode == 17
        _normal(receipt, 17)
        assert receipt["deadline"] == {
            "seconds": None, "expired": False, "kill_sent": False,
        }
        assert receipt["ledger_verdict"] is None
        _unknown_oom(receipt)
        assert bytes(run.stdout) == b"deadline omitted\n" and not run.stderr
        run.assert_dead_and_reaped(0)


@pytest.mark.parametrize("text", BAD_DEADLINES)
def test_invalid_deadline_refuses_without_inventing_budget(lab, text):
    support = _cloud_support()
    document = support._admission(
        overrides={"--deadline-seconds": text}, expected_seconds=None)
    assert document["launcher_failure"] == {
        "code": "quota_launcher_error",
        "args": {"stage": "admission", "reason": "invalid_deadline"},
    }


@pytest.mark.parametrize("text", VALID_DEADLINES)
def test_plain_integer_and_decimal_deadlines_launch_normally(lab, text):
    # This policy must survive admission and a real child exit. The CLI arms
    # it before child startup, so slow startup can still expire the input.
    with _Run(lab, _credentials(lab) + "\nsys.exit(17)\n", seconds=text) as run:
        returncode, receipt = run.collect()
        assert returncode == 17
        _normal(receipt, 17)
        assert receipt["deadline"] == {
            "seconds": float(text), "expired": False, "kill_sent": False,
        }
        assert receipt["ledger_verdict"] is None
        _unknown_oom(receipt)
        assert not run.stdout and not run.stderr
        run.assert_dead_and_reaped(0)


@pytest.mark.parametrize("kind", ("missing", "closed", "readonly", "negative", "standard", "text"))
def test_bad_receipt_carrier_fails_without_child_or_stdout_fallback(lab, kind):
    _cloud_support()._admission(receipt_kind=kind)


@pytest.mark.parametrize("style", ("separate", "equals"))
def test_child_receipt_option_cannot_supply_missing_launcher_value(lab, style):
    _cloud_support()._admission(receipt_kind="missing-value", child_receipt_style=style)


def test_child_receipt_options_after_boundary_remain_child_argv(lab):
    arguments = ("--receipt-fd", "child-value", "--receipt-fd=3")
    code = _credentials(lab) + f"""
assert tuple(sys.argv[1:]) == {arguments!r}
os.write(1, b"child receipt options preserved\\n")
sys.exit(17)
"""
    with _Run(lab, code, child_args=arguments) as run:
        returncode, receipt = run.collect()
        assert returncode == 17
        _normal(receipt, 17)
        assert receipt["ledger_verdict"] is None
        _unknown_oom(receipt)
        assert bytes(run.stdout) == b"child receipt options preserved\n" and not run.stderr
        run.assert_dead_and_reaped(0)


def test_spawn_failure_is_parent_owned_and_has_actual_wait(lab):
    missing = _permitted(ROOT / "docker" / "__quota_proof_missing_executable__")
    print(f"FILES deliberate failed exec follows (must be absent): {missing}",
          file=sys.stderr, flush=True)
    assert not missing.exists(), "SETUP: failed-exec fixture name is occupied"
    with _Run(lab, "", command=(str(missing),), child_expected=False) as run:
        returncode, receipt = run.collect()
        assert returncode != 0
        assert os.WIFEXITED(receipt["wait_status"])
        assert receipt["exit_code"] == os.WEXITSTATUS(receipt["wait_status"]) > 0
        assert receipt["signal"] is None and receipt["ledger_verdict"] is None
        failure = receipt["launcher_failure"]
        assert failure["code"] == "quota_launcher_error"
        assert failure["args"] == {
            "stage": "launch", "reason": "child_setup_failed",
            "operation": "exec", "errno": errno.ENOENT,
        }
        assert receipt["deadline"] == {"seconds": None, "expired": False, "kill_sent": False}
        _unknown_oom(receipt)
        assert not run.stdout and not run.stderr


def test_stdin_passes_through_to_commanded_child(lab):
    payload = b"\x00\xffcaller stdin\nsecond line without newline"
    code = _credentials(lab) + f"""
_input = bytearray()
while True:
    _chunk = os.read(0, 4096)
    if not _chunk:
        break
    _input.extend(_chunk)
    assert len(_input) <= {len(payload)}
assert bytes(_input) == {payload!r}
assert os.write(1, _input) == len(_input)
"""
    with _Run(lab, code, stdin_data=payload) as run:
        returncode, receipt = run.collect()
        assert returncode == 0
        _normal(receipt, 0)
        assert receipt["ledger_verdict"] is None
        assert bytes(run.stdout) == payload and not run.stderr
        run.assert_dead_and_reaped(0)


def _bounded_line(stream):
    """Wait for a line or EOF, retaining only a fixed byte bound."""
    value = bytearray()
    while not value.endswith(b"\n"):
        chunk = os.read(stream.fileno(), 1)
        assert chunk, "outside actor exited before its observation"
        value.extend(chunk)
        assert len(value) <= 4096
    return bytes(value)


@pytest.mark.parametrize("endpoint", (0, 1), ids=("native", "python"))
def test_valid_same_uid_nonmember_hello_is_refused(lab, endpoint):
    _exercise_outside_hello(lab, lab, endpoint, "actor_outside_owned_run")


@pytest.mark.parametrize("endpoint", (0, 1), ids=("native", "python"))
@pytest.mark.parametrize("value", HIGH_IDENTITIES)
@pytest.mark.parametrize("field", ("uid", "gid"))
def test_full_range_off_by_one_kernel_identity_hello_is_refused(lab, field, value, endpoint):
    # Only a trusted host can supply a different real credential after the
    # run's irreversible drop. Relay the real endpoints to a separately owned
    # process using that adjacent identity; never forge SCM_CREDENTIALS.
    if field == "uid":
        owner, outside = Lab(value, lab.gid), Lab(value - 1, lab.gid)
    else:
        owner, outside = Lab(lab.uid, value), Lab(lab.uid, value - 1)
    _exercise_outside_hello(owner, outside, endpoint, "endpoint_credentials")


def _exercise_outside_hello(lab, outside_lab, endpoint, reason):
    # This process is a sibling of the supervisor, in its own session. It
    # receives the REAL two client endpoints from the owned child by SCM_RIGHTS.
    transfer, receiver = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    outside = outside_pidfd = None
    try:
        _files_before_run(CLIENT, sys.executable)
        external_code = f"""
import array, os, resource, socket, sys
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert os.getresuid() == ({outside_lab.uid},) * 3
assert os.getresgid() == ({outside_lab.gid},) * 3 and os.getgroups() == []
os.write(1, ("outsider %d\\n" % os.getpid()).encode("ascii"))
_relay = socket.socket(fileno={receiver.fileno()})
_data, _controls, _flags, _ = _relay.recvmsg(32, socket.CMSG_SPACE(2 * array.array("i").itemsize))
assert _data == b"real-quota-endpoints"
assert not _flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC)
assert len(_controls) == 1
_level, _kind, _bytes = _controls[0]
assert (_level, _kind) == (socket.SOL_SOCKET, socket.SCM_RIGHTS)
_fds = array.array("i")
_fds.frombytes(_bytes)
assert len(_fds) == 2 and _fds[0] != _fds[1]
os.environ["PF_RUN_QUOTA_MODE"] = "managed"
os.environ["PF_RUN_QUOTA_NATIVE_FD"] = str(_fds[0])
os.environ["PF_RUN_QUOTA_PYTHON_FD"] = str(_fds[1])
sys.path.insert(0, {str(CLIENT.parent)!r})
import run_quota as q
assert os.path.abspath(q.__file__) == {str(CLIENT)!r} and q.mode() == "managed"
_channel = socket.socket(fileno=_fds[{endpoint}])
_hello = q.Message.make(q.Opcode.HELLO, 1, role=q.Role.WORKER).encode()
assert len(_hello) == q.WIRE_SIZE == 256
assert _channel.send(_hello) == 256
os.write(1, b"valid-hello-sent 256\\n")
try:
    _response = _channel.recv(257)
except ConnectionResetError:
    _response = b""
if _response:
    _reply = q.Message.decode(_response)
    assert _reply.opcode == int(q.Opcode.HELLO) | q.REPLY and _reply.sequence == 1
    assert q.refusal_from_message(_reply) is not None, "non-member HELLO was admitted"
os.write(1, b"nonmember-refused\\n")
"""
        outside = subprocess.Popen(
            [sys.executable, "-I", "-B", "-c", external_code],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            pass_fds=(receiver.fileno(),), user=outside_lab.uid,
            group=outside_lab.gid, extra_groups=(),
            env={"PATH": os.defpath, "LC_ALL": "C"}, cwd=ROOT, start_new_session=True,
            bufsize=0,
        )
        receiver.close()
        assert _bounded_line(outside.stdout) == f"outsider {outside.pid}\n".encode("ascii")
        outside_pidfd = os.pidfd_open(outside.pid, 0)
        outside_identity = _process_identity(outside.pid)
        assert outside_identity[0] == os.getpid() and outside_identity[1] == outside.pid
        assert outside_identity[2] > 0
        fields = _proc(outside.pid, "stat").rsplit(")", 1)[1].split()
        assert int(fields[3]) == outside.pid
        status = _proc(outside.pid, "status").splitlines()
        for key, expected in (("Uid:", outside_lab.uid), ("Gid:", outside_lab.gid)):
            row = next(line for line in status if line.startswith(key))
            assert tuple(map(int, row.split()[1:])) == (expected,) * 4
        assert not next(line for line in status if line.startswith("Groups:")).split()[1:]
        code = _credentials(lab) + f"""
import array, socket
_relay = socket.socket(fileno={transfer.fileno()})
_pair = array.array("i", [int(os.environ[_key]) for _key in
                       ("PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD")])
assert _relay.sendmsg([b"real-quota-endpoints"],
                     [(socket.SOL_SOCKET, socket.SCM_RIGHTS, _pair)]) == 20
for _fd in _pair:
    os.close(_fd)
_relay.close()
_await_observer()  # Parent authentication must refuse and kill this owned run.
raise AssertionError("non-member did not cause a parent-origin refusal")
"""
        with _Run(lab, code, pass_fds=(transfer.fileno(),)) as run:
            assert outside.pid != run.process.pid
            transfer.close()
            returncode, receipt = run.collect()
            print(f"actual identity-refusal receipt uid={lab.uid} gid={lab.gid}: "
                  + json.dumps(receipt, sort_keys=True), flush=True)
            assert returncode != 0, receipt
            assert receipt["launcher_failure"] == {
                "code": "quota_launcher_error",
                "args": {"stage": "authentication", "reason": reason},
            }, receipt
            assert receipt["ledger_verdict"] is None
            assert receipt["signal"] == signal.SIGKILL
            assert os.WIFSIGNALED(receipt["wait_status"])
            assert os.WTERMSIG(receipt["wait_status"]) == signal.SIGKILL
            assert receipt["deadline"]["expired"] is False
            assert not run.stdout and not run.stderr
            run.assert_dead_and_reaped(0)
        # Keep the non-member generation pinned until its own actual wait.
        assert _process_identity(outside.pid) == outside_identity
        stdout, stderr = outside.communicate()
        assert outside.returncode == 0 and not stderr
        assert stdout == b"valid-hello-sent 256\nnonmember-refused\n"
    finally:
        if outside is not None:
            if outside.poll() is None:
                if outside_pidfd is not None:
                    try:
                        signal.pidfd_send_signal(outside_pidfd, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                else:
                    outside.kill()  # Still the unreaped direct Popen child.
                outside.wait()
            outside.stdout.close()
            outside.stderr.close()
        if outside_pidfd is not None:
            os.close(outside_pidfd)
        transfer.close()
        receiver.close()


def test_native_and_python_endpoints_share_parent_ledger(lab):
    _exercise_shared_endpoints(lab)


@pytest.mark.parametrize("value", HIGH_IDENTITIES)
@pytest.mark.parametrize("field", ("uid", "gid"))
def test_full_range_kernel_identities_use_both_endpoints_and_shared_ledger(lab, field, value):
    identity = Lab(value, lab.gid) if field == "uid" else Lab(lab.uid, value)
    _exercise_shared_endpoints(identity)


def _exercise_shared_endpoints(lab):
    code = _credentials(lab) + _attach_client() + """
import socket
_native = socket.socket(fileno=os.dup(int(os.environ["PF_RUN_QUOTA_NATIVE_FD"])))
_sequence, _nonce = 0, (0, 0)
def _native_exchange(_opcode, **_fields):
    global _sequence
    _sequence += 1
    _fields["role"] = q.Role.WORKER
    if _opcode != q.Opcode.HELLO:
        _fields.update(nonce_hi=_nonce[0], nonce_lo=_nonce[1])
    _request = q.Message.make(_opcode, _sequence, **_fields)
    assert _native.send(_request.encode()) == q.WIRE_SIZE
    _data, _, _flags, _ = _native.recvmsg(q.WIRE_SIZE + 1)
    assert not _flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC)
    _reply = q.Message.decode(_data)
    assert _reply.opcode == int(_opcode) | q.REPLY and _reply.sequence == _sequence
    if _opcode != q.Opcode.HELLO:
        assert (_reply[q.Slot.NONCE_HI], _reply[q.Slot.NONCE_LO]) == _nonce
    return _reply
_hello = _native_exchange(q.Opcode.HELLO)
assert q.refusal_from_message(_hello) is None
assert _hello[q.Slot.PID] == os.getpid()
assert _hello[q.Slot.ROLE] == q.Role.WORKER
assert _hello[q.Slot.SOFT_NOFILE] == resource.getrlimit(resource.RLIMIT_NOFILE)[0]
assert _hello[q.Slot.GENERATION] != client.owner
_nonce = (_hello[q.Slot.NONCE_HI], _hello[q.Slot.NONCE_LO])
assert any(_nonce) and _nonce == client._nonce
_python_ticket = client.reserve(q.Amounts(output_bytes=5), artifact=q.Artifact.STDOUT)
_usage = _native_exchange(q.Opcode.SNAPSHOT, resource=q.Resource.OUTPUT_BYTES)
assert q.refusal_from_message(_usage) is None
assert (_usage[q.Slot.COMMITTED], _usage[q.Slot.RESERVED], _usage[q.Slot.USED]) == (0, 5, 5)
_reserved = _native_exchange(q.Opcode.RESERVE, stage=q.Stage.EXECUTION,
                            artifact=q.Artifact.STDOUT, delta_output=7)
assert q.refusal_from_message(_reserved) is None
_ticket = _reserved[q.Slot.TICKET]
assert _ticket > 0
_python_usage = client.snapshot(q.Resource.OUTPUT_BYTES)
assert (_python_usage.committed, _python_usage.reserved, _python_usage.used) == (0, 12, 12)
assert os.write(1, b"n" * 7) == 7
assert q.refusal_from_message(_native_exchange(q.Opcode.COMMIT, ticket=_ticket,
                                              delta_output=7)) is None
assert q.refusal_from_message(_native_exchange(q.Opcode.CANCEL, ticket=_ticket)) is None
assert os.write(1, b"p" * 5) == 5
_python_ticket.commit(q.Amounts(output_bytes=5), 0)
_python_ticket.cancel()
for _resource, _limit in zip(q.Resource, (65536, 64, 12, 64)):
    _python_usage = client.snapshot(_resource)
    _native_usage = _native_exchange(q.Opcode.SNAPSHOT, resource=_resource)
    assert q.refusal_from_message(_native_usage) is None
    assert _python_usage.limit == _native_usage[q.Slot.LIMIT] == _limit
    assert (_native_usage[q.Slot.COMMITTED], _native_usage[q.Slot.RESERVED],
            _native_usage[q.Slot.USED], _native_usage[q.Slot.PEAK]) == (
        _python_usage.committed, _python_usage.reserved,
        _python_usage.used, _python_usage.peak)
_usage = client.snapshot(q.Resource.OUTPUT_BYTES)
assert (_usage.committed, _usage.reserved, _usage.used, _usage.peak) == (12, 0, 12, 12)
_refused = _native_exchange(q.Opcode.RESERVE, stage=q.Stage.EXECUTION,
                           artifact=q.Artifact.STDOUT, delta_output=1)
_refusal = q.refusal_from_message(_refused)
assert _refusal is not None and _refusal.code == "run_resource_quota_exceeded"
assert dict(_refusal.arguments) == {
    "stage": "execution", "resource": "output_bytes", "artifact": "stdout",
    "used": 12, "requested": 1, "limit": 12, "peak": 12}
sys.exit(17)
"""
    with _Run(lab, code, policy=(POLICY[0], POLICY[1], "12", POLICY[3])) as run:
        returncode, receipt = run.collect()
        print(f"actual shared-ledger receipt uid={lab.uid} gid={lab.gid}: "
              + json.dumps(receipt, sort_keys=True), flush=True)
        assert returncode == 17, (returncode, receipt, bytes(run.stderr))
        _normal(receipt, 17)
        _quota_refusal(receipt, 12, 1, 12, 12)
        assert bytes(run.stdout) == b"n" * 7 + b"p" * 5
        assert bytes(run.stderr) == f"FILES fixture imports: {CLIENT}\n".encode("ascii")
        run.assert_dead_and_reaped(0)


def _cgroup2(fd):
    libc = ctypes.CDLL(None, use_errno=True)
    libc.fstatfs.argtypes = (ctypes.c_int, ctypes.c_void_p)
    libc.fstatfs.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    assert libc.fstatfs(fd, buffer) == 0, "SETUP: cgroup filesystem type unavailable"
    return ctypes.c_long.from_buffer(buffer).value == 0x63677270


def _cgroup_directory(path):
    """Root-owned, no-symlink walk; never follow an unchecked command path."""
    path = _permitted(path)
    assert path.is_absolute() and ".." not in path.parts
    print(f"FILES cgroup directory walk follows: {path}", file=sys.stderr, flush=True)
    current = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for part in path.parts[1:]:
            following = os.open(
                part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                dir_fd=current,
            )
            os.close(current)
            current = following
        assert _cgroup2(current), "SETUP: supplied root is not cgroup v2"
        item = os.fstat(current)
        assert item.st_uid == 0 and not item.st_mode & 0o022, "SETUP: delegated cgroup root"
        return current
    except BaseException:
        os.close(current)
        raise


def _cgroup_control(directory, path, name, flags=os.O_RDONLY):
    assert name in (
        "cgroup.procs", "cgroup.threads", "cgroup.events", "cgroup.type",
        "cgroup.controllers", "cgroup.subtree_control", "memory.events",
        "memory.max", "memory.swap.max", "memory.oom.group",
    )
    full = _permitted(path / name)
    print(f"FILES kernel cgroup control follows: {full}", file=sys.stderr, flush=True)
    descriptor = os.open(name, flags | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=directory)
    try:
        item = os.fstat(descriptor)
        assert _cgroup2(descriptor) and stat.S_ISREG(item.st_mode)
        assert item.st_uid == 0 and not item.st_mode & 0o022, "SETUP: writable/delegated control"
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def _cgroup_read(directory, path, name):
    descriptor = _cgroup_control(directory, path, name)
    try:
        data = os.read(descriptor, 65537)
        assert len(data) <= 65536, "SETUP: oversized kernel cgroup record"
        return data.decode("ascii")
    finally:
        os.close(descriptor)


def _kernel_events(directory, path, name):
    counters = {}
    for line in _cgroup_read(directory, path, name).splitlines():
        key, value = line.split()
        assert key not in counters
        assert value.isascii() and value.isdecimal()
        counters[key] = int(value)
    return counters


def _cgroup_write_leaf(directory, path, name, value):
    # Only callers holding the just-created subgroup descriptor call this.
    descriptor = _cgroup_control(directory, path, name, os.O_WRONLY)
    try:
        data = (str(value) + "\n").encode("ascii")
        assert os.write(descriptor, data) == len(data), "SETUP: incomplete subgroup limit write"
    finally:
        os.close(descriptor)
    assert _cgroup_read(directory, path, name).strip() == str(value)


def _unified_membership(pid):
    rows = [line[3:] for line in _proc(pid, "cgroup").splitlines() if line.startswith("0::")]
    assert len(rows) == 1, "SETUP: unified cgroup membership unavailable"
    return rows[0]


def _cgroup_kernel_path(path, directory):
    # The root kit must expose the complete cgroup mount, including ancestors.
    source = _permitted(Path("/proc/self/mountinfo"))
    _files_before_run(source)
    metadata = os.fstat(directory)
    device = f"{os.major(metadata.st_dev)}:{os.minor(metadata.st_dev)}"

    def unescape(text):
        return (text.replace("\\040", " ").replace("\\011", "\t")
                .replace("\\012", "\n").replace("\\134", "\\"))

    best = None
    for line in source.read_text(encoding="ascii").splitlines():
        left, separator, right = line.partition(" - ")
        if not separator or right.split()[0] != "cgroup2":
            continue
        fields = left.split()
        if fields[2] != device:
            continue
        mount = unescape(fields[4])
        if str(path) != mount and not str(path).startswith(mount.rstrip("/") + "/"):
            continue
        if best is None or len(mount) > best[0]:
            best = (len(mount), unescape(fields[3]), mount)
    assert best is not None and best[1] == "/", "SETUP: hidden/unmapped cgroup ancestors"
    suffix = str(path)[len(best[2]):].lstrip("/")
    result = ("/" + suffix).rstrip("/") or "/"
    assert result != "/", "SETUP: refusing the host cgroup root"
    return result


def _protected_cgroup_ancestors(directory, path):
    current = os.dup(directory)
    try:
        while _cgroup2(current):
            item = os.fstat(current)
            assert item.st_uid == 0 and not item.st_mode & 0o022, "SETUP: delegated ancestor"
            for name in ("cgroup.procs", "cgroup.threads"):
                descriptor = _cgroup_control(current, path, name)
                os.close(descriptor)
            parent = os.open(
                "..", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
                dir_fd=current,
            )
            parent_item = os.fstat(parent)
            if (parent_item.st_dev, parent_item.st_ino) == (item.st_dev, item.st_ino):
                os.close(parent)
                break
            os.close(current)
            current = parent
            path = path.parent
    finally:
        os.close(current)


@pytest.fixture
def _owned_oom_subgroup(lab):
    supplied = os.environ.get("PF_QUOTA_TEST_CGROUP_ROOT")
    assert supplied and os.path.isabs(supplied), (
        "SETUP: sealed root kit must supply an exclusive PF_QUOTA_TEST_CGROUP_ROOT")
    assert ".." not in Path(supplied).parts
    path = _permitted(supplied)
    directory = _cgroup_directory(path)
    subgroup = None
    try:
        _protected_cgroup_ancestors(directory, path)
        kernel_root = _cgroup_kernel_path(path, directory)
        membership = _unified_membership(os.getpid())
        assert membership != kernel_root and not membership.startswith(kernel_root + "/"), (
            "SETUP: supplied cgroup contains the harness")
        assert not _cgroup_read(directory, path, "cgroup.procs").strip()
        assert _kernel_events(directory, path, "cgroup.events")["populated"] == 0, (
            "SETUP: refusing populated/shared cgroup root")
        assert _cgroup_read(directory, path, "cgroup.type").strip() == "domain"
        assert "memory" in _cgroup_read(directory, path, "cgroup.controllers").split()
        assert "memory" in _cgroup_read(directory, path, "cgroup.subtree_control").split(), (
            "SETUP: root kit must enable memory for subgroups; test never edits ancestors")
        name = f"pf-quota-proof-{os.getpid()}-{time.monotonic_ns()}"
        leaf = _permitted(path / name)
        print(f"FILES new exclusively owned cgroup subgroup: {leaf}",
              file=sys.stderr, flush=True)
        os.mkdir(name, mode=0o700, dir_fd=directory)
        subgroup = _cgroup_directory(leaf)
        # These writes are confined to a newly created, still-empty subgroup.
        assert not _cgroup_read(subgroup, leaf, "cgroup.procs").strip()
        assert _kernel_events(subgroup, leaf, "cgroup.events")["populated"] == 0
        assert _cgroup_read(subgroup, leaf, "cgroup.type").strip() == "domain"
        _cgroup_write_leaf(subgroup, leaf, "memory.max", 64 * 1024 * 1024)
        _cgroup_write_leaf(subgroup, leaf, "memory.swap.max", 0)
        _cgroup_write_leaf(subgroup, leaf, "memory.oom.group", 1)
        # Prove the SUT's observation lock is available, then release it for
        # that SUT. An unsupported lock is a real fixture SETUP error.
        fcntl.flock(subgroup, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(subgroup, fcntl.LOCK_UN)
        _files_before_run(leaf / "memory.events")
        yield subgroup, leaf, kernel_root.rstrip("/") + "/" + name, membership
    finally:
        if subgroup is not None:
            os.close(subgroup)
        os.close(directory)
        # Intentionally retain the subgroup, empty after _Run cleanup. The
        # sealed kit's owner handles any later housekeeping, not this test.


def test_populated_owned_cgroup_memory_events_stays_unknown(lab, _owned_oom_subgroup):
    directory, path, kernel_path, harness_membership = _owned_oom_subgroup
    sleeper = sleeper_pidfd = None
    try:
        _files_before_run(sys.executable)
        sleeper_code = f"""
import os, resource
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert os.getresuid() == ({lab.uid},) * 3
assert os.getresgid() == ({lab.gid},) * 3 and os.getgroups() == []
os.write(1, ("populated-owned %d\\n" % os.getpid()).encode("ascii"))
assert os.read(0, 1) == b"."
"""
        sleeper = subprocess.Popen(
            [sys.executable, "-I", "-B", "-c", sleeper_code],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            user=lab.uid, group=lab.gid, extra_groups=(),
            env={"PATH": os.defpath, "LC_ALL": "C"}, cwd=ROOT,
            start_new_session=True, bufsize=0,
        )
        sleeper_pidfd = os.pidfd_open(sleeper.pid, 0)
        identity = _process_identity(sleeper.pid)
        assert identity[0] == os.getpid() and identity[1] == sleeper.pid and identity[2] > 0
        assert _bounded_line(sleeper.stdout) == f"populated-owned {sleeper.pid}\n".encode("ascii")
        status = _proc(sleeper.pid, "status").splitlines()
        for key, value in (("Uid:", lab.uid), ("Gid:", lab.gid)):
            row = next(line for line in status if line.startswith(key))
            assert tuple(map(int, row.split()[1:])) == (value,) * 4
        assert not next(line for line in status if line.startswith("Groups:")).split()[1:]
        assert not _cgroup_read(directory, path, "cgroup.procs").strip()
        _cgroup_write_leaf(directory, path, "cgroup.procs", sleeper.pid)
        assert _process_identity(sleeper.pid) == identity
        assert _unified_membership(sleeper.pid) == kernel_path
        assert _kernel_events(directory, path, "cgroup.events")["populated"] == 1
        before = _kernel_events(directory, path, "memory.events")
        assert "oom" in before and "oom_kill" in before
        observed = 0
        code = _credentials(lab) + """
os.write(1, ("populated-negative %d\\n" % os.getpid()).encode("ascii"))
_await_observer()
sys.exit(17)
"""

        def observe(run, line):
            nonlocal observed
            kind, number = line.decode("ascii").split()
            pid = int(number)
            assert kind == "populated-negative" and observed == 0
            run.remember(0, pid, run.process.pid)
            # Unknown counters alone would also pass a late population check.
            # Prove the SUT refused this observation BEFORE migrating its child.
            assert _unified_membership(pid) == harness_membership
            assert _unified_membership(run.process.pid) == harness_membership
            assert _unified_membership(os.getpid()) == harness_membership
            assert _process_identity(sleeper.pid) == identity
            assert not select.select((sleeper_pidfd,), (), (), 0)[0]
            assert _unified_membership(sleeper.pid) == kernel_path
            assert tuple(map(int, _cgroup_read(directory, path, "cgroup.procs").split())) == (
                sleeper.pid,)
            assert _kernel_events(directory, path, "cgroup.events")["populated"] == 1
            observed += 1
            run.acknowledge()

        with _Run(lab, code, memory_events=path / "memory.events") as run:
            returncode, receipt = run.collect(observe)
            assert observed == 1 and returncode == 17
            _normal(receipt, 17)
            _unknown_oom(receipt)
            assert receipt["ledger_verdict"] is None
            assert not run.stdout and not run.stderr
            run.assert_dead_and_reaped(0)
        assert _process_identity(sleeper.pid) == identity
        assert not select.select((sleeper_pidfd,), (), (), 0)[0]
        assert _unified_membership(sleeper.pid) == kernel_path
        after = _kernel_events(directory, path, "memory.events")
        assert (after["oom"], after["oom_kill"]) == (before["oom"], before["oom_kill"])
        stdout, stderr = sleeper.communicate(b".")
        assert sleeper.returncode == 0 and not stdout and not stderr
        assert select.select((sleeper_pidfd,), (), (), 0)[0]
        assert not _cgroup_read(directory, path, "cgroup.procs").strip()
        assert _kernel_events(directory, path, "cgroup.events")["populated"] == 0
        assert _unified_membership(os.getpid()) == harness_membership
    finally:
        if sleeper is not None:
            if sleeper.poll() is None:
                if sleeper_pidfd is not None:
                    try:
                        signal.pidfd_send_signal(sleeper_pidfd, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                else:
                    sleeper.kill()  # Unreaped direct Popen child; no PID reuse.
                sleeper.wait()
            for stream in (sleeper.stdin, sleeper.stdout, sleeper.stderr):
                stream.close()
        if sleeper_pidfd is not None:
            os.close(sleeper_pidfd)


def test_real_cgroup_v2_oom_has_attributable_kernel_evidence(lab, _owned_oom_subgroup):
    directory, path, kernel_path, harness_membership = _owned_oom_subgroup
    before = _kernel_events(directory, path, "memory.events")
    assert "oom_kill" in before and "oom" in before
    verified = 0
    code = _credentials(lab) + """
os.write(1, ("oom-ready %d\\n" % os.getpid()).encode("ascii"))
_await_observer()  # Independent kernel membership check must precede growth.
resource.setrlimit(resource.RLIMIT_AS, (256 * 1024 * 1024, 256 * 1024 * 1024))
_blocks = []
for _index in range(96):  # At most 96 MiB user payload; group limit is 64 MiB.
    _block = bytearray(1024 * 1024)
    for _page in range(0, len(_block), 4096):
        _block[_page] = 1
    _blocks.append(_block)
raise AssertionError("bounded allocation completed without an attributable kernel OOM")
"""

    def observe(run, line):
        nonlocal verified
        kind, pid = line.decode("ascii").split()
        assert kind == "oom-ready" and verified == 0
        pid = int(pid)
        run.remember(0, pid, run.process.pid)
        _only_direct_child(run.process.pid, pid)
        assert _unified_membership(pid) == kernel_path
        assert _unified_membership(run.process.pid) != kernel_path
        assert _unified_membership(os.getpid()) == harness_membership
        assert tuple(map(int, _cgroup_read(directory, path, "cgroup.procs").split())) == (pid,)
        assert _kernel_events(directory, path, "cgroup.events")["populated"] == 1
        gated = _kernel_events(directory, path, "memory.events")
        assert gated["oom_kill"] == before["oom_kill"] and gated["oom"] == before["oom"]
        assert _cgroup_read(directory, path, "memory.max").strip() == str(64 * 1024 * 1024)
        assert _cgroup_read(directory, path, "memory.swap.max").strip() == "0"
        verified += 1
        run.acknowledge()

    with _Run(lab, code, memory_events=path / "memory.events") as run:
        returncode, receipt = run.collect(observe)
        assert verified == 1, "allocation was never released after verified membership"
        after = _kernel_events(directory, path, "memory.events")
        delta = after["oom_kill"] - before["oom_kill"]
        assert delta > 0 and after["oom"] > before["oom"], "no positive kernel OOM movement"
        assert returncode == 128 + signal.SIGKILL
        assert os.WIFSIGNALED(receipt["wait_status"])
        assert os.WTERMSIG(receipt["wait_status"]) == signal.SIGKILL
        assert receipt["signal"] == signal.SIGKILL and receipt["exit_code"] is None
        assert {key: value for key, value in receipt["oom"].items()
                if key != "diagnostic"} == {
            "proven": True, "before": before["oom_kill"],
            "after": after["oom_kill"], "delta": delta,
        }, "receipt omitted/misattributed real kernel OOM evidence"
        assert receipt["oom"].get("diagnostic") is None
        assert receipt["launcher_failure"] is None and receipt["ledger_verdict"] is None
        assert receipt["deadline"] == {"seconds": None, "expired": False, "kill_sent": False}
        assert not run.stderr and not run.stdout
        run.assert_dead_and_reaped(0)
        assert not _cgroup_read(directory, path, "cgroup.procs").strip()
        assert _kernel_events(directory, path, "cgroup.events")["populated"] == 0
        assert _unified_membership(os.getpid()) == harness_membership
        print(f"actual kernel OOM proof: group={path}, before={before['oom_kill']}, "
              f"after={after['oom_kill']}, delta={delta}, wait={receipt['wait_status']}")
