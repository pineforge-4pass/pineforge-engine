"""Quota accounting, process generations, and native/Python transport."""
from __future__ import annotations

import argparse
import ast
from contextlib import ExitStack, contextmanager
from dataclasses import FrozenInstanceError
import importlib
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading

import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
CAP = (1 << 53) - 1
ENV_LIMITS = ("PF_RUN_SCRATCH_BYTES", "PF_RUN_SCRATCH_FILES",
              "PF_RUN_OUTPUT_BYTES", "PF_RUN_DESCRIPTOR_LIMIT")
ENV_MARKER = ("PF_RUN_QUOTA_MODE", "PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD")
FRAME = struct.Struct("<4sHHQ30Q")


def load_environment_module(monkeypatch):
    # A fresh host load reads the actual environment. No internal mode flag or
    # descriptor/liveness function is stubbed to admit the test.
    name = "quota_environment_case"
    spec = importlib.util.spec_from_file_location(name, ROOT / "docker" / "run_quota.py")
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, name, module)
    spec.loader.exec_module(module)
    return module


@contextmanager
def marker_channels(monkeypatch):
    native_host, native_child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    python_host, python_child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        monkeypatch.setenv(ENV_MARKER[0], "managed")
        monkeypatch.setenv(ENV_MARKER[1], str(native_child.fileno()))
        monkeypatch.setenv(ENV_MARKER[2], str(python_child.fileno()))
        yield native_host, native_child, python_host, python_child
    finally:
        native_host.close()
        native_child.close()
        python_host.close()
        python_child.close()


@pytest.fixture
def q(monkeypatch):
    with marker_channels(monkeypatch):
        yield load_environment_module(monkeypatch)


@pytest.fixture
def native_program():
    path = Path(os.environ["PF_QUOTA_TEST_NATIVE"]).resolve()
    assert path.is_file(), f"native quota test executable is absent: {path}"
    return str(path)


def owner(q, ledger, *, pid=None, uid=None, gid=None, generation=1, soft=64, role=None):
    return ledger.open_owner(pid=os.getpid() if pid is None else pid,
                             uid=os.getuid() if uid is None else uid,
                             gid=os.getgid() if gid is None else gid, generation=generation,
                             role=q.Role.WORKER if role is None else role, soft_nofile=soft)


@contextmanager
def waiting_process():
    process = subprocess.Popen([sys.executable, "-B", "-c",
                                "import sys; sys.stdin.buffer.read()"], stdin=subprocess.PIPE)
    try:
        yield process
    finally:
        process.stdin.close()
        assert process.wait(timeout=10) == 0


def start_server(q, ledger, channel, actor):
    errors = []

    def run():
        try:
            q.serve(ledger, channel, actor)
        except BaseException as error:
            errors.append(error)
            channel.close()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, errors


@contextmanager
def python_client(q, ledger, *, soft=64):
    server, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    q.prepare_socket(server)
    actor = owner(q, ledger, soft=soft)
    thread, errors = start_server(q, ledger, server, actor)
    client = None
    try:
        client = q.QuotaClient(child)
        yield client, actor
    finally:
        child.close()
        thread.join()
        assert not errors, errors


def captured_native_stderr(output):
    # Test support only. pread leaves the child's shared file offset alone and
    # never waits for EOF when admission exits before sending HELLO.
    return os.pread(output.fileno(), os.fstat(output.fileno()).st_size, 0).decode(
        "utf-8", errors="replace")


def assert_native_diagnostics(stderr, refusals, *, code=None):
    lines = stderr.splitlines()
    assert not stderr or stderr.endswith("\n"), stderr
    if code is not None:
        assert lines and lines[0] == code, stderr
        lines = lines[1:]
    prefix = "admission_diagnostic: "
    assert len(lines) == refusals, stderr
    assert all(line.startswith(prefix) and len(line) > len(prefix) for line in lines), stderr


def assert_marker_capture_diagnostic(stderr, environment):
    # Verify the child's retained capture, not values read later by its driver.
    assert " diagnostic_revision=6 " in stderr, stderr
    assert " call=from_fd " in stderr, stderr
    assert " marker_source=cache " in stderr, stderr

    def number(name):
        match = re.search(rf"(?:^| ){name}=(-?[0-9]+)(?= |$)", stderr.rstrip("\n"))
        assert match, stderr
        return int(match[1])

    assert number("record") >= 2, stderr  # Fresh refusal, then this cache replay.
    assert number("pid") == number("capture_pid") > 0, stderr
    assert number("capture_count") == number("marker_capture") == 1, stderr
    assert number("marker_captured") == 1, stderr
    present = truncated = 0
    for index, (name, field) in enumerate(zip(
            ENV_MARKER, ("env_mode", "env_native_fd", "env_python_fd"))):
        raw = environment.get(name)
        if raw is None:
            expected = "<unset>"
        else:
            encoded = os.fsencode(raw)
            expected = encoded[:32].decode("latin-1")
            present |= 1 << index
            if len(encoded) > 32:
                truncated |= 1 << index
        match = re.search(rf' {field}=("(?:[^"\\]|\\.)*")(?= |$)', stderr.rstrip("\n"))
        assert match, stderr
        assert ast.literal_eval(match[1]) == expected, stderr
        if index and raw and raw.isascii() and raw.isdecimal() and 3 <= int(raw) <= 2147483647:
            assert number(("marker_native_fd", "marker_python_fd")[index - 1]) == int(raw), stderr
    assert number("env_present") == present, stderr
    assert number("env_truncated") == truncated, stderr
    if stderr.startswith("admission_diagnostic: marker_fds_same_number "):
        assert number("native_fd") == number("python_fd") >= 3, stderr


def receive_native_frame(channel, stderr_output):
    try:
        frame = channel.recv(FRAME.size + 1)
    except OSError as error:
        raise AssertionError(
            f"native frame receive failed: {error}\n{captured_native_stderr(stderr_output)}") from error
    assert len(frame) == FRAME.size, (
        f"native frame has {len(frame)} bytes, expected {FRAME.size}\n"
        f"{captured_native_stderr(stderr_output)}")
    return frame


