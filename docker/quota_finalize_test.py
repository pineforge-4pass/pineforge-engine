"""Real finalizer subprocesses with a real ledger and host-confirmed spools."""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
from types import SimpleNamespace

import pytest


HERE = Path(__file__).resolve().parent
TOOL = HERE / "pineforge_sink_finalize.py"
MARKERS = ("PF_RUN_QUOTA_MODE", "PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD")
LIMITS = ("PF_RUN_SCRATCH_BYTES", "PF_RUN_SCRATCH_FILES",
          "PF_RUN_OUTPUT_BYTES", "PF_RUN_DESCRIPTOR_LIMIT")


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8") + b"\n"


@pytest.fixture
def payload():
    # Independent v1 input, including UTF-8 and escaped newline bytes.
    trade_fields = [
        "n", "side", "entry_time", "exit_time", "entry_price", "exit_price", "qty",
        "pnl", "pnl_pct", "max_runup", "max_drawdown", "commission",
        "entry_bar_index", "exit_bar_index", "open_at_end", "entry_incarnation",
        "close_cause", "entry_id", "exit_id", "exit_comment",
    ]
    header = {
        "schema": "pf-report-sink", "version": 1, "run_id": "雪\nsink",
        "engine": "fixture", "records": [
            {"type": "E", "fields": ["time_ms", "equity", "open_profit"],
             "types": ["i64", "f64?", "f64?"],
             "units": ["unix_ms", "account_ccy", "account_ccy"]},
            {"type": "T", "fields": trade_fields,
             "types": ["i64", "str", "i64", "i64"] + ["f64?"] * 8
                      + ["i64", "i64", "bool", "u64", "i64", "str", "str", "str"],
             "units": ["", "", "unix_ms", "unix_ms", "quote_ccy", "quote_ccy", "qty",
                       "account_ccy", "percent", "account_ccy", "account_ccy",
                       "account_ccy", "script_bar", "script_bar", "", "", "", "", "", ""]},
        ],
    }
    rows = [
        ["E", 1700000040000, 10000.0, -0.0],
        ["E", 1700000100000, 10001.0, 0.0],
        ["T", 1, "long", 1700000040000, 1700000100000, 100.0, 101.0, 2.0,
         1.0, 0.5, 2.0, -1.0, 0.25, 0, 1, False, 1, 0, 'L"雪\n', "", ""],
    ]
    prefix = encoded(header) + b"".join(encoded(row) for row in rows)
    counts = {item["type"]: sum(row[0] == item["type"] for row in rows)
              for item in header["records"]}
    return prefix + encoded({
        "trailer": "pf-report-sink", "version": 1, "counts": counts,
        "sha256": hashlib.sha256(prefix).hexdigest(),
    })


def receipt_bytes(payload, *, complete, sink_bytes=None, cap=None, counts=None):
    trailer = json.loads(payload.splitlines()[-1])
    folded = trailer["counts"] if complete else ({} if counts is None else counts)
    return encoded({
        "complete": complete,
        **{"count_" + tag.lower(): folded.get(tag, 0) for tag in "ETOVKXH"},
        **({"reason": "sink_exhausted"} if cap is not None else {}),
        "sink_bytes": len(payload) if sink_bytes is None else sink_bytes,
        **({"cap_bytes": cap} if cap is not None else {}),
        "sha256": trailer["sha256"] if complete else "",
        "finalizer_version": "1.1.0",
    })


