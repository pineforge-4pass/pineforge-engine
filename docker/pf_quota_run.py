#!/usr/bin/env python3
"""Run one command with a parent-owned cooperative quota ledger and receipt.

Linux only. The trusted caller supplies every quota allowance, separate non-root
child credentials, and an already open receipt fd. A caller deadline is optional
and has no default. Only the two client sockets, standard streams, and explicit
--pass-fd descriptors survive
the child's exec. stdout, stderr, and stdin are inherited without buffering.

An optional --memory-events names memory.events in a host-reserved, empty,
root-owned cgroup v2 directory. No cgroup or limit is created here. If that
directory can be verified and entered before the privilege drop, the launcher
pins it, observes membership before releasing exec, and samples its kernel
oom_kill counter. The hosting owner must reserve it exclusively for this run.
Unavailable or unprovable evidence stays unknown; it never selects quota mode.

Hosting still owns image installation, trusted runtime attachment before
strategy load, object observation, and containment of direct strategy syscalls.
This file does not make those hooks present in entrypoint.sh or run_json.py.
The shipped ledger's allocation/lifetime audit is separate from this launcher's
fixed endpoint, error, wait, and receipt state.
"""

from __future__ import annotations

import argparse
import array
import ctypes
import errno
import fcntl
import importlib.util
import json
import math
import operator
import os
import re
import resource
import select
import signal
import socket
import stat
import struct
import sys
import threading
import time


_SCHEMA = "pf-quota-receipt/v1"
_QUOTA_LINE = "quota=managed enforcement=cooperative"
_MARKERS = (
    "PF_RUN_QUOTA_MODE", "PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD",
)
_LIMITS = (
    "PF_RUN_SCRATCH_BYTES", "PF_RUN_SCRATCH_FILES",
    "PF_RUN_OUTPUT_BYTES", "PF_RUN_DESCRIPTOR_LIMIT",
)
_POLICY_OPTIONS = (
    "scratch_bytes", "scratch_files", "output_bytes", "descriptor_limit",
)
_INT_MAX = (1 << 31) - 1
_ID_MAX = (1 << 32) - 2
_U64_MAX = (1 << 64) - 1
_CREDENTIALS = struct.Struct("=iII")
_SETUP = struct.Struct("=iii")
_READY = 1
_SETUP_FAILED = 2
_SETUP_STAGES = (
    "session", "cgroup", "descriptors", "privileges", "gate", "exec",
)
_CHILD_SETUP_EXIT = 126
_PARENT_SIGNALS = (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM)
_NATIVE_PIDFD_OPEN = getattr(os, "pidfd_open", None)
_NATIVE_PIDFD_SEND_SIGNAL = getattr(signal, "pidfd_send_signal", None)
_PIDFD_ARCHITECTURES = ("x86_64", "amd64", "aarch64", "arm64")


def _machine():
    return os.uname().machine.lower()


# Resolve the missing interpreter wrappers once. These syscall numbers are
# shared by the two supported 64-bit Linux ABIs; never guess for another ABI.
_PIDFD_SYSCALL = None
if (
    sys.platform == "linux" and ctypes.sizeof(ctypes.c_void_p) == 8
    and (_NATIVE_PIDFD_OPEN is None or _NATIVE_PIDFD_SEND_SIGNAL is None)
    and _machine() in _PIDFD_ARCHITECTURES
):
    try:
        _PIDFD_SYSCALL = ctypes.CDLL(None, use_errno=True).syscall
    except (AttributeError, OSError):
        pass
    else:
        _PIDFD_SYSCALL.argtypes = (ctypes.c_long,)
        _PIDFD_SYSCALL.restype = ctypes.c_long


class _Fault(Exception):
    __slots__ = ("code", "arguments")

    def __init__(self, reason, *, stage="admission", **arguments):
        self.code = "quota_launcher_error"
        self.arguments = {"stage": stage, "reason": reason, **arguments}
        super().__init__(reason)


def _pidfd_open(pid, flags):
    if _NATIVE_PIDFD_OPEN is not None:
        return _NATIVE_PIDFD_OPEN(pid, flags)
    if _PIDFD_SYSCALL is None:
        raise _Fault("linux_containment_unavailable")
    pid, flags = operator.index(pid), operator.index(flags)
    if not -_INT_MAX - 1 <= pid <= _INT_MAX:
        raise OverflowError("pid does not fit in a C int")
    if not 0 <= flags <= _ID_MAX + 1:
        raise OverflowError("flags do not fit in a C unsigned int")
    # The kernel validates the pid and flags, including invalid-flag EINVAL.
    # pidfd_open itself creates the descriptor with FD_CLOEXEC.
    result = _PIDFD_SYSCALL(434, ctypes.c_int(pid), ctypes.c_uint(flags))
    if result == -1:
        number = ctypes.get_errno()
        raise OSError(number, os.strerror(number))
    return result


def _pidfd_send_signal(pidfd, number):
    if _NATIVE_PIDFD_SEND_SIGNAL is not None:
        return _NATIVE_PIDFD_SEND_SIGNAL(pidfd, number)
    if _PIDFD_SYSCALL is None:
        raise _Fault("linux_containment_unavailable")
    pidfd, number = operator.index(pidfd), operator.index(number)
    if any(value < -_INT_MAX - 1 or value > _INT_MAX for value in (pidfd, number)):
        raise OverflowError("pidfd or signal does not fit in a C int")
    result = _PIDFD_SYSCALL(
        424, ctypes.c_int(pidfd), ctypes.c_int(number),
        ctypes.c_void_p(), ctypes.c_uint(0),
    )
    if result == -1:
        number = ctypes.get_errno()
        raise OSError(number, os.strerror(number))
    return None


def _failure(error, stage):
    if isinstance(error, _Fault):
        return {"code": error.code, "args": dict(error.arguments)}
    # Only this parent's imported module can supply a QuotaError here.
    code = getattr(error, "code", None)
    arguments = getattr(error, "code_args", None)
    if isinstance(code, str) and isinstance(arguments, dict):
        return {"code": code, "args": arguments}
    result = {"stage": stage, "reason": type(error).__name__}
    number = getattr(error, "errno", None)
    if type(number) is int:
        result["errno"] = number
    return {"code": "quota_launcher_error", "args": result}