class Native:
    def __init__(self, program, descriptors):
        # The second marker channel is distinct and inherited even when this
        # driver exercises only the native endpoint.
        peer, marker = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        environment = dict(os.environ, PF_RUN_QUOTA_MODE="managed",
                           PF_RUN_QUOTA_NATIVE_FD=str(descriptors[0]),
                           PF_RUN_QUOTA_PYTHON_FD=str(marker.fileno()))
        self.stderr_output = tempfile.TemporaryFile()
        self._stderr = ""
        self.refusals = 0
        try:
            self.process = subprocess.Popen(
                [program, "--client"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=self.stderr_output, text=True, env=environment,
                pass_fds=(*descriptors, marker.fileno()))
        except BaseException:
            self.stderr_output.close()
            peer.close()
            raise
        finally:
            marker.close()
        self.marker_peer = peer

    @property
    def stderr(self):
        return self._stderr if self.stderr_output.closed else captured_native_stderr(self.stderr_output)

    def receive(self):
        line = self.process.stdout.readline()
        assert line, f"native client closed its protocol stream: {self.process.poll()}\n{self.stderr}"
        try:
            result = json.loads(line)
        except ValueError as error:
            raise AssertionError(f"invalid native result: {line!r}\n{self.stderr}") from error
        if "code" in result:
            self.refusals += 1
        return result

    def command(self, operation, *values):
        self.process.stdin.write(" ".join(map(str, (operation, *values))) + "\n")
        self.process.stdin.flush()
        return self.receive()

    def reserve(self, object_id, amounts, *, generation=1, old=0, new=None, device=0, inode=0):
        new = old + amounts[0] if new is None else new
        return self.command("reserve", object_id, generation if object_id else 0, device, inode,
                            old, new, *amounts, 4, 3)

    def release(self, object_id, amounts, *, generation=1, old=0, new=0, device=0, inode=0):
        return self.command("release", object_id, generation if object_id else 0, device, inode,
                            old, new, *amounts, 4, 3)

    def close(self):
        self.process.stdin.close()
        self.process.wait()
        self._stderr = self.stderr
        self.process.stdout.close()
        self.stderr_output.close()
        self.marker_peer.close()
        assert self.process.returncode == 0, self.stderr
        assert_native_diagnostics(self.stderr, self.refusals)


@contextmanager
def native_clients(q, program, *ledgers):
    pairs = [socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET) for _ in ledgers]
    for server, _ in pairs:
        q.prepare_socket(server)
    descriptors = [child.fileno() for _, child in pairs]
    native = Native(program, descriptors)
    native.channels = [server for server, _ in pairs]
    native.owners = [owner(q, ledger, pid=native.process.pid) for ledger in ledgers]
    servers = [start_server(q, ledger, server, actor)
               for ledger, (server, _), actor in zip(ledgers, pairs, native.owners)]
    for _, child in pairs:
        child.close()
    try:
        assert native.receive() == {"ready": True}, native.stderr
        yield native, descriptors
    finally:
        native.close()
        for thread, errors in servers:
            thread.join()
            assert not errors, errors


def assert_usage(usage, committed, reserved, peak, limit):
    assert (usage.committed, usage.reserved, usage.used, usage.peak, usage.limit) == (
        committed, reserved, committed + reserved, peak, limit)


def expected_refusal(resource, used, requested, limit, peak, *, stage="execution", artifact="table"):
    return {"code": "run_resource_quota_exceeded",
            "args": dict(resource=resource, stage=stage, artifact=artifact,
                         used=used, requested=requested, limit=limit, peak=peak)}


def assert_channel_refusal(module, native_program, descriptors):
    assert module.mode() == "managed"
    assert module.quota_line() == "quota=managed enforcement=cooperative"
    expected = dict(stage="admission", reason="protocol", setting="channel")
    for factory in (module.RunQuota.from_environment, module.QuotaClient.from_environment):
        with pytest.raises(module.QuotaError) as caught:
            factory()
        assert caught.value.code == "run_quota_configuration_invalid"
        assert dict(caught.value.arguments) == expected
    environment = dict(os.environ)
    result = subprocess.run([native_program, "--client"], input="", text=True,
                            capture_output=True, env=environment,
                            pass_fds=tuple(descriptors), timeout=10)
    assert result.returncode == 1, result.stderr
    assert_native_diagnostics(result.stderr, 1)
    assert_marker_capture_diagnostic(result.stderr, environment)
    assert json.loads(result.stdout) == {
        "code": "run_quota_configuration_invalid", "args": expected}, result.stderr


@pytest.mark.parametrize("limit", [None, "0", "9" * 5000])
def test_unmanaged_calls_do_not_probe_or_construct_ledger_state(monkeypatch, limit, capsys):
    for name in (*ENV_MARKER, *ENV_LIMITS):
        monkeypatch.delenv(name, raising=False)
    if limit is not None:
        for name in ENV_LIMITS:
            monkeypatch.setenv(name, limit)
    module = load_environment_module(monkeypatch)
    absurd = module.Amounts(*((1 << 200,) * 4))
    assert module.mode() == "unmanaged"
    assert module.quota_line() == "quota=unmanaged"
    # Request/strategy input, even an attempted process-environment update,
    # arrives after the host load. It cannot turn unmanaged accounting on.
    for name in ENV_MARKER:
        monkeypatch.setenv(name, "managed")

    def forbidden(*args, **kwargs):
        raise AssertionError("unmanaged accounting performed work")

    # Exercise real public calls under syscall/constructor traps, rather than
    # asserting a mode flag. Repeated results must reuse the same objects.
    with monkeypatch.context() as traps:
        for name in ("_socket_identity", "_duplicate_channel", "_proc_generation",
                     "_counter", "_invalid", "Ticket", "Usage"):
            traps.setattr(module, name, forbidden)
        traps.setattr(module.Amounts, "validate", forbidden)
        traps.setattr(module.threading, "RLock", forbidden)
        traps.setattr(module.secrets, "randbits", forbidden)
        traps.setattr(module.socket, "socket", forbidden)
        for name in ("dup", "fstat", "getpid", "getuid", "getgid", "close", "open"):
            traps.setattr(module.os, name, forbidden)
        client = module.QuotaClient.from_environment()
        ledger = module.RunQuota.from_environment()
        assert module.QuotaClient(None) is client
        assert module.RunQuota(absurd) is ledger
        assert module.QuotaClient.from_environment() is client
        assert module.RunQuota.from_environment() is ledger
        actor = ledger.open_owner(pid=0, uid=0, gid=0, generation=0, soft_nofile=0)
        ticket = client.reserve(absurd, new_extent=1 << 200)
        assert ticket is client.reserve(absurd, new_extent=1 << 200)
        assert ticket is ledger.reserve(actor, absurd, new_extent=1 << 200)
        assert ticket.number == 0
        ticket.commit(absurd, 1 << 200)
        ticket.cancel()
        client.commit(ticket, absurd, 1 << 200)
        client.cancel(ticket)
        ledger.commit(ticket, absurd, 1 << 200)
        ledger.cancel(ticket)
        client.release(absurd, old_extent=1 << 200)
        ledger.release(actor, absurd, old_extent=1 << 200)
        ledger.grant_object(actor, actor, 0, 0)
        ledger.confirm_object(actor, 0, 0, -1)
        snapshot = client.snapshot()
        assert client.finish() is snapshot
        assert client.snapshot() is snapshot
        rows = ledger.snapshot()
        assert ledger.finish(actor) is rows
        assert ledger.snapshot() is rows
        assert rows[0] is snapshot
        assert snapshot.used == snapshot.reserved == snapshot.peak == 0
        assert client.reserve(absurd) is ticket
        assert ledger.reserve(actor, absurd) is ticket
        assert client.refusal is ledger.refusal is None
        client.check()
        ledger.disconnect(actor)
        client.close()
        ledger.close()
        module.prepare_socket(None)
        module.serve(None, None, None)
        assert module.mode() == "unmanaged"
        assert module.quota_line() == "quota=unmanaged"
    assert capsys.readouterr() == ("", "")


@pytest.mark.parametrize("limit", [None, "0", "broken", str(1 << 100)])
def test_native_unmanaged_calls_have_zero_allocations(native_program, limit):
    environment = {name: value for name, value in os.environ.items()
                   if name not in (*ENV_MARKER, *ENV_LIMITS)}
    if limit is not None:
        environment.update(dict.fromkeys(ENV_LIMITS, limit))
    result = subprocess.run([native_program, "--unmanaged"], env=environment,
                            capture_output=True, timeout=10)
    stderr = result.stderr.decode("utf-8", errors="replace")
    assert result.returncode == 0, stderr
    assert result.stdout == result.stderr == b"", stderr