class QuotaHost:
    """Wire service over the real ledger, with independent backing observation.

    The stock service has no host inode-observation callback. This service
    routes the real protocol to the same public ledger/ticket operations and
    opens the private spool itself before committing. It replaces no ledger
    decisions or finalizer code.
    """

    def __init__(self, q, directory, limits, occupied):
        self.q = q
        self.directory = directory
        self.ledger = q.RunQuota(q.Amounts(*limits))
        self.native_server, self.native_endpoint = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.server, self.endpoint = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_SEQPACKET)
        q.prepare_socket(self.server)
        self.thread = None
        self.errors = []
        self.requests = []
        self.reservations = []
        self.denials = []
        self.spool_fd = None
        self.seed = None
        if occupied:
            owner = self.ledger.open_owner(
                pid=os.getpid(), uid=os.getuid(), gid=os.getgid(), generation=1,
                role=q.Role.WORKER, soft_nofile=64)
            ticket = self.ledger.reserve(
                owner, q.Amounts(scratch_bytes=occupied, scratch_files=1),
                object_id=1, generation=1, new_extent=occupied,
                stage=q.Stage.ASSEMBLY, artifact=q.Artifact.TABLE)
            self.seed = tempfile.TemporaryFile(dir=directory)
            written = self.seed.write(b"s" * occupied)
            assert written == occupied
            self.seed.flush()
            metadata = os.fstat(self.seed.fileno())
            self.ledger.confirm_object(owner, 1, 1, self.seed.fileno())
            ticket.commit(q.Amounts(scratch_bytes=occupied, scratch_files=1),
                          occupied, device=metadata.st_dev, inode=metadata.st_ino)
            ticket.cancel()
            self.ledger.finish(owner)

    def spool_bytes(self):
        if self.spool_fd is None:
            return None
        size = os.fstat(self.spool_fd).st_size
        return os.pread(self.spool_fd, size, 0)

    def confirm(self, actor, fields, request):
        q = self.q
        if self.spool_fd is None:
            paths = list((self.directory / "spool").iterdir())
            assert len(paths) == 1, paths
            self.spool_fd = os.open(paths[0], os.O_RDONLY)
        metadata = os.fstat(self.spool_fd)
        assert (metadata.st_dev, metadata.st_ino) == (
            request[q.Slot.DEVICE], request[q.Slot.INODE])
        self.ledger.confirm_object(
            actor, fields["object_id"], fields["generation"], self.spool_fd)

    def start(self, process):
        # Parent copies must close so child exit makes EOF observable.
        self.native_endpoint.close()
        self.endpoint.close()

        def serve():
            q, ledger, actor = self.q, self.ledger, None
            tickets = {}
            sequence = 0
            credentials = struct.Struct("3i")
            try:
                while True:
                    data, controls, flags, _ = self.server.recvmsg(
                        q.WIRE_SIZE + 1, socket.CMSG_SPACE(credentials.size))
                    if not data:
                        return
                    assert not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC)
                    request = q.Message.decode(data)
                    peers = [value for level, kind, value in controls
                             if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS]
                    assert len(peers) == 1
                    assert credentials.unpack(peers[0]) == (
                        process.pid, os.getuid(), os.getgid())
                    assert request.sequence == sequence + 1
                    sequence = request.sequence
                    opcode = q.Opcode(request.opcode)
                    self.requests.append(opcode)
                    assert request[q.Slot.ROLE] == q.Role.FINALIZER
                    try:
                        extra = {}
                        if opcode == q.Opcode.HELLO:
                            assert actor is None
                            actor = ledger.open_owner(
                                pid=process.pid, uid=os.getuid(), gid=os.getgid(),
                                generation=1, role=q.Role.FINALIZER, soft_nofile=64)
                            extra.update(generation=actor.number, role=actor.role,
                                         pid=actor.pid, soft_nofile=actor.soft_nofile)
                        else:
                            assert actor is not None
                            assert (request[q.Slot.NONCE_HI], request[q.Slot.NONCE_LO]) == ledger.nonce
                            fields = dict(
                                object_id=request[q.Slot.OBJECT],
                                generation=request[q.Slot.GENERATION],
                                old_extent=request[q.Slot.OLD_EXTENT],
                                new_extent=request[q.Slot.NEW_EXTENT],
                                stage=request[q.Slot.STAGE], artifact=request[q.Slot.ARTIFACT],
                                device=request[q.Slot.DEVICE], inode=request[q.Slot.INODE])
                            amounts = q.Amounts(*(request[slot] for slot in (
                                q.Slot.DELTA_DISK, q.Slot.DELTA_FILES,
                                q.Slot.DELTA_OUTPUT, q.Slot.DELTA_FDS)))
                            if opcode == q.Opcode.RESERVE:
                                before = self.spool_bytes()
                                usage = ledger.snapshot()
                                try:
                                    ticket = ledger.reserve(actor, amounts, **fields)
                                except q.QuotaError as error:
                                    self.denials.append(SimpleNamespace(
                                        error=error, before=before, after=self.spool_bytes(),
                                        usage_before=usage, usage_after=ledger.snapshot()))
                                    raise
                                self.reservations.append((amounts, fields, before))
                                tickets[ticket.number] = (ticket, fields)
                                extra["ticket"] = ticket.number
                            elif opcode in (q.Opcode.COMMIT, q.Opcode.CANCEL):
                                number = request[q.Slot.TICKET]
                                ticket, reserved_fields = tickets[number]
                                if opcode == q.Opcode.COMMIT:
                                    if reserved_fields["object_id"]:
                                        self.confirm(actor, reserved_fields, request)
                                    ticket.commit(
                                        amounts, request[q.Slot.NEW_EXTENT],
                                        device=request[q.Slot.DEVICE], inode=request[q.Slot.INODE])
                                else:
                                    ticket.cancel()
                                    del tickets[number]
                            elif opcode == q.Opcode.RELEASE:
                                ledger.release(actor, amounts, **fields)
                            elif opcode in (q.Opcode.SNAPSHOT, q.Opcode.FINISH):
                                resource = q.Resource(request[q.Slot.RESOURCE])
                                snapshot = (ledger.finish(actor) if opcode == q.Opcode.FINISH
                                            else ledger.snapshot())
                                if opcode == q.Opcode.FINISH and ledger.refusal is not None:
                                    raise q.QuotaError(ledger.refusal)
                                usage = snapshot[resource - 1]
                                extra.update(resource=resource, committed=usage.committed,
                                             reserved=usage.reserved, used=usage.used,
                                             peak=usage.peak, limit=usage.limit)
                            else:
                                raise AssertionError(f"unexpected opcode {opcode}")
                        reply = q.Message.make(
                            opcode | q.REPLY, sequence, nonce_hi=ledger.nonce[0],
                            nonce_lo=ledger.nonce[1], **extra)
                    except q.QuotaError as error:
                        reply = q._failure_reply(request, error.snapshot, ledger.nonce)
                    self.server.sendall(reply.encode())
            except BaseException as error:
                self.errors.append(error)
            finally:
                if actor is not None:
                    ledger.disconnect(actor)
                self.server.close()

        self.thread = threading.Thread(target=serve, daemon=True)
        self.thread.start()

    def wait(self):
        if self.thread is not None:
            self.thread.join(10)
            assert not self.thread.is_alive(), "quota host did not stop"
        assert not self.errors, self.errors

    def close(self):
        try:
            self.wait()
        finally:
            for channel in (self.native_server, self.native_endpoint, self.server, self.endpoint):
                channel.close()
            if self.spool_fd is not None:
                os.close(self.spool_fd)
            if self.seed is not None:
                self.seed.close()
            self.ledger.close()


