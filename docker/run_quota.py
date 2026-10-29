"""Host-owned, finite run quotas and the fixed little-endian quota protocol.

The host creates the ledger and authenticates each pre-opened endpoint before
launch. Clients reserve before a mutation, report cumulative realization, then
cancel only the remainder. No destructor, disconnect, FINISH or output-file
deletion refunds an unknown extent or previously produced output.

The launcher imports this module before strategy code. With no host marker,
ledger/client factories return prebuilt no-op objects; limits never select a
mode. Managed accounting is cooperative until host containment is established.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum
import os
import secrets
import select
import socket
import stat
import struct
import threading
from types import MappingProxyType
from uuid import UUID


MAX_COUNTER = (1 << 53) - 1
MAX_ID = (1 << 64) - 1
WIRE_SIZE = 256
VERSION = 1
REPLY = 0x8000
MODE_ENV = "PF_RUN_QUOTA_MODE"
NATIVE_FD_ENV = "PF_RUN_QUOTA_NATIVE_FD"
PYTHON_FD_ENV = "PF_RUN_QUOTA_PYTHON_FD"
MARKER_ENV = (MODE_ENV, NATIVE_FD_ENV, PYTHON_FD_ENV)
ENFORCEMENT = "cooperative"
MANAGED_LINE = "quota=managed enforcement=" + ENFORCEMENT
UNMANAGED_LINE = "quota=unmanaged"
_HEADER = struct.Struct("<4sHHQ")
_PAYLOAD = struct.Struct("<30Q")
_CREDENTIALS = struct.Struct("3i")


class Opcode(IntEnum):
    HELLO = 1
    RESERVE = 2
    COMMIT = 3
    CANCEL = 4
    RELEASE = 5
    SNAPSHOT = 6
    FINISH = 7


class Stage(IntEnum):
    ADMISSION = 1
    TRANSPILE = 2
    COMPILE = 3
    EXECUTION = 4
    ASSEMBLY = 5
    FINALIZER = 6
    CONVERTER = 7
    PUBLICATION = 8


class Artifact(IntEnum):
    WORKDIR = 1
    COMPILER = 2
    TABLE = 3
    LEDGER = 4
    ARENA = 5
    CURVE = 6
    SINK = 7
    SPOOL = 8
    COLUMN_INDEX = 9
    COLUMN_DATA = 10
    JSON = 11
    STDOUT = 12
    CONVERSION = 13
    DIAGNOSTIC = 14


class Role(IntEnum):
    HOST = 1
    COMPILER = 2
    WORKER = 3
    FINALIZER = 4
    CONVERTER = 5
    REAPER = 6


class Resource(IntEnum):
    SCRATCH_BYTES = 1
    SCRATCH_FILES = 2
    OUTPUT_BYTES = 3
    DESCRIPTORS = 4


class Setting(IntEnum):
    SCRATCH_BYTES = 1
    SCRATCH_FILES = 2
    OUTPUT_BYTES = 3
    DESCRIPTORS = 4
    COMPILER = 5
    FACTS = 6
    WORKDIR = 7
    CHANNEL = 8
    PROFILE = 9
    PROTOCOL = 10


class Status(IntEnum):
    OK = 0
    RESOURCE_EXCEEDED = 1
    DESCRIPTORS_EXCEEDED = 2
    MISSING_LIMIT = 3
    MALFORMED_LIMIT = 4
    COUNTER_OVERFLOW = 5
    PROTOCOL = 6
    CONTAINMENT_UNAVAILABLE = 7
    UNMANAGED_PATH = 8
    UNKNOWN_BOUND = 9
    UNSUPPORTED_ROUTE = 10


class Slot(IntEnum):
    NONCE_HI = 0
    NONCE_LO = 1
    OBJECT = 2
    GENERATION = 3
    TICKET = 4
    STAGE = 5
    ARTIFACT = 6
    ROLE = 7
    RESOURCE = 8
    OLD_EXTENT = 9
    NEW_EXTENT = 10
    DELTA_DISK = 11
    DELTA_FILES = 12
    DELTA_OUTPUT = 13
    DELTA_FDS = 14
    STATUS = 15
    USED = 16
    REQUESTED = 17
    LIMIT = 18
    PEAK = 19
    COMMITTED = 20
    RESERVED = 21
    SOFT_NOFILE = 22
    PID = 23
    TID = 24
    DEVICE = 25
    INODE = 26
    ZERO0 = 27
    ZERO1 = 28
    ZERO2 = 29


@dataclass(frozen=True)
class Refusal:
    status: Status
    stage: Stage = Stage.ADMISSION
    artifact: Artifact = Artifact.WORKDIR
    role: Role = Role.WORKER
    resource: int = Setting.PROTOCOL
    used: int = 0
    requested: int = 0
    limit: int = 0
    peak: int = 0
    committed: int = 0
    reserved: int = 0
    soft_nofile: int = 0

    @property
    def code(self):
        if self.status == Status.RESOURCE_EXCEEDED:
            return "run_resource_quota_exceeded"
        if self.status == Status.DESCRIPTORS_EXCEEDED:
            return "run_descriptor_limit_exceeded"
        return "run_quota_configuration_invalid"

    @property
    def arguments(self):
        result = {"stage": self.stage.name.lower()}
        if self.status in (Status.RESOURCE_EXCEEDED, Status.DESCRIPTORS_EXCEEDED):
            result.update(used=self.used, requested=self.requested,
                          limit=self.limit, peak=self.peak)
            if self.status == Status.RESOURCE_EXCEEDED:
                result.update(resource=Resource(self.resource).name.lower(),
                              artifact=self.artifact.name.lower())
            else:
                result.update(process_role=self.role.name.lower(),
                              soft_nofile=self.soft_nofile)
        else:
            result.update(reason=self.status.name.lower(),
                          setting=Setting(self.resource).name.lower())
        return MappingProxyType(result)


class QuotaError(RuntimeError):
    def __init__(self, snapshot: Refusal):
        self.snapshot = snapshot
        self.code = snapshot.code
        self.arguments = snapshot.arguments
        super().__init__("run quota refused")

    @property
    def code_args(self):
        # Wrapper adapters get a copy; mutating it cannot rewrite the verdict.
        return dict(self.arguments)


def _invalid(status=Status.PROTOCOL, setting=Setting.PROTOCOL, stage=Stage.ADMISSION):
    return QuotaError(Refusal(status, stage=stage, resource=setting))


def _marker_fd(text):
    if not isinstance(text, str) or not text:
        raise ValueError("missing descriptor")
    value = 0
    for char in text:
        if char < "0" or char > "9" or value > (((1 << 31) - 1) - (ord(char) - 48)) // 10:
            raise ValueError("invalid descriptor")
        value = value * 10 + ord(char) - 48
    if value < 3:
        raise ValueError("descriptor overlaps report channels")
    return value


def _duplicate_channel(descriptor):
    duplicate = os.dup(descriptor)
    try:
        channel = socket.socket(fileno=duplicate)
    except BaseException:
        os.close(duplicate)
        raise
    try:
        if (channel.family != socket.AF_UNIX
                or channel.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) != socket.SOCK_SEQPACKET
                or channel.get_inheritable()):
            raise ValueError("invalid quota channel")
        channel.getpeername()  # A listening/unconnected socket is not a capability.
        return channel
    except BaseException:
        channel.close()
        raise


def _socket_identity(descriptor):
    channel = _duplicate_channel(descriptor)
    try:
        metadata = os.fstat(channel.fileno())
        if not stat.S_ISSOCK(metadata.st_mode):
            raise ValueError("descriptor is not a socket")
        return metadata.st_dev, metadata.st_ino
    finally:
        channel.close()


@dataclass(frozen=True)
class _HostMarker:
    mode: str
    descriptors: tuple = ()
    identities: tuple = ()
    status: Status = Status.OK


def _capture_host_marker():
    setting = os.environ.get(MODE_ENV)
    native = os.environ.get(NATIVE_FD_ENV)
    python = os.environ.get(PYTHON_FD_ENV)
    if setting is None and native is None and python is None:
        return _HostMarker("unmanaged")
    try:
        if setting != "managed":
            raise ValueError("invalid mode marker")
        descriptors = (_marker_fd(native), _marker_fd(python))
        if descriptors[0] == descriptors[1]:
            raise ValueError("channels must be distinct")
        identities = tuple(_socket_identity(fd) for fd in descriptors)
        if identities[0] == identities[1]:
            raise ValueError("channels alias")
        return _HostMarker("managed", descriptors, identities)
    except (OSError, ValueError):
        return _HostMarker("managed", status=Status.PROTOCOL)
    except (AttributeError, NotImplementedError):
        return _HostMarker("managed", status=Status.CONTAINMENT_UNAVAILABLE)


# Capture once at import, before any strategy is loaded. Neither user-supplied
# mappings nor later environment edits can enable/disable the decision.
_HOST_MARKER = _capture_host_marker()


def mode():
    return _HOST_MARKER.mode


def quota_line():
    """Exact stderr/usage-receipt text, without a newline; never prints."""
    return MANAGED_LINE if mode() == "managed" else UNMANAGED_LINE


def _validate_host_marker():
    if _HOST_MARKER.status != Status.OK:
        raise _invalid(_HOST_MARKER.status, Setting.CHANNEL)
    try:
        for descriptor, identity in zip(_HOST_MARKER.descriptors, _HOST_MARKER.identities):
            if _socket_identity(descriptor) != identity:
                raise ValueError("inherited channel replaced")
    except (OSError, ValueError):
        raise _invalid(Status.PROTOCOL, Setting.CHANNEL) from None
    except (AttributeError, NotImplementedError):
        raise _invalid(Status.CONTAINMENT_UNAVAILABLE, Setting.CHANNEL) from None


def _counter(value, setting=Setting.PROTOCOL, stage=Stage.ADMISSION):
    if type(value) is not int or value < 0:
        raise _invalid(Status.MALFORMED_LIMIT, setting, stage)
    if value > MAX_COUNTER:
        raise _invalid(Status.COUNTER_OVERFLOW, setting, stage)
    return value


def _identity(value):
    if type(value) is not int or not 0 <= value <= MAX_ID:
        raise _invalid()
    return value


def _enum(kind, value):
    if isinstance(value, bool) or not isinstance(value, int):
        raise _invalid()
    try:
        return kind(value)
    except (TypeError, ValueError):
        raise _invalid() from None


def rounded_extent(extent, block_size):
    _counter(extent, Setting.SCRATCH_BYTES)
    _counter(block_size, Setting.SCRATCH_BYTES)
    if block_size == 0:
        raise _invalid(Status.MALFORMED_LIMIT, Setting.SCRATCH_BYTES)
    extra = (-extent) % block_size
    if extra > MAX_COUNTER - extent:
        raise _invalid(Status.COUNTER_OVERFLOW, Setting.SCRATCH_BYTES)
    return extent + extra


@dataclass(frozen=True)
class Amounts:
    scratch_bytes: int = 0
    scratch_files: int = 0
    output_bytes: int = 0
    descriptors: int = 0

    def values(self):
        return (self.scratch_bytes, self.scratch_files, self.output_bytes, self.descriptors)

    def validate(self, stage=Stage.ADMISSION):
        for setting, value in zip(Resource, self.values()):
            _counter(value, Setting(setting), stage)
        return self


LIMIT_ENV = (
    "PF_RUN_SCRATCH_BYTES", "PF_RUN_SCRATCH_FILES",
    "PF_RUN_OUTPUT_BYTES", "PF_RUN_DESCRIPTOR_LIMIT",
)


def parse_limits(environ=None):
    """Managed host-policy parser, never a mode selector or a request parser."""
    environ = os.environ if environ is None else environ
    limits = []
    for setting, name in zip(Resource, LIMIT_ENV):
        if name not in environ:
            raise _invalid(Status.MISSING_LIMIT, Setting(setting))
        text = environ[name]
        if not isinstance(text, str) or not text or any(c < "0" or c > "9" for c in text):
            raise _invalid(Status.MALFORMED_LIMIT, Setting(setting))
        # Avoid Python's arbitrary integer/string conversion bound. Validate
        # before each multiplication/addition, including very long decimals.
        value = 0
        for char in text:
            digit = ord(char) - ord("0")
            if value > (MAX_COUNTER - digit) // 10:
                raise _invalid(Status.COUNTER_OVERFLOW, Setting(setting))
            value = value * 10 + digit
        limits.append(value)
    return Amounts(*limits)


@dataclass(frozen=True)
class Message:
    opcode: int
    sequence: int
    slots: tuple = (0,) * 30

    @classmethod
    def make(cls, opcode, sequence=0, **fields):
        slots = [0] * 30
        for name, value in fields.items():
            slots[Slot[name.upper()]] = int(value)
        return cls(int(opcode), sequence, tuple(slots))

    def __getitem__(self, slot):
        return self.slots[slot]

    def encode(self):
        _identity(self.sequence)
        _enum(Opcode, self.opcode & ~REPLY)
        if self.opcode & ~(REPLY | 7) or len(self.slots) != 30 or any(self.slots[27:]):
            raise _invalid()
        for value in self.slots:
            _identity(value)
        return _HEADER.pack(b"PFQ1", VERSION, self.opcode, self.sequence) + _PAYLOAD.pack(*self.slots)

    @classmethod
    def decode(cls, data):
        if len(data) != WIRE_SIZE:
            raise _invalid()
        magic, version, opcode, sequence = _HEADER.unpack_from(data)
        if magic != b"PFQ1" or version != VERSION:
            raise _invalid()
        message = cls(opcode, sequence, _PAYLOAD.unpack_from(data, _HEADER.size))
        message.encode()  # Also checks opcode, zero slots and u64 identities.
        return message


@dataclass(frozen=True)
class Usage:
    committed: int
    reserved: int
    peak: int
    limit: int

    @property
    def used(self):
        return self.committed + self.reserved


class _UnmanagedTicket:
    __slots__ = ()
    number = 0

    def commit(self, realized, actual_extent, *, device=0, inode=0):
        pass

    def cancel(self):
        pass


class _UnmanagedClient:
    __slots__ = ()
    refusal = None
    owner = None

    def check(self):
        pass

    def reserve(self, amounts, *, object_id=0, generation=0, old_extent=0, new_extent=0,
                stage=Stage.EXECUTION, artifact=Artifact.TABLE, device=0, inode=0):
        return _UNMANAGED_TICKET

    def commit(self, ticket, realized, actual_extent, *, device=0, inode=0):
        pass

    def cancel(self, ticket):
        pass

    def release(self, amounts, *, object_id=0, generation=0, old_extent=0, new_extent=0,
                stage=Stage.EXECUTION, artifact=Artifact.TABLE, device=0, inode=0):
        pass

    def snapshot(self, resource=Resource.SCRATCH_BYTES):
        return _UNMANAGED_USAGE

    def finish(self, resource=Resource.SCRATCH_BYTES):
        return _UNMANAGED_USAGE

    def close(self):
        pass


class _UnmanagedLedger:
    __slots__ = ()
    refusal = None

    def open_owner(self, *, pid, uid, gid, generation, role=Role.WORKER, soft_nofile):
        return None

    def grant_object(self, owner, recipient, object_id, generation):
        pass

    def confirm_object(self, owner, object_id, generation, descriptor, *, disk_backed=True):
        pass

    def reserve(self, owner, amounts, *, object_id=0, generation=0, old_extent=0,
                new_extent=0, stage=Stage.EXECUTION, artifact=Artifact.TABLE, device=0, inode=0):
        return _UNMANAGED_TICKET

    def commit(self, ticket, realized, actual_extent, *, device=0, inode=0):
        pass

    def cancel(self, ticket):
        pass

    def release(self, owner, amounts, *, object_id=0, generation=0, old_extent=0,
                new_extent=0, stage=Stage.EXECUTION, artifact=Artifact.TABLE, device=0, inode=0):
        pass

    def snapshot(self):
        return _UNMANAGED_USAGES

    def finish(self, owner):
        return _UNMANAGED_USAGES

    def disconnect(self, owner):
        pass

    def close(self):
        pass


# Created once, outside the unmanaged calls. No per-call tickets, counters,
# locks, sockets, identity probes or refusal objects exist on those paths.
_UNMANAGED_TICKET = _UnmanagedTicket()
_UNMANAGED_USAGE = Usage(0, 0, 0, 0)
_UNMANAGED_USAGES = (_UNMANAGED_USAGE,) * 4
_UNMANAGED_CLIENT = _UnmanagedClient()
_UNMANAGED_LEDGER = _UnmanagedLedger()


@dataclass(frozen=True)
class Owner:
    # Issued by the host. Neither packet pid/tid nor an environment marker
    # authenticates an owner. generation is the host's registration token;
    # the live process proof is recorded separately, never supplied on wire.
    number: int
    nonce: tuple
    pid: int
    uid: int
    gid: int
    generation: int
    role: Role
    soft_nofile: int


def _proc_generation(pid):
    # comm (field 2) can contain spaces and ')'; split after its final ')'.
    try:
        with open(f"/proc/{pid}/stat", "rb") as source:
            record = source.read()
    except FileNotFoundError:
        raise ProcessLookupError(pid) from None
    _, separator, tail = record.rpartition(b")")
    fields = tail.split()
    if (not record.startswith(f"{pid} (".encode("ascii")) or not separator
            or len(fields) < 20 or not fields[19].isdigit()):
        raise ValueError("process start time is unreadable")
    # tail begins at field 3, so field 22 (starttime) is at index 19.
    start_time = int(fields[19])
    with open("/proc/sys/kernel/random/boot_id", encoding="ascii") as source:
        boot_id = str(UUID(source.read().strip()))
    return pid, start_time, boot_id


@dataclass(frozen=True)
class _ProcessProof:
    pidfd: int | None = None
    proc_generation: tuple | None = None


@dataclass
class _OwnerState:
    owner: Owner
    connected: bool = True
    finished: bool = False
    loose_fds: int = 0
    virtual_ticket: int = 0


@dataclass
class _Object:
    key: tuple
    custodian: int
    extent: int = 0
    files: int = 0
    identity: tuple | None = None
    confirmed_identity: tuple | None = None
    observed_extent: int = 0
    fds: dict = field(default_factory=dict)
    grants: set = field(default_factory=set)
    pending: int = 0


@dataclass
class _Pending:
    owner: Owner
    key: tuple | None
    amounts: tuple
    old_extent: int
    stage: Stage
    artifact: Artifact
    realized: tuple = (0, 0, 0, 0)


class Ticket:
    """An explicit reservation retaining its original ledger/client and owner."""
    def __init__(self, endpoint, owner, number, request=None):
        self.endpoint, self.owner, self.number, self.request = endpoint, owner, number, request

    def commit(self, realized, actual_extent, *, device=0, inode=0):
        return self.endpoint.commit(self, realized, actual_extent, device=device, inode=inode)

    def cancel(self):
        return self.endpoint.cancel(self)


class RunQuota:
    def __new__(cls, limits: Amounts, *, nonce=None, max_owners=64):
        if mode() == "unmanaged":
            return _UNMANAGED_LEDGER
        return super().__new__(cls)

    def __init__(self, limits: Amounts, *, nonce=None, max_owners=64):
        _validate_host_marker()
        self.limits = limits.validate().values()
        self.nonce = (secrets.randbits(64), secrets.randbits(64)) if nonce is None else tuple(nonce)
        if len(self.nonce) != 2 or not any(self.nonce):
            raise _invalid()
        for value in self.nonce:
            _identity(value)
        _counter(max_owners)
        if not max_owners:
            raise _invalid()
        # This is the separately reserved control-record pool, not a limit on
        # data files. The host must include it in its containment policy.
        self.max_owners = max_owners
        self._lock = threading.RLock()
        self._committed, self._pending, self._peak = [0] * 4, [0] * 4, [0] * 4
        self._owners, self._objects, self._inodes, self._tickets = {}, {}, {}, {}
        self._process = {}
        self._process_handles = {}
        self._next_ticket = 1
        self._refusal = None

    @classmethod
    def from_environment(cls, *, nonce=None, max_owners=64):
        if mode() == "unmanaged":
            return _UNMANAGED_LEDGER
        _validate_host_marker()
        return cls(parse_limits(), nonce=nonce, max_owners=max_owners)

    @property
    def refusal(self):
        with self._lock:
            return self._refusal

    def _fail(self, refusal):
        if self._refusal is None:
            self._refusal = refusal
        raise QuotaError(self._refusal)

    def _protocol(self):
        self._fail(Refusal(Status.PROTOCOL, resource=Setting.PROTOCOL))

    def _owner(self, owner, *, growth=False):
        state = self._owners.get(owner.number)
        if state is None or state.owner is not owner or owner.nonce != self.nonce:
            self._protocol()
        proof = self._process_handles.get((owner.pid, owner.generation))
        if proof is None:
            self._fail(Refusal(Status.PROTOCOL, resource=Setting.CHANNEL))
        if proof.pidfd is not None:
            # Strong proof: the kernel handle pins this process generation.
            poller = select.poll()
            poller.register(proof.pidfd, select.POLLIN)
            if poller.poll(0):
                self._fail(Refusal(Status.PROTOCOL, resource=Setting.CHANNEL))
        else:
            # Fallback proof: boot id + pid + starttime excludes PID reuse.
            # The reaper still keeps unknown extents charged until verified
            # cleanup; observing process death never refunds a reservation.
            try:
                generation = _proc_generation(owner.pid)
            except ProcessLookupError:
                self._fail(Refusal(Status.PROTOCOL, resource=Setting.CHANNEL))
            except (OSError, ValueError):
                self._fail(Refusal(Status.CONTAINMENT_UNAVAILABLE, resource=Setting.CHANNEL))
            if generation != proof.proc_generation:
                self._fail(Refusal(Status.PROTOCOL, resource=Setting.CHANNEL))
        if not state.connected or (growth and state.finished):
            self._fail(Refusal(Status.PROTOCOL, resource=Setting.CHANNEL))
        if growth and self._refusal is not None:
            raise QuotaError(self._refusal)
        return state

    def open_owner(self, *, pid, uid, gid, generation, role=Role.WORKER, soft_nofile):
        """Host-only registration after recording the descendant's generation."""
        with self._lock:
            if len(self._owners) >= self.max_owners:
                self._fail(Refusal(Status.UNKNOWN_BOUND, resource=Setting.CHANNEL))
            for value in (pid, uid, gid, generation):
                _identity(value)
            if not pid or not generation:
                self._protocol()
            role = _enum(Role, role)
            _counter(soft_nofile, Setting.DESCRIPTORS)
            process_key = (pid, generation)
            previous = self._process.get(process_key)
            if previous is not None and previous[2] != soft_nofile:
                self._protocol()
            if process_key not in self._process_handles:
                for (recorded_pid, recorded_generation) in self._process_handles:
                    if recorded_pid == pid and recorded_generation != generation:
                        self._protocol()
                proof = None
                if hasattr(os, "pidfd_open") and hasattr(select, "poll"):
                    try:
                        proof = _ProcessProof(pidfd=os.pidfd_open(pid, 0))
                    except (OSError, OverflowError, NotImplementedError):
                        pass
                if proof is None:
                    # Sandboxes may deny pidfd_open even on recent Python.
                    # Accept only an actual /proc generation, never a pid alone.
                    try:
                        proof = _ProcessProof(proc_generation=_proc_generation(pid))
                    except (OSError, ValueError):
                        self._fail(Refusal(Status.CONTAINMENT_UNAVAILABLE, resource=Setting.CHANNEL))
                self._process_handles[process_key] = proof
            self._process.setdefault(process_key, [0, 0, soft_nofile])
            owner = Owner(len(self._owners) + 1, self.nonce, pid, uid, gid,
                          generation, role, soft_nofile)
            self._owners[owner.number] = _OwnerState(owner)
            return owner

    def close(self):
        """Host shutdown after joining servers; data/unknown extents stay charged."""
        with self._lock:
            for proof in self._process_handles.values():
                if proof.pidfd is not None:
                    os.close(proof.pidfd)
            self._process_handles.clear()
            for state in self._owners.values():
                state.connected = False

    def __del__(self):
        # Only control handles, never data-accounting refunds or filesystem
        # cleanup. Each live server/ticket retains the ledger independently.
        for proof in getattr(self, "_process_handles", {}).values():
            try:
                if proof.pidfd is not None:
                    os.close(proof.pidfd)
            except OSError:
                pass

    def grant_object(self, owner, recipient, object_id, generation):
        """Host-only capability for an alias; it does not refund the backing."""
        with self._lock:
            self._owner(owner)
            self._owner(recipient)
            obj = self._objects.get((object_id, generation))
            if obj is None or obj.custodian != owner.number or not obj.files:
                self._protocol()
            obj.grants.add(recipient.number)

    def confirm_object(self, owner, object_id, generation, descriptor, *, disk_backed=True):
        """Host-only fstat of an admitted backing, before accepting its COMMIT.

        The host's syscall monitor/creation helper supplies its own descriptor.
        A child's claimed device/inode fields alone cannot confirm an object.
        The host must admit this observation descriptor in its phase inventory.
        """
        with self._lock:
            self._owner(owner)
            if type(disk_backed) is not bool:
                self._protocol()
            obj = self._objects.get((object_id, generation))
            if obj is None or obj.custodian != owner.number or not obj.pending:
                self._protocol()
            try:
                metadata = os.fstat(descriptor)
            except OSError:
                self._fail(Refusal(Status.CONTAINMENT_UNAVAILABLE, resource=Setting.WORKDIR))
            if not (stat.S_ISREG(metadata.st_mode) or stat.S_ISDIR(metadata.st_mode)):
                self._protocol()
            _counter(metadata.st_size, Setting.SCRATCH_BYTES)
            identity = (metadata.st_dev, metadata.st_ino)
            for value in identity:
                _identity(value)
            if not identity[1] or (obj.identity is not None and obj.identity != identity):
                self._protocol()
            if identity in self._inodes and self._inodes[identity] != obj.key:
                self._protocol()
            obj.confirmed_identity = identity
            # Memfd/shmem capacity belongs to the host's memory allowance.
            # This host-only flag cannot be supplied by a wire client.
            obj.observed_extent = metadata.st_size if disk_backed else 0
            return identity

    def snapshot(self):
        with self._lock:
            return tuple(Usage(c, p, peak, limit) for c, p, peak, limit in zip(
                self._committed, self._pending, self._peak, self.limits))

    def reserve(self, owner, amounts, *, object_id=0, generation=0, old_extent=0,
                new_extent=0, stage=Stage.EXECUTION, artifact=Artifact.TABLE,
                device=0, inode=0):
        with self._lock:
            state = self._owner(owner, growth=True)
            stage, artifact = _enum(Stage, stage), _enum(Artifact, artifact)
            try:
                amounts.validate(stage)
                _counter(old_extent, Setting.SCRATCH_BYTES, stage)
                _counter(new_extent, Setting.SCRATCH_BYTES, stage)
                for value in (object_id, generation, device, inode):
                    _identity(value)
            except QuotaError as error:
                self._fail(error.snapshot)
            deltas = amounts.values()
            if (new_extent < old_extent
                    or deltas[0] != new_extent - old_extent or deltas[1] > 1):
                self._protocol()
            key = (object_id, generation) if object_id else None
            obj = self._objects.get(key)
            if key is None:
                if generation or device or inode or old_extent or new_extent or deltas[1] or state.virtual_ticket:
                    self._protocol()
            elif not generation:
                self._protocol()
            elif obj is None:
                if old_extent or not deltas[1] or device or inode:
                    self._protocol()
            elif (owner.number not in obj.grants or obj.pending or deltas[1]
                  or old_extent != obj.extent or (device, inode) != obj.identity):
                self._protocol()
            # All validations precede arithmetic. The atomic aggregate includes
            # pending reservations from every endpoint, in this exact order.
            process = self._process[(owner.pid, owner.generation)]
            for index, delta in enumerate(deltas):
                used = self._committed[index] + self._pending[index]
                limit = self.limits[index]
                if delta > limit - used:
                    self._fail(Refusal(
                        Status.DESCRIPTORS_EXCEEDED if index == 3 else Status.RESOURCE_EXCEEDED,
                        stage, artifact, owner.role, index + 1, used, delta, limit,
                        self._peak[index], self._committed[index], self._pending[index],
                        owner.soft_nofile))
                if index == 3:
                    process_used = process[0] + process[1]
                    if delta > owner.soft_nofile - process_used:
                        self._fail(Refusal(
                            Status.DESCRIPTORS_EXCEEDED, stage, artifact, owner.role,
                            Resource.DESCRIPTORS, process_used, delta, owner.soft_nofile,
                            process[3] if len(process) > 3 else process_used,
                            process[0], process[1], owner.soft_nofile))
            if self._next_ticket > MAX_ID:
                self._fail(Refusal(Status.COUNTER_OVERFLOW, stage, resource=Setting.PROTOCOL))
            number = self._next_ticket
            self._next_ticket += 1
            if key is not None and obj is None:
                obj = _Object(key, owner.number, grants={owner.number})
                self._objects[key] = obj
            if obj is not None:
                obj.pending = number
            else:
                state.virtual_ticket = number
            self._tickets[number] = _Pending(owner, key, deltas, old_extent, stage, artifact)
            for index, delta in enumerate(deltas):
                self._pending[index] += delta
                self._peak[index] = max(self._peak[index], self._committed[index] + self._pending[index])
            process[1] += deltas[3]
            if len(process) == 3:
                process.append(0)
            process[3] = max(process[3], process[0] + process[1])
            return Ticket(self, owner, number)

    def _ticket(self, ticket):
        if ticket.endpoint is not self:
            self._protocol()
        self._owner(ticket.owner)
        pending = self._tickets.get(ticket.number)
        if pending is None or pending.owner is not ticket.owner:
            self._protocol()
        return pending

    def commit(self, ticket, realized, actual_extent, *, device=0, inode=0):
        with self._lock:
            pending = self._ticket(ticket)
            try:
                realized.validate(pending.stage)
                _counter(actual_extent, Setting.SCRATCH_BYTES, pending.stage)
                _identity(device)
                _identity(inode)
            except QuotaError as error:
                self._fail(error.snapshot)
            values = realized.values()
            if any(not before <= after <= cap for before, after, cap in zip(
                    pending.realized, values, pending.amounts)):
                self._protocol()
            if (actual_extent < pending.old_extent
                    or actual_extent - pending.old_extent != values[0]):
                self._protocol()
            obj = self._objects.get(pending.key)
            if obj is None:
                if actual_extent or device or inode or values[1]:
                    self._protocol()
            else:
                identity = (device, inode)
                if values[1] or obj.files:
                    if not inode or (obj.identity is not None and obj.identity != identity):
                        self._protocol()
                    if obj.confirmed_identity is None:
                        self._fail(Refusal(Status.CONTAINMENT_UNAVAILABLE, pending.stage,
                                           resource=Setting.WORKDIR))
                    if obj.confirmed_identity != identity:
                        self._protocol()
                    other = self._inodes.get(identity)
                    if other is not None and other != pending.key:
                        self._protocol()
                elif device or inode or values[0] or values[3]:
                    self._protocol()
            increments = tuple(after - before for after, before in zip(values, pending.realized))
            # No operation below can exceed committed+pending, already admitted.
            for index, increment in enumerate(increments):
                self._pending[index] -= increment
                self._committed[index] += increment
            process = self._process[(ticket.owner.pid, ticket.owner.generation)]
            process[1] -= increments[3]
            process[0] += increments[3]
            if obj is not None:
                obj.extent = actual_extent
                obj.files += increments[1]
                obj.fds[ticket.owner.number] = obj.fds.get(ticket.owner.number, 0) + increments[3]
                if obj.files:
                    obj.identity = (device, inode)
                    self._inodes[obj.identity] = pending.key
            else:
                self._owners[ticket.owner.number].loose_fds += increments[3]
            pending.realized = values
            # Keep the ticket until explicit CANCEL, even after full COMMIT:
            # cumulative COMMIT is idempotent and cannot count retries twice.

    def cancel(self, ticket):
        with self._lock:
            pending = self._ticket(ticket)
            obj = self._objects.get(pending.key)
            if obj is not None and obj.confirmed_identity is not None and (
                    not obj.files or obj.extent < obj.observed_extent):
                # The host has observed a real inode/extent. An omitted or
                # rejected COMMIT cannot turn it back into unused capacity.
                self._protocol()
            remainder = tuple(cap - done for cap, done in zip(pending.amounts, pending.realized))
            for index, delta in enumerate(remainder):
                self._pending[index] -= delta
            self._process[(ticket.owner.pid, ticket.owner.generation)][1] -= remainder[3]
            if pending.key is None:
                self._owners[ticket.owner.number].virtual_ticket = 0
            else:
                obj.pending = 0
                if not obj.files:
                    del self._objects[pending.key]
            del self._tickets[ticket.number]
            ticket.number = 0

    def release(self, owner, amounts, *, object_id=0, generation=0, old_extent=0,
                new_extent=0, stage=Stage.EXECUTION, artifact=Artifact.TABLE,
                device=0, inode=0):
        """Only after successful shrink/close/removal; aliases keep the inode live."""
        with self._lock:
            state = self._owner(owner)
            stage, artifact = _enum(Stage, stage), _enum(Artifact, artifact)
            try:
                amounts.validate(stage)
                _counter(old_extent, Setting.SCRATCH_BYTES, stage)
                _counter(new_extent, Setting.SCRATCH_BYTES, stage)
                for value in (object_id, generation, device, inode):
                    _identity(value)
            except QuotaError as error:
                self._fail(error.snapshot)
            values = amounts.values()
            if values[2] or old_extent < new_extent or old_extent - new_extent != values[0]:
                self._protocol()
            obj = self._objects.get((object_id, generation))
            if object_id == 0:
                if (generation or old_extent or new_extent or values[1] or device or inode
                        or values[3] > state.loose_fds or state.virtual_ticket):
                    self._protocol()
            elif (obj is None or owner.number not in obj.grants or obj.pending
                  or obj.extent != old_extent or obj.identity != (device, inode)
                  or values[1] > obj.files or values[3] > obj.fds.get(owner.number, 0)):
                self._protocol()
            if obj is not None and values[1]:
                if (obj.custodian != owner.number or new_extent
                        or sum(obj.fds.values()) != values[3]):
                    self._protocol()
            if obj is not None:
                obj.extent = new_extent
                obj.observed_extent = min(obj.observed_extent, new_extent)
                obj.fds[owner.number] = obj.fds.get(owner.number, 0) - values[3]
                obj.files -= values[1]
                if not obj.files:
                    del self._inodes[obj.identity]
                    del self._objects[obj.key]
            else:
                state.loose_fds -= values[3]
            for index, delta in enumerate(values):
                self._committed[index] -= delta
            self._process[(owner.pid, owner.generation)][0] -= values[3]

    def finish(self, owner):
        with self._lock:
            self._owner(owner).finished = True
            return self.snapshot()

    def disconnect(self, owner):
        with self._lock:
            state = self._owners.get(owner.number)
            if state is None or state.owner is not owner:
                self._protocol()
            state.connected = False
            # A graceful FINISH forbids that owner's growth without poisoning
            # the remaining stages. Unknown in-flight growth stays charged.
            if not state.finished and self._refusal is None:
                self._refusal = Refusal(Status.PROTOCOL, resource=Setting.CHANNEL)


