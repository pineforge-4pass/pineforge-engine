#!/usr/bin/env python3
"""ROOT policy proof; source-only until separately admitted and run remotely.

This file is deliberately outside the run_json_*_test.py discovery glob. It
executes the complete shipped supervisor, never imports it, and imports the
shipped ledger only in real workload actors. The caller must supply the
deadline policy for the deadline control; all other rows omit that option.
No harness time limit, elapsed-time oracle, provider action, installation,
OOM induction, or skip is supplied here.

R01 checks the nonfatal oom.diagnostic envelope and the exact requested path.
Receipt-version treatment requires a publication decision; see the companion
runtime contract. Expected objects never substitute for observed evidence.

Every work process has its own RESULT, completion binding, and exit record.
The controller also records its actual wait for each work process. C00 really
crashes a work process through the same recorder before any product controls.
A failed control stops discovery-order execution. Missing facilities, broken
evidence, interruptions, and setup failures are INCOMPLETE, never target RED.
"""

from __future__ import annotations

import argparse
import ctypes
from dataclasses import asdict, dataclass
import errno
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import select
import selectors
import signal
import socket
import stat
import struct
import subprocess
import sys
import traceback


ROOT = Path(__file__).absolute().parents[1]
SELF = ROOT / "docker" / "pf_quota_policy_privileged_proof.py"
SUPERVISOR = ROOT / "docker" / "pf_quota_run.py"
LEDGER = ROOT / "docker" / "run_quota.py"
SCRATCH = ROOT / ".policy-author-scratch"
SCHEMA = "pf-quota-receipt/v1"
FIELDS = {
    "schema", "quota_line", "ledger_verdict", "wait_status", "exit_code",
    "signal", "oom", "deadline", "launcher_failure",
}
LIMIT_NAMES = (
    "PF_RUN_SCRATCH_BYTES", "PF_RUN_SCRATCH_FILES",
    "PF_RUN_OUTPUT_BYTES", "PF_RUN_DESCRIPTOR_LIMIT",
)
ENDPOINT_NAMES = ("PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD")
REFUSED_COMPONENTS = (
    ("ssh", (".ssh",)),
    ("aws", (".aws",)),
    ("codex", (".codex",)),
    ("env-suffix", ("requested.env",)),
    ("zsh-keys", (".config", "zsh", "keys")),
    ("pineforge-config", (".config", "pineforge")),
)


class Incomplete(RuntimeError):
    """The requested experiment did not establish its prerequisites/evidence."""


class ContractMismatch(AssertionError):
    def __init__(self, assertion_id, expected, actual):
        self.assertion_id, self.expected, self.actual = assertion_id, expected, actual
        super().__init__(assertion_id)


class DeliberatePolicyProofCrash(RuntimeError):
    pass


def need(condition, message):
    if not condition:
        raise Incomplete(message)


def compare(assertion_id, expected, actual):
    if expected != actual:
        raise ContractMismatch(assertion_id, expected, actual)


def forbidden(path):
    parts = Path(path).parts
    return (
        any(p in (".ssh", ".aws", ".codex") or p.endswith(".env") for p in parts)
        or any(parts[i:i + 3] == (".config", "zsh", "keys")
               for i in range(len(parts)))
        or any(parts[i:i + 2] == (".config", "pineforge")
               for i in range(len(parts)))
    )


def named(path):
    """Check lexical names before lstat/readlink/open, including link targets."""
    pending = Path(path).absolute()
    need(not forbidden(pending), f"REFUSED sensitive path by name: {pending}")
    need(".." not in pending.parts, f"REFUSED parent traversal: {pending}")
    print(f"FILES will follow: {pending}", file=sys.stderr, flush=True)
    seen = set()
    while True:
        need(str(pending) not in seen, "REFUSED symlink cycle")
        seen.add(str(pending))
        current = Path(pending.anchor)
        for index, part in enumerate(pending.parts[1:], 1):
            current = current / part
            need(not forbidden(current), f"REFUSED sensitive path by name: {current}")
            item = os.lstat(current)
            if stat.S_ISLNK(item.st_mode):
                target = Path(os.readlink(current))
                if not target.is_absolute():
                    target = current.parent / target
                target = target.joinpath(*pending.parts[index + 1:])
                # Check BEFORE normalizing: a sensitive lexical component
                # cannot be hidden by a later '..'.
                need(not forbidden(target), f"REFUSED sensitive link target: {target}")
                pending = Path(os.path.normpath(target))
                need(not forbidden(pending), f"REFUSED sensitive link target: {pending}")
                print(f"FILES will follow checked link target: {pending}",
                      file=sys.stderr, flush=True)
                break
        else:
            return pending


def read_bytes(path):
    return named(path).read_bytes()


def read_text(path):
    return read_bytes(path).decode("ascii")


def new_file(path, data):
    path = Path(path).absolute()
    need(not forbidden(path) and ".." not in path.parts,
         f"REFUSED output path by name: {path}")
    named(path.parent)
    need(path.is_relative_to(SCRATCH), "output must stay under .policy-author-scratch")
    print(f"FILES will create exclusively: {path}", file=sys.stderr, flush=True)
    with path.open("xb") as target:
        target.write(data)
        target.flush()
        os.fsync(target.fileno())