@pytest.fixture
def quota_host(monkeypatch):
    # Capture managed mode with real, distinct marker sockets, as the column
    # store's host fixture does. Each finalizer gets its own inherited pair.
    markers = (*socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET),
               *socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET))
    monkeypatch.setenv(MARKERS[0], "managed")
    monkeypatch.setenv(MARKERS[1], str(markers[1].fileno()))
    monkeypatch.setenv(MARKERS[2], str(markers[3].fileno()))
    name = "_finalizer_host_run_quota"
    spec = importlib.util.spec_from_file_location(name, HERE / "run_quota.py")
    q = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, name, q)
    spec.loader.exec_module(q)

    @contextmanager
    def host(directory, limits, *, occupied=0):
        instance = QuotaHost(q, directory, limits, occupied)
        try:
            yield instance
        finally:
            instance.close()

    try:
        yield host
    finally:
        for channel in markers:
            channel.close()


def invoke(directory, payload, *, stdout_mode=False, cap=None, host=None,
           receipt_channel="stderr", policy_only=False, watched=()):
    directory.mkdir(exist_ok=True)
    spool = directory / "spool"
    spool.mkdir(exist_ok=True)
    out = directory / "out.sink"
    environment = os.environ.copy()
    for name in (*MARKERS, *LIMITS):
        environment.pop(name, None)
    if policy_only:
        environment.update({name: "0" for name in LIMITS})
    inherited = list(watched)
    if host is not None:
        environment.update(zip(MARKERS, (
            "managed", str(host.native_endpoint.fileno()), str(host.endpoint.fileno()))))
        inherited += [host.native_endpoint.fileno(), host.endpoint.fileno()]
    with tempfile.TemporaryFile() as receipt_file:
        receipt_fd = {"stderr": 2, "stdout": 1, "file": receipt_file.fileno()}[receipt_channel]
        if receipt_channel == "file":
            inherited.append(receipt_fd)
        command = [sys.executable, "-B", "-S", str(TOOL), "--in-fd", "0",
                   "--out", "-" if stdout_mode else str(out), "--spool-dir", str(spool),
                   "--receipt-fd", str(receipt_fd)]
        if cap is not None:
            command += ["--max-bytes", str(cap)]
        process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, pass_fds=inherited, env=environment, cwd=directory)
        try:
            if host is not None:
                host.start(process)
            stdout, stderr = process.communicate(payload)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        if host is not None:
            host.wait()
        receipt_file.seek(0)
        wire = {"stderr": stderr, "stdout": stdout, "file": receipt_file.read()}[receipt_channel]
    return SimpleNamespace(
        returncode=process.returncode, stdout=stdout, stderr=stderr,
        receipt_bytes=wire, receipt=json.loads(wire) if wire else None, out=out, spool=spool)