_COMMON = {Slot.NONCE_HI, Slot.NONCE_LO, Slot.ROLE}
_CONTEXT = {Slot.STAGE, Slot.ARTIFACT}
_OBJECT = {Slot.OBJECT, Slot.GENERATION, Slot.DEVICE, Slot.INODE}
_DELTAS = {Slot.DELTA_DISK, Slot.DELTA_FILES, Slot.DELTA_OUTPUT, Slot.DELTA_FDS}
_EXTENTS = {Slot.OLD_EXTENT, Slot.NEW_EXTENT}
_ALLOWED = {
    Opcode.HELLO: {Slot.ROLE},
    Opcode.RESERVE: _COMMON | _CONTEXT | _OBJECT | _DELTAS | _EXTENTS,
    Opcode.COMMIT: _COMMON | _DELTAS | {Slot.TICKET, Slot.NEW_EXTENT, Slot.DEVICE, Slot.INODE},
    Opcode.CANCEL: _COMMON | {Slot.TICKET},
    Opcode.RELEASE: _COMMON | _CONTEXT | _OBJECT | _DELTAS | _EXTENTS,
    Opcode.SNAPSHOT: _COMMON | {Slot.RESOURCE},
    Opcode.FINISH: _COMMON | {Slot.RESOURCE},
}
_REPLY_COMMON = {Slot.NONCE_HI, Slot.NONCE_LO}
_USAGE_FIELDS = {Slot.RESOURCE, Slot.USED, Slot.LIMIT, Slot.PEAK, Slot.COMMITTED, Slot.RESERVED}
_FAILURE_FIELDS = _USAGE_FIELDS | {
    Slot.STATUS, Slot.STAGE, Slot.ARTIFACT, Slot.ROLE, Slot.REQUESTED, Slot.SOFT_NOFILE,
}
_REPLY_ALLOWED = {
    Opcode.HELLO: {Slot.GENERATION, Slot.ROLE, Slot.PID, Slot.SOFT_NOFILE},
    Opcode.RESERVE: {Slot.TICKET},
    Opcode.COMMIT: set(),
    Opcode.CANCEL: set(),
    Opcode.RELEASE: set(),
    Opcode.SNAPSHOT: _USAGE_FIELDS,
    Opcode.FINISH: _USAGE_FIELDS,
}


