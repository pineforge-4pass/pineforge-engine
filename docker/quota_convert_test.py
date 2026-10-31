"""Real converter, retained files and credential-checked Python quota ledger.

The reference report and multiplex geometry are derived from generated input,
independently of converter/store constants. Fresh module packages capture each
host marker once, without replacing converter or ledger functions. Optional
host hooks are passed only when supported so the old converter reaches the
behavioral assertions (publishing over budget), rather than a signature error.
"""
from __future__ import annotations

from contextlib import contextmanager
import errno
import hashlib
import importlib
import inspect
import itertools
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import types

import pytest


HERE = Path(__file__).resolve().parent
MARKERS = ("PF_RUN_QUOTA_MODE", "PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD")
LIMITS = ("PF_RUN_SCRATCH_BYTES", "PF_RUN_SCRATCH_FILES",
          "PF_RUN_OUTPUT_BYTES", "PF_RUN_DESCRIPTOR_LIMIT")
PACKAGES = itertools.count()


def encoded(value, *, ascii=True):
    return json.dumps(value, ensure_ascii=ascii, separators=(",", ":"),
                      allow_nan=False).encode("utf-8")


def fd_entries():
    result = set()
    for name in os.listdir("/proc/self/fd"):
        descriptor = int(name)
        try:
            os.fstat(descriptor)
        except OSError as error:
            if error.errno == errno.EBADF:
                continue
            raise
        result.add(descriptor)
    return result


@contextmanager
def fresh_modules():
    # Real modules, isolated because the quota module captures at import.
    name = "_quota_convert_modules_" + str(next(PACKAGES))
    package = types.ModuleType(name)
    package.__path__ = [str(HERE)]
    sys.modules[name] = package
    try:
        q = importlib.import_module(name + ".run_quota")
        converter = importlib.import_module(name + ".pineforge_sink_to_json")
        yield converter, q
    finally:
        for key in tuple(sys.modules):
            if key == name or key.startswith(name + "."):
                del sys.modules[key]


@pytest.fixture
def quota_host(monkeypatch):
    """Same real socket/ledger fixture approach as quota_columns_test."""
    @contextmanager
    def host(limits, *, serving=True):
        native_server, native = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        server, endpoint = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        ledger, thread = None, None
        errors, confirmed, traffic = [], {}, {"messages": 0}
        try:
            with monkeypatch.context() as patch:
                patch.setenv(MARKERS[0], "managed")
                patch.setenv(MARKERS[1], str(native.fileno()))
                patch.setenv(MARKERS[2], str(endpoint.fileno()))
                with fresh_modules() as (converter, q):
                    ledger = q.RunQuota(q.Amounts(*limits))
                    actor = ledger.open_owner(
                        pid=os.getpid(), uid=os.getuid(), gid=os.getgid(),
                        generation=1, role=q.Role.CONVERTER, soft_nofile=64)
                    q.prepare_socket(server)

                    def profile(frame, event, value):
                        if event == "call" and frame.f_code is q.Message.decode.__func__.__code__:
                            traffic["messages"] += 1

                    def serve():
                        previous = sys.getprofile()
                        sys.setprofile(profile)
                        try:
                            q.serve(ledger, server, actor)
                        except BaseException as error:
                            errors.append(error)
                            server.close()
                        finally:
                            sys.setprofile(previous)

                    def confirm(object_id, generation, descriptor):
                        # The host owns the observation descriptor, just as
                        # its independent creation helper would. No self-claim
                        # of device/inode is accepted in place of confirmation.
                        observation = os.dup(descriptor)
                        try:
                            identity = ledger.confirm_object(
                                actor, object_id, generation, observation)
                            confirmed[identity] = descriptor
                        finally:
                            os.close(observation)

                    if serving:
                        thread = threading.Thread(target=serve, daemon=True)
                        thread.start()
                    yield types.SimpleNamespace(
                        converter=converter, q=q, ledger=ledger, confirm=confirm,
                        confirmed=confirmed, endpoint=endpoint, traffic=traffic,
                        servers=(native_server, server))
        finally:
            endpoint.close()
            native.close()
            try:
                if thread is not None:
                    thread.join(10)
                    assert not thread.is_alive(), "quota server did not stop"
                assert not errors, errors
            finally:
                server.close()
                native_server.close()
                if ledger is not None:
                    ledger.close()
    return host


def descriptor(tag, triples):
    return {"type": tag, "fields": [entry[0] for entry in triples],
            "types": [entry[1] for entry in triples], "units": [entry[2] for entry in triples]}