def assert_quota_refusal(result, host, *, resource, artifact, used, requested, limit):
    assert result.returncode == 3, (result.stdout, result.stderr)
    receipt = result.receipt
    assert receipt["complete"] is False and receipt["sha256"] == ""
    assert receipt["tool"] == "pineforge-sink-finalize"
    assert receipt["event"] == "sink_refused" and receipt["exit_code"] == 3
    assert receipt["code"] == "run_resource_quota_exceeded"
    assert tuple(receipt) == (
        "complete", *("count_" + tag.lower() for tag in "ETOVKXH"),
        "sink_bytes", "sha256", "finalizer_version",
        "tool", "event", "exit_code", "detail", "code", "args")
    assert receipt["args"] == {
        "stage": "finalizer", "resource": resource, "artifact": artifact,
        "used": used, "requested": requested, "limit": limit, "peak": used,
    }
    assert "reason" not in receipt and "cap_bytes" not in receipt
    assert len(result.receipt_bytes.splitlines()) == 1
    assert result.receipt_bytes == encoded(receipt)
    assert host.ledger.refusal is not None
    assert receipt["code"] == host.ledger.refusal.code
    assert receipt["args"] == dict(host.ledger.refusal.arguments)
    assert len(host.denials) == 1
    denial = host.denials[0]
    assert denial.error.code == receipt["code"]
    assert denial.error.code_args == receipt["args"]
    assert denial.usage_before == denial.usage_after
    assert not result.out.exists() and list(result.spool.iterdir()) == []
    assert all(usage.reserved == 0 for usage in host.ledger.snapshot())


def test_spool_growth_denied_before_disk_changes(quota_host, tmp_path, payload):
    lines = payload.splitlines(keepends=True)
    admitted = sum(map(len, lines[:2]))
    occupied = len(payload) - admitted
    # The sink alone fits both the run limit and the local cap. A second
    # live representation consumes capacity on the same real run ledger.
    with quota_host(tmp_path, (len(payload), 2, len(payload), 0),
                    occupied=occupied) as host:
        result = invoke(tmp_path, payload, cap=len(payload), host=host)
        assert_quota_refusal(
            result, host, resource="scratch_bytes", artifact="spool",
            used=len(payload), requested=1, limit=len(payload))
        denial = host.denials[0]
        assert denial.before == denial.after == payload[:admitted]
        assert os.fstat(host.spool_fd).st_size == admitted
        assert host.spool_bytes() == denial.before
        usage = host.ledger.snapshot()
        assert usage[host.q.Resource.SCRATCH_BYTES - 1].committed == occupied
        assert usage[host.q.Resource.SCRATCH_FILES - 1].committed == 1
        assert usage[host.q.Resource.OUTPUT_BYTES - 1].committed == admitted