def _usage_from_message(message):
    values = [message[slot] for slot in (
        Slot.COMMITTED, Slot.RESERVED, Slot.PEAK, Slot.LIMIT, Slot.USED)]
    for value in values:
        _counter(value)
    committed, reserved, peak, limit, used = values
    if committed + reserved != used or not used <= peak <= limit:
        raise _invalid()
    return Usage(committed, reserved, peak, limit)


def _failure_reply(request, refusal, nonce):
    fields = dict(nonce_hi=nonce[0], nonce_lo=nonce[1], status=refusal.status,
                  stage=refusal.stage, artifact=refusal.artifact, role=refusal.role,
                  resource=refusal.resource, used=refusal.used, requested=refusal.requested,
                  limit=refusal.limit, peak=refusal.peak, committed=refusal.committed,
                  reserved=refusal.reserved, soft_nofile=refusal.soft_nofile)
    return Message.make(request.opcode | REPLY, request.sequence, **fields)


def refusal_from_message(message):
    status = _enum(Status, message[Slot.STATUS])
    if status == Status.OK:
        return None
    stage, role = _enum(Stage, message[Slot.STAGE]), _enum(Role, message[Slot.ROLE])
    artifact = _enum(Artifact, message[Slot.ARTIFACT])
    if status in (Status.RESOURCE_EXCEEDED, Status.DESCRIPTORS_EXCEEDED):
        resource = _enum(Resource, message[Slot.RESOURCE])
        if (status == Status.DESCRIPTORS_EXCEEDED) != (resource == Resource.DESCRIPTORS):
            raise _invalid()
    else:
        resource = _enum(Setting, message[Slot.RESOURCE])
    counters = [message[slot] for slot in (Slot.USED, Slot.REQUESTED, Slot.LIMIT, Slot.PEAK,
                                         Slot.COMMITTED, Slot.RESERVED, Slot.SOFT_NOFILE)]
    for value in counters:
        _counter(value)
    if (counters[0] != counters[4] + counters[5] or counters[0] > counters[3]
            or counters[3] > counters[2]):
        raise _invalid()
    return Refusal(status, stage, artifact, role, resource, *counters)