def _decimal(text, maximum):
    if not isinstance(text, str) or not text:
        raise _Fault("invalid_integer")
    value = 0
    for character in text:
        if character < "0" or character > "9":
            raise _Fault("invalid_integer")
        digit = ord(character) - ord("0")
        if value > (maximum - digit) // 10:
            raise _Fault("invalid_integer")
        value = value * 10 + digit
    return value


def _fd_number(text):
    value = _decimal(text, _INT_MAX)
    if value < 3:
        raise _Fault("descriptor_overlaps_standard_stream")
    return value


class _Parser(argparse.ArgumentParser):
    def error(self, message):
        # Do not copy arbitrary command/environment text into the receipt.
        raise _Fault("arguments")


class _Once(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest) is not None:
            raise _Fault("duplicate_option", option=option_string)
        setattr(namespace, self.dest, values)


def _arguments(argv):
    try:
        delimiter = argv.index("--")
    except ValueError:
        delimiter = len(argv)
    head, command = argv[:delimiter], argv[delimiter + 1:]
    parser = _Parser(add_help=False, allow_abbrev=False)
    for option in (
        "receipt-fd", "scratch-bytes", "scratch-files", "output-bytes",
        "descriptor-limit", "deadline-seconds", "child-uid", "child-gid",
        "memory-events",
    ):
        parser.add_argument("--" + option, action=_Once)
    parser.add_argument("--pass-fd", action="append", default=[])
    args = parser.parse_args(head)
    if not command:
        raise _Fault("missing_command")
    return args, command


def _find_receipt(argv):
    """Recover a usable carrier even when another admission option is invalid."""
    candidate = None
    index = 0
    while index < len(argv) and argv[index] != "--":
        argument = argv[index]
        text = None
        if (
            argument == "--receipt-fd"
            and index + 1 < len(argv)
            and argv[index + 1] != "--"
        ):
            index += 1
            text = argv[index]
        elif argument.startswith("--receipt-fd="):
            text = argument.partition("=")[2]
        if text is not None:
            try:
                candidate = _fd_number(text)
            except _Fault:
                pass
        index += 1
    return candidate


def _identity(fd):
    metadata = os.fstat(fd)
    return metadata.st_dev, metadata.st_ino


def _validate_receipt(fd):
    if fd is None:
        raise _Fault("missing_receipt_fd")
    receipt_identity = _identity(fd)
    if fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY:
        raise _Fault("receipt_not_writable")
    for stream_fd in (0, 1, 2):
        try:
            collision = _identity(stream_fd) == receipt_identity
        except OSError as error:
            if error.errno != errno.EBADF:
                raise
            collision = False
        if collision:
            raise _Fault("receipt_fd_collision", descriptor=stream_fd)
    os.set_inheritable(fd, False)


def _forwarded(args, receipt_fd):
    receipt_identity = _identity(receipt_fd)
    # This is a trusted, launch-time configuration, never an event history.
    descriptors = tuple(sorted({_fd_number(text) for text in args.pass_fd}))
    for fd in descriptors:
        if fd == receipt_fd or _identity(fd) == receipt_identity:
            raise _Fault("receipt_fd_collision", descriptor=fd)
    return descriptors


def _trusted_environment(args, clients):
    environment = dict(os.environ)
    for name in _MARKERS + _LIMITS:
        environment.pop(name, None)
        os.environ.pop(name, None)
    for name, fd in zip(_MARKERS[1:], clients):
        environment[name] = str(fd)
    environment[_MARKERS[0]] = "managed"
    for name, option in zip(_LIMITS, _POLICY_OPTIONS):
        text = getattr(args, option)
        if text is not None:
            environment[name] = text
    # run_quota captures mode at import, including the actual socket identities.
    for name in _MARKERS + _LIMITS:
        if name in environment:
            os.environ[name] = environment[name]
    return environment


def _load_ledger():
    sys.dont_write_bytecode = True
    filename = os.path.join(os.path.dirname(os.path.realpath(__file__)), "run_quota.py")
    name = "_pf_quota_parent_ledger"
    spec = importlib.util.spec_from_file_location(name, filename)
    if spec is None or spec.loader is None:
        raise _Fault("ledger_import")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    if module.mode() != "managed" or tuple(module.LIMIT_ENV) != _LIMITS:
        raise _Fault("ledger_host_interface")
    return module


class _CapHeader(ctypes.Structure):
    _fields_ = (("version", ctypes.c_uint32), ("pid", ctypes.c_int))


class _CapData(ctypes.Structure):
    _fields_ = (
        ("effective", ctypes.c_uint32), ("permitted", ctypes.c_uint32),
        ("inheritable", ctypes.c_uint32),
    )