@pytest.mark.parametrize("receipt_channel", ("stderr", "file"))
@pytest.mark.parametrize("over", (False, True))
def test_stdout_output_boundary(quota_host, tmp_path, payload, over, receipt_channel):
    limit = len(payload) - int(over)
    with quota_host(tmp_path, (0, 0, limit, 0)) as host:
        result = invoke(tmp_path, payload, stdout_mode=True, cap=len(payload),
                        host=host, receipt_channel=receipt_channel)
        assert len(result.stdout) == limit
        assert result.stdout == payload[:limit]
        if over:
            assert_quota_refusal(
                result, host, resource="output_bytes", artifact="sink",
                used=limit, requested=1, limit=limit)
        else:
            assert result.returncode == 0, result.stderr
            assert result.receipt_bytes == receipt_bytes(payload, complete=True)
            assert host.ledger.refusal is None
        usage = host.ledger.snapshot()
        assert usage[host.q.Resource.OUTPUT_BYTES - 1].committed == limit
        assert usage[host.q.Resource.SCRATCH_BYTES - 1].used == 0
        assert usage[host.q.Resource.SCRATCH_FILES - 1].used == 0
        assert all(item.reserved == 0 for item in usage)


def test_file_exact_fit_rename_has_one_charge(quota_host, tmp_path, payload):
    with quota_host(tmp_path, (len(payload), 1, len(payload), 0)) as host:
        result = invoke(tmp_path, payload, host=host)  # Unset local cap stays legal.
        assert result.returncode == 0, result.stderr
        assert result.receipt_bytes == receipt_bytes(payload, complete=True)
        assert result.out.read_bytes() == payload
        assert host.q.Opcode.HELLO in host.requests
        assert host.spool_fd is not None, "host never confirmed the spool"
        metadata = result.out.stat()
        observed = os.fstat(host.spool_fd)
        assert (metadata.st_dev, metadata.st_ino) == (observed.st_dev, observed.st_ino)
        assert list(result.spool.iterdir()) == []
        assert host.ledger.refusal is None
        usage = host.ledger.snapshot()
        assert [item.committed for item in usage] == [len(payload), 1, len(payload), 0]
        assert [item.peak for item in usage] == [len(payload), 1, len(payload), 0]
        assert all(item.reserved == 0 for item in usage)
        assert sum(amounts.scratch_files for amounts, _, _ in host.reservations) == 1
        for amounts, fields, before in host.reservations:
            if amounts.scratch_files:
                assert before is None
            else:
                assert len(before) == fields["old_extent"]
                assert amounts.scratch_bytes == amounts.output_bytes


def test_file_output_over_limit_stops_spool_before_next_byte(quota_host, tmp_path, payload):
    limit = len(payload) - 1
    with quota_host(tmp_path, (len(payload), 1, limit, 0)) as host:
        result = invoke(tmp_path, payload, cap=len(payload), host=host)
        assert_quota_refusal(
            result, host, resource="output_bytes", artifact="spool",
            used=limit, requested=1, limit=limit)
        assert host.denials[0].before == host.denials[0].after == payload[:limit]
        assert os.fstat(host.spool_fd).st_size == limit
        usage = host.ledger.snapshot()
        assert usage[host.q.Resource.SCRATCH_BYTES - 1].used == 0
        assert usage[host.q.Resource.SCRATCH_FILES - 1].used == 0
        assert usage[host.q.Resource.OUTPUT_BYTES - 1].committed == limit


def test_spool_file_zero_refuses_before_creation(quota_host, tmp_path, payload):
    with quota_host(tmp_path, (len(payload), 0, len(payload), 0)) as host:
        result = invoke(tmp_path, payload, host=host)
        assert_quota_refusal(
            result, host, resource="scratch_files", artifact="spool",
            used=0, requested=1, limit=0)
        assert host.spool_fd is None
        assert host.denials[0].before is host.denials[0].after is None
        assert all(item.used == 0 for item in host.ledger.snapshot())