def encoded(value):
    return (json.dumps(value, ensure_ascii=True, allow_nan=False,
                       sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")


def new_json(path, value):
    new_file(path, encoded(value))


def unique(pairs):
    result = {}
    for key, value in pairs:
        need(key not in result, f"duplicate evidence JSON key: {key}")
        result[key] = value
    return result


def bad_constant(value):
    raise Incomplete(f"non-JSON constant: {value}")


def document(data):
    need(data.endswith(b"\n") and data.count(b"\n") == 1,
         "expected one newline-terminated ASCII JSON document")
    result = json.loads(data.decode("ascii"), object_pairs_hook=unique,
                        parse_constant=bad_constant)
    need(type(result) is dict, "evidence document is not an object")
    return result


def mkdir_new(path):
    path = Path(path).absolute()
    need(not forbidden(path) and ".." not in path.parts, "REFUSED directory name")
    need(path.is_relative_to(SCRATCH), "scratch directory escaped named root")
    named(path.parent)
    print(f"FILES will create exclusively: {path}", file=sys.stderr, flush=True)
    path.mkdir(mode=0o700)
    return path


@dataclass(frozen=True)
class Row:
    id: str
    kind: str
    target: bool = False
    endpoint: int = 1
    delegate: bool = False
    limit: int = 23
    produced: int = 5
    requested: int = 1
    exit_code: int = 0
    soft_nofile: int = 96
    number: int = 0
    literal_parts: tuple = ()


def rows():
    """Exact registered inputs, not a count-only inventory."""
    result = [Row("C00.recorder-deliberate-crash", "proof-crash")]
    for endpoint, label in enumerate(("native", "python")):
        for delegate, actor in ((False, "leader"), (True, "descendant")):
            tag = f"{label}.{actor}"
            size = 3 + 4 * endpoint + int(delegate)
            limit = 11 + 12 * endpoint + 2 * int(delegate)
            code = 17 if delegate else 0
            result.append(Row(f"C01.normal.{tag}", "normal", endpoint=endpoint,
                              delegate=delegate, limit=limit, produced=size,
                              exit_code=code))
            result.append(Row(f"C02.refusal-exit0.{tag}", "refusal", endpoint=endpoint,
                              delegate=delegate, limit=limit, produced=limit,
                              requested=2 + endpoint + int(delegate)))
        for kind in ("pgrp", "session"):
            result.append(Row(f"C03.membership.{label}.{kind}", kind, endpoint=endpoint))
        for soft in (64, 144):
            result.append(Row(f"C04.finite-nofile.{label}.{soft}", "nofile",
                              endpoint=endpoint, soft_nofile=soft))
        result.append(Row(f"C05.registration-recheck.{label}", "recheck",
                          endpoint=endpoint, soft_nofile=96 + 16 * endpoint))
        result.append(Row(f"C06.one-actor.{label}", "reuse", endpoint=endpoint))
    for code in (0, 17):
        result.append(Row(f"C07.cleanup.normal-exit{code}", "tree", exit_code=code))
    for number in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM):
        result.append(Row(f"C08.launcher-signal.{signal.Signals(number).name}",
                          "launcher-signal", number=int(number)))
    result.extend((
        Row("C09.explicit-caller-deadline", "deadline"),
        Row("C10.real-leader-crash", "crash"),
        Row("C11.permitted-noncgroup-path", "plain-path"),
        Row("C12.permitted-kernel-cgroup", "kernel-path"),
    ))
    for number, label in enumerate(("missing-uid", "missing-gid", "root-uid", "root-gid",
                                    "receipt-forwarding-collision")):
        result.append(Row(f"C13.precondition.{label}", "admission", number=number))
    result.append(Row("C14.real-exec-failure", "exec-failure"))
    for index, (label, parts) in enumerate(REFUSED_COMPONENTS):
        result.append(Row(f"R01.refused-path-diagnostic.{label}", "refused-path",
                          target=True, endpoint=index % 2, delegate=bool(index % 2),
                          limit=31 + index * 7, produced=2 + index,
                          literal_parts=parts))
    for endpoint, label in enumerate(("native", "python")):
        for delegate, actor in ((False, "leader"), (True, "descendant")):
            result.append(Row(f"R02.absent-deadline.{label}.{actor}", "no-deadline",
                              target=True, endpoint=endpoint, delegate=delegate,
                              limit=37 + endpoint * 11 + int(delegate),
                              produced=3 + endpoint * 2 + int(delegate),
                              exit_code=17 if delegate else 0))
    need(len({row.id for row in result}) == len(result), "duplicate registered row id")
    return tuple(result)


def proc(pid, leaf):
    need(type(pid) is int and pid > 0, "invalid observed pid")
    need(leaf in ("stat", "status", "limits", "cgroup"), "unregistered procfs leaf")
    return read_text(Path("/proc") / str(pid) / leaf)


def kernel_identity(pid):
    fields = proc(pid, "stat").rsplit(")", 1)[1].split()
    status = {}
    for line in proc(pid, "status").splitlines():
        key, _, tail = line.partition(":")
        if key in ("Uid", "Gid", "Groups"):
            status[key] = [int(value) for value in tail.split()]
        elif key in ("NoNewPrivs", "Seccomp"):
            status[key] = int(tail.strip())
        elif key in ("CapInh", "CapPrm", "CapEff", "CapBnd", "CapAmb", "SigIgn"):
            status[key] = int(tail.strip(), 16)
    nofile = None
    for line in proc(pid, "limits").splitlines():
        fields_limit = line.split()
        if fields_limit[:3] == ["Max", "open", "files"]:
            need(nofile is None, "duplicate kernel nofile record")
            nofile = fields_limit[3]
    need(nofile is not None and nofile.isascii() and nofile.isdecimal(),
         "actual finite kernel nofile is unavailable")
    return {
        "pid": pid, "ppid": int(fields[1]), "pgrp": int(fields[2]),
        "session": int(fields[3]), "generation": int(fields[19]),
        "status": status, "soft_nofile": int(nofile),
    }


def host_facts():
    need(sys.platform == "linux" and struct.calcsize("P") == 8,
         "requires real 64-bit Linux")
    need(os.getresuid() == (0, 0, 0), "requires real root uid fields")
    for owner, name in (
        (os, "pidfd_open"), (signal, "pidfd_send_signal"), (os, "waitid"),
        (os, "WNOWAIT"), (socket, "SO_PASSCRED"), (socket, "SCM_CREDENTIALS"),
        (resource, "prlimit"),
    ):
        need(hasattr(owner, name), f"unsupported required facility: {name}")
    return {
        "uname": list(os.uname()), "resuid": list(os.getresuid()),
        "resgid": list(os.getresgid()), "groups": os.getgroups(),
        "python": sys.version, "executable": str(named(sys.executable)),
        "boot_id": read_text("/proc/sys/kernel/random/boot_id").strip(),
        "process": kernel_identity(os.getpid()),
        "cgroup": proc(os.getpid(), "cgroup"),
    }


def sources():
    return {str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path): {
        "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
    } for path in (SELF, SUPERVISOR, LEDGER)
        for data in (read_bytes(path),)}


def emit(event, **fields):
    os.write(1, encoded({"event": event, **fields}))


def ack():
    need(os.read(0, 1) == b".", "observer acknowledgement missing")


def private_fd_probe(receipt_identity):
    directory = named("/proc/self/fd")
    identities = []
    for entry in os.listdir(directory):
        try:
            item = os.fstat(int(entry))
        except OSError as error:
            if error.errno == errno.EBADF:
                continue  # The descriptor used by listdir has already closed.
            raise
        identities.append([int(entry), item.st_dev, item.st_ino])
        need((item.st_dev, item.st_ino) != receipt_identity,
             "actual supervisor receipt pipe reached a workload process")
    return identities


def actor_ready(role, args, row, *, privileges=True):
    observed = kernel_identity(os.getpid())
    need(observed["status"]["Uid"] == [args.child_uid] * 4, "workload uid fields")
    need(observed["status"]["Gid"] == [args.child_gid] * 4, "workload gid fields")
    need(observed["status"]["Groups"] == [], "workload supplementary groups")
    if privileges:
        need(observed["status"]["NoNewPrivs"] == 1, "workload no_new_privs absent")
        for name in ("CapInh", "CapPrm", "CapEff", "CapBnd", "CapAmb"):
            need(observed["status"][name] == 0, f"retained workload {name}")
    emit("ready", role=role, identity=observed,
         fds=private_fd_probe((args.receipt_device, args.receipt_inode)))
    ack()


def load_client():
    path = named(LEDGER)
    sys.path.insert(0, str(path.parent))
    import run_quota as q
    need(Path(q.__file__).absolute() == LEDGER, "wrong ledger client imported")
    need(q.mode() == "managed", "actual client did not capture managed endpoints")
    return q


def attach(q, endpoint):
    descriptor = int(os.environ[ENDPOINT_NAMES[endpoint]])
    return q.QuotaClient(socket.socket(fileno=os.dup(descriptor)))


def usage(row):
    return [row.committed, row.reserved, row.peak, row.limit, row.used]


def finish_client(client):
    client.finish()
    client.close()