class _Linux:
    __slots__ = ("libc", "children_fd")

    def __init__(self):
        self.children_fd = None
        if (
            sys.platform != "linux" or ctypes.sizeof(ctypes.c_void_p) != 8
            or not hasattr(os, "fork") or not hasattr(os, "pipe2")
            or (
                (_NATIVE_PIDFD_OPEN is None or _NATIVE_PIDFD_SEND_SIGNAL is None)
                and (_PIDFD_SYSCALL is None or _machine() not in _PIDFD_ARCHITECTURES)
            )
            or not hasattr(os, "waitid") or not hasattr(os, "WNOWAIT")
            or not hasattr(os, "getresuid") or not hasattr(os, "setresuid")
            or not hasattr(os, "getresgid") or not hasattr(os, "setresgid")
            or not hasattr(socket, "SO_PASSCRED")
            or not hasattr(socket, "SCM_CREDENTIALS")
            or not hasattr(socket, "MSG_CMSG_CLOEXEC")
        ):
            raise _Fault("linux_containment_unavailable")
        if os.getresuid() != (0, 0, 0):
            raise _Fault("privileged_launcher_required")
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.prctl.argtypes = (
            ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_ulong, ctypes.c_ulong,
        )
        self.libc.prctl.restype = ctypes.c_int
        for name in ("capset", "capget"):
            function = getattr(self.libc, name)
            function.argtypes = (
                ctypes.POINTER(_CapHeader), ctypes.POINTER(_CapData),
            )
            function.restype = ctypes.c_int
        self.libc.fstatfs.argtypes = (ctypes.c_int, ctypes.c_void_p)
        self.libc.fstatfs.restype = ctypes.c_int
        self.prctl(4, 0)  # PR_SET_DUMPABLE: protect the privileged parent.
        self.prctl(36, 1)  # PR_SET_CHILD_SUBREAPER: own orphan cleanup/reaping.
        signal.signal(signal.SIGCHLD, signal.SIG_DFL)
        _proc_stat(os.getpid())
        # Reserve enumeration before launch. Cleanup must still work when no
        # descriptor can be allocated, including a pidfd-open EMFILE fallback.
        self.children_fd = os.open(
            f"/proc/self/task/{os.getpid()}/children", os.O_RDONLY | os.O_CLOEXEC,
        )
        try:
            if os.read(self.children_fd, 1):
                # Cleanup owns every child of this fresh standalone launcher.
                # It must never adopt an unrelated pre-existing workload.
                raise _Fault("launcher_already_has_children")
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.children_fd is not None:
            os.close(self.children_fd)
            self.children_fd = None

    def prctl(self, option, value=0, second=0):
        result = self.libc.prctl(option, value, second, 0, 0)
        if result == -1:
            number = ctypes.get_errno()
            raise OSError(number, os.strerror(number))
        return result

    def drop(self, uid, gid, parent_pid):
        self.prctl(38, 1)  # PR_SET_NO_NEW_PRIVS, inherited across exec.
        self.prctl(8, 0)  # PR_SET_KEEPCAPS.
        self.prctl(47, 4)  # PR_CAP_AMBIENT / PR_CAP_AMBIENT_CLEAR_ALL.
        capability = 0
        while True:
            try:
                present = self.prctl(23, capability)  # PR_CAPBSET_READ.
            except OSError as error:
                if error.errno == errno.EINVAL:
                    break  # The kernel, not a guessed capability count.
                raise
            if present:
                self.prctl(24, capability)  # PR_CAPBSET_DROP.
            capability += 1
        os.setgroups([])
        os.setresgid(gid, gid, gid)
        os.setresuid(uid, uid, uid)
        header = _CapHeader(0x20080522, 0)  # Linux capability ABI v3.
        capabilities = (_CapData * 2)()
        if self.libc.capset(ctypes.byref(header), capabilities) != 0:
            number = ctypes.get_errno()
            raise OSError(number, os.strerror(number))
        if self.libc.capget(ctypes.byref(header), capabilities) != 0:
            number = ctypes.get_errno()
            raise OSError(number, os.strerror(number))
        if any(
            item.effective or item.permitted or item.inheritable
            for item in capabilities
        ):
            raise _Fault("retained_child_capabilities", stage="launch")
        if (
            os.getresuid() != (uid, uid, uid)
            or os.getresgid() != (gid, gid, gid) or os.getgroups()
            or self.prctl(39) != 1  # PR_GET_NO_NEW_PRIVS.
        ):
            raise _Fault("child_privilege_drop", stage="launch")
        # A credential transition clears PDEATHSIG, so set it after dropping.
        self.prctl(1, signal.SIGKILL)
        if os.getppid() != parent_pid:
            raise _Fault("launcher_died_during_setup", stage="launch")

    def cgroup2(self, fd):
        # Linux statfs starts with a native long f_type. The fixed buffer is
        # larger than the supported Linux statfs ABI; no variable data follows.
        buffer = ctypes.create_string_buffer(4096)
        if self.libc.fstatfs(fd, buffer) != 0:
            return False
        return ctypes.c_long.from_buffer(buffer).value == 0x63677270


def _proc_stat(pid):
    with open(f"/proc/{pid}/stat", "rb") as source:
        record = source.read(4097)
    if len(record) > 4096 or not record.startswith(f"{pid} (".encode("ascii")):
        raise _Fault("process_generation_unreadable", stage="authentication")
    _, separator, tail = record.rpartition(b")")
    fields = tail.split()
    if not separator or len(fields) < 20:
        raise _Fault("process_generation_unreadable", stage="authentication")
    state = fields[0]
    return (
        int(fields[1]), int(fields[2]), int(fields[3]),
        int(fields[19]), state,
    )


def _process_credentials(pid):
    uids = gids = groups = None
    with open(f"/proc/{pid}/status", "rb") as source:
        for line in source:
            if line.startswith(b"Uid:"):
                uids = tuple(int(value) for value in line.split()[1:])
            elif line.startswith(b"Gid:"):
                gids = tuple(int(value) for value in line.split()[1:])
            elif line.startswith(b"Groups:"):
                groups = tuple(int(value) for value in line.split()[1:])
    return uids, gids, groups


def _soft_nofile(pid):
    try:
        return resource.prlimit(pid, resource.RLIMIT_NOFILE)[0]
    except (PermissionError, AttributeError, NotImplementedError):
        # A root container can lack CAP_SYS_RESOURCE for cross-uid prlimit.
        # procfs still exposes the actual kernel limit; this is no default.
        result = None
        with open(f"/proc/{pid}/limits", "rb") as source:
            for line in source:
                fields = line.split()
                if fields[:3] != [b"Max", b"open", b"files"]:
                    continue
                if result is not None or len(fields) != 6:
                    raise _Fault("actor_nofile_unavailable", stage="authentication")
                result = _decimal(fields[3].decode("ascii"), _U64_MAX)
        if result is None:
            raise _Fault("actor_nofile_unavailable", stage="authentication")
        return result


def _live(pidfd):
    poller = select.poll()
    poller.register(pidfd, select.POLLIN)
    return not poller.poll(0)