def prepare_socket(channel):
    """Enable per-message Linux credentials BEFORE the child can send HELLO."""
    if mode() == "unmanaged":
        return
    if (channel.family != socket.AF_UNIX
            or channel.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) != socket.SOCK_SEQPACKET
            or not hasattr(socket, "SO_PASSCRED")):
        raise _invalid(Status.CONTAINMENT_UNAVAILABLE, Setting.CHANNEL)
    channel.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)


def serve(ledger, channel, owner):
    """Serve a host-registered actor. A bad frame/credential terminates the channel."""
    if mode() == "unmanaged":
        return
    prepare_socket(channel)
    sequence, greeted = 0, False
    tickets = {}
    try:
        while True:
            data, controls, flags, _ = channel.recvmsg(WIRE_SIZE + 1, socket.CMSG_SPACE(_CREDENTIALS.size))
            if not data:
                return
            request = Message.make(Opcode.HELLO)
            try:
                if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
                    raise _invalid()
                request = Message.decode(data)
                opcode = _enum(Opcode, request.opcode)
                credentials = [value for level, kind, value in controls
                               if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS]
                if (len(credentials) != 1 or len(credentials[0]) != _CREDENTIALS.size
                        or _CREDENTIALS.unpack(credentials[0]) != (owner.pid, owner.uid, owner.gid)):
                    raise _invalid(Status.PROTOCOL, Setting.CHANNEL)
                if request.sequence != sequence + 1 or request[Slot.ROLE] != owner.role:
                    raise _invalid()
                if any(value and Slot(index) not in _ALLOWED[opcode]
                       for index, value in enumerate(request.slots)):
                    raise _invalid()
                sequence = request.sequence
                if opcode == Opcode.HELLO:
                    if greeted:
                        raise _invalid()
                    with ledger._lock:
                        ledger._owner(owner, growth=True)
                    greeted = True
                    response = Message.make(
                        opcode | REPLY, sequence, nonce_hi=ledger.nonce[0], nonce_lo=ledger.nonce[1],
                        generation=owner.number, role=owner.role, pid=owner.pid,
                        soft_nofile=owner.soft_nofile)
                else:
                    if not greeted or (request[Slot.NONCE_HI], request[Slot.NONCE_LO]) != ledger.nonce:
                        raise _invalid()
                    common = dict(object_id=request[Slot.OBJECT], generation=request[Slot.GENERATION],
                                  old_extent=request[Slot.OLD_EXTENT], new_extent=request[Slot.NEW_EXTENT],
                                  stage=request[Slot.STAGE], artifact=request[Slot.ARTIFACT],
                                  device=request[Slot.DEVICE], inode=request[Slot.INODE])
                    amounts = Amounts(*(request[slot] for slot in (
                        Slot.DELTA_DISK, Slot.DELTA_FILES, Slot.DELTA_OUTPUT, Slot.DELTA_FDS)))
                    extra = {}
                    if opcode == Opcode.RESERVE:
                        ticket = ledger.reserve(owner, amounts, **common)
                        tickets[ticket.number] = ticket
                        extra["ticket"] = ticket.number
                    elif opcode in (Opcode.COMMIT, Opcode.CANCEL):
                        number = request[Slot.TICKET]
                        ticket = tickets.get(number)
                        if ticket is None:
                            raise _invalid()
                        if opcode == Opcode.COMMIT:
                            ticket.commit(amounts, request[Slot.NEW_EXTENT],
                                          device=request[Slot.DEVICE], inode=request[Slot.INODE])
                        else:
                            ticket.cancel()
                            del tickets[number]
                    elif opcode == Opcode.RELEASE:
                        ledger.release(owner, amounts, **common)
                    elif opcode in (Opcode.SNAPSHOT, Opcode.FINISH):
                        resource = _enum(Resource, request[Slot.RESOURCE])
                        usage = (ledger.finish(owner) if opcode == Opcode.FINISH else ledger.snapshot())[resource - 1]
                        if opcode == Opcode.FINISH and ledger.refusal is not None:
                            raise QuotaError(ledger.refusal)
                        extra.update(resource=resource, committed=usage.committed, reserved=usage.reserved,
                                     used=usage.used, peak=usage.peak, limit=usage.limit)
                    response = Message.make(opcode | REPLY, sequence,
                                            nonce_hi=ledger.nonce[0], nonce_lo=ledger.nonce[1], **extra)
                channel.sendall(response.encode())
            except QuotaError as error:
                with ledger._lock:
                    if ledger._refusal is None:
                        ledger._refusal = error.snapshot
                    refusal = ledger._refusal
                channel.sendall(_failure_reply(request, refusal, ledger.nonce).encode())
                if error.snapshot.status in (Status.PROTOCOL, Status.CONTAINMENT_UNAVAILABLE):
                    return
    except (OSError, ConnectionError):
        return
    finally:
        ledger.disconnect(owner)
        channel.close()