def workload(args, row):
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    need(hard == resource.RLIM_INFINITY or hard >= row.soft_nofile,
         "workload inherited insufficient hard nofile")
    resource.setrlimit(resource.RLIMIT_NOFILE, (row.soft_nofile, hard))
    actor_ready("leader", args, row)
    if row.kind == "tree":
        first = os.fork()
        if first == 0:
            os.setsid()
            actor_ready("intermediate", args, row)
            ready_read, ready_write = os.pipe2(os.O_CLOEXEC)
            second = os.fork()
            if second == 0:
                os.close(ready_read)
                actor_ready("sleeper", args, row)
                os.write(ready_write, b".")
                os.close(ready_write)
                while True:
                    signal.pause()
            os.close(ready_write)
            need(os.read(ready_read, 1) == b".", "sleeper handshake failed")
            os.close(ready_read)
            os._exit(0)
        waited, raw = os.waitpid(first, 0)
        need(waited == first and raw == 0, "intermediate did not exit normally")
        emit("adopted", intermediate=first, raw_status=raw)
        ack()
        return row.exit_code
    if row.kind in ("launcher-signal", "deadline", "crash"):
        if row.kind == "launcher-signal":
            for number in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM):
                signal.signal(number, signal.SIG_IGN)
        emit("armed", pid=os.getpid(), kind=row.kind)
        ack()
        if row.kind == "crash":
            os.abort()
        while True:
            signal.pause()
    if row.delegate or row.kind in ("pgrp", "session"):
        child = os.fork()
        if child:
            for name in ENDPOINT_NAMES:
                os.close(int(os.environ[name]))
            waited, raw = os.waitpid(child, 0)
            emit("delegate_wait", pid=waited, raw_status=raw)
            if os.WIFEXITED(raw):
                return os.WEXITSTATUS(raw)
            return 128 + os.WTERMSIG(raw)
        if row.kind == "pgrp":
            os.setpgid(0, 0)
        elif row.kind == "session":
            os.setsid()
        actor_ready("actor", args, row)
    else:
        emit("actor", pid=os.getpid())
    need(os.environ.get("PF_POLICY_PROOF_SENTINEL") == row.id,
         "unrelated inherited environment was changed")
    need(os.environ.get("PF_RUN_QUOTA_MODE") == "managed", "mode not replaced")
    need([os.environ.get(name) for name in LIMIT_NAMES] ==
         ["65536", "64", str(row.limit), str(row.soft_nofile + 17)],
         "trusted limits not replaced from explicit CLI")
    q = load_client()
    try:
        client = attach(q, row.endpoint)
    except q.QuotaError as error:
        if row.kind not in ("pgrp", "session", "recheck"):
            raise
        emit("authentication_channel_closed", code=error.code, args=error.code_args)
        # Keep the real leader alive for the SUT's authentication-error kill.
        # An EOF must not race an ordinary Python exception exit into the
        # negative control's expected SIGKILL wait.
        while True:
            signal.pause()
    emit("hello", pid=os.getpid(), owner=client.owner,
         initial=usage(client.snapshot(q.Resource.OUTPUT_BYTES)))
    if row.kind == "reuse":
        child = os.fork()
        if child == 0:
            actor_ready("second-actor", args, row)
            try:
                client.snapshot(q.Resource.OUTPUT_BYTES)
            except q.QuotaError as error:
                emit("second_refused", code=error.code, args=error.code_args)
                return 0
            raise RuntimeError("second actor unexpectedly used the bound endpoint")
        waited, raw = os.waitpid(child, 0)
        emit("second_wait", pid=waited, raw_status=raw)
        need(raw == 0, "second actor did not complete its real exchange")
        client.close()
        return 0
    if row.kind in ("pgrp", "session", "recheck"):
        # A HELLO reaching this point is itself the contract violation. The
        # worker judge will see a completed real HELLO rather than a fake error.
        finish_client(client)
        return 0
    if row.kind == "nofile":
        try:
            client.reserve(q.Amounts(descriptors=row.soft_nofile + 1))
        except q.QuotaError as error:
            emit("refused", code=error.code, args=error.code_args)
            client.close()
            return 0
        raise RuntimeError("kernel-recorded nofile was ignored by the real ledger")
    ticket = client.reserve(q.Amounts(output_bytes=row.produced), artifact=q.Artifact.SINK)
    emit("pending", usage=usage(client.snapshot(q.Resource.OUTPUT_BYTES)))
    payload = bytes((65 + row.endpoint,)) * row.produced
    need(os.write(args.payload_fd, payload) == len(payload),
         "actual output artifact write did not complete")
    ticket.commit(q.Amounts(output_bytes=row.produced), 0)
    ticket.cancel()
    emit("committed", usage=usage(client.snapshot(q.Resource.OUTPUT_BYTES)))
    if row.kind == "refusal":
        try:
            client.reserve(q.Amounts(output_bytes=row.requested), artifact=q.Artifact.SINK)
        except q.QuotaError as error:
            emit("refused", code=error.code, args=error.code_args)
            client.close()
            return 0  # Deliberately retain a genuine refused run with leader exit 0.
        raise RuntimeError("real ledger admitted over-limit reservation")
    finish_client(client)
    return row.exit_code


class Trace:
    """Kernel syscall barrier only; no product import, patch, or replacement.

    On native x86-64/aarch64, stop after the second successful pidfd_open for
    the observed actor. The first is authenticate's pin; the second is the
    real ledger's registration pin. Change the actor's actual soft limit at
    that stop, before the supervisor's registration recheck.
    """

    def __init__(self):
        need(os.uname().machine in ("x86_64", "aarch64"), "native ptrace ABI required")
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.ptrace.restype = ctypes.c_long
        self.libc.ptrace.argtypes = (
            ctypes.c_uint, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
        )
        self.pending = {}
        self.counts = {}
        self.tasks = set()
        self.stopped = set()
        self.change = None

    def call(self, request, pid, address=0, data=0):
        ctypes.set_errno(0)
        result = self.libc.ptrace(request, pid, ctypes.c_void_p(address),
                                  ctypes.c_void_p(data))
        if result == -1 and ctypes.get_errno():
            raise Incomplete(f"ptrace facility refused: errno={ctypes.get_errno()}")
        return result

    def begin(self, process):
        pid, raw = os.waitpid(process.pid, 0)
        need(pid == process.pid and os.WIFSTOPPED(raw)
             and os.WSTOPSIG(raw) == signal.SIGSTOP, "trace launch did not stop")
        self.tasks.add(pid)
        self.stopped.add(pid)
        # TRACESYSGOOD, TRACECLONE, EXITKILL. Never trace workload forks.
        self.call(0x4200, pid, data=1 | 8 | 0x100000)
        self.call(24, pid)  # PTRACE_SYSCALL
        self.stopped.discard(pid)

    def step(self, process, actor, original_soft):
        pid, raw = os.waitpid(-1, 0x40000000)  # __WALL: traced endpoint threads.
        if os.WIFEXITED(raw) or os.WIFSIGNALED(raw):
            self.tasks.discard(pid)
            self.stopped.discard(pid)
            if pid == process.pid:
                process.returncode = os.waitstatus_to_exitcode(raw)
                return raw
            return None
        need(os.WIFSTOPPED(raw), "unexpected ptrace wait state")
        self.tasks.add(pid)
        self.stopped.add(pid)
        stopped = os.WSTOPSIG(raw)
        event = raw >> 16
        if event == 3:  # PTRACE_EVENT_CLONE
            child = ctypes.c_ulong()
            self.call(0x4201, pid, data=ctypes.addressof(child))
            self.tasks.add(child.value)
        elif stopped == (signal.SIGTRAP | 0x80):
            # Linux ptrace_syscall_info: 24-byte header followed by a union.
            # Read actual syscall metadata instead of guessing register layout.
            info = ctypes.create_string_buffer(128)
            size = self.call(0x420E, pid, address=128, data=ctypes.addressof(info))
            need(size >= 24, "PTRACE_GET_SYSCALL_INFO unsupported")
            operation = info.raw[0]
            arch = struct.unpack_from("=I", info.raw, 4)[0]
            need(arch in (0xC000003E, 0xC00000B7), "unexpected traced syscall ABI")
            if operation == 1:
                number, arg0 = struct.unpack_from("=QQ", info.raw, 24)
                if number == 434 and actor is not None and arg0 == actor["pid"]:
                    self.counts[actor["pid"]] = self.counts.get(actor["pid"], 0) + 1
                    self.pending[pid] = self.counts[actor["pid"]]
                else:
                    self.pending.pop(pid, None)
            elif operation == 2:
                occurrence = self.pending.pop(pid, None)
                if occurrence == 2 and self.change is None:
                    returned = struct.unpack_from("=q", info.raw, 24)[0]
                    need(returned >= 0, "registration pidfd_open did not succeed")
                    before = kernel_identity(actor["pid"])
                    need(before == actor, "actor changed before registration intervention")
                    limits = resource.prlimit(actor["pid"], resource.RLIMIT_NOFILE)
                    need(limits[0] == original_soft, "unexpected pre-intervention nofile")
                    resource.prlimit(actor["pid"], resource.RLIMIT_NOFILE,
                                     (original_soft - 1, limits[1]))
                    after = kernel_identity(actor["pid"])
                    need(after["soft_nofile"] == original_soft - 1,
                         "real prlimit intervention did not change the kernel limit")
                    self.change = {
                        "tid": pid, "syscall": 434, "occurrence": occurrence,
                        "return_value": returned, "before": before, "after": after,
                    }
        forwarded = 0 if event or stopped in (signal.SIGTRAP, signal.SIGSTOP,
                                               signal.SIGTRAP | 0x80) else stopped
        self.call(24, pid, data=forwarded)
        self.stopped.discard(pid)
        return None

    def cleanup(self, process):
        """Release trace barriers for the SUT's own interrupted-run cleanup.

        This path never supplies experiment evidence and never retries the
        refused metadata/intervention operation.
        """
        for pid in tuple(self.stopped):
            self.call(7, pid)  # PTRACE_CONT, no more syscall experiments.
            self.stopped.discard(pid)
        while self.tasks:
            pid, raw = os.waitpid(-1, 0x40000000)
            if os.WIFEXITED(raw) or os.WIFSIGNALED(raw):
                self.tasks.discard(pid)
                if pid == process.pid:
                    process.returncode = os.waitstatus_to_exitcode(raw)
            elif os.WIFSTOPPED(raw):
                self.tasks.add(pid)
                event, number = raw >> 16, os.WSTOPSIG(raw)
                self.call(7, pid, data=0 if event or number in
                          (signal.SIGTRAP, signal.SIGSTOP, signal.SIGTRAP | 0x80) else number)