def _path_allowed(path):
    parts = path.split("/")
    for index, part in enumerate(parts):
        if part in (".ssh", ".aws", ".codex") or part.endswith(".env"):
            return False
        if parts[index:index + 3] == [".config", "zsh", "keys"]:
            return False
        if parts[index:index + 2] == [".config", "pineforge"]:
            return False
    return True


def _open_directory(path):
    # Never resolve a user-named symlink into an uninspected/sensitive target.
    if not _path_allowed(path):
        raise ValueError("forbidden observation path")
    path = os.path.abspath(path)
    if not _path_allowed(path):
        raise ValueError("forbidden observation path")
    descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for part in path.split("/"):
            if not part:
                continue
            following = os.open(
                part, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=descriptor,
            )
            os.close(descriptor)
            descriptor = following
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def _mount_text(text):
    # mountinfo's four specified pathname escapes, not arbitrary backslashes.
    return (
        text.replace("\\040", " ").replace("\\011", "\t")
        .replace("\\012", "\n").replace("\\134", "\\")
    )


def _cgroup_path(directory, metadata):
    """Map a pinned cgroupfs directory through this host's actual mount table."""
    best_length = -1
    result = None
    device = f"{os.major(metadata.st_dev)}:{os.minor(metadata.st_dev)}"
    with open("/proc/self/mountinfo", encoding="ascii") as source:
        for line in source:
            left, separator, right = line.partition(" - ")
            if not separator or right.split()[0] != "cgroup2":
                continue
            fields = left.split()
            if len(fields) < 6 or fields[2] != device:
                continue
            root, mount = _mount_text(fields[3]), _mount_text(fields[4])
            prefix = mount.rstrip("/") + "/"
            if directory != mount and not directory.startswith(prefix):
                continue
            if len(mount) <= best_length:
                continue
            suffix = directory[len(mount):].lstrip("/")
            # A subtree bind mount hides migration-control ancestors. It
            # cannot establish ownership of the complete visible hierarchy.
            if root == "/":
                result = ("/" + suffix).rstrip("/") or "/"
            else:
                result = None
            best_length = len(mount)
    return result


def _protected_cgroup_ancestry(directory, linux):
    """Reject delegation that could let the dropped uid migrate out of the run."""
    current = os.dup(directory)
    try:
        while linux.cgroup2(current):
            metadata = os.fstat(current)
            if metadata.st_uid != 0 or metadata.st_mode & 0o022:
                raise ValueError("delegated cgroup ancestor")
            for name in ("cgroup.procs", "cgroup.threads"):
                control = os.open(
                    name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=current,
                )
                try:
                    item = os.fstat(control)
                    if (
                        not linux.cgroup2(control) or item.st_uid != 0
                        or item.st_mode & 0o022
                    ):
                        raise ValueError("delegated cgroup migration control")
                finally:
                    os.close(control)
            parent = os.open(
                "..", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=current,
            )
            if _identity(parent) == _identity(current):
                os.close(parent)
                return
            os.close(current)
            current = parent
    finally:
        os.close(current)


def _membership(pid):
    result = None
    with open(f"/proc/{pid}/cgroup", encoding="ascii") as source:
        for line in source:
            if line.startswith("0::"):
                if result is not None:
                    raise ValueError("duplicate unified cgroup membership")
                result = line[3:].rstrip("\n")
    return result


def _kernel_counter(fd, name):
    value = None
    os.lseek(fd, 0, os.SEEK_SET)
    # These are kernel event records. A malformed/extended record makes the
    # observation unknown; it cannot refuse or cap the strategy's work.
    with os.fdopen(os.dup(fd), "rb", buffering=0) as source:
        while True:
            line = source.readline(257)
            if not line:
                break
            if len(line) > 256 or not line.endswith(b"\n"):
                raise ValueError("malformed kernel counter")
            fields = line.split()
            if len(fields) != 2:
                raise ValueError("malformed kernel counter")
            parsed = _decimal(fields[1].decode("ascii"), _U64_MAX)
            if fields[0] == name:
                if value is not None:
                    raise ValueError("duplicate kernel counter")
                value = parsed
    if value is None:
        raise ValueError("missing kernel counter")
    return value