@pytest.mark.parametrize("present", range(1, 7))
def test_partial_marker_refuses_in_both_runtimes(monkeypatch, native_program, present):
    with marker_channels(monkeypatch) as (_, native, _, python):
        for index, name in enumerate(ENV_MARKER):
            if not present & (1 << index):
                monkeypatch.delenv(name)
        module = load_environment_module(monkeypatch)
        assert_channel_refusal(module, native_program, (native.fileno(), python.fileno()))


@pytest.mark.parametrize("fault", [
    "mode", "empty_mode", "negative", "overflow", "stdio", "closed",
    "regular", "stream", "unconnected", "same_fd", "alias",
    "empty_fd", "quoted_fd", "control_fd", "long_fd",
])
def test_invalid_marker_refuses_in_both_runtimes(monkeypatch, native_program, tmp_path, fault):
    with marker_channels(monkeypatch) as (_, native, _, python), ExitStack() as extra:
        descriptors = [native.fileno(), python.fileno()]
        if fault == "mode":
            monkeypatch.setenv(ENV_MARKER[0], "unmanaged")
        elif fault == "empty_mode":
            monkeypatch.setenv(ENV_MARKER[0], "")
        elif fault in ("negative", "overflow", "stdio"):
            text = {"negative": "-1", "overflow": str(1 << 100), "stdio": "1"}[fault]
            monkeypatch.setenv(ENV_MARKER[1], text)
        elif fault in ("empty_fd", "quoted_fd", "control_fd", "long_fd"):
            text = {"empty_fd": "", "quoted_fd": '"\\invalid',
                    "control_fd": "\n\t\rinvalid", "long_fd": "9" * 64}[fault]
            monkeypatch.setenv(ENV_MARKER[1], text)
        elif fault == "closed":
            descriptors.remove(native.fileno())
            native.close()
        elif fault == "regular":
            backing = extra.enter_context(open(tmp_path / "non-socket", "w+b"))
            descriptors.append(backing.fileno())
            monkeypatch.setenv(ENV_MARKER[1], str(backing.fileno()))
        elif fault == "stream":
            host, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
            extra.enter_context(host)
            extra.enter_context(child)
            descriptors.append(child.fileno())
            monkeypatch.setenv(ENV_MARKER[1], str(child.fileno()))
        elif fault == "unconnected":
            child = extra.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
            descriptors.append(child.fileno())
            monkeypatch.setenv(ENV_MARKER[1], str(child.fileno()))
        elif fault == "same_fd":
            monkeypatch.setenv(ENV_MARKER[1], str(python.fileno()))
        elif fault == "alias":
            alias = os.dup(python.fileno())
            extra.callback(os.close, alias)
            descriptors.append(alias)
            monkeypatch.setenv(ENV_MARKER[1], str(alias))
        module = load_environment_module(monkeypatch)
        assert_channel_refusal(module, native_program, descriptors)


@pytest.mark.parametrize("setting", range(4))
@pytest.mark.parametrize("text,reason", [
    (None, "missing_limit"), ("broken", "malformed_limit"), (str(1 << 53), "counter_overflow"),
])
def test_managed_environment_limit_refusals(monkeypatch, native_program, setting, text, reason):
    with marker_channels(monkeypatch) as (host, native, _, python):
        for name in ENV_LIMITS:
            monkeypatch.setenv(name, "17")
        if text is None:
            monkeypatch.delenv(ENV_LIMITS[setting])
        else:
            monkeypatch.setenv(ENV_LIMITS[setting], text)
        module = load_environment_module(monkeypatch)
        assert module.mode() == "managed"
        assert module.quota_line() == "quota=managed enforcement=cooperative"
        with pytest.raises(module.QuotaError) as caught:
            module.RunQuota.from_environment()
        expected = dict(stage="admission", reason=reason,
                        setting=("scratch_bytes", "scratch_files", "output_bytes", "descriptors")[setting])
        assert caught.value.code == "run_quota_configuration_invalid"
        assert dict(caught.value.arguments) == expected
        # The host parser supplies its actual verdict on the normal HELLO
        # reply. The native client does not invent or parse policy defaults.
        stderr_output = tempfile.TemporaryFile()
        process = subprocess.Popen([native_program, "--client"], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=stderr_output, text=True,
                                   env=dict(os.environ), pass_fds=(native.fileno(), python.fileno()))
        try:
            host.settimeout(10)
            request = module.Message.decode(receive_native_frame(host, stderr_output))
            assert request.opcode == module.Opcode.HELLO, captured_native_stderr(stderr_output)
            host.sendall(module._failure_reply(request, caught.value.snapshot, (11, 19)).encode())
            stdout, _ = process.communicate(timeout=10)
            stderr = captured_native_stderr(stderr_output)
            assert process.returncode == 1, stderr
            assert_native_diagnostics(stderr, 1)
            assert json.loads(stdout) == {
                "code": "run_quota_configuration_invalid", "args": expected}, stderr
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            stderr_output.close()


@pytest.mark.parametrize("runtime", ["python", "native"])
@pytest.mark.parametrize("setting", range(4))
def test_managed_environment_zero_is_literal(monkeypatch, native_program, runtime, setting):
    with marker_channels(monkeypatch) as (_, _, host, python):
        for name in ENV_LIMITS:
            monkeypatch.setenv(name, "17")
        monkeypatch.setenv(ENV_LIMITS[setting], "0")
        module = load_environment_module(monkeypatch)
        ledger = module.RunQuota.from_environment()
        amounts = [0] * 4
        amounts[setting] = 1
        if setting == 0:
            amounts[1] = 1  # A new scratch backing also reserves its inode.
        object_id = 1 if setting < 2 else 0
        fields = dict(object_id=object_id, generation=1 if object_id else 0,
                      new_extent=amounts[0])
        stderr = ""
        if runtime == "python":
            actor = owner(module, ledger)
            module.prepare_socket(host)
            thread, errors = start_server(module, ledger, host, actor)
            client = module.QuotaClient.from_environment()
            try:
                with pytest.raises(module.QuotaError) as caught:
                    client.reserve(module.Amounts(*amounts), **fields)
                code, arguments = caught.value.code, dict(caught.value.arguments)
                assert module.mode() == "managed"
                assert module.quota_line() == "quota=managed enforcement=cooperative"
            finally:
                client.close()
                python.close()  # The factory owns a duplicate; allow the server to see EOF.
                thread.join(timeout=10)
                assert not thread.is_alive() and not errors
        else:
            with native_clients(module, native_program, ledger) as (native, _):
                result = native.reserve(object_id, amounts, generation=fields["generation"])
                assert "code" in result and "args" in result, native.stderr
                code, arguments = result["code"], result["args"]
                stderr = native.stderr
        assert code == ("run_descriptor_limit_exceeded" if setting == 3
                        else "run_resource_quota_exceeded"), stderr
        assert arguments["used"] == arguments["limit"] == arguments["peak"] == 0, stderr
        assert arguments["requested"] == 1, stderr
        assert all(row.used == 0 for row in ledger.snapshot())