def traced_exec():
    trace = Trace()
    trace.call(0, 0)  # PTRACE_TRACEME, before the complete supervisor exec.
    os.kill(os.getpid(), signal.SIGSTOP)
    need(len(sys.argv) > 2 and sys.argv[2] == "--", "invalid trace exec boundary")
    # Every explicit file has already been listed and checked by the parent.
    os.execv(sys.executable, (sys.executable, "-I", "-B", *sys.argv[3:]))


def cgroup_snapshot(path):
    path = named(path)
    need(path.name == "memory.events", "control must name actual memory.events")
    parent = named(path.parent)
    item = os.stat(parent)
    need(item.st_uid == 0 and not item.st_mode & 0o022, "delegated cgroup control")
    fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        libc.fstatfs.argtypes = (ctypes.c_int, ctypes.c_void_p)
        libc.fstatfs.restype = ctypes.c_int
        fs = ctypes.create_string_buffer(4096)
        need(libc.fstatfs(fd, fs) == 0
             and ctypes.c_long.from_buffer(fs).value == 0x63677270,
             "control is not kernel cgroup v2")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)
    counters = {}
    for line in read_text(path).splitlines():
        key, value = line.split()
        need(key not in counters and value.isascii() and value.isdecimal(),
             "unusable real memory.events")
        counters[key] = int(value)
    population = dict(line.split() for line in
                      read_text(parent / "cgroup.events").splitlines())
    procs = read_text(parent / "cgroup.procs").split()
    device = f"{os.major(item.st_dev)}:{os.minor(item.st_dev)}"
    mounts = []
    for line in read_text("/proc/self/mountinfo").splitlines():
        left, separator, right = line.partition(" - ")
        if not separator or right.split()[0] != "cgroup2":
            continue
        fields = left.split()
        mount = fields[4].replace("\\040", " ").replace("\\011", "\t")
        mount = mount.replace("\\012", "\n").replace("\\134", "\\")
        if fields[2] == device and (
                str(parent) == mount or str(parent).startswith(mount.rstrip("/") + "/")):
            mounts.append((len(mount), fields[3], mount))
    need(mounts, "kernel cgroup mount mapping unavailable")
    _, mount_root, mount = max(mounts)
    need(mount_root == "/", "cgroup ancestors hidden by subtree mount")
    kernel_path = "/" + str(parent)[len(mount):].lstrip("/")
    need(kernel_path != "/", "control must not be the host cgroup root")
    return {"counters": counters, "population": population, "procs": procs,
            "kernel_path": kernel_path}


def membership(pid):
    rows_found = [line[3:] for line in proc(pid, "cgroup").splitlines()
                  if line.startswith("0::")]
    need(len(rows_found) == 1, "unified membership unavailable")
    return rows_found[0]