def retained_input(directory, *, columns=5, count=8, outputs=True, metadata=True,
                   message='雪 "quote" \\\n\t🚀'):
    directory.mkdir()
    trade_fields = (
        ("n", "i64", ""), ("side", "str", ""),
        ("entry_time", "i64", "unix_ms"), ("exit_time", "i64", "unix_ms"),
        ("entry_price", "f64?", "quote_ccy"), ("exit_price", "f64?", "quote_ccy"),
        ("qty", "f64?", "qty"), ("pnl", "f64?", "account_ccy"),
        ("pnl_pct", "f64?", "percent"), ("max_runup", "f64?", "account_ccy"),
        ("max_drawdown", "f64?", "account_ccy"), ("commission", "f64?", "account_ccy"),
        ("entry_bar_index", "i64", "script_bar"), ("exit_bar_index", "i64", "script_bar"),
        ("open_at_end", "bool", ""), ("entry_incarnation", "u64", ""),
        ("close_cause", "i64", ""), ("entry_id", "str", ""),
        ("exit_id", "str", ""), ("exit_comment", "str", ""),
    )
    records = [
        descriptor("E", (("time_ms", "i64", "unix_ms"), ("equity", "f64?", "account_ccy"),
                         ("open_profit", "f64?", "account_ccy"))),
        descriptor("T", trade_fields),
    ]
    if outputs:
        records += [
            descriptor("O", (("row", "i64", ""), ("open_ms", "i64?", "unix_ms"),
                             ("close_ms", "i64?", "unix_ms")) +
                       tuple((f"v{slot}", "f64?", "") for slot in range(columns))),
            descriptor("V", (
                ("sequence", "u64", ""), ("output_index", "i64", ""),
                ("bar_index", "i64", ""), ("bar_open_ms", "i64?", "unix_ms"),
                ("bar_close_ms", "i64?", "unix_ms"), ("ordinal_in_bar", "i64", ""),
                ("phase", "i64", ""), ("confirmed", "i64", ""), ("value", "f64?", ""),
                ("message_hash64", "hex64", ""), ("message", "str?", ""))),
            descriptor("K", (("c0", "f64?", ""), ("c1", "f64?", ""))),
        ]
    header = {"schema": "pf-report-sink", "version": 1, "run_id": "conversion-fixture",
              "engine": "test", "records": records}
    rows = []
    for index in range(count):
        timestamp = 1700000000000 + index * 60000
        pnl = (2.5, -1.25, -0.0)[index % 3]
        rows += [
            ["E", timestamp, 10000.0 + index, -0.0],
            ["T", index + 1, "long" if index % 2 else "short", timestamp, timestamp + 60000,
             100.0, 101.0, 2.0, pnl, pnl / 2.0, 4.0, -2.0, 0.25,
             index, index + 1, index == count - 1, index + 1, 1, "entry", "exit", "closed"],
        ]
        if outputs:
            values = [(-0.0, None, 1e-300, 4294967295.0)[(slot + index) % 4]
                      for slot in range(columns)]
            rows += [
                ["O", index, timestamp if index % 2 else None, timestamp + 60000, *values],
                ["V", index + 1, columns + 1, index, timestamp, timestamp + 60000,
                 0, (0, 1, 2, 9)[index % 4], 1, pnl, format(index, "016x"),
                 message if index % 2 else None],
            ]
    if outputs:
        rows.append(["K", 50.0, 4294967295.0])
    source = {}
    if outputs and metadata:
        source["outputs"] = {"manifest": {
            "outputs": [{"index": slot, "id": f"p{slot}", "kind": "plot"}
                        for slot in range(columns)] + [
                {"index": columns, "id": "level", "kind": "hline", "price": {"constant": 0}},
                {"index": columns + 1, "id": "alert", "kind": "alert", "freq": "once"}],
            "series": [{"slot": slot, "output": f"p{slot}",
                        "encoding": "rgba-u32" if slot % 2 else "f64"}
                       for slot in range(columns)],
            "constants": [{"index": 0}, {"index": 1, "encoding": "rgba-u32"}],
        }}
    sink, presentation = directory / "retained.sink", directory / "stdout.json"
    counts = {entry["type"]: 0 for entry in records}
    digest = hashlib.sha256()
    with sink.open("wb") as destination:
        for value in (header, *rows):
            line = encoded(value, ascii=False) + b"\n"
            destination.write(line)
            digest.update(line)
            if type(value) is list:
                counts[value[0]] += 1
        destination.write(encoded({"trailer": "pf-report-sink", "version": 1,
                                   "counts": counts, "sha256": digest.hexdigest()}) + b"\n")
    presentation.write_bytes(encoded(source) + b"\n")
    return types.SimpleNamespace(sink=sink, presentation=presentation,
                                 header=header, rows=rows, source=source)


