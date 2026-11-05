"""Run native ownership rows against the shipped quota ledger and service.

Scratch COMMITs use the real-inode observer pattern in test_quota_native.cpp.
Requests and production replies pass through unchanged.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import resource
import signal
import socket
import stat
import subprocess
import sys
import threading
import traceback


MARKERS = ("PF_RUN_QUOTA_MODE", "PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD")
OUTPUT_LIMITS = (9, 999, 999999)
CAPACITIES = (1, 17, 257)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def refuse_path(path):
    parts = path.parts
    require(not any(part in (".ssh", ".aws", ".codex") for part in parts),
            f"refusing protected path: {path}")
    text = path.as_posix()
    require("/.config/zsh/keys/" not in text + "/"
            and "/.config/pineforge/" not in text + "/"
            and not any(part.endswith(".env") for part in parts),
            f"refusing protected path: {path}")


def checked_path(value):
    refuse_path(Path(value))
    path = Path(os.path.abspath(value))
    refuse_path(path)
    # Inspect each symlink's target name before traversing it. In particular,
    # resolving a harmless-looking alias must not enter a protected directory.
    pending = list(path.parts[1:])
    path = Path(path.anchor)
    links = 0
    while pending:
        part = pending.pop(0)
        if part == "..":
            path = path.parent
            continue
        candidate = path / part
        refuse_path(candidate)
        if candidate.is_symlink():
            links += 1
            require(links <= 40, f"too many symbolic links: {candidate}")
            target = Path(os.readlink(candidate))
            refuse_path(target)
            if target.is_absolute():
                path = Path(target.anchor)
                pending = list(target.parts[1:]) + pending
            else:
                pending = list(target.parts) + pending
        else:
            path = candidate
    return path


def checked_file(value):
    path = checked_path(value)
    require(path.is_file(), f"required file is absent: {path}")
    return path


def announce_files(*paths):
    print("managed driver follows these files:", file=sys.stderr, flush=True)
    for path in paths:
        print(f"  {path}", file=sys.stderr, flush=True)


def child_status(returncode):
    # Preserve ordinary exit codes; expose both the raw signal and shell status.
    print(f"managed driver child returncode={returncode}", file=sys.stderr, flush=True)
    return returncode if returncode >= 0 else 128 - returncode


def crash_control():
    script = checked_file(__file__)
    interpreter = checked_file(sys.executable)
    announce_files(script, interpreter)
    child = subprocess.Popen([str(interpreter), "-I", "-B", str(script), "--crash-child"])
    try:
        raw = child.wait()
    finally:
        if child.poll() is None:
            child.kill()
        child.wait()
    status = child_status(raw)
    require(raw == -signal.SIGKILL and status == 128 + signal.SIGKILL,
            f"deliberate crash was not propagated: raw={raw}, status={status}")
    # This is a driver lifecycle control, never a native ownership verdict.
    return 0


class ServiceSocket:
    """Forward unchanged socket calls and retain errors serve() would consume."""

    def __init__(self, channel, fail):
        self.channel = channel
        self.fail = fail
        self.received = 0
        self.sent = 0

    def __getattr__(self, name):
        return getattr(self.channel, name)

    def recvmsg(self, *args):
        try:
            result = self.channel.recvmsg(*args)
            if result[0]:
                self.received += 1
            return result
        except BaseException as error:
            self.fail(error)
            raise

    def sendall(self, data):
        try:
            result = self.channel.sendall(data)
            self.sent += 1
            return result
        except BaseException as error:
            self.fail(error)
            raise


class FirstDatagramSocket:
    """Forward an already received marker tuple once, then actual traffic."""

    def __init__(self, channel, first, q):
        self.channel, self.first, self.q = channel, first, q
        self.finished = False

    def __getattr__(self, name):
        return getattr(self.channel, name)

    def recvmsg(self, *args):
        if self.first is not None:
            result, self.first = self.first, None
        else:
            result = self.channel.recvmsg(*args)
        if not result[0]:
            require(not result[1] and not result[2] and result[3] is None,
                    "Python marker received an empty datagram with metadata")
        # The underlying ServiceSocket counted the actual receive exactly once.
        return result

    def sendall(self, data):
        # Send only the unchanged production reply, including a real refusal.
        result = self.channel.sendall(data)
        reply = self.q.Message.decode(data)
        refusal = self.q.refusal_from_message(reply)
        if refusal is not None:
            # An earlier scratch refusal must not hide a bad marker exchange.
            self.channel.fail(self.q.QuotaError(refusal))
        elif reply.opcode == self.q.Opcode.FINISH | self.q.REPLY:
            self.finished = True
        return result


class ObservedSocket(ServiceSocket):
    """Confirm actual scratch backing fds before unchanged production COMMIT."""

    def __init__(self, channel, fail, q, ledger, actor, directory, block_size):
        super().__init__(channel, fail)
        self.q, self.ledger, self.actor = q, ledger, actor
        self.directory, self.block_size = directory, block_size
        self.requests, self.tickets = {}, {}
        self.confirmations = self.commits = self.opened = self.closed = 0
        self.objects, self.extents = set(), set()

    def confirm(self, request, controls, flags):
        q = self.q
        require(not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC),
                "scratch COMMIT was truncated")
        credentials = [value for level, kind, value in controls
                       if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS]
        require(len(credentials) == 1 and len(credentials[0]) == q._CREDENTIALS.size
                and q._credentials_match(credentials[0], self.actor),
                "scratch COMMIT credentials do not identify the actual actor")
        require(request[q.Slot.ROLE] == self.actor.role
                and (request[q.Slot.NONCE_HI], request[q.Slot.NONCE_LO]) == self.ledger.nonce,
                "scratch COMMIT owner/nonce changed")
        original = self.tickets[request[q.Slot.TICKET]]
        object_id, generation = original[q.Slot.OBJECT], original[q.Slot.GENERATION]
        require(object_id == self.actor.pid and generation > 0,
                "scratch reservation does not identify an actual actor object")
        identity = request[q.Slot.DEVICE], request[q.Slot.INODE]
        require(identity[1] != 0, "scratch COMMIT omitted its inode")
        root = Path(f"/proc/{self.actor.pid}/fd")
        announce_files(root)
        # This is only the registered actor's fd table. No discovery subprocess
        # or traversal of any other process's descriptors is involved.
        entries = [root / name for name in os.listdir(root)]
        announce_files(*entries)
        for entry in entries:
            require(entry.name.isascii() and entry.name.isdecimal(),
                    f"unexpected actor descriptor name: {entry}")
            target = os.readlink(entry)
            # Linux appends this suffix to anonymous backing names. Check the
            # underlying name before any stat/open follows the proc magic link.
            target = target.removesuffix(" (deleted)")
            refuse_path(Path(target))
            target = Path(os.path.normpath(target))
            refuse_path(target)
            if not target.is_absolute() or self.directory not in target.parents:
                continue
            announce_files(target)
            metadata = os.stat(entry)
            if (metadata.st_dev, metadata.st_ino) != identity:
                continue
            require(stat.S_ISREG(metadata.st_mode), "scratch backing is not a regular file")
            descriptor = os.open(entry, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK)
            self.opened += 1
            try:
                metadata = os.fstat(descriptor)
                require(stat.S_ISREG(metadata.st_mode)
                        and (metadata.st_dev, metadata.st_ino) == identity,
                        "opened scratch descriptor identity changed")
                geometry = os.fstatvfs(descriptor)
                require(geometry.f_bsize == self.block_size
                        and q.rounded_extent(metadata.st_size, geometry.f_bsize)
                        == request[q.Slot.NEW_EXTENT],
                        "scratch COMMIT extent differs from its actual backing")
                confirmed = self.ledger.confirm_object(
                    self.actor, object_id, generation, descriptor)
                require(confirmed == identity, "production inode confirmation mismatch")
                after = os.fstat(descriptor)
                require((after.st_dev, after.st_ino, after.st_size)
                        == (metadata.st_dev, metadata.st_ino, metadata.st_size),
                        "scratch backing changed during confirmation")
                self.confirmations += 1
                self.objects.add((object_id, generation))
                self.extents.add(metadata.st_size)
                return
            finally:
                os.close(descriptor)
                self.closed += 1
        raise RuntimeError("host could not open the actual admitted scratch backing")

    def recvmsg(self, *args):
        try:
            result = super().recvmsg(*args)
            if result[0]:
                request = self.q.Message.decode(result[0])
                require(request.sequence not in self.requests, "duplicate pending scratch request")
                self.requests[request.sequence] = request
                if request.opcode == self.q.Opcode.COMMIT:
                    self.confirm(request, result[1], result[2])
            return result
        except BaseException as error:
            self.fail(error)
            raise

    def sendall(self, data):
        try:
            q = self.q
            reply = q.Message.decode(data)
            request = self.requests.pop(reply.sequence)
            require(reply.opcode == request.opcode | q.REPLY, "scratch reply opcode changed")
            if reply[q.Slot.STATUS] == q.Status.OK:
                if request.opcode == q.Opcode.RESERVE:
                    number = reply[q.Slot.TICKET]
                    require(number != 0 and number not in self.tickets,
                            "production scratch reservation has an invalid ticket")
                    self.tickets[number] = request
                elif request.opcode == q.Opcode.CANCEL:
                    self.tickets.pop(request[q.Slot.TICKET])
                elif request.opcode == q.Opcode.COMMIT:
                    self.commits += 1
            # Only production serve() constructs the reply. Never re-encode it.
            return super().sendall(data)
        except BaseException as error:
            self.fail(error)
            raise


def run_managed(binary, mode):
    script = checked_file(__file__)
    module_path = checked_file(script.parent.parent / "docker" / "run_quota.py")
    binary = checked_file(binary)
    announce_files(script, module_path, binary)
    original_environment = dict(os.environ)
    pairs, ledgers, services, threads, errors = [], [], [], [], []
    child = None
    returncode = None
    directory = geometry = None
    error_lock = threading.Lock()

    def fail(error):
        with error_lock:
            errors.append(error)
        if child is not None and child.poll() is None:
            child.kill()

    def cleanup(action):
        try:
            return action()
        except BaseException as error:
            with error_lock:
                errors.append(error)

    try:
        if mode == "scratch":
            scratch = checked_path(script.parent.parent / ".proof-scratch")
            announce_files(scratch)
            require(scratch == script.parent.parent / ".proof-scratch",
                    "scratch directory escaped the worktree")
            scratch.mkdir(mode=0o700, exist_ok=True)
            require(scratch.is_dir(), f"required scratch directory is absent: {scratch}")
            directory = scratch / ("runtime-context-managed-" + os.urandom(16).hex())
            announce_files(directory)
            os.mkdir(directory, mode=0o700)
            # Retain this directory on every exit; no TemporaryDirectory cleanup.
            geometry = os.statvfs(directory)
            require(geometry.f_bsize > 0, "scratch filesystem has no block size")
        # Anchor, Python marker, and (for endpoint mode) three independent
        # native endpoints. Every marker names a real inherited socket.
        for _ in range(5 if mode == "endpoint" else 2):
            pairs.append(socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET))
        identities = [os.fstat(client.fileno()) for _, client in pairs]
        require(len({(item.st_dev, item.st_ino) for item in identities}) == len(pairs),
                "managed channels alias")
        bindings = dict(zip(MARKERS, ("managed", str(pairs[0][1].fileno()),
                                     str(pairs[1][1].fileno()))))
        try:
            os.environ.update(bindings)
            name = "_runtime_context_production_quota"
            spec = importlib.util.spec_from_file_location(name, module_path)
            require(spec is not None and spec.loader is not None, "quota import has no loader")
            q = importlib.util.module_from_spec(spec)
            sys.modules[name] = q
            spec.loader.exec_module(q)
            require(q.mode() == "managed" and tuple(q.MARKER_ENV) == MARKERS,
                    "production quota marker was not captured")
            soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
            require(0 <= soft <= q.MAX_COUNTER, "finite descriptor allowance is required")
            limits = (4096, *OUTPUT_LIMITS) if mode == "endpoint" else (4096,)
            for output_limit in limits:
                scratch_bytes = (2 * q.rounded_extent(1048576, geometry.f_bsize)
                                 if mode == "scratch" else 1048576)
                ledgers.append(q.RunQuota(q.Amounts(
                    scratch_bytes, 2 if mode == "scratch" else 64, output_limit, soft)))
        finally:
            for name in MARKERS:
                if name in original_environment:
                    os.environ[name] = original_environment[name]
                else:
                    os.environ.pop(name, None)

        # Credentials must be enabled before the child can send its first frame.
        for server, _ in pairs:
            q.prepare_socket(server)
        environment = dict(original_environment)
        environment.update(bindings)
        argv = [str(binary)]
        if mode == "endpoint":
            argv += ["--managed-quota-endpoint-observation"]
            argv += [str(client.fileno()) for _, client in pairs[2:]]
        elif mode == "scratch":
            argv += ["--managed-scratch-observation"]
        else:
            argv += ["--managed-endpoint-observer-race"]
        print(f"native argv: {argv!r}", file=sys.stderr, flush=True)
        # Inherit stdout/stderr directly; do not parse or replace native output.
        child = subprocess.Popen(argv, env=environment, cwd=directory,
                                 pass_fds=tuple(client.fileno() for _, client in pairs))
        announce_files(Path(f"/proc/{child.pid}/stat"),
                       Path("/proc/sys/kernel/random/boot_id"))
        pid, generation, _ = q._proc_generation(child.pid)
        child_soft, _ = resource.prlimit(pid, resource.RLIMIT_NOFILE)
        require(child_soft == soft and generation > 0, "child identity/limit changed")
        channel_ledgers = [ledgers[0], ledgers[0], *ledgers[1:]]
        owners = [
            (None if index == 1 else
             ledger.open_owner(pid=pid, uid=os.getuid(), gid=os.getgid(),
                               generation=generation, role=q.Role.WORKER,
                               soft_nofile=child_soft))
            for index, ledger in enumerate(channel_ledgers)
        ]

        def serve(ledger, channel, owner):
            try:
                optional_marker = owner is None
                if optional_marker:
                    # prepare_socket ran before launch. Read with production's
                    # exact sizes; an unused marker has no protocol owner.
                    first = channel.recvmsg(
                        q.WIRE_SIZE + 1, socket.CMSG_SPACE(q._CREDENTIALS.size))
                    if not first[0]:
                        require(not first[1] and not first[2] and first[3] is None,
                                "unused Python marker did not close with clean EOF")
                        return
                    owner = ledger.open_owner(
                        pid=pid, uid=os.getuid(), gid=os.getgid(),
                        generation=generation, role=q.Role.WORKER,
                        soft_nofile=child_soft)
                    channel = FirstDatagramSocket(channel, first, q)
                q.serve(ledger, channel, owner)
                if optional_marker:
                    require(channel.finished,
                            "used Python marker disconnected without a successful FINISH")
            except BaseException as error:
                fail(error)

        for index, ((server, _), ledger, owner) in enumerate(zip(pairs, channel_ledgers, owners)):
            channel = (ObservedSocket(server, fail, q, ledger, owner, directory, geometry.f_bsize)
                       if mode == "scratch" and index == 0 else ServiceSocket(server, fail))
            services.append(channel)
            thread = threading.Thread(target=serve, args=(ledger, channel, owner))
            thread.start()
            threads.append(thread)
        for _, client in pairs:
            client.close()
        returncode = child.wait()
        for thread in threads:
            thread.join()

        if returncode == 0 and not errors:
            # The native TU owns the detailed observation/census verdict.
            # Independently require real host traffic and stateful accounting.
            native_services = [services[0], *services[2:]]
            require(all(item.received > 0 and item.sent == item.received
                        for item in native_services), "required host channel was unused/incomplete")
            if mode != "scratch":
                require(ledgers[0].refusal is None, "clean anchor acquired a host refusal")
            for ledger in ledgers:
                require(all(usage.committed == usage.reserved == 0
                            for usage in ledger.snapshot()), "host retained charged resources")
            if mode == "scratch":
                observed = services[0]
                require(observed.confirmations > 0
                        and observed.commits == observed.confirmations
                        and observed.opened == observed.closed == observed.confirmations
                        and len(observed.objects) > 1 and len(observed.extents) > 2
                        and not observed.requests and not observed.tickets,
                        "real backing confirmation/descriptor lifecycle was incomplete")
                usage = ledgers[0].snapshot()
                require(usage[q.Resource.SCRATCH_BYTES - 1].peak > 0
                        and usage[q.Resource.SCRATCH_FILES - 1].peak == 2,
                        "scratch host did not account for simultaneous real backings")
                refusal = ledgers[0].refusal
                output = usage[q.Resource.OUTPUT_BYTES - 1]
                require(refusal is not None and refusal.status == q.Status.RESOURCE_EXCEEDED
                        and refusal.resource == q.Resource.OUTPUT_BYTES
                        and refusal.stage == q.Stage.EXECUTION and refusal.artifact == q.Artifact.JSON
                        and refusal.limit == output.limit and refusal.requested == output.limit + 1
                        and refusal.used == refusal.peak == output.peak == 0,
                        "scratch row did not retain its real output refusal")
                announce_files(directory)
                require(not os.listdir(directory), "scratch actor left named backing residue")
            if mode == "endpoint":
                for ledger, limit, capacity in zip(ledgers[1:], OUTPUT_LIMITS, CAPACITIES):
                    refusal = ledger.refusal
                    require(refusal is not None
                            and refusal.status == q.Status.RESOURCE_EXCEEDED
                            and refusal.resource == q.Resource.OUTPUT_BYTES
                            and refusal.limit == limit and refusal.requested == limit + 1
                            and refusal.used == 0 and refusal.peak == capacity,
                            f"real endpoint refusal/accounting mismatch for limit {limit}")
    except BaseException as error:
        # Retain setup/assertion errors along with any concurrently failing
        # server and the child's own status; cleanup must not overwrite either.
        with error_lock:
            errors.append(error)
    finally:
        # Close our copies before joining, including failure during registration.
        for _, client in pairs:
            cleanup(client.close)
        if child is not None:
            if child.poll() is None:
                cleanup(child.kill)
            reaped = cleanup(child.wait)
            if reaped is not None:
                returncode = reaped
        for thread in threads:
            cleanup(thread.join)
        for server, _ in pairs:
            cleanup(server.close)
        for ledger in ledgers:
            cleanup(ledger.close)
        for error in errors:
            traceback.print_exception(type(error), error, error.__traceback__)
    if returncode is None:
        print("managed driver has no child exit status", file=sys.stderr, flush=True)
        return 1
    status = child_status(returncode)
    return status if status else int(bool(errors))


def main():
    if sys.argv[1:] == ["--crash-child"]:
        os.kill(os.getpid(), signal.SIGKILL)
        raise RuntimeError("SIGKILL did not terminate the crash-control child")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", required=True, choices=("endpoint", "race", "scratch", "crash-control"))
    parser.add_argument("--binary")
    args = parser.parse_args()
    if args.mode == "crash-control":
        require(args.binary is None, "crash control does not substitute a native binary")
        return crash_control()
    require(args.binary is not None, "a real native binary path is required")
    return run_managed(args.binary, args.mode)


if __name__ == "__main__":
    sys.exit(main())