@pytest.mark.parametrize("runtime", ["python", "native"])
@pytest.mark.parametrize("hostile", ["0", "unmanaged", str(1 << 100)])
def test_managed_mode_survives_hostile_input(monkeypatch, native_program, runtime, hostile):
    with marker_channels(monkeypatch) as (_, _, host, python):
        for name in ENV_LIMITS:
            monkeypatch.setenv(name, "1")
        module = load_environment_module(monkeypatch)
        ledger = module.RunQuota.from_environment()
        stderr = ""
        if runtime == "python":
            actor = owner(module, ledger)
            module.prepare_socket(host)
            thread, errors = start_server(module, ledger, host, actor)
            client = module.QuotaClient.from_environment()
            try:
                # Strategy-controlled data is also applied to the live
                # environment after host startup, a stronger opt-out attempt.
                request = {"limit": hostile, "quota": "unmanaged"}
                for name in ENV_MARKER:
                    monkeypatch.delenv(name, raising=False)
                for name in ENV_LIMITS:
                    monkeypatch.setenv(name, request["limit"])
                assert module.mode() == "managed"
                assert module.quota_line() == "quota=managed enforcement=cooperative"
                with pytest.raises(module.QuotaError) as caught:
                    client.reserve(module.Amounts(output_bytes=2))
                result = {"code": caught.value.code, "args": dict(caught.value.arguments)}
            finally:
                client.close()
                python.close()  # The inherited endpoint also keeps the connection alive.
                thread.join(timeout=10)
                assert not thread.is_alive() and not errors
        else:
            with native_clients(module, native_program, ledger) as (native, _):
                assert native.command("hostile_environment", hostile) == {"ok": True}, native.stderr
                result = native.reserve(0, (0, 0, 2, 0))
                stderr = native.stderr
        assert result == expected_refusal("output_bytes", 0, 2, 1, 0), stderr


@pytest.mark.parametrize("runtime", ["python", "native"])
def test_replaced_inherited_channel_refuses(monkeypatch, native_program, runtime):
    with marker_channels(monkeypatch) as (_, original, _, _):
        module = load_environment_module(monkeypatch)
        stderr = ""
        if runtime == "python":
            host, replacement = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            try:
                os.dup2(replacement.fileno(), original.fileno())
                with pytest.raises(module.QuotaError) as caught:
                    module.QuotaClient.from_environment()
                result = {"code": caught.value.code, "args": dict(caught.value.arguments)}
                assert module.mode() == "managed"
            finally:
                host.close()
                replacement.close()
        else:
            first = module.RunQuota(module.Amounts(10, 1, 10, 8))
            second = module.RunQuota(module.Amounts(10, 1, 10, 8))
            with native_clients(module, native_program, first, second) as (native, descriptors):
                result = native.command("replace_channel", descriptors[0], descriptors[1])
                stderr = native.stderr
        assert result == {"code": "run_quota_configuration_invalid",
                          "args": dict(stage="admission", reason="protocol", setting="channel")}, stderr


@pytest.mark.parametrize("setting", range(4))
@pytest.mark.parametrize("text,reason", [
    (None, "missing_limit"), ("", "malformed_limit"), ("-1", "malformed_limit"),
    ("+1", "malformed_limit"), (" 1", "malformed_limit"), ("1\n", "malformed_limit"),
    ("1.0", "malformed_limit"), ("0x10", "malformed_limit"), ("１", "malformed_limit"),
    (str(1 << 53), "counter_overflow"), ("9" * 5000, "counter_overflow"),
])
def test_limit_defaults_and_decimal_validation(q, setting, text, reason):
    environment = dict.fromkeys(ENV_LIMITS, "17")
    if text is None:
        del environment[ENV_LIMITS[setting]]
    else:
        environment[ENV_LIMITS[setting]] = text
    with pytest.raises(q.QuotaError) as caught:
        q.parse_limits(environment)
    assert caught.value.code == "run_quota_configuration_invalid"
    assert dict(caught.value.arguments) == {
        "stage": "admission", "reason": reason,
        "setting": ("scratch_bytes", "scratch_files", "output_bytes", "descriptors")[setting]}


def test_zero_and_maximum_are_literal_finite_limits(q):
    assert q.parse_limits(dict.fromkeys(ENV_LIMITS, "000")).values() == (0, 0, 0, 0)
    assert q.parse_limits(dict.fromkeys(ENV_LIMITS, str(CAP))).values() == (CAP,) * 4
    ledger = q.RunQuota(q.Amounts(0, 0, 0, 0))
    actor = owner(q, ledger)
    empty = ledger.reserve(actor, q.Amounts())
    empty.commit(q.Amounts(), 0)
    empty.cancel()
    assert all((row.used, row.peak) == (0, 0) for row in ledger.snapshot())
    with pytest.raises(q.QuotaError) as caught:
        ledger.reserve(actor, q.Amounts(1, 1, 0, 1), object_id=1, generation=1, new_extent=1)
    assert caught.value.code == "run_resource_quota_exceeded"
    assert dict(caught.value.arguments) == expected_refusal("scratch_bytes", 0, 1, 0, 0)["args"]
    assert all(row.used == 0 for row in ledger.snapshot())


@pytest.mark.parametrize("limits,resource,requested", [
    ((6, 0, 4, 1), "scratch_bytes", 7),
    ((7, 0, 4, 1), "scratch_files", 1),
    ((7, 1, 4, 1), "output_bytes", 5),
    ((7, 1, 5, 1), "descriptors", 2),
])
def test_atomic_checks_use_bytes_files_output_descriptors_order(q, limits, resource, requested):
    ledger = q.RunQuota(q.Amounts(*limits))
    actor = owner(q, ledger)
    with pytest.raises(q.QuotaError) as caught:
        ledger.reserve(actor, q.Amounts(7, 1, 5, 2), object_id=1, generation=1, new_extent=7)
    args = dict(caught.value.arguments)
    assert (args["used"], args["requested"], args["limit"], args["peak"]) == (
        0, requested, limits[("scratch_bytes", "scratch_files", "output_bytes", "descriptors").index(resource)], 0)
    if resource == "descriptors":
        assert caught.value.code == "run_descriptor_limit_exceeded"
        assert args["process_role"] == "worker" and args["soft_nofile"] == 64
    else:
        assert caught.value.code == "run_resource_quota_exceeded" and args["resource"] == resource
    assert [(row.committed, row.reserved, row.peak) for row in ledger.snapshot()] == [(0, 0, 0)] * 4


def test_descriptor_soft_limit_counts_both_owners_of_same_process(q):
    ledger = q.RunQuota(q.Amounts(0, 0, 0, 10))
    first, second = owner(q, ledger, soft=3), owner(q, ledger, soft=3)
    ticket = ledger.reserve(first, q.Amounts(descriptors=2))
    with pytest.raises(q.QuotaError) as caught:
        ledger.reserve(second, q.Amounts(descriptors=2))
    assert caught.value.code == "run_descriptor_limit_exceeded"
    assert dict(caught.value.arguments) == dict(
        stage="execution", used=2, requested=2, limit=3, peak=2,
        process_role="worker", soft_nofile=3)
    ticket.cancel()
    assert ledger.refusal is caught.value.snapshot
    assert_usage(ledger.snapshot()[3], 0, 0, 2, 10)