def reference_report(case):
    """Small independent oracle from the input schema and presentation."""
    populations = {entry["type"]: [] for entry in case.header["records"]}
    for row in case.rows:
        populations[row[0]].append(row[1:])
    trade_fields = next(entry["fields"] for entry in case.header["records"] if entry["type"] == "T")
    trades = [dict(zip(trade_fields[:16], row[:16])) for row in populations["T"]]
    equity = [dict(zip(("time_ms", "equity", "open_profit"), row)) for row in populations["E"]]
    pnls = [trade["pnl"] for trade in trades]
    total, peak, drawdown = 0.0, 0.0, 0.0
    for pnl in pnls:
        total += pnl
        peak = max(peak, total)
        drawdown = min(drawdown, total - peak)
    wins = sum(pnl > 0 for pnl in pnls)
    summary = dict(total_trades=len(trades), wins=wins, losses=sum(pnl < 0 for pnl in pnls),
                   win_rate_pct=round(wins / len(pnls) * 100.0 if pnls else 0.0, 4),
                   net_pnl=total, avg_trade=total / len(pnls) if pnls else 0.0,
                   best_trade=max(pnls) if pnls else 0.0,
                   worst_trade=min(pnls) if pnls else 0.0, max_drawdown=drawdown)
    metrics = {}
    for name, selected in (("all", trades),
                           ("longs", [row for row in trades if row["side"] == "long"]),
                           ("shorts", [row for row in trades if row["side"] == "short"])):
        streaks = []
        for positive in (True, False):
            streaks.append(max((len(list(group)) for sign, group in itertools.groupby(
                row["pnl"] > 0 if positive else row["pnl"] < 0 for row in selected) if sign),
                default=0))
        metrics[name] = dict(
            num_trades=len(selected), num_wins=sum(row["pnl"] > 0 for row in selected),
            num_losses=sum(row["pnl"] < 0 for row in selected),
            num_even=sum(row["pnl"] == 0 for row in selected),
            max_consecutive_wins=streaks[0], max_consecutive_losses=streaks[1])
    document = dict(engine="pineforge", summary=summary, trades=trades,
                    metrics=metrics, equity_curve=equity)
    if "O" in populations:
        manifest = case.source.get("outputs", {}).get("manifest", {})
        identities = {entry["index"]: entry for entry in manifest.get("outputs", [])}
        series = {entry["slot"]: entry for entry in manifest.get("series", [])}
        constant_encodings = {entry["index"]: entry.get("encoding")
                              for entry in manifest.get("constants", [])}

        def value(number, encoding):
            return int(number) if number is not None and encoding == "rgba-u32" else number

        constants = [value(number, constant_encodings.get(index))
                     for index, number in enumerate(populations["K"][0])]
        events = []
        for row in populations["V"]:
            identity = identities.get(row[1], {})
            event = dict(sequence=row[0], output=identity.get("id"), bar_index=row[2],
                         bar_open_ms=row[3], bar_close_ms=row[4], ordinal_in_bar=row[5],
                         phase={0: "batch", 1: "warmup", 2: "realtime"}.get(row[6], str(row[6])),
                         value=row[8], message=row[10])
            if identity.get("kind") == "alert":
                event["freq"] = identity.get("freq")
            events.append(event)
        block = dict(schema_version="pineforge-outputs/v1", message_format="pineforge/v1")
        if manifest:
            block["manifest"] = manifest
        slots = len(next(entry["fields"] for entry in case.header["records"]
                         if entry["type"] == "O")) - len(("row", "open_ms", "close_ms"))
        block.update(
            bars={key: [row[index] for row in populations["O"]]
                  for key, index in (("open_ms", 1), ("close_ms", 2))},
            series=[dict(slot=slot, output=series.get(slot, {}).get("output"),
                         values=[value(row[3 + slot], series.get(slot, {}).get("encoding"))
                                 for row in populations["O"]]) for slot in range(slots)],
            constants=constants,
            hlines=[dict(output=entry["id"], price=constants[entry["price"]["constant"]])
                    for entry in identities.values() if entry.get("kind") == "hline"],
            events=events)
        document["outputs"] = block
    return document