class QuotaClient:
    """Python endpoint of the same host ledger the native client uses."""
    def __new__(cls, channel, *, role=Role.WORKER):
        if mode() == "unmanaged":
            return _UNMANAGED_CLIENT
        return super().__new__(cls)

    def __init__(self, channel, *, role=Role.WORKER):
        _validate_host_marker()
        if (channel.family != socket.AF_UNIX
                or channel.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) != socket.SOCK_SEQPACKET):
            raise _invalid(Status.PROTOCOL, Setting.CHANNEL)
        self.channel, self.role = channel, _enum(Role, role)
        self._lock, self._sequence, self._nonce = threading.RLock(), 0, (0, 0)
        self._refusal = None
        self._finished = False
        reply = self._exchange(Opcode.HELLO, role=self.role)
        self._nonce = (reply[Slot.NONCE_HI], reply[Slot.NONCE_LO])
        if (not any(self._nonce) or not reply[Slot.GENERATION]
                or reply[Slot.PID] != os.getpid() or reply[Slot.ROLE] != self.role):
            raise _invalid(Status.PROTOCOL, Setting.CHANNEL)
        self.owner = reply[Slot.GENERATION]

    @classmethod
    def from_environment(cls, *, role=Role.WORKER):
        if mode() == "unmanaged":
            return _UNMANAGED_CLIENT
        _validate_host_marker()
        channel = None
        try:
            # A non-inheritable duplicate survives accidental close/reuse of
            # the original number. Arbitrary in-process close still requires
            # host containment; descriptor flags cannot prevent it.
            channel = _duplicate_channel(_HOST_MARKER.descriptors[1])
            metadata = os.fstat(channel.fileno())
            if (metadata.st_dev, metadata.st_ino) != _HOST_MARKER.identities[1]:
                raise ValueError("inherited channel replaced during attachment")
            return cls(channel, role=role)
        except QuotaError:
            if channel is not None:
                channel.close()
            raise
        except (OSError, ValueError):
            if channel is not None:
                channel.close()
            raise _invalid(Status.PROTOCOL, Setting.CHANNEL) from None

    @property
    def refusal(self):
        with self._lock:
            return self._refusal

    def check(self):
        with self._lock:
            if self._refusal is not None:
                raise QuotaError(self._refusal)
            if self._finished:
                self._refusal = Refusal(Status.PROTOCOL, resource=Setting.CHANNEL)
                raise QuotaError(self._refusal)

    def _validate_mutation(self, amounts, fields, stage=Stage.EXECUTION):
        with self._lock:
            try:
                stage = _enum(Stage, fields.get("stage", stage))
                amounts.validate(stage)
                if "artifact" in fields:
                    _enum(Artifact, fields["artifact"])
                for name, value in fields.items():
                    if name not in ("object", "generation", "old_extent", "new_extent",
                                    "stage", "artifact", "device", "inode"):
                        raise _invalid()
                    if name in ("old_extent", "new_extent"):
                        _counter(value, Setting.SCRATCH_BYTES, stage)
                    elif name not in ("stage", "artifact"):
                        _identity(value)
            except QuotaError as error:
                if self._refusal is None:
                    self._refusal = error.snapshot
                raise QuotaError(self._refusal) from None

    def _exchange(self, opcode, **fields):
        with self._lock:
            if opcode == Opcode.RESERVE:
                self.check()
            if self._sequence == MAX_ID:
                if self._refusal is None:
                    self._refusal = Refusal(Status.COUNTER_OVERFLOW, resource=Setting.PROTOCOL)
                raise QuotaError(self._refusal)
            self._sequence += 1
            if opcode != Opcode.HELLO:
                fields.update(nonce_hi=self._nonce[0], nonce_lo=self._nonce[1], role=self.role)
            try:
                request = Message.make(opcode, self._sequence, **fields)
                self.channel.sendall(request.encode())
                data, _, flags, _ = self.channel.recvmsg(WIRE_SIZE + 1)
                if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
                    raise _invalid()
                response = Message.decode(data)
                if response.opcode != opcode | REPLY or response.sequence != self._sequence:
                    raise _invalid()
                if opcode != Opcode.HELLO and (
                        response[Slot.NONCE_HI], response[Slot.NONCE_LO]) != self._nonce:
                    raise _invalid()
                allowed = _REPLY_COMMON | (
                    _FAILURE_FIELDS if response[Slot.STATUS] else _REPLY_ALLOWED[opcode])
                if any(value and Slot(index) not in allowed for index, value in enumerate(response.slots)):
                    raise _invalid()
                if opcode in (Opcode.SNAPSHOT, Opcode.FINISH) and not response[Slot.STATUS]:
                    if response[Slot.RESOURCE] != fields["resource"]:
                        raise _invalid()
                    _usage_from_message(response)
                _counter(response[Slot.SOFT_NOFILE], Setting.DESCRIPTORS)
                refusal = refusal_from_message(response)
                if refusal is not None:
                    raise QuotaError(refusal)
                return response
            except (OSError, QuotaError) as error:
                refusal = error.snapshot if isinstance(error, QuotaError) else Refusal(
                    Status.PROTOCOL, resource=Setting.CHANNEL)
                if opcode == Opcode.HELLO and refusal.status == Status.PROTOCOL:
                    refusal = Refusal(Status.PROTOCOL, resource=Setting.CHANNEL)
                if self._refusal is None:
                    self._refusal = refusal
                raise QuotaError(self._refusal) from None

    def reserve(self, amounts, **fields):
        fields = dict(fields)
        if "object_id" in fields:
            fields["object"] = fields.pop("object_id")
        fields.setdefault("stage", Stage.EXECUTION)
        fields.setdefault("artifact", Artifact.TABLE)
        self._validate_mutation(amounts, fields)
        fields.update(zip(("delta_disk", "delta_files", "delta_output", "delta_fds"), amounts.values()))
        response = self._exchange(Opcode.RESERVE, **fields)
        if not response[Slot.TICKET]:
            if self._refusal is None:
                self._refusal = Refusal(Status.PROTOCOL, resource=Setting.PROTOCOL)
            raise QuotaError(self._refusal)
        return Ticket(self, self.owner, response[Slot.TICKET], request=fields)

    def commit(self, ticket, realized, actual_extent, *, device=0, inode=0):
        if ticket.endpoint is not self or ticket.owner != self.owner or not ticket.number:
            raise _invalid()
        self._validate_mutation(realized, dict(new_extent=actual_extent, device=device, inode=inode),
                                stage=ticket.request["stage"])
        fields = dict(zip(("delta_disk", "delta_files", "delta_output", "delta_fds"), realized.values()))
        self._exchange(Opcode.COMMIT, ticket=ticket.number, new_extent=actual_extent,
                       device=device, inode=inode, **fields)

    def cancel(self, ticket):
        if ticket.endpoint is not self or ticket.owner != self.owner or not ticket.number:
            raise _invalid()
        self._exchange(Opcode.CANCEL, ticket=ticket.number)
        ticket.number = 0

    def release(self, amounts, **fields):
        fields = dict(fields)
        if "object_id" in fields:
            fields["object"] = fields.pop("object_id")
        fields.setdefault("stage", Stage.EXECUTION)
        fields.setdefault("artifact", Artifact.TABLE)
        self._validate_mutation(amounts, fields)
        fields.update(zip(("delta_disk", "delta_files", "delta_output", "delta_fds"), amounts.values()))
        self._exchange(Opcode.RELEASE, **fields)

    def snapshot(self, resource=Resource.SCRATCH_BYTES):
        reply = self._exchange(Opcode.SNAPSHOT, resource=_enum(Resource, resource))
        return _usage_from_message(reply)

    def finish(self, resource=Resource.SCRATCH_BYTES):
        self._finished = True
        reply = self._exchange(Opcode.FINISH, resource=_enum(Resource, resource))
        if self._refusal is not None:
            raise QuotaError(self._refusal)
        return _usage_from_message(reply)

    def close(self):
        self.channel.close()