def test_native_and_python_share_exact_fit_and_one_byte_refusal(q, native_program):
    ledger = q.RunQuota(q.Amounts(100, 3, 100, 8))
    with python_client(q, ledger) as (python, _):
        with native_clients(q, native_program, ledger) as (native, _):
            native_ticket = native.reserve(0xfedcba9876543210, (37, 1, 0, 1))["ticket"]
            python_ticket = python.reserve(q.Amounts(63, 1, 0, 1),
                                           object_id=2, generation=1, new_extent=63)
            assert_usage(python.snapshot(), 0, 100, 100, 100)
            assert native.command("snapshot", 1) == dict(
                committed=0, reserved=100, used=100, peak=100, limit=100), native.stderr
            expected = expected_refusal("scratch_bytes", 100, 1, 100, 100)
            assert native.reserve(3, (1, 1, 0, 1)) == expected, native.stderr
            frozen = ledger.refusal
            assert (frozen.committed, frozen.reserved) == (0, 100)
            assert native.command("cancel", native_ticket) == {"ok": True}, native.stderr
            python_ticket.cancel()
            assert_usage(python.snapshot(), 0, 0, 100, 100)
            assert native.command("refusal") == expected, native.stderr
            assert native.command("render") == expected, native.stderr
            assert native.command("check") == expected, native.stderr
            assert ledger.refusal is frozen
            with pytest.raises(FrozenInstanceError):
                frozen.used = 0
            with pytest.raises(TypeError):
                frozen.arguments["used"] = 0
            assert native.command("finish", 1) == expected, native.stderr
        with pytest.raises(q.QuotaError) as caught:
            python.reserve(q.Amounts(output_bytes=1))
        assert dict(caught.value.arguments) == expected["args"]


def test_python_owner_refusal_includes_native_pending(q, native_program):
    ledger = q.RunQuota(q.Amounts(100, 3, 100, 8))
    with python_client(q, ledger) as (python, _):
        with native_clients(q, native_program, ledger) as (native, _):
            number = native.reserve(1, (70, 1, 0, 1))["ticket"]
            with pytest.raises(q.QuotaError) as caught:
                python.reserve(q.Amounts(40, 1), object_id=2, generation=1, new_extent=40)
            assert dict(caught.value.arguments) == expected_refusal("scratch_bytes", 70, 40, 100, 70)["args"]
            assert_usage(ledger.snapshot()[0], 0, 70, 70, 100)
            assert native.command("cancel", number) == {"ok": True}, native.stderr
            native.command("finish", 1)