def multiplex_cost(document):
    # Independent geometry from the specified <4*u64> index and <u64,u32,u32>
    # block header. No product constants or precomputed expected byte counts.
    outputs = document.get("outputs", {})
    streams = [document["trades"], document["equity_curve"],
               outputs.get("bars", {}).get("open_ms", []),
               outputs.get("bars", {}).get("close_ms", []), outputs.get("events", [])]
    streams += [entry["values"] for entry in outputs.get("series", [])]
    payloads = [b",".join(encoded(item) for item in stream) for stream in streams]
    block_size = 64 * 1024
    block_header = struct.calcsize("<QII")
    index = struct.calcsize("<QQQQ") * len(streams)
    blocks = sum((len(payload) + block_size - block_header - 1) //
                 (block_size - block_header) for payload in payloads)
    scratch = ((index + block_size - 1) // block_size + blocks) * block_size
    return types.SimpleNamespace(
        output=index + blocks * block_header + sum(map(len, payloads)),
        scratch=scratch, index_extent=((index + block_size - 1) // block_size) * block_size)


def arguments(case, *, cap=None):
    return ["--in", str(case.sink), "--stdout-json", str(case.presentation),
            "--out", "-", "--max-bytes", str(case.sink.stat().st_size if cap is None else cap),
            "--run-id", case.header["run_id"]]


def invoke(converter, case, capture, *, host=None, observe=None, cap=None):
    hooks = {}
    supported = inspect.signature(converter.main).parameters
    if host is not None and "confirm_backing" in supported:
        hooks["confirm_backing"] = host.confirm
    if observe is not None and "observe" in supported:
        hooks["observe"] = observe
    code = converter.main(arguments(case, cap=cap), **hooks)
    captured = capture.readouterr()
    return types.SimpleNamespace(returncode=code, stdout=captured.out, stderr=captured.err)


def unmanaged(case, capture, monkeypatch):
    with monkeypatch.context() as patch:
        for name in MARKERS:
            patch.delenv(name, raising=False)
        with fresh_modules() as (converter, _):
            result = invoke(converter, case, capture)
    assert result.returncode == 0, result.stderr
    assert result.stdout == encoded(reference_report(case)) + b"\n"
    return result


def terminal(result):
    return json.loads(result.stderr.splitlines()[-1])


def prior_output(host, count):
    q = host.q
    owner = host.ledger.open_owner(pid=os.getpid(), uid=os.getuid(), gid=os.getgid(),
                                  generation=1, role=q.Role.WORKER, soft_nofile=64)
    ticket = host.ledger.reserve(owner, q.Amounts(output_bytes=count),
                                 stage=q.Stage.EXECUTION, artifact=q.Artifact.SINK)
    ticket.commit(q.Amounts(output_bytes=count), 0)
    ticket.cancel()
    host.ledger.finish(owner)


def assert_output_refusal(result, *, used, requested, limit):
    assert result.returncode == 3, result.stderr
    assert result.stdout == b""
    line = terminal(result)
    assert line["event"] == "quota_refused" and line["exit_code"] == result.returncode
    assert line["sink_verified"] is True and line["billing"] == "normal"
    assert line["code"] == "run_resource_quota_exceeded"
    assert line["args"] == dict(stage="converter", resource="output_bytes", artifact="stdout",
                                used=used, requested=requested, limit=limit, peak=used)


def test_expansion_refuses_before_first_stdout_byte(quota_host, tmp_path, capfdbinary, monkeypatch):
    case = retained_input(tmp_path / "expanded")
    baseline = unmanaged(case, capfdbinary, monkeypatch)
    assert len(baseline.stdout) > case.sink.stat().st_size
    cost = multiplex_cost(reference_report(case))
    assert cost.output < len(baseline.stdout)
    limit = cost.output + (len(baseline.stdout) - cost.output) // 2
    assert cost.output <= limit < len(baseline.stdout)
    with quota_host((cost.scratch, 2, limit, 2)) as host:
        result = invoke(host.converter, case, capfdbinary, host=host)
        assert_output_refusal(result, used=cost.output, requested=len(baseline.stdout), limit=limit)
        assert host.ledger.refusal.arguments == terminal(result)["args"]


def test_prior_output_joins_converter_admission(quota_host, tmp_path, capfdbinary, monkeypatch):
    case = retained_input(tmp_path / "aggregate")
    baseline = unmanaged(case, capfdbinary, monkeypatch)
    cost = multiplex_cost(reference_report(case))
    already_produced = case.sink.stat().st_size
    limit = already_produced + cost.output + len(baseline.stdout) - 1
    with quota_host((cost.scratch, 2, limit, 2)) as host:
        prior_output(host, already_produced)
        result = invoke(host.converter, case, capfdbinary, host=host)
        assert_output_refusal(result, used=already_produced + cost.output,
                              requested=len(baseline.stdout), limit=limit)
        assert host.ledger.refusal.arguments == terminal(result)["args"]
        usage = host.ledger.snapshot()
        assert usage[host.q.Resource.OUTPUT_BYTES - 1].committed == already_produced + cost.output
        assert all(item.reserved == 0 for item in usage)
        assert all(usage[resource - 1].used == 0 for resource in (
            host.q.Resource.SCRATCH_BYTES, host.q.Resource.SCRATCH_FILES, host.q.Resource.DESCRIPTORS))


@pytest.mark.parametrize("shortfall", (0, 1))
@pytest.mark.parametrize("shape", ("outputs", "no-outputs", "unlabelled", "empty"))
def test_exact_fit_and_one_byte_over(quota_host, tmp_path, capfdbinary, monkeypatch, shortfall, shape):
    case = retained_input(tmp_path / "fit", outputs=shape != "no-outputs",
                          metadata=shape != "unlabelled", count=0 if shape == "empty" else 3)
    baseline = unmanaged(case, capfdbinary, monkeypatch)
    cost = multiplex_cost(reference_report(case))
    limit = cost.output + len(baseline.stdout) - shortfall
    phases = []
    with quota_host((cost.scratch, 2, limit, 2)) as host:
        def observe(phase, sink, columns):
            usage = host.ledger.snapshot()[host.q.Resource.OUTPUT_BYTES - 1]
            phases.append((phase, usage.committed, usage.reserved))
            if phase in ("prepared", "admitted"):
                assert os.fstat(1).st_size == 0, "stdout preceded exact output admission"

        result = invoke(host.converter, case, capfdbinary, host=host, observe=observe)
        if shortfall:
            assert_output_refusal(result, used=cost.output, requested=len(baseline.stdout), limit=limit)
        else:
            assert result.returncode == baseline.returncode == 0, result.stderr
            assert result.stdout == baseline.stdout
            assert phases == [("prepared", cost.output, 0),
                              ("admitted", cost.output, len(baseline.stdout)),
                              ("emitted", limit, 0)]
            assert host.ledger.refusal is None
        usage = host.ledger.snapshot()
        assert usage[host.q.Resource.OUTPUT_BYTES - 1].committed == (
            cost.output if shortfall else limit)
        assert all(item.reserved == 0 for item in usage)


def test_local_input_cap_precedes_quota_attachment(quota_host, tmp_path, capfdbinary):
    case = retained_input(tmp_path / "input-cap")
    with quota_host((0, 0, 0, 0)) as host:
        result = invoke(host.converter, case, capfdbinary, host=host, cap=case.sink.stat().st_size - 1)
        assert result.returncode == 8 and result.stdout == b""
        line = terminal(result)
        assert line["event"] == "sink_refused" and line["sink_verified"] is False
        assert line["detail"] == "retained sink exceeds --max-bytes"
        assert "code" not in line and "args" not in line
        assert host.ledger.refusal is None and not host.confirmed
        assert all(item.used == 0 for item in host.ledger.snapshot())
        assert host.traffic["messages"] == 0


def test_column_scratch_uses_the_same_client(quota_host, tmp_path, capfdbinary):
    case = retained_input(tmp_path / "scratch")
    cost = multiplex_cost(reference_report(case))
    with quota_host((0, 2, cost.output + len(encoded(reference_report(case))) + 1, 2)) as host:
        before = fd_entries()
        result = invoke(host.converter, case, capfdbinary, host=host)
        assert result.returncode == 3 and result.stdout == b""
        line = terminal(result)
        assert line["code"] == "run_resource_quota_exceeded"
        assert line["args"] == dict(stage="converter", resource="scratch_bytes",
                                    artifact="column_index", used=0,
                                    requested=cost.index_extent, limit=0, peak=0)
        assert fd_entries() == before and not host.confirmed
        assert all(item.used == 0 for item in host.ledger.snapshot())


def test_many_columns_keep_assembly_descriptors_windows_and_traversals_constant(
        quota_host, tmp_path, capfdbinary, monkeypatch):
    counts = []
    for columns in (5, 500):
        case = retained_input(tmp_path / str(columns), columns=columns, count=2)
        baseline = unmanaged(case, capfdbinary, monkeypatch)
        cost = multiplex_cost(reference_report(case))
        with quota_host((cost.scratch, 2, cost.output + len(baseline.stdout), 2)) as host:
            before = fd_entries()
            assemblies, traversals, windows = [], set(), []

            def profile(frame, event, value):
                if event == "call" and frame.f_code is host.converter._Sink.records.__code__:
                    traversals.add(frame)
                if (event == "call" and frame.f_code is host.converter._emit.__code__
                        and type(frame.f_locals["value"]) is dict
                        and "trades" in frame.f_locals["value"]):
                    assemblies.append(fd_entries())

            def observe(phase, sink, store):
                assert sink.traversals == 2
                buffers = tuple(window.buffer for window in store._data_windows) + (
                    store._index_window.buffer,)
                assert len(buffers) == 5
                assert all(len(buffer) == 64 * 1024 for buffer in buffers)
                windows.append(tuple(map(id, buffers)))

            previous = sys.getprofile()
            sys.setprofile(profile)
            try:
                result = invoke(host.converter, case, capfdbinary, host=host, observe=observe)
            finally:
                sys.setprofile(previous)
            assert result.returncode == 0 and result.stdout == baseline.stdout, result.stderr
            assert len(traversals) == 2, "assembly reopened the retained sink"
            assert len(assemblies) == 2 and assemblies[0] == assemblies[1]
            assert len(host.confirmed) == 2
            assert set(host.confirmed.values()) <= assemblies[0] - before
            # New fds are retained reader, private quota endpoint and the mux
            # pair. Metadata and the host's confirmation dup are already closed.
            assert len(assemblies[0] - before) == len(("reader", "quota", "index", "data"))
            assert len(windows) == 3 and windows[0] == windows[1] == windows[2]
            counts.append(len(assemblies[0]))
            traversals.clear()  # release profile frames retaining closed store objects
            assert fd_entries() == before
    assert counts[0] == counts[1]


def test_large_escaped_events_and_authoritative_arrays_keep_original_bytes(
        quota_host, tmp_path, capfdbinary, monkeypatch):
    case = retained_input(tmp_path / "escaped", count=3, message='雪🚀"\\\n' * 16000)
    expected = reference_report(case)
    # Equal metadata arrays force bounded decoding for _same, including an
    # event spanning several windows. Object key order must stay irrelevant.
    case.source.update(trades=[dict(reversed(tuple(row.items()))) for row in expected["trades"]],
                       equity_curve=expected["equity_curve"])
    for key in ("bars", "series", "events", "constants", "hlines"):
        case.source["outputs"][key] = expected["outputs"][key]
    case.presentation.write_bytes(encoded(case.source) + b"\n")
    baseline = unmanaged(case, capfdbinary, monkeypatch)
    cost = multiplex_cost(expected)
    with quota_host((cost.scratch, 2, cost.output + len(baseline.stdout), 2)) as host:
        result = invoke(host.converter, case, capfdbinary, host=host)
        assert result.returncode == 0 and result.stdout == baseline.stdout, result.stderr
        assert result.stderr == baseline.stderr
        assert terminal(result)["metadata_drops"] == 0


def test_unmanaged_bytes_exit_and_zero_ledger_traffic(
        quota_host, tmp_path, capfdbinary, monkeypatch):
    case = retained_input(tmp_path / "unmanaged")
    expected = encoded(reference_report(case)) + b"\n"
    # Keep real host sockets/ledger available, but omit every marker. No
    # service thread consumes unexpected messages, so traffic cannot hide.
    with quota_host((0, 0, 0, 0), serving=False) as host:
        before = host.ledger.snapshot()
        with monkeypatch.context() as patch:
            for name in MARKERS:
                patch.delenv(name, raising=False)
            for name in LIMITS:
                patch.setenv(name, "0")
            with fresh_modules() as (converter, q):
                assert q.mode() == "unmanaged"
                result = invoke(converter, case, capfdbinary)
            cli = subprocess.run(
                [sys.executable, "-I", "-B", "-S", str(HERE / "pineforge_sink_to_json.py"),
                 *arguments(case)], stdin=subprocess.DEVNULL, capture_output=True, check=False)
        assert result.returncode == cli.returncode == 0
        assert result.stdout == cli.stdout == expected
        assert result.stderr == cli.stderr
        assert host.ledger.snapshot() == before and host.ledger.refusal is None
        for channel in host.servers:
            with pytest.raises(BlockingIOError):
                channel.recv(1, socket.MSG_DONTWAIT)