class _Cgroup:
    """Pinned optional observation state; never a fabricated OOM fallback."""

    __slots__ = (
        "directory", "events", "population", "procs", "path", "before", "admitted",
        "diagnostic",
    )

    def __init__(self):
        self.directory = self.events = self.population = self.procs = None
        self.path = self.before = None
        self.admitted = False
        self.diagnostic = None

    @classmethod
    def prepare(cls, path, linux):
        observation = cls()
        if path is None:
            return observation
        try:
            if not _path_allowed(path) or not _path_allowed(os.path.abspath(path)):
                observation.diagnostic = _failure(
                    _Fault(
                        "arguments", stage="oom_observation",
                        option="--memory-events", path=path,
                    ),
                    "oom_observation",
                )
                return observation
            if os.path.basename(path) != "memory.events":
                return observation
            directory = os.path.abspath(os.path.dirname(path))
            observation.directory = _open_directory(directory)
            metadata = os.fstat(observation.directory)
            if not linux.cgroup2(observation.directory):
                raise ValueError("not a kernel cgroup v2 directory")
            if metadata.st_uid != 0 or metadata.st_mode & 0o022:
                raise ValueError("cgroup is not host owned")
            # Cooperating launcher instances must not race an empty group.
            # The host reservation also excludes other privileged writers;
            # no userspace lock can constrain an unrelated privileged host.
            fcntl.flock(observation.directory, fcntl.LOCK_EX | fcntl.LOCK_NB)
            _protected_cgroup_ancestry(observation.directory, linux)
            observation.path = _cgroup_path(directory, metadata)
            parent_path = _membership(os.getpid())
            if (
                observation.path is None or observation.path == "/"
                or parent_path is None or parent_path == observation.path
                or parent_path.startswith(observation.path + "/")
            ):
                raise ValueError("shared or unmapped observation cgroup")
            for name, attribute, flags in (
                ("memory.events", "events", os.O_RDONLY),
                ("cgroup.events", "population", os.O_RDONLY),
                ("cgroup.procs", "procs", os.O_RDWR),
            ):
                fd = os.open(
                    name, flags | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=observation.directory,
                )
                setattr(observation, attribute, fd)
                item = os.fstat(fd)
                if (
                    not stat.S_ISREG(item.st_mode)
                    or not linux.cgroup2(fd) or item.st_uid != 0
                    or item.st_mode & 0o022
                ):
                    raise ValueError("unprotected cgroup control")
            if os.read(observation.procs, 1):
                raise ValueError("cgroup already has processes")
            if _kernel_counter(observation.population, b"populated") != 0:
                raise ValueError("cgroup subtree already has processes")
            observation.before = _kernel_counter(observation.events, b"oom_kill")
            return observation
        except (OSError, ValueError, _Fault, UnicodeError, IndexError):
            observation.close()
            return cls()

    def enter_child(self):
        if self.procs is None:
            return False
        try:
            # Kernel "0" means the writing child, never a payload-provided pid.
            return os.write(self.procs, b"0") == 1
        except OSError:
            return False

    def admit(self, pid, moved):
        try:
            self.admitted = bool(
                moved and self.before is not None
                and _membership(pid) == self.path
                and _kernel_counter(self.population, b"populated") == 1
            )
        except (OSError, ValueError, _Fault, UnicodeError):
            self.admitted = False

    def result(self):
        unknown = {
            "proven": False, "before": None, "after": None, "delta": None,
            "diagnostic": self.diagnostic,
        }
        if not self.admitted:
            return unknown
        try:
            # All run members have been killed/reaped before this observation.
            if _kernel_counter(self.population, b"populated") != 0:
                return unknown
            os.lseek(self.procs, 0, os.SEEK_SET)
            if os.read(self.procs, 1):
                return unknown
            after = _kernel_counter(self.events, b"oom_kill")
            if after < self.before:
                return unknown
            delta = after - self.before
            return {
                "proven": delta > 0, "before": self.before,
                "after": after, "delta": delta, "diagnostic": None,
            }
        except (OSError, ValueError, _Fault, UnicodeError):
            return unknown

    def close(self):
        for attribute in ("events", "population", "procs", "directory"):
            descriptor = getattr(self, attribute)
            if descriptor is not None:
                os.close(descriptor)
                setattr(self, attribute, None)


class _Errors:
    __slots__ = ("lock", "event", "first")

    def __init__(self):
        self.lock = threading.Lock()
        self.event = threading.Event()
        self.first = None

    def record(self, error, stage):
        value = _failure(error, stage)
        with self.lock:
            if self.first is None:
                self.first = value
        self.event.set()


def _credentials_from_peek(controls):
    credentials = None
    unexpected = False
    for level, kind, value in controls:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            # Even MSG_PEEK can install received rights. Close every delivered
            # descriptor before rejecting the ancillary message.
            descriptors = array.array("i")
            descriptors.frombytes(value[:len(value) - len(value) % descriptors.itemsize])
            for fd in descriptors:
                os.close(fd)
            unexpected = True
        elif level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS:
            if credentials is not None or len(value) != _CREDENTIALS.size:
                unexpected = True
            else:
                credentials = _CREDENTIALS.unpack(value)
        else:
            unexpected = True
    if unexpected or credentials is None:
        raise _Fault("endpoint_credentials", stage="authentication")
    return credentials


class _Endpoint:
    __slots__ = (
        "module", "ledger", "channel", "run_pid", "uid", "gid", "stop",
        "errors", "thread",
    )

    def __init__(self, module, ledger, channel, run_pid, uid, gid, stop, errors):
        self.module, self.ledger, self.channel = module, ledger, channel
        self.run_pid, self.uid, self.gid = run_pid, uid, gid
        self.stop, self.errors = stop, errors
        self.thread = threading.Thread(target=self.work, daemon=True)

    def start(self):
        self.thread.start()

    def authenticate(self, credentials, role):
        pid, uid, gid = credentials
        if pid <= 0 or uid != self.uid or gid != self.gid:
            raise _Fault("endpoint_credentials", stage="authentication")
        pidfd = _pidfd_open(pid, 0)
        owner = None
        try:
            first = _proc_stat(pid)
            if (
                first[1:3] != (self.run_pid, self.run_pid) or first[3] <= 0
                or first[4] in (b"Z", b"X", b"x") or not _live(pidfd)
            ):
                raise _Fault("actor_outside_owned_run", stage="authentication")
            if _process_credentials(pid) != ((uid,) * 4, (gid,) * 4, ()):
                raise _Fault("actor_credentials_changed", stage="authentication")
            soft = _soft_nofile(pid)
            if soft == resource.RLIM_INFINITY or soft < 0:
                raise _Fault("actor_nofile_unavailable", stage="authentication")
            if _proc_stat(pid)[1:4] != first[1:4] or not _live(pidfd):
                raise _Fault("actor_generation_changed", stage="authentication")
            owner = self.ledger.open_owner(
                pid=pid, uid=uid, gid=gid, generation=first[3],
                role=role, soft_nofile=soft,
            )
            # open_owner takes its own process handle. Keep our proof alive
            # until that registration and its final host checks have finished.
            if (
                _proc_stat(pid)[1:4] != first[1:4] or not _live(pidfd)
                or _process_credentials(pid) != ((uid,) * 4, (gid,) * 4, ())
                or _soft_nofile(pid) != soft
            ):
                raise _Fault("actor_changed_during_registration", stage="authentication")
            return owner
        except BaseException:
            if owner is not None:
                self.ledger.disconnect(owner)
            raise
        finally:
            os.close(pidfd)

    def work(self):
        q = self.module
        owner = None
        handed_to_serve = False
        try:
            poller = select.poll()
            poller.register(self.channel, select.POLLIN | select.POLLHUP | select.POLLERR)
            while not self.stop.is_set():
                if not poller.poll(50):
                    continue
                data, controls, flags, _ = self.channel.recvmsg(
                    q.WIRE_SIZE + 1, socket.CMSG_SPACE(_CREDENTIALS.size),
                    socket.MSG_PEEK | socket.MSG_CMSG_CLOEXEC,
                )
                if not data and not controls:
                    return  # No client is not a quota refusal.
                credentials = _credentials_from_peek(controls)
                if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
                    raise _Fault("endpoint_hello_truncated", stage="authentication")
                candidate = q.Message.decode(data)
                # Peek only a candidate HELLO. The shipped service consumes and
                # validates the actual first frame and every subsequent frame.
                if candidate.opcode != q.Opcode.HELLO:
                    raise _Fault("endpoint_missing_hello", stage="authentication")
                role = q.Role(candidate[q.Slot.ROLE])
                owner = self.authenticate(credentials, role)
                handed_to_serve = True
                q.serve(self.ledger, self.channel, owner)
                return
        except BaseException as error:
            self.errors.record(error, "endpoint_service")
        finally:
            if owner is not None and not handed_to_serve:
                try:
                    self.ledger.disconnect(owner)
                except BaseException as error:
                    self.errors.record(error, "endpoint_disconnect")
            try:
                self.channel.close()
            except BaseException as error:
                self.errors.record(error, "endpoint_close")

    def shutdown(self):
        try:
            self.channel.shutdown(socket.SHUT_RDWR)
        except OSError as error:
            if error.errno not in (errno.EBADF, errno.ENOTCONN):
                raise