class Observation:
    def __init__(self, row, args, directory):
        self.row, self.args, self.directory = row, args, directory
        self.events, self.owned = [], {}
        self.buffers = {"stdout": bytearray(), "stderr": bytearray(), "receipt": bytearray()}
        self.partial = bytearray()
        self.process = self.launcher_pin = None
        self.trace = Trace() if row.kind == "recheck" else None
        self.actor = None
        self.kernel_before = self.kernel_after = None
        self.requested_path = None
        self.launcher_raw = None
        self.payload = None
        self.payload_fd = None
        self.launcher_identity = None
        self.kernel_memberships = None

    def event(self, value):
        need(type(value.get("event")) is str, "unusable workload event")
        self.events.append(value)
        kind = value["event"]
        if kind == "ready":
            role = value["role"]
            observed = value["identity"]
            actual = kernel_identity(observed["pid"])
            need(actual == observed, "workload identity disagrees with live kernel")
            leader = self.owned.get("leader")
            if role == "leader":
                need(actual["ppid"] == self.process.pid
                     and actual["pgrp"] == actual["session"] == actual["pid"],
                     "not the supervisor's actual session leader")
            elif role == "intermediate":
                need(actual["ppid"] == leader["pid"]
                     and actual["pgrp"] == actual["session"] == actual["pid"],
                     "escaped intermediate not independently established")
            elif role == "sleeper":
                first = self.owned["intermediate"]
                need(actual["ppid"] == first["pid"]
                     and actual["pgrp"] == actual["session"] == first["pid"],
                     "escaped sleeper not independently established")
            else:
                need(actual["ppid"] == leader["pid"], "actor is not the owned descendant")
                if self.row.kind == "pgrp":
                    need(actual["pgrp"] == actual["pid"]
                         and actual["session"] == leader["pid"],
                         "new-pgrp negative was not established")
                elif self.row.kind == "session":
                    need(actual["pgrp"] == actual["session"] == actual["pid"],
                         "new-session negative was not established")
                else:
                    need(actual["pgrp"] == actual["session"] == leader["pid"],
                         "actor is outside the leader's group/session")
            need(actual["status"]["Uid"] == [self.args.child_uid] * 4
                 and actual["status"]["Gid"] == [self.args.child_gid] * 4
                 and actual["status"]["Groups"] == [],
                 "actual four-field uid/gid or groups contract unavailable")
            need(all((entry[1], entry[2]) != self.receipt_identity
                     for entry in value["fds"]), "actual private receipt identity leaked")
            pin = os.pidfd_open(actual["pid"], 0)
            need(kernel_identity(actual["pid"]) == actual, "actor changed while pinning")
            self.owned[role] = {**actual, "pin": pin}
            if role == "actor" or (role == "leader" and not self.row.delegate):
                self.actor = actual
            if self.row.kind == "kernel-path" and role == "leader":
                self.kernel_memberships = {
                    "leader": membership(actual["pid"]),
                    "supervisor": membership(self.process.pid),
                    "observer": membership(os.getpid()),
                }
                need(self.kernel_memberships["leader"] == self.kernel_before["kernel_path"],
                     "SETUP: control cgroup admission could not be independently established")
                need(self.kernel_memberships["supervisor"] != self.kernel_before["kernel_path"],
                     "supervisor entered workload cgroup")
            self.acknowledge()
        elif kind == "actor":
            need(value["pid"] == self.owned["leader"]["pid"], "actor/leader pid mismatch")
        elif kind == "adopted":
            sleeper = self.owned["sleeper"]
            need(membership(self.process.pid) is not None, "launcher membership disappeared")
            adopted = kernel_identity(sleeper["pid"])
            need(adopted["ppid"] == self.process.pid
                 and adopted["generation"] == sleeper["generation"],
                 "real escaped orphan was not adopted by the supervisor")
            self.acknowledge()
        elif kind == "armed":
            need(value["pid"] == self.owned["leader"]["pid"], "armed pid mismatch")
            self.acknowledge()
            if self.row.kind == "launcher-signal":
                actual = kernel_identity(value["pid"])
                need(actual["status"]["SigIgn"] & (1 << (self.row.number - 1)),
                     "child did not install the signal negative control")
                signal.pidfd_send_signal(self.launcher_pin, self.row.number)

    def acknowledge(self):
        need(self.process.stdin.write(b".") == 1, "failed observer acknowledgement")
        self.process.stdin.flush()

    def consume(self, poller, key):
        chunk = os.read(key.fd, 4096)
        if not chunk:
            poller.unregister(key.fd)
            return
        self.buffers[key.data].extend(chunk)
        if key.data == "stdout":
            self.partial.extend(chunk)
            while b"\n" in self.partial:
                line, _, remainder = self.partial.partition(b"\n")
                self.partial[:] = remainder
                self.event(document(bytes(line) + b"\n"))

    def run(self):
        row = self.row
        for path in (SELF, SUPERVISOR, LEDGER, sys.executable):
            named(path)
        need(named(SUPERVISOR).parent == named(LEDGER).parent,
             "REFUSED supervisor would import an unlisted ledger sibling")
        if row.kind == "kernel-path":
            need(self.args.memory_events_control is not None,
                 "explicit host-reserved memory-events control is required")
            self.requested_path = str(named(self.args.memory_events_control))
            self.kernel_before = cgroup_snapshot(self.requested_path)
            need(not self.kernel_before["procs"]
                 and self.kernel_before["population"].get("populated") == "0",
                 "control cgroup is not exclusively empty")
            need(membership(os.getpid()) != self.kernel_before["kernel_path"],
                 "control contains the observer")
        elif row.kind == "plain-path":
            plain = mkdir_new(self.directory / "plain")
            self.requested_path = str(plain / "memory.events")
            new_file(self.requested_path, b"oom 817\noom_kill 613\n")
        elif row.kind == "refused-path":
            # No exists/lstat/resolve/open/mkdir of this literal or any of its
            # sensitive-name components. The source-admitted SUT must refuse
            # it lexically before attempting any observation filesystem access.
            self.requested_path = str(self.directory / "literal-only" /
                                      Path(*row.literal_parts) / "memory.events")
            need(forbidden(self.requested_path), "bad refused-name test literal")
            print(f"LITERAL ONLY; will never follow: {self.requested_path}",
                  file=sys.stderr, flush=True)
        reader, writer = os.pipe2(os.O_CLOEXEC)
        high_writer = None
        try:
            high_writer = fcntl.fcntl(writer, fcntl.F_DUPFD_CLOEXEC, 256)
        finally:
            os.close(writer)
        item = os.fstat(high_writer)
        self.receipt_identity = (item.st_dev, item.st_ino)
        payload_path = self.directory / "payload.bin"
        new_file(payload_path, b"")
        self.payload_fd = os.open(named(payload_path), os.O_WRONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        argv = [sys.executable, "-I", "-B", str(SUPERVISOR),
                "--receipt-fd", str(high_writer),
                "--scratch-bytes=65536", "--scratch-files=64",
                f"--output-bytes={row.limit}",
                f"--descriptor-limit={row.soft_nofile + 17}",
                "--pass-fd", str(self.payload_fd)]
        if row.kind != "admission" or row.number != 0:
            argv.append(f"--child-uid={0 if row.kind == 'admission' and row.number == 2 else self.args.child_uid}")
        if row.kind != "admission" or row.number != 1:
            argv.append(f"--child-gid={0 if row.kind == 'admission' and row.number == 3 else self.args.child_gid}")
        if row.kind == "admission" and row.number == 4:
            argv.extend(("--pass-fd", str(high_writer)))
        if row.kind == "deadline":
            # The CLI arms this policy during admission, before the workload's
            # ready/armed barriers; slow startup can preempt their observation.
            argv.append("--deadline-seconds=" + self.args.caller_deadline_seconds)
        if self.requested_path is not None:
            argv.extend(("--memory-events", self.requested_path))
        argv.extend(("--", sys.executable, "-I", "-B", str(SELF), "--actor", row.id,
                     "--child-uid", str(self.args.child_uid),
                     "--child-gid", str(self.args.child_gid),
                     "--receipt-device", str(item.st_dev), "--receipt-inode", str(item.st_ino),
                     "--payload-fd", str(self.payload_fd)))
        if row.kind == "exec-failure":
            missing = ROOT / "docker" / "__quota_policy_never_created_executable__"
            print(f"FILES will check absence, then request failed exec: {missing}",
                  file=sys.stderr, flush=True)
            need(not missing.exists(), "failed-exec name is occupied")
            boundary = argv.index("--")
            argv = argv[:boundary + 1] + [str(missing)]
        if self.trace is not None:
            argv = [sys.executable, "-I", "-B", str(SELF), "--trace-exec", "--", *argv[3:]]
        environment = {
            "PATH": os.defpath, "LC_ALL": "C", "PYTHONDONTWRITEBYTECODE": "1",
            "PF_POLICY_PROOF_SENTINEL": row.id, "PF_RUN_QUOTA_MODE": "untrusted",
            "PF_RUN_QUOTA_NATIVE_FD": "0", "PF_RUN_QUOTA_PYTHON_FD": "1",
            **{name: "untrusted" for name in LIMIT_NAMES},
        }
        try:
            self.process = subprocess.Popen(
                argv, cwd=ROOT, env=environment, pass_fds=(high_writer, self.payload_fd),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                start_new_session=True, bufsize=0,
            )
        finally:
            os.close(high_writer)
        try:
            self.launcher_pin = os.pidfd_open(self.process.pid, 0)
            launcher = kernel_identity(self.process.pid)
            self.launcher_identity = launcher
            need(launcher["ppid"] == os.getpid() and launcher["generation"] > 0,
                 "actual supervisor process not pinned")
            if self.trace is not None:
                self.trace.begin(self.process)
            with selectors.DefaultSelector() as poller:
                for fd, label in ((self.process.stdout.fileno(), "stdout"),
                                  (self.process.stderr.fileno(), "stderr"),
                                  (reader, "receipt")):
                    poller.register(fd, selectors.EVENT_READ, label)
                while poller.get_map() or (self.trace is not None and self.trace.tasks):
                    # No time budget. Polling is solely to interleave real
                    # kernel trace stops and the workload's blocking barriers.
                    for key, _ in poller.select(0 if self.trace is not None
                                                and self.trace.tasks else None):
                        self.consume(poller, key)
                    if self.trace is not None and self.trace.tasks:
                        raw = self.trace.step(self.process, self.actor, row.soft_nofile)
                        if raw is not None:
                            self.launcher_raw = raw
                need(not self.partial, "truncated workload event")
            if self.launcher_raw is None:
                waited, self.launcher_raw = os.waitpid(self.process.pid, 0)
                need(waited == self.process.pid, "wrong actual supervisor wait")
                self.process.returncode = os.waitstatus_to_exitcode(self.launcher_raw)
            # Supervisor completion precedes these immediate pidfd state checks.
            # No duration is used to establish death or reaping.
            for role, owned in self.owned.items():
                need(select.select((owned["pin"],), (), (), 0)[0],
                     f"owned {role} remains alive after supervisor completion")
                try:
                    now = kernel_identity(owned["pid"])
                except FileNotFoundError:
                    pass
                else:
                    need(now["generation"] != owned["generation"],
                         f"owned {role} remains unreaped")
            if row.kind == "kernel-path":
                self.kernel_after = cgroup_snapshot(self.requested_path)
            self.payload = read_bytes(payload_path)
            new_json(self.directory / "supervisor.wait.json", {
                "pid": self.process.pid, "identity_at_spawn": launcher,
                "raw_status": self.launcher_raw, "returncode": self.process.returncode,
            })
            receipt = document(bytes(self.buffers["receipt"]))
            need(FIELDS <= receipt.keys() and receipt["schema"] == SCHEMA,
                 "missing or unsupported private supervisor receipt")
            for name in ("wait_status", "exit_code", "signal"):
                need(receipt[name] is None or type(receipt[name]) is int,
                     f"untyped actual receipt {name}")
            need(type(receipt["oom"]) is dict and type(receipt["oom"].get("proven")) is bool,
                 "untyped actual OOM evidence")
            need(type(receipt["deadline"]) is dict
                 and type(receipt["deadline"].get("expired")) is bool
                 and type(receipt["deadline"].get("kill_sent")) is bool,
                 "untyped actual deadline evidence")
            seconds = receipt["deadline"].get("seconds")
            need(seconds is None or (
                type(seconds) in (int, float) and math.isfinite(seconds) and seconds > 0),
                "untyped/nonfinite actual deadline seconds")
            need(receipt["quota_line"] == "quota=managed enforcement=cooperative",
                 "unexpected declared enforcement label")
            for name in ("ledger_verdict", "launcher_failure"):
                failure = receipt[name]
                if failure is not None:
                    need(type(failure) is dict and type(failure.get("code")) is str
                         and bool(failure["code"]) and type(failure.get("args")) is dict,
                         "untyped actual refusal")
                    for counter in ("used", "requested", "limit", "peak", "soft_nofile"):
                        if counter in failure["args"]:
                            value = failure["args"][counter]
                            need(type(value) is int and 0 <= value <= (1 << 53) - 1,
                                 "untyped/out-of-domain actual refusal counter")
            return receipt
        finally:
            if self.process.returncode is None:
                # Preserve the real launcher's signal handler and cleanup.
                # No timer or fallback success verdict is installed.
                if not self.process.stdin.closed:
                    self.process.stdin.close()
                if self.launcher_pin is not None:
                    try:
                        signal.pidfd_send_signal(self.launcher_pin, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                if self.trace is not None:
                    self.trace.cleanup(self.process)
                self.process.wait()
            os.close(reader)
            if self.payload_fd is not None:
                os.close(self.payload_fd)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                stream.close()
            for owned in self.owned.values():
                os.close(owned["pin"])
            if self.launcher_pin is not None:
                os.close(self.launcher_pin)
            for label, data in self.buffers.items():
                new_file(self.directory / f"supervisor.{label}", bytes(data))
            new_json(self.directory / "workload.events.json", self.events)
            if self.trace is not None:
                new_json(self.directory / "registration-intervention.json", {
                    "pidfd_counts": self.trace.counts, "change": self.trace.change,
                })
            if self.kernel_before is not None:
                new_json(self.directory / "kernel-control.json", {
                    "before": self.kernel_before, "at_workload_gate": self.kernel_memberships,
                    "after": self.kernel_after,
                })


def events(observed, kind):
    return [event for event in observed.events if event["event"] == kind]


def normal_wait(observed, receipt, code):
    compare("REAL_LEADER_WAIT_FIELDS", [code << 8, code, None],
            [receipt["wait_status"], receipt["exit_code"], receipt["signal"]])
    compare("SEPARATE_SUPERVISOR_WAIT", code, observed.process.returncode)
    compare("NO_LAUNCHER_FAILURE", None, receipt["launcher_failure"])


def signaled_wait(observed, receipt, number, launcher_code):
    need(type(receipt["wait_status"]) is int
         and os.WIFSIGNALED(receipt["wait_status"]), "no real signaled leader wait")
    compare("REAL_LEADER_SIGNAL", [number, None, number],
            [os.WTERMSIG(receipt["wait_status"]), receipt["exit_code"], receipt["signal"]])
    compare("SEPARATE_SUPERVISOR_WAIT", launcher_code, observed.process.returncode)


def judge(row, args, observed, receipt):
    deadline = receipt["deadline"]
    need(type(deadline) is dict and all(name in deadline for name in
                                      ("seconds", "expired", "kill_sent")),
         "unusable deadline evidence")
    if row.kind == "admission":
        need(not observed.owned and not observed.events and observed.payload == b"",
             "admission refusal unexpectedly performed workload")
        reasons = ("invalid_integer", "invalid_integer", "child_identity_not_separate",
                   "child_identity_not_separate", "receipt_fd_collision")
        failure = receipt["launcher_failure"]
        need(type(failure) is dict and type(failure.get("args")) is dict,
             "admission control did not produce a typed diagnostic")
        compare("P2_4_TYPED_ADMISSION", ["quota_launcher_error", "admission", reasons[row.number]],
                [failure.get("code"), failure["args"].get("stage"), failure["args"].get("reason")])
        if row.number == 4:
            need(type(failure["args"].get("descriptor")) is int
                 and failure["args"]["descriptor"] >= 256,
                 "collision diagnostic did not name actual private carrier descriptor")
        compare("ADMISSION_HAS_NO_LEADER_WAIT", [None, None, None, None],
                [receipt[name] for name in ("wait_status", "exit_code", "signal", "ledger_verdict")])
        compare("ADMISSION_SUPERVISOR_WAIT", 1, observed.process.returncode)
        compare("ADMISSION_HAS_NO_DEADLINE",
                {"seconds": None, "expired": False, "kill_sent": False},
                deadline)
        compare("ADMISSION_HAS_NO_OOM", [False, None, None, None],
                [receipt["oom"][name] for name in ("proven", "before", "after", "delta")])
        return
    if row.kind == "exec-failure":
        need(not observed.owned and not observed.events and observed.payload == b"",
             "missing executable unexpectedly ran workload")
        compare("P2_4_REAL_EXEC_FAILURE", {
            "code": "quota_launcher_error",
            "args": {"stage": "launch", "reason": "child_setup_failed",
                     "operation": "exec", "errno": errno.ENOENT},
        }, receipt["launcher_failure"])
        compare("REAL_FAILED_EXEC_WAIT", [126 << 8, 126, None, None],
                [receipt[name] for name in ("wait_status", "exit_code", "signal", "ledger_verdict")])
        compare("EXEC_FAILURE_SUPERVISOR_WAIT", 1, observed.process.returncode)
        compare("EXEC_FAILURE_HAS_NO_DEADLINE",
                {"seconds": None, "expired": False, "kill_sent": False},
                deadline)
        return
    if row.kind == "no-deadline" and not observed.owned:
        need(receipt["launcher_failure"] == {
            "code": "quota_launcher_error",
            "args": {"stage": "admission", "reason": "invalid_deadline"},
        } and receipt["wait_status"] is None and receipt["ledger_verdict"] is None
             and observed.process.returncode == 1,
             "absent-deadline experiment failed before establishing its target")
        compare("P2_4_ABSENT_DEADLINE_ACCEPTED", "real workload admitted",
                {"receipt": receipt, "supervisor_wait": observed.launcher_raw})
    need("leader" in observed.owned, "commanded workload did not establish its real identity")
    oom = receipt["oom"]
    need(type(oom) is dict and {"proven", "before", "after", "delta"} <= oom.keys(),
         "unusable OOM evidence")
    if row.kind == "kernel-path":
        need(observed.kernel_before is not None and observed.kernel_after is not None,
             "kernel control observations missing")
        before, after = observed.kernel_before, observed.kernel_after
        need(not after["procs"] and after["population"].get("populated") == "0",
             "kernel control cgroup did not become empty")
        need("oom_kill" in before["counters"] and "oom_kill" in after["counters"],
             "actual kernel oom_kill evidence unavailable")
        compare("NO_OOM_INDUCTION", before["counters"], after["counters"])
        compare("ACTUAL_KERNEL_OOM_COUNTERS",
                [False, before["counters"]["oom_kill"], after["counters"]["oom_kill"], 0],
                [oom[name] for name in ("proven", "before", "after", "delta")])
    else:
        compare("UNPROVEN_OOM_STAYS_UNKNOWN", [False, None, None, None],
                [oom[name] for name in ("proven", "before", "after", "delta")])
    expected_seconds = float(args.caller_deadline_seconds) if row.kind == "deadline" else None
    compare("EXPLICIT_CALLER_POLICY_RETAINED" if row.kind == "deadline"
            else "P2_4_NO_INVENTED_DEADLINE", expected_seconds, deadline["seconds"])
    if row.kind == "deadline":
        need(events(observed, "armed"), "explicit deadline elapsed before real workload armed")
        signaled_wait(observed, receipt, signal.SIGKILL, 128 + signal.SIGKILL)
        compare("EXPLICIT_POLICY_KILL", [True, True],
                [deadline["expired"], deadline["kill_sent"]])
        compare("DEADLINE_IS_NOT_LAUNCHER_ERROR", None, receipt["launcher_failure"])
    elif row.kind in ("pgrp", "session", "recheck"):
        if row.kind == "recheck":
            need(observed.trace.change is not None, "registration barrier was not established")
        else:
            need("actor" in observed.owned, "membership negative actor was never pinned")
        signaled_wait(observed, receipt, signal.SIGKILL, 1)
        reason = ("actor_changed_during_registration" if row.kind == "recheck"
                  else "actor_outside_owned_run")
        compare("P2_3_AUTHENTICATION_REFUSAL", {
            "code": "quota_launcher_error",
            "args": {"stage": "authentication", "reason": reason},
        }, receipt["launcher_failure"])
    elif row.kind == "launcher-signal":
        need(events(observed, "armed"), "launcher signal control was not armed")
        signaled_wait(observed, receipt, signal.SIGKILL, 128 + row.number)
        compare("P2_4_LAUNCHER_SIGNAL", {
            "code": "quota_launcher_error",
            "args": {"stage": "supervision", "reason": "launcher_interrupted",
                     "signal": row.number},
        }, receipt["launcher_failure"])
    elif row.kind == "crash":
        signaled_wait(observed, receipt, signal.SIGABRT, 128 + signal.SIGABRT)
        compare("CRASH_IS_NOT_LAUNCHER_ERROR", None, receipt["launcher_failure"])
    else:
        normal_wait(observed, receipt, row.exit_code)
    if row.kind != "deadline":
        compare("NO_UNREQUESTED_EXPIRY", [False, False],
                [deadline["expired"], deadline["kill_sent"]])
    if row.kind == "refusal":
        expected = {"code": "run_resource_quota_exceeded", "args": {
            "stage": "execution", "resource": "output_bytes", "artifact": "sink",
            "used": row.limit, "requested": row.requested, "limit": row.limit,
            "peak": row.limit,
        }}
        compare("P2_2_RECEIPT_REFUSAL_WITH_EXIT0", expected, receipt["ledger_verdict"])
        compare("REAL_CLIENT_REFUSAL_AGREES", [expected],
                [{k: event[k] for k in ("code", "args")} for event in events(observed, "refused")])
    elif row.kind == "nofile":
        expected = {"code": "run_descriptor_limit_exceeded", "args": {
            "stage": "execution", "process_role": "worker", "soft_nofile": row.soft_nofile,
            "used": 0, "requested": row.soft_nofile + 1, "limit": row.soft_nofile, "peak": 0,
        }}
        compare("P2_3_REAL_FINITE_NOFILE", expected, receipt["ledger_verdict"])
        compare("REAL_CLIENT_REFUSAL_AGREES", [expected],
                [{k: event[k] for k in ("code", "args")} for event in events(observed, "refused")])
    elif row.kind in ("reuse", "recheck"):
        expected = {"code": "run_quota_configuration_invalid", "args": {
            "stage": "admission", "reason": "protocol", "setting": "channel",
        }}
        compare("P2_3_ONE_ACTOR_PER_ENDPOINT" if row.kind == "reuse"
                else "REGISTRATION_FAILURE_DISCONNECTS_OWNER", expected, receipt["ledger_verdict"])
        if row.kind == "reuse":
            need("second-actor" in observed.owned, "second actor never established")
            compare("REAL_SECOND_ACTOR_REFUSED", [expected],
                    [{k: event[k] for k in ("code", "args")}
                     for event in events(observed, "second_refused")])
    else:
        compare("NO_LEDGER_REFUSAL_IN_CONTROL", None, receipt["ledger_verdict"])
    if row.kind not in ("pgrp", "session", "recheck", "nofile", "reuse", "tree",
                        "launcher-signal", "deadline", "crash"):
        compare("REAL_LEDGER_INITIAL", [[0, 0, 0, row.limit, 0]],
                [event["initial"] for event in events(observed, "hello")])
        compare("REAL_LEDGER_RESERVATION", [[0, row.produced, row.produced,
                                            row.limit, row.produced]],
                [event["usage"] for event in events(observed, "pending")])
        compare("REAL_LEDGER_COMMIT", [[row.produced, 0, row.produced,
                                      row.limit, row.produced]],
                [event["usage"] for event in events(observed, "committed")])
        compare("ACTUAL_OUTPUT_ARTIFACT", (bytes((65 + row.endpoint,)) * row.produced).hex(),
                observed.payload.hex())
    if row.delegate and row.kind not in ("pgrp", "session"):
        compare("REAL_DELEGATE_WAIT", [row.exit_code << 8],
                [event["raw_status"] for event in events(observed, "delegate_wait")])
    if row.kind == "refused-path":
        # This expected object never enters the observed receipt or
        # substitutes for missing diagnostics.
        expected_diagnostic = {"code": "quota_launcher_error", "args": {
            "stage": "oom_observation", "reason": "arguments",
            "option": "--memory-events", "path": observed.requested_path,
        }}
        compare("P2_1_REFUSED_PATH_HAS_TYPED_DIAGNOSTIC",
                expected_diagnostic, oom.get("diagnostic"))
    else:
        compare("NO_INVENTED_OBSERVATION_DIAGNOSTIC", None, oom.get("diagnostic"))


def validate_arguments(args):
    need(args.child_uid is not None and args.child_gid is not None,
         "explicit child uid/gid required; there is no account fallback")
    need(0 < args.child_uid <= (1 << 32) - 2 and 0 < args.child_gid <= (1 << 32) - 2,
         "child uid/gid outside product domain")
    text = args.caller_deadline_seconds
    need(text is not None and re.fullmatch(r"[0-9]+(\.[0-9]+)?", text) is not None,
         "explicit caller policy required for the deadline control")
    seconds = float(text)
    need(math.isfinite(seconds) and seconds > 0, "invalid explicit caller policy")
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    need(soft == resource.RLIM_INFINITY or soft > 272, "host nofile cannot carry private pipe")
    need(hard == resource.RLIM_INFINITY or hard >= 144, "host hard nofile below fixture inputs")


def result_files(directory, result, status, *, complete):
    payload = encoded(result)
    new_file(directory / "RUN.RESULT.json", payload)
    if complete:
        new_json(directory / "RUN.COMPLETE", {
            "state": "COMPLETE", "result": "RUN.RESULT.json",
            "sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload),
        })
    new_file(directory / ".exit", (str(status) + "\n").encode("ascii"))
    return status


def worker(args, row, directory):
    result = {"row": asdict(row), "state": "INCOMPLETE"}
    try:
        if row.kind == "proof-crash":
            raise DeliberatePolicyProofCrash("deliberate real work-recorder crash")
        validate_arguments(args)
        result["host"] = host_facts()
        result["sources_before"] = sources()
        # Actual credential/traversal probe; permission errors are setup.
        for path in (SELF, LEDGER, sys.executable, "/dev/null"):
            named(path)
        probe = subprocess.run(
            [sys.executable, "-I", "-B", str(SELF), "--probe",
             "--child-uid", str(args.child_uid), "--child-gid", str(args.child_gid)],
            cwd=ROOT, env={"PATH": os.defpath, "LC_ALL": "C"},
            user=args.child_uid, group=args.child_gid, extra_groups=(),
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        new_file(directory / "identity-probe.stdout", probe.stdout)
        new_file(directory / "identity-probe.stderr", probe.stderr)
        new_json(directory / "identity-probe.wait.json", {"returncode": probe.returncode})
        need(probe.returncode == 0, "actual dropped-identity traversal probe failed")
        result["identity_probe"] = document(probe.stdout)
        observed = Observation(row, args, directory)
        receipt = observed.run()
        result["private_receipt"] = receipt
        result["supervisor_wait"] = {
            "raw_status": observed.launcher_raw, "returncode": observed.process.returncode,
        }
        if observed.kernel_before is not None:
            result["kernel_control"] = {
                "before": observed.kernel_before, "at_workload_gate": observed.kernel_memberships,
                "after": observed.kernel_after,
            }
        result["sources_after"] = sources()
        need(result["sources_before"] == result["sources_after"], "source changed during row")
        judge(row, args, observed, receipt)
        result.update(state="COMPLETE", verdict="GREEN")
        return result_files(directory, result, 0, complete=True)
    except ContractMismatch as error:
        result.update(state="COMPLETE", verdict="RED", mismatch={
            "assertion_id": error.assertion_id, "expected": error.expected, "actual": error.actual,
        })
        return result_files(directory, result, 1, complete=True)
    except BaseException as error:
        result.update(state="INCOMPLETE", verdict="INCOMPLETE",
                      fatal={"type": type(error).__name__, "message": str(error)})
        traceback.print_exc()
        return result_files(directory, result, 2, complete=False)


def verify_result(directory, wait_code):
    result_wire = read_bytes(directory / "RUN.RESULT.json")
    result = document(result_wire)
    own_exit = read_bytes(directory / ".exit")
    need(own_exit == (str(wait_code) + "\n").encode("ascii"),
         "work's own exit differs from actual process wait")
    if result["state"] == "COMPLETE":
        completion = document(read_bytes(directory / "RUN.COMPLETE"))
        need(completion == {
            "state": "COMPLETE", "result": "RUN.RESULT.json",
            "sha256": hashlib.sha256(result_wire).hexdigest(), "bytes": len(result_wire),
        }, "work completion does not bind its own result")
        need((wait_code, result["verdict"]) in ((0, "GREEN"), (1, "RED")),
             "work wait/verdict disagreement")
    else:
        need(wait_code != 0, "incomplete work returned success")
        complete = directory / "RUN.COMPLETE"
        print(f"FILES will check completion presence: {complete}", file=sys.stderr, flush=True)
        need(not complete.exists(), "incomplete work has a completion binding")
    return result


def controller(args, directory):
    result = {"state": "INCOMPLETE", "required_rows": [asdict(row) for row in rows()],
              "rows": [], "claims": "policy proof only; no engine/parity/ROOT campaign acceptance"}
    try:
        validate_arguments(args)
        result["host"] = host_facts()
        result["sources"] = sources()
        for row in rows():
            work_dir = mkdir_new(directory / row.id)
            for path in (SELF, sys.executable, "/dev/null"):
                named(path)
            stdout_path, stderr_path = work_dir / "work.stdout", work_dir / "work.stderr"
            for path in (stdout_path, stderr_path):
                print(f"FILES will create exclusively: {path}", file=sys.stderr, flush=True)
            with stdout_path.open("xb") as out, stderr_path.open("xb") as err:
                argv = [sys.executable, "-I", "-B", str(SELF), "--work", row.id,
                        "--result-dir", str(work_dir), "--child-uid", str(args.child_uid),
                        "--child-gid", str(args.child_gid),
                        "--caller-deadline-seconds", args.caller_deadline_seconds]
                if args.memory_events_control is not None:
                    named(args.memory_events_control)
                    argv.extend(("--memory-events-control", args.memory_events_control))
                process = subprocess.Popen(
                    argv, cwd=ROOT, env={"PATH": os.defpath, "LC_ALL": "C"},
                    stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                )
                waited, raw = os.waitpid(process.pid, 0)
                need(waited == process.pid, "wrong work wait")
                process.returncode = os.waitstatus_to_exitcode(raw)
            new_json(work_dir / "work.wait.json", {
                "pid": process.pid, "raw_status": raw, "returncode": process.returncode,
            })
            # This remains nonzero for a genuinely crashed/killed work process,
            # even when no Python finally handler could write that work's .exit.
            new_file(work_dir / "work.exit",
                     (str(process.returncode) + "\n").encode("ascii"))
            actual = verify_result(work_dir, process.returncode)
            need(actual.get("row") == json.loads(json.dumps(asdict(row))),
                 "work result is not the exact registered row and inputs")
            if row.kind == "proof-crash":
                need(process.returncode == 2 and actual["state"] == "INCOMPLETE"
                     and actual.get("fatal", {}).get("type") == "DeliberatePolicyProofCrash",
                     "deliberate crash control did not establish nonzero incomplete work")
                result["rows"].append({"id": row.id, "verdict": "GREEN",
                                       "control": actual, "work_returncode": process.returncode})
                continue
            result["rows"].append(actual)
            if actual.get("verdict") == "INCOMPLETE":
                raise Incomplete(f"row incomplete; sequence stopped: {row.id}")
            if not row.target and actual.get("verdict") != "GREEN":
                raise Incomplete(f"control failed; no later row ran: {row.id}")
            if row.target and actual.get("verdict") == "RED":
                intended = ({"P2_1_REFUSED_PATH_HAS_TYPED_DIAGNOSTIC"}
                            if row.kind == "refused-path" else {
                                "P2_4_ABSENT_DEADLINE_ACCEPTED", "P2_4_NO_INVENTED_DEADLINE"})
                need(actual.get("mismatch", {}).get("assertion_id") in intended,
                     f"unexpected target mismatch; sequence stopped: {row.id}")
        need(len(result["rows"]) == len(result["required_rows"]), "incomplete discovery execution")
        result["sources_after"] = sources()
        need(result["sources_after"] == result["sources"], "source changed across proof")
        verdict = "RED" if any(row.get("verdict") == "RED" for row in result["rows"]) else "GREEN"
        result.update(state="COMPLETE", verdict=verdict)
        return result_files(directory, result, 1 if verdict == "RED" else 0, complete=True)
    except BaseException as error:
        result.update(state="INCOMPLETE", verdict="INCOMPLETE",
                      fatal={"type": type(error).__name__, "message": str(error)})
        traceback.print_exc()
        return result_files(directory, result, 2, complete=False)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--trace-exec":
        traced_exec()
        return 2
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--list-rows", action="store_true")
    modes.add_argument("--run", action="store_true")
    modes.add_argument("--work")
    modes.add_argument("--actor")
    modes.add_argument("--probe", action="store_true")
    parser.add_argument("--result-dir")
    parser.add_argument("--child-uid", type=int)
    parser.add_argument("--child-gid", type=int)
    parser.add_argument("--caller-deadline-seconds")
    parser.add_argument("--memory-events-control")
    parser.add_argument("--receipt-device", type=int)
    parser.add_argument("--receipt-inode", type=int)
    parser.add_argument("--payload-fd", type=int)
    args = parser.parse_args()
    if args.list_rows:
        os.write(1, encoded({"state": "DISCOVERY_ONLY", "rows": [asdict(row) for row in rows()]}))
        return 0
    if args.probe:
        need(sys.platform == "linux" and os.getresuid() == (args.child_uid,) * 3
             and os.getresgid() == (args.child_gid,) * 3 and os.getgroups() == [],
             "real identity transition failed")
        for path in (SELF, LEDGER):
            with named(path).open("rb") as source:
                need(bool(source.read(1)), "workload cannot read source")
        os.write(1, encoded({"pid": os.getpid(), "resuid": list(os.getresuid()),
                            "resgid": list(os.getresgid()), "groups": os.getgroups()}))
        return 0
    selected = {row.id: row for row in rows()}
    if args.actor is not None:
        need(args.actor in selected, "unregistered actor row")
        return workload(args, selected[args.actor])
    need(args.result_dir is not None, "result directory required")
    directory = Path(args.result_dir).absolute()
    need(directory.is_relative_to(SCRATCH) and not forbidden(directory)
         and ".." not in directory.parts, "REFUSED result directory")
    if args.run:
        # The hosting owner creates the named scratch root; never use /tmp.
        scratch_item = os.stat(named(SCRATCH))
        need(scratch_item.st_uid == 0 and not scratch_item.st_mode & 0o077,
             "named scratch root must be root-owned and private")
        directory = mkdir_new(directory)
    else:
        named(directory)
        need(args.work in selected, "unregistered work row")
    new_json(directory / "RUN.START.json", {
        "pid": os.getpid(), "kind": "controller" if args.run else "work",
        "row": args.work, "cwd": str(ROOT),
    })
    if args.run:
        return controller(args, directory)
    return worker(args, selected[args.work], directory)


if __name__ == "__main__":
    sys.exit(main())