def test_concurrent_endpoints_cannot_both_take_the_same_capacity(q):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    with python_client(q, ledger) as (first, _), python_client(q, ledger) as (second, _):
        barrier, accepted, denied, errors = threading.Barrier(2), [], [], []

        def reserve(client, object_id):
            try:
                barrier.wait()
                accepted.append(client.reserve(q.Amounts(70, 1), object_id=object_id,
                                               generation=1, new_extent=70))
            except q.QuotaError as error:
                denied.append(error)
            except BaseException as error:
                errors.append(error)

        threads = [threading.Thread(target=reserve, args=(client, object_id))
                   for client, object_id in ((first, 1), (second, 2))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        assert not errors
        assert len(accepted) == len(denied) == 1
        assert dict(denied[0].arguments) == expected_refusal("scratch_bytes", 70, 70, 100, 70)["args"]
        assert_usage(ledger.snapshot()[0], 0, 70, 70, 100)
        accepted[0].cancel()
        assert_usage(ledger.snapshot()[0], 0, 0, 70, 100)


def test_partial_commit_cancel_and_successful_shrink_use_real_extent(q, tmp_path):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    path = tmp_path / "backing"
    with python_client(q, ledger) as (client, actor):
        ticket = client.reserve(q.Amounts(100, 1, 10, 1),
                                object_id=1, generation=1, new_extent=100)
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        try:
            os.ftruncate(descriptor, 60)
            metadata = os.fstat(descriptor)
            assert ledger.confirm_object(actor, 1, 1, descriptor) == (metadata.st_dev, metadata.st_ino)
            ticket.commit(q.Amounts(60, 1, 6, 1), metadata.st_size,
                          device=metadata.st_dev, inode=metadata.st_ino)
            ticket.commit(q.Amounts(60, 1, 6, 1), metadata.st_size,
                          device=metadata.st_dev, inode=metadata.st_ino)
            assert_usage(client.snapshot(), 60, 40, 100, 100)
            ticket.cancel()
            assert_usage(client.snapshot(), 60, 0, 100, 100)
            assert_usage(client.snapshot(q.Resource.OUTPUT_BYTES), 6, 0, 10, 100)
            with pytest.raises(OSError):
                os.ftruncate(descriptor, -1)
            assert os.fstat(descriptor).st_size == 60
            assert_usage(client.snapshot(), 60, 0, 100, 100)
            os.ftruncate(descriptor, 40)
            client.release(q.Amounts(20), object_id=1, generation=1, old_extent=60, new_extent=40,
                           device=metadata.st_dev, inode=metadata.st_ino)
            assert_usage(client.snapshot(), 40, 0, 100, 100)
            os.unlink(path)
            assert_usage(client.snapshot(q.Resource.SCRATCH_FILES), 1, 0, 1, 2)
            assert_usage(client.snapshot(), 40, 0, 100, 100)
        finally:
            os.close(descriptor)
        client.release(q.Amounts(40, 1, 0, 1), object_id=1, generation=1, old_extent=40,
                       device=metadata.st_dev, inode=metadata.st_ino)
        assert_usage(client.snapshot(), 0, 0, 100, 100)
        assert_usage(client.snapshot(q.Resource.OUTPUT_BYTES), 6, 0, 10, 100)
        client.finish()


def test_native_commit_cancel_release_and_finish(q, native_program, tmp_path):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    with native_clients(q, native_program, ledger) as (native, _):
        number = native.reserve(9, (90, 1, 8, 1))["ticket"]
        # The host can confirm an inode opened by another stage; identities
        # are conveyed as u64s, not truncated through JSON floating point.
        with open(tmp_path / "native-backing", "w+b") as backing:
            backing.truncate(50)
            info = os.fstat(backing.fileno())
            ledger.confirm_object(native.owners[0], 9, 1, backing.fileno())
            assert native.command("commit", number, 50, 1, 5, 1, 50, info.st_dev, info.st_ino) == {"ok": True}, native.stderr
            assert native.command("snapshot", 1) == dict(
                committed=50, reserved=40, used=90, peak=90, limit=100), native.stderr
            assert native.command("cancel", number) == {"ok": True}, native.stderr
            backing.truncate(30)
            assert native.release(9, (20, 0, 0, 0), old=50, new=30,
                                  device=info.st_dev, inode=info.st_ino) == {"ok": True}, native.stderr
        os.unlink(tmp_path / "native-backing")
        assert native.release(9, (30, 1, 0, 1), old=30,
                              device=info.st_dev, inode=info.st_ino) == {"ok": True}, native.stderr
        assert native.command("snapshot", 3) == dict(committed=5, reserved=0, used=5, peak=8, limit=100), native.stderr
        assert native.command("finish", 1) == dict(committed=0, reserved=0, used=0, peak=90, limit=100), native.stderr
        result = native.reserve(10, (1, 1, 0, 1))
        assert result["code"] == "run_quota_configuration_invalid", native.stderr
        assert result["args"]["reason"] == "protocol", native.stderr


def test_checked_counter_boundary_does_not_wrap_or_saturate(q):
    ledger = q.RunQuota(q.Amounts(CAP, 2, CAP, CAP))
    actor = owner(q, ledger)
    ticket = ledger.reserve(actor, q.Amounts(CAP, 1), object_id=1, generation=1, new_extent=CAP)
    assert_usage(ledger.snapshot()[0], 0, CAP, CAP, CAP)
    with pytest.raises(q.QuotaError) as caught:
        ledger.reserve(actor, q.Amounts(1, 1), object_id=2, generation=1, new_extent=1)
    assert dict(caught.value.arguments) == expected_refusal("scratch_bytes", CAP, 1, CAP, CAP)["args"]
    ticket.cancel()
    assert_usage(ledger.snapshot()[0], 0, 0, CAP, CAP)
    for value in (1 << 53, (1 << 64) - 1):
        fresh = q.RunQuota(q.Amounts(CAP, 1, CAP, 8))
        with pytest.raises(q.QuotaError) as overflow:
            fresh.reserve(owner(q, fresh), q.Amounts(value, 1),
                          object_id=1, generation=1, new_extent=value)
        assert dict(overflow.value.arguments) == dict(
            stage="execution", reason="counter_overflow", setting="scratch_bytes")
        assert_usage(fresh.snapshot()[0], 0, 0, 0, CAP)


def test_native_overflow_is_coded_and_latched_before_send(q, native_program):
    ledger = q.RunQuota(q.Amounts(CAP, 2, CAP, 8))
    with native_clients(q, native_program, ledger) as (native, _):
        result = native.reserve(1, (1 << 53, 1, 0, 1))
        assert result == {"code": "run_quota_configuration_invalid", "args": dict(
            stage="execution", reason="counter_overflow", setting="scratch_bytes")}, native.stderr
        assert native.command("check") == result, native.stderr
        assert native.command("refusal") == result, native.stderr
        assert_usage(ledger.snapshot()[0], 0, 0, 0, CAP)
        assert native.command("finish", 1) == result, native.stderr


def test_old_ticket_cannot_cancel_into_a_new_attempt(q, native_program):
    first = q.RunQuota(q.Amounts(100, 2, 100, 8))
    second = q.RunQuota(q.Amounts(100, 2, 100, 8))
    assert first.nonce != second.nonce
    with native_clients(q, native_program, first, second) as (native, descriptors):
        old = native.reserve(1, (60, 1, 0, 1))["ticket"]
        assert native.command("attach", descriptors[1]) == {"ok": True}, native.stderr
        current = native.reserve(1, (25, 1, 0, 1))["ticket"]
        assert native.command("cancel", old) == {"ok": True}, native.stderr
        assert_usage(first.snapshot()[0], 0, 0, 60, 100)
        assert_usage(second.snapshot()[0], 0, 25, 25, 100)
        assert native.command("cancel", current) == {"ok": True}, native.stderr
        native.command("finish", 1)


def test_direct_ticket_owner_and_inode_aliases(q, tmp_path):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    first, second = owner(q, ledger), owner(q, ledger)
    ticket = ledger.reserve(first, q.Amounts(60, 1, 0, 1),
                            object_id=1, generation=1, new_extent=60)
    with open(tmp_path / "alias-backing", "w+b") as backing:
        backing.truncate(60)
        device, inode = ledger.confirm_object(first, 1, 1, backing.fileno())
        ticket.commit(q.Amounts(60, 1, 0, 1), 60, device=device, inode=inode)
        ticket.cancel()
        ledger.grant_object(first, second, 1, 1)
        alias = ledger.reserve(second, q.Amounts(descriptors=1), object_id=1, generation=1,
                               old_extent=60, new_extent=60, device=device, inode=inode)
        with os.fdopen(os.dup(backing.fileno()), "r+b") as reader:
            alias.commit(q.Amounts(descriptors=1), 60, device=device, inode=inode)
            alias.cancel()
            assert_usage(ledger.snapshot()[0], 60, 0, 60, 100)
            assert_usage(ledger.snapshot()[1], 1, 0, 1, 2)
            assert_usage(ledger.snapshot()[3], 2, 0, 2, 8)
            backing.close()
            ledger.release(first, q.Amounts(descriptors=1), object_id=1, generation=1,
                           old_extent=60, new_extent=60, device=device, inode=inode)
            os.unlink(tmp_path / "alias-backing")
            assert os.fstat(reader.fileno()).st_size == 60
            with pytest.raises(q.QuotaError):
                ledger.release(first, q.Amounts(60, 1), object_id=1, generation=1,
                               old_extent=60, device=device, inode=inode)
            assert_usage(ledger.snapshot()[0], 60, 0, 60, 100)
    ledger.release(second, q.Amounts(descriptors=1), object_id=1, generation=1,
                   old_extent=60, new_extent=60, device=device, inode=inode)
    ledger.release(first, q.Amounts(60, 1), object_id=1, generation=1,
                   old_extent=60, device=device, inode=inode)
    assert_usage(ledger.snapshot()[0], 0, 0, 60, 100)


def test_cancel_cannot_refund_host_observed_unreported_growth(q, tmp_path):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    actor = owner(q, ledger)
    ticket = ledger.reserve(actor, q.Amounts(100, 1), object_id=1, generation=1, new_extent=100)
    with open(tmp_path / "unreported", "w+b") as backing:
        backing.truncate(60)
        device, inode = ledger.confirm_object(actor, 1, 1, backing.fileno())
        with pytest.raises(q.QuotaError):
            ticket.cancel()
        assert_usage(ledger.snapshot()[0], 0, 100, 100, 100)
        assert_usage(ledger.snapshot()[1], 0, 1, 1, 2)
        ticket.commit(q.Amounts(60, 1), 60, device=device, inode=inode)
        ticket.cancel()
        assert_usage(ledger.snapshot()[0], 60, 0, 100, 100)
    os.unlink(tmp_path / "unreported")
    ledger.release(actor, q.Amounts(60, 1), object_id=1, generation=1,
                   old_extent=60, device=device, inode=inode)
    assert_usage(ledger.snapshot()[0], 0, 0, 100, 100)


def test_commit_requires_host_confirmation_and_keeps_unverified_extent_reserved(q):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    actor = owner(q, ledger)
    ticket = ledger.reserve(actor, q.Amounts(60, 1), object_id=1, generation=1, new_extent=60)
    with pytest.raises(q.QuotaError) as caught:
        ticket.commit(q.Amounts(60, 1), 60, device=12, inode=34)
    assert caught.value.code == "run_quota_configuration_invalid"
    assert dict(caught.value.arguments) == dict(
        stage="execution", reason="containment_unavailable", setting="workdir")
    assert_usage(ledger.snapshot()[0], 0, 60, 60, 100)


def test_ticket_from_other_ledger_cannot_refund_current_attempt(q):
    old, current = (q.RunQuota(q.Amounts(100, 2, 100, 8)) for _ in range(2))
    old_owner, current_owner = owner(q, old), owner(q, current)
    old_ticket = old.reserve(old_owner, q.Amounts(60, 1), object_id=1, generation=1, new_extent=60)
    current_ticket = current.reserve(current_owner, q.Amounts(25, 1),
                                     object_id=1, generation=1, new_extent=25)
    with pytest.raises(q.QuotaError):
        current.cancel(old_ticket)
    assert old_owner is not current_owner and old_owner.nonce != current_owner.nonce
    old_ticket.cancel()
    assert_usage(old.snapshot()[0], 0, 0, 60, 100)
    assert_usage(current.snapshot()[0], 0, 25, 25, 100)
    current_ticket.cancel()


def test_disconnect_retains_pending_and_forbids_growth(q):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    with waiting_process() as recorded:
        actor, other = owner(q, ledger, pid=recorded.pid), owner(q, ledger)
        ledger.reserve(actor, q.Amounts(60, 1), object_id=1, generation=1, new_extent=60)
        ledger.disconnect(actor)
        assert recorded.poll() is None
        assert_usage(ledger.snapshot()[0], 0, 60, 60, 100)
        with pytest.raises(q.QuotaError) as caught:
            ledger.reserve(other, q.Amounts(output_bytes=1))
        assert caught.value.code == "run_quota_configuration_invalid"
        assert dict(caught.value.arguments) == dict(stage="admission", reason="protocol", setting="channel")


def test_native_disconnect_refuses_growth_and_preserves_pending(q, native_program):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    with native_clients(q, native_program, ledger) as (native, _):
        native.reserve(1, (60, 1, 0, 1))
        native.channels[0].shutdown(socket.SHUT_RDWR)
        result = native.reserve(2, (1, 1, 0, 1))
        assert result == {"code": "run_quota_configuration_invalid", "args": dict(
            stage="admission", reason="protocol", setting="channel")}, native.stderr
        assert native.command("check") == result, native.stderr
        assert_usage(ledger.snapshot()[0], 0, 60, 60, 100)


@pytest.mark.parametrize("opcode", range(1, 8))
@pytest.mark.parametrize("reply", [0, 0x8000])
def test_every_wire_slot_and_opcode_is_little_endian(q, native_program, opcode, reply):
    values = tuple(0x8102030405060708 + index for index in range(27)) + (0, 0, 0)
    frame = FRAME.pack(b"PFQ1", 1, opcode | reply, 0xfedcba9876543210, *values)
    message = q.Message.decode(frame)
    assert message.opcode == opcode | reply and message.sequence == 0xfedcba9876543210
    assert message.slots == values and message.encode() == frame
    result = subprocess.run([native_program, "--wire"], input=frame, capture_output=True)
    stderr = result.stderr.decode("utf-8", errors="replace")
    assert result.returncode == 0 and result.stderr == b"", stderr
    assert result.stdout == frame, stderr
    assert FRAME.unpack(result.stdout) == (b"PFQ1", 1, opcode | reply, 0xfedcba9876543210, *values), stderr


def damaged_frames(good):
    return [good[:1], good[:-1], good + b"x", good + bytes(1024),
            b"BAD!" + good[4:], good[:4] + b"\x02\x00" + good[6:],
            good[:6] + b"\x08\x00" + good[8:],
            good[:232] + b"\x01" + good[233:]]


def bad_frames():
    values = [0] * 30
    values[7] = 3  # Otherwise a valid HELLO from the recorded worker.
    return damaged_frames(FRAME.pack(b"PFQ1", 1, 1, 1, *values))


@pytest.mark.parametrize("variant", range(13))
def test_native_endpoint_rejects_malformed_replies(native_program, variant):
    server, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    native = Native(native_program, [child.fileno()])
    child.close()
    try:
        server.settimeout(10)
        request = FRAME.unpack(receive_native_frame(server, native.stderr_output))
        assert request[:4] == (b"PFQ1", 1, 1, 1), native.stderr
        values = [0] * 30
        values[0], values[1], values[3], values[7] = 11, 19, 1, 3
        values[22], values[23] = 64, native.process.pid
        good = FRAME.pack(b"PFQ1", 1, 0x8001, 1, *values)
        if variant < 8:
            frame = damaged_frames(good)[variant]
        elif variant == 8:
            values[2] = 1  # object is unused in a HELLO reply.
            frame = FRAME.pack(b"PFQ1", 1, 0x8001, 1, *values)
        else:
            if variant == 9:
                values[0] = values[1] = 0
            elif variant == 10:
                values[23] += 1
            elif variant == 11:
                values[7] = 2
            else:
                values[3] = 0
            frame = FRAME.pack(b"PFQ1", 1, 0x8001, 1, *values)
        server.sendall(frame)
        failure = native.receive()
        assert failure["code"] == "run_quota_configuration_invalid", native.stderr
        assert failure["args"] == dict(stage="admission", reason="protocol", setting="channel"), native.stderr
        stdout, _ = native.process.communicate()
        assert native.process.returncode == 1 and stdout == "", native.stderr
        assert_native_diagnostics(native.stderr, 1)
    finally:
        server.close()
        native.marker_peer.close()
        if native.process.poll() is None:
            native.process.kill()
            native.process.communicate()
        native.stderr_output.close()


@pytest.mark.parametrize("variant", range(13))
def test_python_environment_rejects_malformed_hello(monkeypatch, variant, capsys):
    with marker_channels(monkeypatch) as (_, _, host, _):
        module = load_environment_module(monkeypatch)
        errors = []

        def respond():
            try:
                host.settimeout(10)
                request = FRAME.unpack(host.recv(257))
                assert request[:4] == (b"PFQ1", 1, 1, 1)
                values = [0] * 30
                values[0], values[1], values[3], values[7] = 11, 19, 1, 3
                values[22], values[23] = 64, os.getpid()
                good = FRAME.pack(b"PFQ1", 1, 0x8001, 1, *values)
                if variant < 8:
                    frame = damaged_frames(good)[variant]
                else:
                    if variant == 8:
                        values[2] = 1
                    elif variant == 9:
                        values[0] = values[1] = 0
                    elif variant == 10:
                        values[23] += 1
                    elif variant == 11:
                        values[7] = 2
                    else:
                        values[3] = 0
                    frame = FRAME.pack(b"PFQ1", 1, 0x8001, 1, *values)
                host.sendall(frame)
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=respond, daemon=True)
        thread.start()
        try:
            with pytest.raises(module.QuotaError) as caught:
                module.QuotaClient.from_environment()
            assert caught.value.code == "run_quota_configuration_invalid"
            assert dict(caught.value.arguments) == dict(
                stage="admission", reason="protocol", setting="channel")
            assert module.mode() == "managed"
            assert module.quota_line() == "quota=managed enforcement=cooperative"
        finally:
            thread.join(timeout=10)
            assert not thread.is_alive() and not errors
    assert capsys.readouterr() == ("", "")