def _close_except(descriptors):
    previous = 2
    for descriptor in sorted(set(descriptors)):
        os.closerange(previous + 1, descriptor)
        previous = descriptor
    os.closerange(previous + 1, _INT_MAX)


def _child(
    linux, observation, uid, gid, parent_pid, clients, forwarded, setup_fd, gate_fd,
    command, environment,
):
    stage = 0
    try:
        for number in _PARENT_SIGNALS + (
            signal.SIGCHLD, signal.SIGPIPE, signal.SIGXFSZ,
        ):
            signal.signal(number, signal.SIG_DFL)
        os.setsid()
        stage = 1
        moved = observation.enter_child()
        stage = 2
        linux.close()  # The parent's enumeration fd never reaches exec.
        keep = clients + forwarded + (setup_fd, gate_fd)
        _close_except(keep)
        for descriptor in clients + forwarded:
            os.set_inheritable(descriptor, True)
        stage = 3
        linux.drop(uid, gid, parent_pid)
        stage = 4
        if os.write(setup_fd, _SETUP.pack(_READY, int(moved), 0)) != _SETUP.size:
            raise OSError(errno.EIO, "short setup write")
        if os.read(gate_fd, 1) != b"1":
            raise OSError(errno.EPIPE, "launcher gate closed")
        os.close(gate_fd)
        stage = 5
        os.execvpe(command[0], command, environment)
    except BaseException as error:
        number = getattr(error, "errno", 0)
        if type(number) is not int or not 0 <= number <= _INT_MAX:
            number = 0
        try:
            os.write(setup_fd, _SETUP.pack(_SETUP_FAILED, stage, number))
        except OSError:
            pass
        os._exit(_CHILD_SETUP_EXIT)


def _direct_children(children_fd):
    """Stream procfs decimal ids with constant storage, including fork storms."""
    os.lseek(children_fd, 0, os.SEEK_SET)
    value, digits = 0, False
    while True:
        chunk = os.read(children_fd, 4096)
        if not chunk:
            if digits:
                yield value
            return
        for byte in chunk:
            if 48 <= byte <= 57:
                digit = byte - 48
                if value > (_INT_MAX - digit) // 10:
                    raise _Fault("invalid_proc_child", stage="cleanup")
                value, digits = value * 10 + digit, True
            elif byte in (9, 10, 32):
                if digits:
                    yield value
                    value, digits = 0, False
            else:
                raise _Fault("invalid_proc_child", stage="cleanup")


def _kill_adopted(children_fd):
    for pid in _direct_children(children_fd):
        # Prove unreaped direct-child ownership without allocating another fd.
        # In particular, a pidfd-open EMFILE fallback must not itself require a
        # procfs open. Only this cleanup thread wait-reaps these children.
        try:
            os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        except ChildProcessError:
            raise _Fault("cleanup_child_changed", stage="cleanup") from None
        try:
            descriptor = _pidfd_open(pid, 0)
        except ProcessLookupError:
            continue
        except (OSError, OverflowError, NotImplementedError):
            # waitid(WNOWAIT) retained this direct child's pid reservation.
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            continue
        try:
            try:
                _pidfd_send_signal(descriptor, signal.SIGKILL)
            except ProcessLookupError:
                pass  # An unreaped child already exited; waitpid supplies status.
            except (OSError, OverflowError, NotImplementedError):
                # The pidfd pinned this generation, and it remains unreaped;
                # numeric kill cannot cross a PID-reuse boundary here.
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        finally:
            os.close(descriptor)