@pytest.mark.parametrize("stdout_mode", (False, True))
def test_local_cap_precedes_aggregate_byte_denial(quota_host, tmp_path, payload, stdout_mode):
    # B=1 refuses before even a header can be parsed or spooled, regardless
    # of how the input pipe splits the rest of this sink.
    cap = len(payload[:1])
    with quota_host(tmp_path, (0, 1, 0, 0)) as host:
        result = invoke(tmp_path, payload, stdout_mode=stdout_mode, cap=cap, host=host)
        assert result.returncode == 8, (result.stdout, result.stderr)
        assert result.stdout == b""
        assert result.receipt_bytes == receipt_bytes(
            payload, complete=False, sink_bytes=cap + 1, cap=cap)
        assert host.ledger.refusal is None and host.denials == []
        assert not result.out.exists() and list(result.spool.iterdir()) == []
        assert all(item.used == 0 for item in host.ledger.snapshot())
        assert all(amounts.scratch_bytes == amounts.output_bytes == 0
                   for amounts, _, _ in host.reservations)


@pytest.mark.parametrize("empty", (False, True))
def test_file_local_zero_semantics(quota_host, tmp_path, payload, empty):
    with quota_host(tmp_path, (0, 1, 0, 0)) as host:
        result = invoke(tmp_path, b"" if empty else payload, cap=0, host=host)
        assert result.returncode == (6 if empty else 8), result.stderr
        assert result.receipt_bytes == receipt_bytes(
            payload, complete=False, sink_bytes=0 if empty else 1,
            cap=None if empty else 0)
        assert host.ledger.refusal is None and host.denials == []
        assert not result.out.exists() and list(result.spool.iterdir()) == []
        assert all(item.used == 0 for item in host.ledger.snapshot())


def test_stdout_local_zero_is_still_usage_error(quota_host, tmp_path, payload):
    with quota_host(tmp_path, (0, 0, 0, 0)) as host:
        result = invoke(tmp_path, payload, stdout_mode=True, cap=0,
                        host=host, receipt_channel="file")
        assert result.returncode == 2 and result.stdout == b""
        assert result.receipt_bytes == b"" and result.receipt is None
        assert b"requires a positive --max-bytes" in result.stderr
        assert host.requests == [] and host.ledger.refusal is None


@pytest.mark.parametrize("stdout_mode", (False, True))
def test_managed_output_zero_is_literal_zero(quota_host, tmp_path, payload, stdout_mode):
    with quota_host(tmp_path, (len(payload), 1, 0, 0)) as host:
        result = invoke(tmp_path, payload, stdout_mode=stdout_mode,
                        cap=len(payload), host=host)
        assert_quota_refusal(
            result, host, resource="output_bytes",
            artifact="sink" if stdout_mode else "spool", used=0, requested=1, limit=0)
        assert result.stdout == b""
        if not stdout_mode:
            assert host.spool_bytes() == b""
        assert all(item.used == 0 for item in host.ledger.snapshot())


@pytest.mark.parametrize("stdout_mode", (False, True))
@pytest.mark.parametrize("outcome", ("success", "overflow", "zero"))
def test_unmanaged_frozen_bytes_and_no_channel_traffic(tmp_path, payload, stdout_mode, outcome):
    cap = len(payload) if outcome == "success" else (1 if outcome == "overflow" else 0)
    channels = (*socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET),
                *socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET))
    try:
        result = invoke(
            tmp_path, payload, stdout_mode=stdout_mode, cap=cap,
            receipt_channel="file", policy_only=True,
            watched=(channels[1].fileno(), channels[3].fileno()))
        if stdout_mode and cap == 0:
            assert result.returncode == 2
            assert result.stdout == result.receipt_bytes == b""
            assert b"requires a positive --max-bytes" in result.stderr
        elif outcome == "success":
            assert result.returncode == 0 and result.stderr == b""
            assert result.receipt_bytes == receipt_bytes(payload, complete=True)
            assert result.stdout == (payload if stdout_mode else b"")
            if not stdout_mode:
                assert result.out.read_bytes() == payload
        else:
            assert result.returncode == 8 and result.stdout == b""
            assert result.stderr == b"pineforge-sink-finalize: stream exceeds --max-bytes\n"
            assert result.receipt_bytes == receipt_bytes(
                payload, complete=False, sink_bytes=cap + 1, cap=cap)
            assert not result.out.exists()
        assert list(result.spool.iterdir()) == []
        for server in (channels[0], channels[2]):
            server.setblocking(False)
            with pytest.raises(BlockingIOError):
                server.recv(1)
    finally:
        for channel in channels:
            channel.close()