@pytest.mark.parametrize("frame", bad_frames())
def test_malformed_short_oversized_frames_refuse_in_both_codecs(q, native_program, frame):
    with pytest.raises(q.QuotaError) as caught:
        q.Message.decode(frame)
    assert caught.value.code == "run_quota_configuration_invalid"
    result = subprocess.run([native_program, "--wire"], input=frame, capture_output=True)
    stderr = result.stderr.decode("utf-8", errors="replace")
    assert result.returncode == 1 and result.stdout == b"", stderr
    assert_native_diagnostics(stderr, 1, code="run_quota_configuration_invalid")


@pytest.mark.parametrize("frame", bad_frames())
def test_server_refuses_bad_packet_before_growth(q, frame):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    server, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    q.prepare_socket(server)
    thread, errors = start_server(q, ledger, server, owner(q, ledger))
    try:
        client.sendall(frame)
        response = client.recv(257)
        fields = FRAME.unpack(response)
        assert fields[:3] == (b"PFQ1", 1, 0x8001)
        assert fields[4 + 15] == 6  # status = protocol, independently located.
        assert all(row.used == 0 for row in ledger.snapshot())
    finally:
        client.close()
        thread.join()
        assert not errors, errors


@pytest.mark.parametrize("uid", [2147483648, 2147483649, 4294967294, 4294967295])
@pytest.mark.parametrize("gid", [2147483648, 2147483649, 4294967294, 4294967295])
def test_kernel_credentials_decode_unsigned_uid_and_gid(q, uid, gid):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    actor = owner(q, ledger, uid=uid, gid=gid)
    credentials = struct.pack("=iII", actor.pid, uid, gid)
    assert len(credentials) == 12
    assert q._credentials_match(credentials, actor) is True