class _Run:
    __slots__ = (
        "pid", "pidfd", "generation", "setup_read", "setup_write", "gate_read",
        "gate_write", "setup_data", "ready", "setup_eof", "raw_status", "kill_sent",
    )

    def __init__(self):
        self.pid = self.pidfd = self.generation = None
        self.setup_read = self.setup_write = self.gate_read = self.gate_write = None
        self.setup_data = b""
        self.ready = self.setup_eof = False
        self.raw_status = None
        self.kill_sent = False

    def launch(self, linux, observation, uid, gid, clients, forwarded, command, environment):
        self.setup_read, self.setup_write = os.pipe2(os.O_CLOEXEC)
        self.gate_read, self.gate_write = os.pipe2(os.O_CLOEXEC)
        parent_pid = os.getpid()
        self.pid = os.fork()
        if self.pid == 0:
            _child(
                linux, observation, uid, gid, parent_pid, clients, forwarded,
                self.setup_write, self.gate_read, command, environment,
            )
            os._exit(_CHILD_SETUP_EXIT)
        os.close(self.setup_write)
        self.setup_write = None
        os.close(self.gate_read)
        self.gate_read = None
        os.set_blocking(self.setup_read, False)
        # This pins the launched leader only. It is never endpoint authority.
        self.pidfd = _pidfd_open(self.pid, 0)
        self.generation = _proc_stat(self.pid)[3]
        if self.generation <= 0:
            raise _Fault("leader_generation_unavailable", stage="launch")

    def setup(self, observation, deadline):
        if self.setup_eof:
            return
        try:
            chunk = os.read(self.setup_read, _SETUP.size - len(self.setup_data))
        except BlockingIOError:
            return
        if not chunk:
            self.setup_eof = True
            if self.setup_data:
                raise _Fault("truncated_child_setup", stage="launch")
            return
        self.setup_data += chunk
        if len(self.setup_data) != _SETUP.size:
            return
        kind, value, number = _SETUP.unpack(self.setup_data)
        self.setup_data = b""
        if kind == _SETUP_FAILED:
            stage = _SETUP_STAGES[value] if 0 <= value < len(_SETUP_STAGES) else "unknown"
            raise _Fault("child_setup_failed", stage="launch", operation=stage, errno=number)
        if kind != _READY or self.ready or value not in (0, 1) or number:
            raise _Fault("invalid_child_setup", stage="launch")
        current = _proc_stat(self.pid)
        if (
            current[1:3] != (self.pid, self.pid)
            or current[3] != self.generation or not _live(self.pidfd)
        ):
            raise _Fault("leader_changed_during_setup", stage="launch")
        observation.admit(self.pid, bool(value))
        if deadline is not None and time.monotonic() >= deadline:
            return False  # Leave exec gated until the owned run is killed.
        if os.write(self.gate_write, b"1") != 1:
            raise _Fault("child_gate_write", stage="launch")
        os.close(self.gate_write)
        self.gate_write = None
        self.ready = True

    def exited(self):
        # WNOWAIT reserves the leader's pid until the group kill. No reused
        # numeric group id can be targeted between exit observation and killpg.
        return os.waitid(
            os.P_PID, self.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT,
        ) is not None

    def terminate(self):
        if self.raw_status is not None:
            return self.kill_sent
        try:
            # This thread alone reaps the fork child. WNOWAIT reserves its pid
            # and original group identity even if pidfd signaling is refused.
            os.waitid(os.P_PID, self.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        except ChildProcessError:
            return self.kill_sent  # No owned generation remains to signal.
        try:
            os.killpg(self.pid, signal.SIGKILL)
            self.kill_sent = True
        except ProcessLookupError:
            pass  # setsid may not have completed; still kill the pinned child.
        numeric = self.pidfd is None
        if self.pidfd is not None:
            try:
                _pidfd_send_signal(self.pidfd, signal.SIGKILL)
                self.kill_sent = True
            except ProcessLookupError:
                pass
            except (OSError, NotImplementedError):
                numeric = True
        if numeric:
            # Our fork child is unreaped, so its numeric pid is still reserved.
            try:
                os.kill(self.pid, signal.SIGKILL)
                self.kill_sent = True
            except ProcessLookupError:
                pass
        return self.kill_sent

    def reap(self, children_fd):
        # Subreaper adoption catches orphaned descendants, including processes
        # that escaped the original group by making another session. There is
        # no saved pid list, per-descendant map, or artificial descendant cap.
        while True:
            _kill_adopted(children_fd)
            while True:
                try:
                    pid, status = os.waitpid(-1, os.WNOHANG)
                except ChildProcessError:
                    if self.raw_status is None:
                        raise _Fault("missing_child_wait_status", stage="cleanup")
                    return
                if pid == 0:
                    break
                if pid == self.pid and self.raw_status is None:
                    self.raw_status = status
            time.sleep(0.01)

    def close(self):
        for attribute in ("setup_read", "setup_write", "gate_read", "gate_write", "pidfd"):
            descriptor = getattr(self, attribute)
            if descriptor is not None:
                os.close(descriptor)
                setattr(self, attribute, None)


def _write_receipt(fd, receipt):
    data = (
        json.dumps(receipt, ensure_ascii=True, allow_nan=False, separators=(",", ":")) + "\n"
    ).encode("ascii")
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    try:
        # A full/broken carrier must fail the launcher, never block indefinitely
        # or divert trusted data to a child's output stream.
        fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        offset = 0
        while offset < len(data):
            try:
                count = os.write(fd, memoryview(data)[offset:])
            except InterruptedError:
                continue
            if count <= 0:
                raise OSError(errno.EIO, "short receipt write")
            offset += count
    finally:
        fcntl.fcntl(fd, fcntl.F_SETFL, flags)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    receipt_fd = _find_receipt(argv)
    receipt_ready = False
    receipt = {
        "schema": _SCHEMA,
        "quota_line": _QUOTA_LINE,
        "ledger_verdict": None,
        "wait_status": None,
        "exit_code": None,
        "signal": None,
        "oom": {
            "proven": False, "before": None, "after": None, "delta": None,
            "diagnostic": None,
        },
        "deadline": {"seconds": None, "expired": False, "kill_sent": False},
        "launcher_failure": None,
    }
    errors = _Errors()
    stop = threading.Event()
    run = _Run()
    observation = _Cgroup()
    ledger = module = linux = None
    native_server = native_client = python_server = python_client = None
    native_worker = python_worker = None
    interruption = 0
    end = None
    setup_registered = False
    result = 1

    def interrupted(number, frame):
        nonlocal interruption
        # Signal handlers never acquire a lock that they might interrupt.
        if interruption == 0:
            interruption = number

    try:
        _validate_receipt(receipt_fd)
        receipt_ready = True
        args, command = _arguments(argv)
        if _fd_number(args.receipt_fd) != receipt_fd:
            raise _Fault("ambiguous_receipt_fd")
        if args.deadline_seconds is not None:
            if re.fullmatch(r"[0-9]+(\.[0-9]+)?", args.deadline_seconds) is None:
                raise _Fault("invalid_deadline")
            try:
                seconds = float(args.deadline_seconds)
            except (TypeError, ValueError, OverflowError):
                raise _Fault("invalid_deadline") from None
            if not math.isfinite(seconds) or seconds <= 0:
                raise _Fault("invalid_deadline")
            receipt["deadline"]["seconds"] = seconds
            end = time.monotonic() + seconds
        uid, gid = _decimal(args.child_uid, _ID_MAX), _decimal(args.child_gid, _ID_MAX)
        if uid == 0 or gid == 0 or uid in os.getresuid():
            raise _Fault("child_identity_not_separate")
        forwarded = _forwarded(args, receipt_fd)
        # Managed-marker capture needs two real, distinct connected sockets.
        # Policy parsing needs neither privileged setup nor a ledger service.
        native_server, native_client = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_SEQPACKET,
        )
        python_server, python_client = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_SEQPACKET,
        )
        clients = (native_client.fileno(), python_client.fileno())
        environment = _trusted_environment(args, clients)
        module = _load_ledger()
        policy = {name: environment[name] for name in module.LIMIT_ENV if name in environment}
        limits = module.parse_limits(policy)
        # Only admitted CLI/policy reaches mandatory isolation and services.
        linux = _Linux()
        # These calls still precede any child send on either endpoint.
        for channel in (native_server, python_server):
            channel.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
        ledger = module.RunQuota(limits)
        receipt["quota_line"] = module.quota_line()
        for channel in (native_server, python_server):
            module.prepare_socket(channel)
        observation = _Cgroup.prepare(args.memory_events, linux)
        for number in _PARENT_SIGNALS:
            signal.signal(number, interrupted)
        if threading.active_count() != 1:
            raise _Fault("threads_before_fork")
        if end is not None and time.monotonic() >= end:
            receipt["deadline"]["expired"] = True
            raise _Fault("deadline_before_spawn", stage="launch")
        run.launch(linux, observation, uid, gid, clients, forwarded, command, environment)
        native_client.close()
        python_client.close()
        native_client = python_client = None
        native_worker = _Endpoint(
            module, ledger, native_server, run.pid, uid, gid, stop, errors,
        )
        python_worker = _Endpoint(
            module, ledger, python_server, run.pid, uid, gid, stop, errors,
        )
        native_worker.start()
        python_worker.start()
        poller = select.poll()
        poller.register(run.pidfd, select.POLLIN)
        poller.register(run.setup_read, select.POLLIN | select.POLLHUP)
        setup_registered = True
        while True:
            if interruption:
                raise _Fault("launcher_interrupted", stage="supervision", signal=interruption)
            if errors.event.is_set():
                break
            if run.exited():
                # Drain a setup failure queued immediately before child exit.
                run.setup(observation, end)
                break
            remaining = None if end is None else end - time.monotonic()
            if remaining is not None and remaining <= 0:
                receipt["deadline"]["expired"] = True
                break
            if run.setup(observation, end) is False:
                receipt["deadline"]["expired"] = True
                break
            if run.setup_eof and setup_registered:
                poller.unregister(run.setup_read)
                setup_registered = False
            milliseconds = 50
            if remaining is not None and remaining < 0.05:
                milliseconds = max(1, math.ceil(remaining * 1000))
            poller.poll(milliseconds)
    except BaseException as error:
        errors.record(error, "launcher")
    finally:
        if run.pid is not None and run.pid > 0:
            try:
                run.terminate()
            except BaseException as error:
                errors.record(error, "cleanup")
            finally:
                if receipt["deadline"]["expired"]:
                    receipt["deadline"]["kill_sent"] = run.kill_sent
            # A signaling failure must never suppress the actual wait path.
            try:
                run.reap(linux.children_fd)
            except BaseException as error:
                errors.record(error, "cleanup")
        stop.set()
        for worker in (native_worker, python_worker):
            if worker is not None:
                try:
                    worker.shutdown()
                except BaseException as error:
                    errors.record(error, "endpoint_shutdown")
        workers_stopped = True
        for worker in (native_worker, python_worker):
            if worker is not None and worker.thread.ident is not None:
                try:
                    worker.thread.join(5)
                    if worker.thread.is_alive():
                        workers_stopped = False
                        errors.record(
                            _Fault("worker_did_not_stop", stage="shutdown"), "shutdown",
                        )
                except BaseException as error:
                    workers_stopped = False
                    errors.record(error, "worker_join")
        if ledger is not None and workers_stopped:
            try:
                refusal = ledger.refusal
                if refusal is not None:
                    receipt["ledger_verdict"] = {
                        "code": refusal.code, "args": dict(refusal.arguments),
                    }
                ledger.close()
            except BaseException as error:
                errors.record(error, "ledger_shutdown")
        try:
            receipt["oom"] = observation.result()
        except BaseException as error:
            errors.record(error, "oom_observation")
        for channel in (native_client, python_client, native_server, python_server):
            if channel is not None:
                try:
                    channel.close()
                except BaseException as error:
                    errors.record(error, "channel_close")
        for owned in (observation, run, linux):
            if owned is None:
                continue
            try:
                owned.close()
            except BaseException as error:
                errors.record(error, "control_close")
        raw_status = run.raw_status
        if raw_status is not None:
            receipt["wait_status"] = raw_status
            if os.WIFEXITED(raw_status):
                result = receipt["exit_code"] = os.WEXITSTATUS(raw_status)
            elif os.WIFSIGNALED(raw_status):
                receipt["signal"] = os.WTERMSIG(raw_status)
                result = 128 + receipt["signal"]
            else:
                errors.record(_Fault("nonterminal_wait_status", stage="cleanup"), "cleanup")
        if interruption:
            errors.record(
                _Fault("launcher_interrupted", stage="supervision", signal=interruption),
                "supervision",
            )
        receipt["launcher_failure"] = errors.first
        if errors.first is not None:
            result = 128 + interruption if interruption else 1
        try:
            if not receipt_ready:
                result = 1
            else:
                _write_receipt(receipt_fd, receipt)
        except BaseException:
            result = 1
    return result


if __name__ == "__main__":
    sys.exit(main())