@pytest.mark.parametrize("uid,wire_uid", [
    (2147483648, 2147483649),
    (2147483649, 2147483650),
    (4294967294, 4294967295),
    (4294967295, 4294967294),
])
def test_kernel_credentials_uid_off_by_one_refuses(q, uid, wire_uid):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    actor = owner(q, ledger, uid=uid, gid=2147483648)
    credentials = struct.pack("=iII", actor.pid, uid, 2147483648)
    assert q._credentials_match(credentials, actor) is True
    credentials = struct.pack("=iII", actor.pid, wire_uid, 2147483648)
    assert q._credentials_match(credentials, actor) is False


@pytest.mark.parametrize("gid,wire_gid", [
    (2147483648, 2147483649),
    (2147483649, 2147483650),
    (4294967294, 4294967295),
    (4294967295, 4294967294),
])
def test_kernel_credentials_gid_off_by_one_refuses(q, gid, wire_gid):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    actor = owner(q, ledger, uid=2147483648, gid=gid)
    credentials = struct.pack("=iII", actor.pid, 2147483648, gid)
    assert q._credentials_match(credentials, actor) is True
    credentials = struct.pack("=iII", actor.pid, 2147483648, wire_gid)
    assert q._credentials_match(credentials, actor) is False


def test_kernel_credentials_pid_off_by_one_refuses(q):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    actor = owner(q, ledger, uid=2147483648, gid=2147483648)
    credentials = struct.pack("=iII", actor.pid, 2147483648, 2147483648)
    assert q._credentials_match(credentials, actor) is True
    credentials = struct.pack("=iII", actor.pid + 1, 2147483648, 2147483648)
    assert q._credentials_match(credentials, actor) is False


def test_kernel_credentials_negative_pid_does_not_match_non_negative_owner(q):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    actor = owner(q, ledger, uid=2147483648, gid=2147483648)
    credentials = struct.pack("=iII", actor.pid, 2147483648, 2147483648)
    assert q._credentials_match(credentials, actor) is True
    credentials = struct.pack("=iII", -2147483648, 2147483648, 2147483648)
    assert q._credentials_match(credentials, actor) is False


@pytest.mark.parametrize("uid,gid,wire_uid,wire_gid", [
    (4294967296, 2147483648, 2147483648, 2147483648),
    (2147483648, 4294967296, 2147483648, 2147483648),
    (4294967296, 2147483648, 4294967295, 4294967295),
    (2147483648, 4294967296, 4294967295, 4294967295),
])
def test_kernel_credentials_registered_value_above_uint32_never_matches(
        q, uid, gid, wire_uid, wire_gid):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    valid = owner(q, ledger, uid=wire_uid, gid=wire_gid)
    impossible = owner(q, ledger, uid=uid, gid=gid)
    credentials = struct.pack("=iII", valid.pid, wire_uid, wire_gid)
    assert q._credentials_match(credentials, valid) is True
    assert q._credentials_match(credentials, impossible) is False


def test_real_kernel_credentials_accept_current_process(q):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    server, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    q.prepare_socket(server)
    actor = owner(q, ledger)
    thread, errors = start_server(q, ledger, server, actor)
    values = [0] * 30
    values[7] = 3
    # The real high-id proof requires a privilege drop; these decode rows
    # cover the unsigned boundary and this row uses the process credentials
    # that the kernel supplies on the existing HELLO path.
    try:
        assert actor.pid == os.getpid()
        assert actor.uid == os.getuid()
        assert actor.gid == os.getgid()
        client.sendall(FRAME.pack(b"PFQ1", 1, 1, 1, *values))
        fields = FRAME.unpack(client.recv(257))
        assert fields[:3] == (b"PFQ1", 1, 0x8001)
        assert fields[3] == 1
        assert fields[4 + 15] == 0
    finally:
        client.close()
        thread.join()
        assert not errors, errors


def test_sender_credentials_are_not_packet_identity(q):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    recorded = subprocess.Popen([sys.executable, "-B", "-c",
                                 "import sys; sys.stdin.buffer.read()"], stdin=subprocess.PIPE)
    server, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    q.prepare_socket(server)
    thread, errors = start_server(q, ledger, server, owner(q, ledger, pid=recorded.pid))
    values = [0] * 30
    values[7] = 3  # A correct role cannot authenticate the wrong kernel pid.
    try:
        client.sendall(FRAME.pack(b"PFQ1", 1, 1, 1, *values))
        fields = FRAME.unpack(client.recv(257))
        assert fields[4 + 15] == 6 and fields[4 + 8] == 8
        assert all(row.used == 0 for row in ledger.snapshot())
    finally:
        client.close()
        thread.join()
        recorded.stdin.close()
        assert recorded.wait() == 0
        assert not errors, errors


def test_recorded_process_exit_forbids_growth_without_reusing_its_generation(q):
    ledger = q.RunQuota(q.Amounts(100, 2, 100, 8))
    with waiting_process() as recorded:
        actor = owner(q, ledger, pid=recorded.pid)
        ledger.reserve(actor, q.Amounts(60, 1), object_id=1, generation=1, new_extent=60)
        recorded.stdin.close()
        assert recorded.wait() == 0
        with pytest.raises(q.QuotaError) as caught:
            ledger.reserve(actor, q.Amounts(output_bytes=1))
        assert dict(caught.value.arguments) == dict(
            stage="admission", reason="protocol", setting="channel")
        assert_usage(ledger.snapshot()[0], 0, 60, 60, 100)
        assert_usage(ledger.snapshot()[2], 0, 0, 0, 100)


def test_process_generation_matches_real_proc_stat_and_boot_id(q):
    with waiting_process() as recorded:
        with open(f"/proc/{recorded.pid}/stat", "rb") as source:
            fields = source.read().split()
        # This child runs the Python executable; verify its comm is one token
        # before independently reading the documented, one-based field 22.
        assert fields[0] == str(recorded.pid).encode("ascii")
        assert fields[1].startswith(b"(") and fields[1].endswith(b")")
        with open("/proc/sys/kernel/random/boot_id", encoding="ascii") as source:
            boot_id = source.read().strip()
        expected = (recorded.pid, int(fields[21]), boot_id)
        generation = q._proc_generation(recorded.pid)
        assert generation == expected
        assert q._proc_generation(recorded.pid) == generation
        recorded.stdin.close()
        assert recorded.wait(timeout=10) == 0
        with pytest.raises(ProcessLookupError):
            q._proc_generation(recorded.pid)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path)
    options, rest = parser.parse_known_args()
    if options.native is not None:
        os.environ["PF_QUOTA_TEST_NATIVE"] = str(options.native.resolve())
    raise SystemExit(pytest.main(["-q", "-p", "no:cacheprovider", __file__, *rest]))
