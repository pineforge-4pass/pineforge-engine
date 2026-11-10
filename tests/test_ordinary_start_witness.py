"""Ordinary START CLI and components, using real socket bytes/native fixture.

The independent parent plan follows ec8c7d0d. The recorder freezes socket bytes
at entry, then delegates to the real whole-archive engine fixture. No receiver,
deadline, forced receipt or fake report lives here. Execute on an admitted
worker only; automatic recovery uses an input-driven native fixture below.
"""
from __future__ import annotations

import contextlib
import csv
import ctypes
import fcntl
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
import run_json
import pf_bars

MODULE = None
RECOVERY_MODULE = None
BASELINE_HOST = None
KEYS = {
    "record", "version", "boundary", "launch_nonce", "nonce", "run_id_sha256",
    "call_serial", "handle_serial", "role", "role_index", "loaded_values_sha256",
    "bar_count", "first_timestamp", "last_timestamp", "call_options_sha256",
    "module_sha256", "engine_version", "start_monotonic_ns", "feed_bytes_sha256",
}
ROLES = ("warmup", "repeat", "body", "metrics_recovery")


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def reference_c(value):
    """Independent byte emitter from the parent pin; never calls product C."""
    if value is None:
        return b'["n"]'
    if type(value) is bool:
        return b'["b",' + (b"true" if value else b"false") + b"]"
    if type(value) is int:
        return b'["i","' + str(value).encode("ascii") + b'"]'
    if type(value) is float:
        return b'["f","' + struct.pack(">d", value).hex().encode("ascii") + b'"]'
    if type(value) in (bytes, str):
        raw = value if type(value) is bytes else value.encode("utf-8")
        return (b'["x","' if type(value) is bytes else b'["s","') + raw.hex().encode() + b'"]'
    if type(value) in (list, tuple):
        return b'["a",[' + b",".join(reference_c(item) for item in value) + b"]]"
    if type(value) is dict:
        pairs = sorted((key.encode("utf-8"), item) for key, item in value.items())
        return b'["m",[' + b",".join(
            b'["' + key.hex().encode() + b'",' + reference_c(item) + b"]"
            for key, item in pairs) + b"]]"
    raise TypeError(type(value).__name__)


def reference_options(role, rows, *, benchmark=False, warmup=0, repeats=0,
                      inputs=(), overrides=(), syminfo=None, extras=(),
                      inventory_sha=None, generated_sha=None, verify_bin="full"):
    """Parent plan for this fixture, independently constructed before launch."""
    values = digest(b"pineforge:ohlcv:barc-le:v1\0" + b"".join(
        struct.pack("<5dq", *row[1:], row[0]) for row in rows))
    return {
        "options_version": 1, "role": role,
        "ffi": {"input_tf": b"1", "script_tf": b"1", "bar_magnifier": 0,
                "magnifier_samples": 4, "magnifier_dist": 3},
        "settings": {"inputs": list(inputs), "overrides": list(overrides)},
        "syminfo": syminfo if syminfo is not None else {
            "file_bytes_sha256": None, "calls": []},
        "feeds": {"primary": {
            "loaded_values_sha256": values, "bar_count": len(rows),
            "first_timestamp": rows[0][0] if rows else None,
            "last_timestamp": rows[-1][0] if rows else None}, "extras": list(extras)},
        "time": {"trade_start_ms": None, "chart_tz": b""},
        "volume_weighted": False,
        "output": {"outputs": False, "profile": "unjailed", "report_mode": "FULL",
                   "sink_kind": "none", "sink_format": 1, "replay": False,
                   "replay_sink": False, "spill_dir": None, "verify_bin": verify_bin},
        "limits": {"max_output_series": None, "max_map_pairs": None,
                   "max_symbol_feeds": None, "max_inventory_bytes": None},
        "benchmark": {"enabled": benchmark, "warmup": warmup, "repeats": repeats},
        "metrics_attempt_policy": {"requested_mode": None,
                                   "force_legacy": role == "metrics_recovery"},
        "source_manifest": {"generated_cpp_sha256": generated_sha,
                            "transpiled_request": "",
                            "symbol_inventory_bytes_sha256": inventory_sha},
    }


def reference_options_digest(document):
    return digest(b"pineforge:ordinary-start:actual-options:v1\0" + reference_c(document))


class OptionsEncodingTests(unittest.TestCase):
    def test_independent_typed_byte_vectors_and_map_order(self):
        value = {"z": "\u00e9", "a": [None, True, False, (1 << 53) + 1, -0.0, b"\0\xff"]}
        expected = (
            b'["m",[["61",["a",[["n"],["b",true],["b",false],'
            b'["i","9007199254740993"],["f","8000000000000000"],["x","00ff"]]]],'
            b'["7a",["s","c3a9"]]]]')
        self.assertEqual(reference_c(value), expected)
        self.assertEqual(run_json._ordinary_start_canonical(value), expected)
        nan = struct.unpack(">d", bytes.fromhex("7ff8000000000042"))[0]
        self.assertEqual(run_json._ordinary_start_canonical(nan), b'["f","7ff8000000000042"]')
        self.assertEqual(run_json._ordinary_start_canonical({"\u00e9": 2, "z": 1}),
                         reference_c({"\u00e9": 2, "z": 1}))
        self.assertNotEqual(run_json._ordinary_start_canonical([b"a", b"b"]),
                            run_json._ordinary_start_canonical([b"b", b"a"]))
        for value in (object(), {1: "bad key"}, "\ud800"):
            with self.assertRaises(run_json._OrdinaryStartFailure):
                run_json._ordinary_start_canonical(value)

    def test_complete_options_preimage_and_each_required_component(self):
        import copy
        rows = [(1735776000000, 10.0, 11.0, 9.0, 10.5, 3.0)]
        base = reference_options("body", rows)
        self.assertEqual(run_json._ordinary_start_options_digest(base),
                         reference_options_digest(base))
        changes = {
            "role": "repeat",
            "ffi": {**base["ffi"], "input_tf": b"5"},
            "settings": {"inputs": [[b"length", b"1000.0"]], "overrides": []},
            "syminfo": {"file_bytes_sha256": digest(b"syminfo"),
                        "calls": [["strategy_set_syminfo_mintick", [0.25]]]},
            "feeds": {**base["feeds"], "extras": [{
                "symbol": b"VENUE:\xc3\xa9", "facts": [[b"mintick", b"0.25"]],
                "feeds": [{"timeframe": b"1D", "loaded_values_sha256": digest(b"extra"),
                           "bar_count": 0, "first_timestamp": None, "last_timestamp": None}]}]},
            "time": {"trade_start_ms": (1 << 53) + 7, "chart_tz": b"UTC"},
            "volume_weighted": True,
            "output": {**base["output"], "verify_bin": "sidecar"},
            "limits": {**base["limits"], "max_symbol_feeds": 0},
            "benchmark": {"enabled": True, "warmup": 1, "repeats": 2},
            "metrics_attempt_policy": {"requested_mode": "legacy", "force_legacy": False},
            "source_manifest": {**base["source_manifest"],
                                "generated_cpp_sha256": digest(b"actual source")},
        }
        for key, changed in changes.items():
            with self.subTest(component=key):
                actual = {**copy.deepcopy(base), key: changed}
                self.assertEqual(run_json._ordinary_start_options_digest(actual),
                                 reference_options_digest(actual))
                self.assertNotEqual(run_json._ordinary_start_options_digest(actual),
                                    reference_options_digest(base))
        for key in base:
            bad = dict(base)
            del bad[key]
            with self.subTest(missing=key):
                with self.assertRaises(run_json._OrdinaryStartFailure):
                    run_json._ordinary_start_options_digest(bad)
        for bad in ({**base, "options_version": 2},
                    {**base, "ffi": {**base["ffi"], "magnifier_samples": 1 << 31}},
                    {**base, "time": {**base["time"], "trade_start_ms": 1 << 63}}):
            with self.assertRaises(run_json._OrdinaryStartFailure):
                run_json._ordinary_start_options_digest(bad)


class Channel:
    def __init__(self, test):
        source, peer = socket.socketpair()
        # The actual inherited descriptor is parameterized and clear of 3/5.
        fd = fcntl.fcntl(source.fileno(), fcntl.F_DUPFD_CLOEXEC, 6)
        peer_fd = fcntl.fcntl(peer.fileno(), fcntl.F_DUPFD_CLOEXEC, 6)
        source.close()
        peer.close()
        self.source = socket.socket(fileno=fd)
        self.peer = socket.socket(fileno=peer_fd)
        self.peer.setblocking(False)
        test.addCleanup(self.source.close)
        test.addCleanup(self.peer.close)

    def drain(self):
        chunks = []
        while True:
            try:
                data = self.peer.recv(4096)
            except BlockingIOError:
                break
            if not data:
                break
            chunks.append(data)
        return b"".join(chunks)


class OrdinaryStartTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ordinary-start-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.channel = Channel(self)

    def chart(self, offset=0, count=12):
        rows = [
            (1735776000000 + offset + i * 60000,
             100 + i / 8, 101 + i / 8, 99 + i / 8, 100 + i / 8, 1 + i)
            for i in range(count)
        ]
        raw = ("timestamp,open,high,low,close,volume\n" + "".join(
            ",".join(str(value) for value in row) + "\n" for row in rows)).encode()
        path = self.root / ("bars-" + str(offset) + ".csv")
        path.write_bytes(raw)
        bars, n, fields = run_json._ordinary_start_load_csv(path, digest(raw))
        expected = digest(b"pineforge:ohlcv:barc-le:v1\0" + b"".join(
            struct.pack("<5dq", *row[1:], row[0]) for row in rows))
        self.assertEqual(fields["loaded_values_sha256"], expected)
        self.assertEqual(fields["feed_bytes_sha256"], digest(raw))
        return bars, n, fields, rows

    def fixture(self, fields, *, run_id="ordinary-\u00e9", options=None):
        # Opaque component input, deliberately NOT a proposed options serializer.
        # Real CLI rows below independently plan the full pinned O instead.
        options = options if options is not None else {
            role: (role + ":" + run_id).encode() for role in ROLES}
        module_bytes = MODULE.read_bytes()
        module = run_json._OrdinaryStartModule(MODULE, digest(module_bytes))
        self.addCleanup(module.close)
        lib = module.load()
        nonce = os.urandom(16).hex()
        identity = {
            "launch_nonce": nonce,
            "run_id_sha256": digest(os.fsencode(run_id)),
            "module_sha256": digest(module_bytes),
            "engine_version": run_json._ordinary_start_version(lib),
            **fields,
        }
        writer = run_json._OrdinaryStartWriter(
            self.channel.source.fileno(), identity,
            {role: digest(raw) for role, raw in options.items()})
        self.addCleanup(writer.close)
        return lib, module, writer, identity, options

    def actual(self, identity, options, role="body", serial=1, handle=1, index=0):
        return {
            "record": "ordinary_start", "version": 1,
            "boundary": "before_run_backtest_full", **identity,
            "nonce": identity["launch_nonce"],
            "call_serial": str(serial), "handle_serial": str(handle),
            "role": role, "role_index": str(index),
            "call_options_sha256": digest(options[role]),
        }

    def test_socket_snapshot_at_real_native_entry_and_all_actual_roles(self):
        for offset, count in ((0, 12), (60000, 7)):
            with self.subTest(offset=offset, count=count):
                self._exercise_native_roles(offset, count)

    def _exercise_native_roles(self, offset, requested_count):
        self.channel = Channel(self)
        bars, count, fields, rows = self.chart(offset=offset, count=requested_count)
        lib, module, writer, identity, options = self.fixture(
            fields, run_id=f"ordinary-{offset}-{requested_count}")
        native = lib.run_backtest_full
        entries = []

        def recorder(*arguments):
            frozen = self.channel.drain()  # first operation at callable entry
            entry = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            entries.append((frozen, entry, arguments[2],
                            arguments[1][0].timestamp, arguments[1][count - 1].close))
            return native(*arguments)

        lib.run_backtest_full = recorder  # same post-load rebinding as the App
        self.addCleanup(setattr, lib, "run_backtest_full", native)
        sequence = run_json._OrdinaryCallSequence()
        roles = ("warmup", "warmup", "repeat", "repeat", "body", "metrics_recovery")
        expected_indices = (0, 1, 0, 1, 0, 0)
        first_stamp = None
        for index, role in enumerate(roles):
            state = run_json.create_strategy(lib, run_json.uses_checked_settings(lib))
            sequence.created(state)
            report = run_json.ReportC()
            try:
                actual = {
                    **identity, "nonce": identity["launch_nonce"],
                    "call_options_sha256": run_json._ordinary_start_preimage_digest(
                        options[role], digest(options[role])),
                    "module_sha256": module.verify(),
                }
                sample = run_json._invoke_ordinary(
                    lib, (state, bars, count, b"1", b"1", 0, 4, 0, ctypes.byref(report)),
                    sequence, role, start=writer, identity=actual, timed=role == "repeat")
                self.assertIsNone(run_json.run_failure(lib, state))
                self.assertEqual(report.input_bars_processed, len(rows))
                if role == "repeat":
                    self.assertIs(type(sample), int)
                frozen, entered, passed_count, first, last_close = entries[-1]
                self.assertTrue(frozen.endswith(b"\n"), "missing START before native entry")
                self.assertEqual(frozen.count(b"\n"), 1)
                self.assertLessEqual(len(frozen), 2048)
                frame = json.loads(frozen)
                self.assertEqual(set(frame), KEYS)
                self.assertEqual(frame["call_serial"], str(index + 1))
                self.assertEqual(frame["handle_serial"], str(index + 1))
                self.assertEqual((frame["role"], frame["role_index"]),
                                 (role, str(expected_indices[index])))
                self.assertEqual((frame["bar_count"], passed_count), (len(rows), len(rows)))
                self.assertEqual(frame["first_timestamp"], str(first))
                self.assertEqual(last_close, rows[-1][4])
                self.assertLessEqual(int(frame["start_monotonic_ns"]), entered)
                if first_stamp is None:
                    first_stamp = int(frame["start_monotonic_ns"])
                self.assertEqual(writer.first_start_monotonic_ns, first_stamp)
            finally:
                run_json._free_sink_run(lib, state, report, None)
        self.assertEqual(len(entries), len(roles))
        self.assertEqual(self.channel.drain(), b"")

    def test_identity_changes_refuse_before_the_recorder(self):
        bars, count, fields, _ = self.chart()
        lib, module, writer, identity, options = self.fixture(fields)
        native = lib.run_backtest_full
        called = []

        def recorder(*arguments):
            called.append(arguments)
            return native(*arguments)

        lib.run_backtest_full = recorder
        self.addCleanup(setattr, lib, "run_backtest_full", native)
        mutations = {
            "launch_nonce": os.urandom(16).hex(),
            "run_id_sha256": digest(b"another complete run id"),
            "module_sha256": digest(b"other artifact"),
            "loaded_values_sha256": digest(b"other loaded values"),
            "feed_bytes_sha256": digest(b"other file bytes"),
            "call_options_sha256": digest(b"other options"),
            "bar_count": count + 1,
            "first_timestamp": str(bars[0].timestamp + 1),
            "last_timestamp": str(bars[count - 1].timestamp + 1),
            "engine_version": identity["engine_version"].rsplit(".", 1)[0] + "."
                              + str((int(identity["engine_version"].rsplit(".", 1)[1]) + 1)
                                    % (1 << 32)),
            "nonce": os.urandom(16).hex(),
        }
        for key, value in mutations.items():
            with self.subTest(field=key):
                frame = self.actual(identity, options)
                frame[key] = value
                with self.assertRaises(run_json._OrdinaryStartFailure):
                    writer.prepare(frame)
                self.assertEqual(self.channel.drain(), b"")
        self.assertEqual(called, [])
        # Actual buffer changes are observed, even if a caller supplies the old digest.
        bars[0].close += 0.5
        state = run_json.create_strategy(lib, run_json.uses_checked_settings(lib))
        sequence = run_json._OrdinaryCallSequence()
        sequence.created(state)
        report = run_json.ReportC()
        try:
            with self.assertRaises(run_json._OrdinaryStartFailure):
                run_json._invoke_ordinary(
                    lib, (state, bars, count, b"1", b"1", 0, 4, 0, ctypes.byref(report)),
                    sequence, "body", start=writer,
                    identity={**identity, "nonce": identity["launch_nonce"],
                              "call_options_sha256": digest(options["body"])})
            self.assertEqual(called, [])
        finally:
            run_json._free_sink_run(lib, state, report, None)

    def test_lossless_uint64_and_canonical_frame_refusals(self):
        _, _, fields, _ = self.chart()
        _, _, writer, identity, options = self.fixture(fields)
        frame = self.actual(identity, options, serial=(1 << 53) + 7)
        writer.handover(writer.prepare(frame))
        observed = json.loads(self.channel.drain())
        self.assertEqual(observed["call_serial"], str((1 << 53) + 7))
        upper = self.actual(identity, options, serial=(1 << 64) - 1, index=1)
        writer.handover(writer.prepare(upper))
        self.assertEqual(json.loads(self.channel.drain())["call_serial"],
                         "18446744073709551615")
        for value in ("0", "-1", "01", "+1", " 1", "1e3", "1.0",
                      "18446744073709551616", 1, True, "\u0661"):
            with self.subTest(serial=value):
                bad = self.actual(identity, options)
                bad["call_serial"] = value
                with self.assertRaises(run_json._OrdinaryStartFailure):
                    writer.prepare(bad)
        encoded = {**self.actual(identity, options), "start_monotonic_ns": "1"}
        for changes in ({"version": True}, {"bar_count": True}, {"role": "ordinary"},
                        {"module_sha256": "A" * 64}, {"first_timestamp": "-0"},
                        {"last_timestamp": str(1 << 63)}, {"bar_count": 1 << 31},
                        {"first_timestamp": None}, {"nonce": False},
                        {"engine_version": "01.2.3"}, {"engine_version": "1.2.3-pre"},
                        {"engine_version": "4294967296.0.0"}, {"extra": 0}):
            with self.subTest(changes=changes):
                with self.assertRaises(run_json._OrdinaryStartFailure):
                    run_json._ordinary_start_encode({**encoded, **changes})
        edge = {**encoded, "call_serial": str((1 << 64) - 1),
                "handle_serial": str((1 << 64) - 1), "role_index": str((1 << 64) - 1),
                "first_timestamp": str(-(1 << 63)), "last_timestamp": str((1 << 63) - 1)}
        self.assertEqual(json.loads(run_json._ordinary_start_encode(edge)), edge)
        del encoded["nonce"]
        with self.assertRaises(run_json._OrdinaryStartFailure):
            run_json._ordinary_start_encode(encoded)

    def test_duplicate_serial_handle_reuse_and_role_indices(self):
        _, _, fields, _ = self.chart()
        _, _, writer, identity, options = self.fixture(fields)
        writer.handover(writer.prepare(self.actual(identity, options, serial=9, handle=4)))
        self.channel.drain()
        for serial, handle, index in ((9, 4, 1), (8, 4, 1), (10, 3, 1), (10, 4, 0)):
            with self.subTest(serial=serial, handle=handle, index=index):
                with self.assertRaises(run_json._OrdinaryStartFailure):
                    writer.prepare(self.actual(identity, options, serial=serial,
                                               handle=handle, index=index))
        # Same actual handle is legal; a fresh handle must increase.
        writer.handover(writer.prepare(self.actual(identity, options, serial=10,
                                                   handle=4, index=1)))
        writer.handover(writer.prepare(self.actual(identity, options, serial=11,
                                                   handle=5, index=2)))
        self.assertEqual(len(self.channel.drain().splitlines()), 2)
        sequence = run_json._OrdinaryCallSequence()
        sequence.created(123)
        sequence.call_serial = (1 << 64) - 1
        with self.assertRaises(run_json._OrdinaryStartFailure):
            sequence.prepare(123, "body")

    def test_disconnect_prevents_native_started_bookkeeping(self):
        bars, count, fields, _ = self.chart()
        lib, _, writer, identity, options = self.fixture(fields)
        native = lib.run_backtest_full
        called = []
        lib.run_backtest_full = lambda *args: called.append(args)
        self.addCleanup(setattr, lib, "run_backtest_full", native)
        self.channel.peer.close()
        state = run_json.create_strategy(lib, run_json.uses_checked_settings(lib))
        sequence = run_json._OrdinaryCallSequence()
        sequence.created(state)
        abort = SimpleNamespace(started=False, check_before_start=lambda: None)
        report = run_json.ReportC()
        try:
            with self.assertRaises(run_json._OrdinaryStartFailure):
                run_json._invoke_ordinary(
                    lib, (state, bars, count, b"1", b"1", 0, 4, 0, ctypes.byref(report)),
                    sequence, "body", abort=abort, start=writer,
                    identity={**identity, "nonce": identity["launch_nonce"],
                              "call_options_sha256": digest(options["body"])})
            self.assertFalse(abort.started)
            self.assertEqual(sequence.call_serial, 0)
            self.assertEqual(called, [])
        finally:
            run_json._free_sink_run(lib, state, report, None)

    def test_descriptor_validation_and_borrowed_ownership(self):
        _, _, fields, _ = self.chart()
        _, _, writer, identity, options = self.fixture(fields)
        duplicate = os.dup(self.channel.source.fileno())
        self.addCleanup(os.close, duplicate)
        for fd, reserved in ((3, ()), (5, ()),
                             (self.channel.source.fileno(), (duplicate,))):
            with self.subTest(fd=fd):
                with self.assertRaises(run_json._OrdinaryStartFailure):
                    run_json._OrdinaryStartWriter(
                        fd, identity, {role: digest(raw) for role, raw in options.items()},
                        reserved_fds=reserved)
        left, right = socket.socketpair(type=socket.SOCK_DGRAM)
        self.addCleanup(left.close)
        self.addCleanup(right.close)
        with self.assertRaises(run_json._OrdinaryStartFailure):
            run_json._OrdinaryStartWriter(
                left.fileno(), identity, {role: digest(raw) for role, raw in options.items()})
        writer.close()
        os.fstat(self.channel.source.fileno())  # close detached, not closed, the borrowed fd

    def test_short_and_full_socket_handover_latches_failure(self):
        for fault in ("short", "full"):
            with self.subTest(fault=fault):
                self.channel = Channel(self)
                _, _, fields, _ = self.chart()
                _, _, writer, identity, options = self.fixture(fields)
                prepared = writer.prepare(self.actual(identity, options))
                if fault == "full":
                    sent = 0
                    for size in (65536, 4096, 256, 1):
                        while True:
                            try:
                                sent += self.channel.source.send(b"x" * size, socket.MSG_DONTWAIT)
                            except BlockingIOError:
                                break
                    self.assertGreater(sent, 0)
                    with self.assertRaises(run_json._OrdinaryStartFailure) as caught:
                        writer.handover(prepared)
                    self.assertEqual(caught.exception.reason, "would_block")
                    self.assertEqual(self.channel.drain(), b"x" * sent)
                else:
                    original_send = socket.socket.send
                    captured = []
                    descriptor = self.channel.source.fileno()

                    def short_send(endpoint, raw, flags=0):
                        if endpoint.fileno() == descriptor:
                            captured.append(bytes(raw))
                            return original_send(endpoint, raw[:len(raw) // 2], flags)
                        return original_send(endpoint, raw, flags)

                    with mock.patch.object(socket.socket, "send", short_send):
                        with self.assertRaises(run_json._OrdinaryStartFailure) as caught:
                            writer.handover(prepared)
                    self.assertEqual(caught.exception.reason, "short_write")
                    self.assertEqual(len(captured), 1)
                    self.assertEqual(self.channel.drain(), captured[0][:len(captured[0]) // 2])
                self.assertIsNone(writer.first_start_monotonic_ns)
                with self.assertRaises(run_json._OrdinaryStartFailure):
                    writer.handover(prepared)

    def test_pending_cancellation_precedes_handover_and_native_entry(self):
        bars, count, fields, _ = self.chart()
        lib, _, writer, identity, options = self.fixture(fields)
        native = lib.run_backtest_full
        called = []

        def recorder(*arguments):
            called.append(arguments)
            return native(*arguments)

        def cancelled():
            raise run_json._SigtermAbort.failure()

        lib.run_backtest_full = recorder
        self.addCleanup(setattr, lib, "run_backtest_full", native)
        abort = SimpleNamespace(started=False, check_before_start=cancelled)
        sequence = run_json._OrdinaryCallSequence()
        state = run_json.create_strategy(lib, run_json.uses_checked_settings(lib))
        sequence.created(state)
        report = run_json.ReportC()
        try:
            with self.assertRaises(run_json.RunFailure) as caught:
                run_json._invoke_ordinary(
                    lib, (state, bars, count, b"1", b"1", 0, 4, 0, ctypes.byref(report)),
                    sequence, "body", abort=abort, start=writer,
                    identity={**identity, "nonce": identity["launch_nonce"],
                              "call_options_sha256": digest(options["body"])})
            self.assertEqual(caught.exception.code, "run_aborted")
            self.assertFalse(abort.started)
            self.assertEqual(called, [])
            self.assertEqual(self.channel.drain(), b"")
            self.assertIsNone(writer.first_start_monotonic_ns)
        finally:
            run_json._free_sink_run(lib, state, report, None)

    def test_exact_ingress_component_and_malformed_context(self):
        role_options = {role: digest(role.encode()) for role in ROLES}
        _, _, fields, _ = self.chart()
        _, _, _, identity, _ = self.fixture(fields)
        context = json.dumps({**identity, "context_version": 1,
                              "nonce": identity["launch_nonce"],
                              "call_options_sha256_by_role": role_options})
        tokens, fd, manifest = run_json._ordinary_start_parse_ingress(
            ["--so", "strategy.so", "--_run-start-fd", "17"], context)
        self.assertEqual(tokens, ["--so", "strategy.so"])
        self.assertEqual(fd, 17)
        self.assertEqual(manifest["context_version"], 1)
        self.assertEqual(dict(manifest["call_options_sha256_by_role"]), role_options)
        with self.assertRaises(TypeError):
            manifest["context_version"] = 2
        self.assertEqual(run_json._ordinary_start_parse_ingress(["--so", "a"], None),
                         (["--so", "a"], None, None))
        for args, raw in ((["--_run-start-fd", "17"], None), ([], context),
                          (["--_run-start-fd", "17"], "{"),
                          (["--_run-start-fd", "3"], context),
                          (["--_run-start-fd", "17", "--_run-start-fd=18"], context),
                          (["--_run-start-fd=17"], '{"context_version":1,"context_version":1}'),
                          (["--_run-start-fd=17"], context[:-1] + ',"unbound":1e309}'),
                          (["--_run-start-fd=17"], context[:-1] + ',"unbound":"\\ud800"}'),
                          (["--_run-start-fd=17"], '{"context_version":true}')):
            with self.subTest(args=args, raw=raw):
                with self.assertRaises(run_json._OrdinaryStartFailure):
                    run_json._ordinary_start_parse_ingress(args, raw)
        # No App aliases or legacy-prefix expansion in the component.
        with self.assertRaises(run_json._OrdinaryStartFailure):
            run_json._ordinary_start_parse_ingress(["--_run-start-f", "17"], context)

    def test_same_descriptor_module_load_path_and_mutation_detection(self):
        original = MODULE.read_bytes()
        path = self.root / "strategy.so"
        path.write_bytes(original)
        owner = run_json._OrdinaryStartModule(path, digest(original))
        self.addCleanup(owner.close)
        replacement = self.root / "replacement.so"
        replacement.write_bytes(b"not the published artifact")
        replacement.replace(path)
        lib = owner.load()
        self.assertEqual(owner.verify(), digest(original))
        self.assertEqual(os.fstat(owner.fd).st_ino, owner.inode)
        self.assertIsInstance(run_json._ordinary_start_version(lib), str)
        self.assertEqual(os.readlink(owner.load_path), str(path) + " (deleted)")
        altered = self.root / "altered.so"
        altered.write_bytes(original)
        unopened = run_json._OrdinaryStartModule(altered, digest(original))
        self.addCleanup(unopened.close)
        with altered.open("r+b") as output:
            output.write(b"CHANGED")
        with self.assertRaises(run_json._OrdinaryStartFailure):
            unopened.verify()

    def test_run_id_feed_and_options_preimages_are_input_sensitive(self):
        for raw in (b"", b"run-\xff", b"a" * 4097):
            self.assertEqual(run_json._ordinary_start_run_id(os.fsdecode(raw)), digest(raw))
        path = self.root / "bad.csv"
        path.write_bytes(b"timestamp,open,high,low,close,volume\n1,1,2,0,1,3\n")
        with self.assertRaises(run_json._OrdinaryStartFailure):
            run_json._ordinary_start_load_csv(path, digest(b"another file"))
        for raw in (b"", b"\0options\xff", os.urandom(64)):
            self.assertEqual(run_json._ordinary_start_preimage_digest(raw, digest(raw)), digest(raw))
            with self.assertRaises(run_json._OrdinaryStartFailure):
                run_json._ordinary_start_preimage_digest(raw + b"x", digest(raw))
        bars, count, fields, _ = self.chart()
        changed, changed_count, changed_fields, _ = self.chart(offset=60000, count=7)
        self.assertNotEqual(fields["loaded_values_sha256"], changed_fields["loaded_values_sha256"])
        self.assertEqual((count, changed_count), (12, 7))
        self.assertEqual(run_json._ordinary_start_loaded_values(bars, 0)["first_timestamp"], None)
        self.assertEqual(run_json._ordinary_start_loaded_values(changed, 0)["last_timestamp"], None)
        for bad in (True, -1, (1 << 31), len(bars) + 1):
            with self.assertRaises(run_json._OrdinaryStartFailure):
                run_json._ordinary_start_loaded_values(bars, bad)

    def test_binary_identity_hashes_the_mapped_bytes_and_actual_values(self):
        _, _, csv_fields, _ = self.chart()
        binary = self.root / "bars.bin"
        pf_bars.convert_csv(self.root / "bars-0.csv", binary, symbol="fixture", tf="1")
        file_digest = digest(binary.read_bytes())
        chart, fields = run_json._ordinary_start_load_bin(binary, file_digest)
        try:
            self.assertEqual(fields["feed_bytes_sha256"], file_digest)
            self.assertEqual(fields["loaded_values_sha256"], csv_fields["loaded_values_sha256"])
            self.assertNotEqual(fields["loaded_values_sha256"], fields["feed_bytes_sha256"])
            self.assertEqual(fields["bar_count"], chart["n"])
        finally:
            chart.close()
        with self.assertRaises(run_json._OrdinaryStartFailure):
            run_json._ordinary_start_load_bin(binary, digest(b"different binary"))

    def test_runtime_version_requires_real_numeric_observations(self):
        with self.assertRaises(run_json._OrdinaryStartFailure):
            run_json._ordinary_start_version(SimpleNamespace())

    def test_recovery_preserves_transport_failure_and_earlier_diagnostic(self):
        # This row targets the Python recovery catch, not native success or a receipt.
        earlier = ("first native failure", "engine_invariant", {})
        run = run_json._MetricsOnlyRun.__new__(run_json._MetricsOnlyRun)
        run.active = (object(), None, run_json.ReportC())
        run.streaming = True
        run.lib = SimpleNamespace()
        run.checked = False
        run.close = lambda: None
        calls = []

        def invoke(state, report, *, role):
            calls.append(role)
            if role == "metrics_recovery":
                raise run_json._OrdinaryStartFailure("would_block")

        with mock.patch.object(run_json, "run_failure", return_value=earlier), \
                mock.patch.object(run_json, "_metrics_extent_receipt", return_value=None), \
                mock.patch.object(run_json, "_release_settings_receipt", return_value=None):
            with self.assertRaises(run_json._OrdinaryStartFailure) as caught:
                run.execute(invoke, lambda **kwargs: (object(), None), None, None)
        self.assertEqual(calls, ["body", "metrics_recovery"])
        self.assertEqual(caught.exception.earlier_failure, earlier)
        with mock.patch.object(run_json, "_write_diagnostic") as diagnostic, \
                mock.patch.object(run_json, "write_failure"):
            run_json._write_run_failure(caught.exception)
        self.assertIn("first native failure", diagnostic.call_args.args[0])
        self.assertIn("engine_invariant", diagnostic.call_args.args[0])

    def test_bare_main_matches_direct_native_reports_and_emits_zero_frames(self):
        _, _, _, _ = self.chart()
        chart = self.root / "bars-0.csv"
        args = ["--so", str(MODULE), "--ohlcv", str(chart),
                "--input-tf", "1", "--script-tf", "1"]

        def direct(lib, arguments, sequence, role, **kwargs):
            return lib.run_backtest_full(*arguments)

        def run_main():
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                status = run_json.main(args)
            self.assertEqual((status, err.getvalue()), (0, ""))
            document = json.loads(out.getvalue())
            self.assertEqual(document["summary"]["bars_processed"], 12)
            # This pre-existing volatile field is the only removed value.
            document.pop("elapsed_seconds")
            return json.dumps(document, ensure_ascii=True, separators=(",", ":")).encode()

        with mock.patch.object(run_json, "_invoke_ordinary", direct):
            before = run_main()
        after = run_main()
        self.assertEqual(after, before)
        self.assertEqual(self.channel.drain(), b"")


def ordinary_cli_child(tokens):
    """File-based recorder around the actual host; never produces a START."""
    import argparse
    separator = tokens.index("--")
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture-fd", type=int, required=True)
    parser.add_argument("--host-source", type=Path, required=True)
    control = parser.parse_args(tokens[:separator])
    spec = importlib.util.spec_from_file_location("_ordinary_cli_host", control.host_source)
    host = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = host
    spec.loader.exec_module(host)
    peer = socket.socket(fileno=control.capture_fd)
    peer.setblocking(False)
    original_load = host.load_strategy

    def drain():
        pieces = []
        while True:
            try:
                raw = peer.recv(4096)
            except BlockingIOError:
                break
            if not raw:
                break
            pieces.append(raw)
        return b"".join(pieces)

    def load_strategy(path):
        library = original_load(path)
        native = library.run_backtest_full

        def record(*arguments):
            frozen = drain()  # first operation at actual rebound native entry
            entered = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            rows = [
                [arguments[1][i].timestamp, arguments[1][i].open,
                 arguments[1][i].high, arguments[1][i].low, arguments[1][i].close,
                 arguments[1][i].volume] for i in range(arguments[2])]
            result = native(*arguments)
            observation = {"wire_hex": frozen.hex(), "entry_ns": str(entered),
                           "count": arguments[2], "rows": rows,
                           "reported_bars": int(arguments[8]._obj.input_bars_processed)}
            sys.stderr.write("ORDINARY_START_NATIVE_ENTRY=" + json.dumps(observation) + "\n")
            sys.stderr.flush()
            return result

        record.argtypes, record.restype = native.argtypes, native.restype
        library.run_backtest_full = record
        return library

    host.load_strategy = load_strategy
    try:
        return host.main(tokens[separator + 1:])
    finally:
        peer.close()


class OrdinaryCLITests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ordinary-start-cli-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        # Independent numeric query when the worker executes this fixture row.
        class Version(ctypes.Structure):
            _fields_ = [("major", ctypes.c_int), ("minor", ctypes.c_int),
                        ("patch", ctypes.c_int), ("commit_sha", ctypes.c_char_p)]
        library = ctypes.CDLL(str(MODULE))
        library.pf_version_get.restype = Version
        observed = library.pf_version_get()
        self.version = f"{observed.major}.{observed.minor}.{observed.patch}"

    def request(self, run_id, *, count=9, offset=0, bench=False, rich=False,
                metrics_mode=None, binary=False, verify_bin="full", feed=None,
                module=None):
        module = MODULE if module is None else module
        rows = [
            [1735776000000 + offset + i * 60000,
             50 + i / 4, 51 + i / 4, 49 + i / 4, 50 + i / 4, i + 1]
            for i in range(count)]
        raw = ("timestamp,open,high,low,close,volume\n" + "".join(
            ",".join(str(value) for value in row) + "\n" for row in rows)).encode()
        chart = self.root / f"chart-{offset}-{count}.csv"
        chart.write_bytes(raw)
        if feed is not None:
            chart = feed
            raw = chart.read_bytes()
            rows = [[int(row["timestamp"]), *[float(row[key]) for key in
                     ("open", "high", "low", "close", "volume")]]
                    for row in csv.DictReader(io.StringIO(raw.decode("ascii")))]
        chart_option = "--ohlcv"
        if binary:
            converted = chart.with_suffix(".bin")
            pf_bars.convert_csv(chart, converted, symbol="fixture", tf="1")
            chart, chart_option = converted, "--ohlcv-bin"
            raw = chart.read_bytes()
        argv = ["--so", str(module), chart_option, str(chart),
                "--input-tf=1", "--script-tf=1", "--run-id=" + run_id]
        if binary or verify_bin != "full":
            argv += ["--verify-bin=" + verify_bin]
        inputs, syminfo, extras = [], None, []
        inventory_sha = generated_sha = None
        if bench:
            argv += ["--bench", "--warmup=1", "--repeats=2"]
        if metrics_mode is not None:
            argv += ["--metrics-only=" + metrics_mode]
        if rich:
            # Existing normalizer: preserve JSON application order and numeric
            # Python string spelling; no production normalizer is an oracle.
            input_json = '{"gate_input":-1,"gate_bar":-1.0}'
            normalized = json.loads(input_json)
            inputs = [[key.encode(), str(value).encode()] for key, value in normalized.items()]
            argv += ["--inputs", input_json]
            syminfo_file = self.root / "syminfo.json"
            syminfo_raw = json.dumps({
                "mincontract": 0.25, "series_origin_ms": rows[0][0],
                "mintick": 0.125, "pointvalue": 2.0, "timezone": "UTC",
                "session": "0000-2359"}).encode()
            syminfo_file.write_bytes(syminfo_raw)
            syminfo = {
                "file_bytes_sha256": digest(syminfo_raw),
                "calls": [
                    ["strategy_set_syminfo_metadata", [b"qty_step", 0.25]],
                    ["strategy_set_syminfo_metadata", [b"mincontract", 0.25]],
                    ["strategy_set_syminfo_metadata", [b"series_origin_ms", float(rows[0][0])]],
                    ["strategy_set_syminfo_mintick", [0.125]],
                    ["strategy_set_syminfo_pointvalue", [2.0]],
                    ["strategy_set_syminfo_timezone", [b"UTC"]],
                    ["strategy_set_syminfo_session", [b"0000-2359"]],
                ]}
            argv += ["--syminfo", str(syminfo_file)]
            extra_file = self.root / "extra.csv"
            extra_rows = rows[:3]
            extra_file.write_bytes(
                b"timestamp,open,high,low,close,volume,time_close\n" + b"".join(
                    (",".join(str(value) for value in row)
                     + "," + str(row[0] + 60000) + "\n").encode() for row in extra_rows))
            inventory = self.root / "symbols.json"
            inventory_raw = json.dumps({"symbols": {"FIXTURE:\u00e9": {
                "syminfo": {"tickerid": "FIXTURE:\u00e9", "mintick": 0.25},
                "feeds": {"1": extra_file.name}}}}, ensure_ascii=False).encode()
            inventory.write_bytes(inventory_raw)
            inventory_sha = digest(inventory_raw)
            extras = [{
                "symbol": "FIXTURE:\u00e9".encode(),
                "facts": [[b"canonical", "FIXTURE:\u00e9".encode()], [b"mintick", b"0.25"]],
                "feeds": [{
                    "timeframe": b"1",
                    "loaded_values_sha256": digest(
                        b"pineforge:symbol-feed:barc-close-le:v1\0" + b"".join(
                            struct.pack("<5dqq", *row[1:], row[0], row[0] + 60000)
                            for row in extra_rows)),
                    "bar_count": len(extra_rows),
                    "first_timestamp": extra_rows[0][0],
                    "last_timestamp": extra_rows[-1][0]}]}]
            generated = ROOT / "tests" / "test_report_sink_l6b1.cpp"
            generated_sha = digest(generated.read_bytes())
            argv += ["--symbol-feeds", str(inventory), "--generated-cpp", str(generated)]
        documents = {}
        for role in ROLES:
            documents[role] = reference_options(
                role, rows, benchmark=bench, warmup=1 if bench else 0,
                repeats=2 if bench else 0, inputs=inputs, syminfo=syminfo, extras=extras,
                inventory_sha=inventory_sha, generated_sha=generated_sha,
                verify_bin=verify_bin)
            documents[role]["metrics_attempt_policy"]["requested_mode"] = metrics_mode
        nonce = os.urandom(16).hex()
        primary = documents["body"]["feeds"]["primary"]
        manifest = {
            "context_version": 1, "launch_nonce": nonce, "nonce": nonce,
            "run_id_sha256": digest(os.fsencode(run_id)),
            "module_sha256": digest(module.read_bytes()), "engine_version": self.version,
            "loaded_values_sha256": primary["loaded_values_sha256"],
            "feed_bytes_sha256": digest(raw), "bar_count": len(rows),
            "first_timestamp": str(rows[0][0]), "last_timestamp": str(rows[-1][0]),
            "call_options_sha256_by_role": {
                role: reference_options_digest(document) for role, document in documents.items()},
        }
        return SimpleNamespace(argv=argv, manifest=manifest, rows=rows, documents=documents)

    def launch(self, request, *, witnessed=True, orphan=None, extra=(), collide_phase=False):
        channel = Channel(self)
        baseline = BASELINE_HOST is not None and witnessed and orphan is None
        source = BASELINE_HOST if baseline else ROOT / "docker" / "run_json.py"
        argv = list(request.argv)
        environment = {}
        # On the pinned baseline omit ONLY unsupported ingress, never requests
        # or entry assertions. No future flag/setup failure can serve as RED.
        if witnessed and not baseline:
            if orphan != "fd":
                environment["PINEFORGE_ORDINARY_START_CONTEXT_V1"] = json.dumps(request.manifest)
            if orphan != "context":
                argv += ["--_run-start-fd=" + str(channel.source.fileno())]
        argv += list(extra)
        if collide_phase:
            argv += ["--run-phase-fd=" + str(channel.source.fileno())]
        command = [
            sys.executable, "-B", str(Path(__file__).resolve()), "--cli-child",
            "--capture-fd", str(channel.peer.fileno()), "--host-source", str(source),
            "--", *argv]
        process = subprocess.run(
            command, env=environment,
            pass_fds=(channel.source.fileno(), channel.peer.fileno()),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        entries, diagnostics = [], []
        for line in process.stderr.decode().splitlines():
            if line.startswith("ORDINARY_START_NATIVE_ENTRY="):
                entries.append(json.loads(line.partition("=")[2]))
            else:
                diagnostics.append(line)
        return SimpleNamespace(status=process.returncode, stdout=process.stdout,
                               diagnostics=diagnostics, entries=entries)

    def assert_start_refusal(self, result, reason, field=None):
        self.assertNotEqual(result.status, 0)
        self.assertEqual(result.entries, [])
        failure = json.loads(result.stdout)
        self.assertEqual(failure["code"], "harness_internal_error")
        expected = "ordinary START: " + reason
        if field is not None:
            expected += " (" + field + ")"
        self.assertEqual(failure["error"], expected)

    def assert_witnessed(self, request, result, roles):
        self.assertEqual(result.status, 0, result.diagnostics)
        self.assertEqual(result.diagnostics, [])
        self.assertEqual(len(result.entries), len(roles),
                         "prerequisite: genuine ordinary native call count")
        indices, frames = {role: 0 for role in ROLES}, []
        for number, (entry, role) in enumerate(zip(result.entries, roles), 1):
            self.assertEqual(entry["rows"], request.rows,
                             "prerequisite: actual native input rows")
            self.assertEqual(entry["reported_bars"], len(request.rows),
                             "prerequisite: actual native report count")
            raw = bytes.fromhex(entry["wire_hex"])
            self.assertTrue(raw.endswith(b"\n"),
                            "ORDINARY_START_MISSING_START_BEFORE_RUN_BACKTEST_FULL")
            self.assertEqual(raw.count(b"\n"), 1, "exactly one frame frozen at this entry")
            pairs = json.loads(raw, object_pairs_hook=lambda items: items)
            self.assertEqual(len(pairs), 19)
            frame = dict(pairs)
            self.assertEqual(set(frame), KEYS)
            self.assertEqual(frame["call_serial"], str(number))
            self.assertEqual(frame["handle_serial"], str(number))
            self.assertEqual((frame["role"], frame["role_index"]), (role, str(indices[role])))
            indices[role] += 1
            for key in ("launch_nonce", "nonce", "run_id_sha256", "module_sha256",
                        "engine_version", "loaded_values_sha256", "feed_bytes_sha256",
                        "bar_count", "first_timestamp", "last_timestamp"):
                self.assertEqual(frame[key], request.manifest[key], key)
            self.assertEqual(frame["call_options_sha256"],
                             reference_options_digest(request.documents[role]))
            self.assertLessEqual(int(frame["start_monotonic_ns"]), int(entry["entry_ns"]))
            frames.append(frame)
        return frames

    def test_cli_required_run_id_forms(self):
        for value in ("-leading-value", "embedded=equals=value", "\u00e9" + "x" * 2051):
            with self.subTest(run_id_form=value[:30]):
                request = self.request(value)
                self.assertIn("--run-id=" + value, request.argv)
                frames = self.assert_witnessed(request, self.launch(request), ["body"])
                self.assertEqual(frames[0]["run_id_sha256"], digest(os.fsencode(value)))
                self.assertNotEqual(frames[0]["run_id_sha256"],
                                    digest(os.fsencode("--run-id=" + value)))

    def test_recorded_recovery_wire_bytes(self):
        fixture = json.loads((ROOT / "tests/fixtures/ordinary_start/recovery-recorded.json").read_bytes())
        baseline = fixture["cases"]["trigger-baseline"]
        candidate = fixture["cases"]["trigger-candidate"]
        self.assertEqual(baseline["wire_hex"], ["", ""])
        self.assertEqual((baseline["exit_code"], candidate["exit_code"]), (1, 1))
        self.assertEqual((baseline["ffi_calls"], candidate["ffi_calls"]), (2, 2))
        self.assertEqual(baseline["stdout_hex"], candidate["stdout_hex"])
        manifest = json.loads(candidate["ingress_utf8"])
        self.assertEqual(manifest, candidate["recorded_manifest"])
        frames = []
        for encoded, role in zip(candidate["wire_hex"], ("body", "metrics_recovery")):
            raw = bytes.fromhex(encoded)
            frame = json.loads(raw)
            self.assertEqual(set(frame), KEYS)
            self.assertEqual(frame["role"], role)
            self.assertEqual(run_json._ordinary_start_encode(frame), raw)
            # Exercise changed input too: a captured-frame constant cannot pass.
            changed = dict(frame, role_index=str(int(frame["role_index"]) + 1))
            independent = json.dumps(changed, ensure_ascii=True, allow_nan=False,
                                     separators=(",", ":")).encode("ascii") + b"\n"
            self.assertEqual(run_json._ordinary_start_encode(changed), independent)
            self.assertNotEqual(independent, raw)
            frames.append(frame)
        self.assertEqual(len(frames), 2)
        self.assertEqual([f["call_serial"] for f in frames], ["1", "2"])
        self.assertEqual([f["handle_serial"] for f in frames], ["1", "2"])
        for frame in frames:
            for key in ("launch_nonce", "nonce", "module_sha256", "engine_version",
                        "run_id_sha256", "feed_bytes_sha256", "loaded_values_sha256",
                        "bar_count", "first_timestamp", "last_timestamp"):
                self.assertEqual(frame[key], manifest[key], key)
            self.assertEqual(frame["call_options_sha256"],
                             manifest["call_options_sha256_by_role"][frame["role"]])

    def test_cli_automatic_native_recovery_preserves_failure(self):
        fixture_root = ROOT / "tests/fixtures/ordinary_start"
        recorded = json.loads((fixture_root / "recovery-recorded.json").read_bytes())
        control = self.request("-recovery=control-" + "x" * 2050,
                               feed=fixture_root / "recovery-control.csv",
                               module=RECOVERY_MODULE, metrics_mode="auto")
        control_result = self.launch(control)
        self.assertEqual(control_result.status, 0, control_result.diagnostics)
        self.assertNotIn("error", json.loads(control_result.stdout))
        self.assertEqual(len(control_result.entries), 1)
        self.assertEqual(control_result.entries[0]["rows"], control.rows)
        self.assertEqual(control_result.entries[0]["reported_bars"], len(control.rows))
        request = self.request("-recovery=trigger-" + "x" * 2050,
                               feed=fixture_root / "recovery-trigger.csv",
                               module=RECOVERY_MODULE, metrics_mode="auto")
        result = self.launch(request)
        # Confirm the native failure and both real entries BEFORE checking START.
        self.assertEqual(result.status, recorded["cases"]["trigger-candidate"]["exit_code"])
        self.assertEqual(result.stdout,
                         bytes.fromhex(recorded["cases"]["trigger-candidate"]["stdout_hex"]))
        self.assertEqual(len(result.entries), 2, result.diagnostics)
        receipts = [json.loads(line.partition("=")[2]) for line in result.diagnostics
                    if line.startswith("ORDINARY_RECOVERY_NATIVE_RECEIPT=")]
        self.assertEqual(len(receipts), 2, result.diagnostics)
        self.assertEqual([r["status"] for r in receipts], [0, 0])
        self.assertEqual([r["route"] for r in receipts], [3, 2])
        self.assertEqual([r["reason"] for r in receipts], [10, 10])
        self.assertEqual([r["replays"] for r in receipts], [1, 0])
        self.assertEqual([r["count"] for r in receipts], [4, 4])
        self.assertEqual([r["reported_bars"] for r in result.entries], [0, 3])
        frames = []
        for number, (entry, role) in enumerate(zip(result.entries, ("body", "metrics_recovery")), 1):
            self.assertEqual(entry["rows"], request.rows)
            raw = bytes.fromhex(entry["wire_hex"])
            self.assertTrue(raw.endswith(b"\n"),
                            "ORDINARY_START_MISSING_START_BEFORE_RUN_BACKTEST_FULL")
            self.assertEqual(raw.count(b"\n"), 1)
            frame = json.loads(raw)
            self.assertEqual(set(frame), KEYS)
            self.assertEqual((frame["role"], frame["role_index"]), (role, "0"))
            self.assertEqual((frame["call_serial"], frame["handle_serial"]),
                             (str(number), str(number)))
            for key in ("launch_nonce", "nonce", "module_sha256", "engine_version",
                        "run_id_sha256", "feed_bytes_sha256", "loaded_values_sha256",
                        "bar_count", "first_timestamp", "last_timestamp"):
                self.assertEqual(frame[key], request.manifest[key], key)
            self.assertEqual(frame["call_options_sha256"],
                             reference_options_digest(request.documents[role]))
            self.assertLessEqual(int(frame["start_monotonic_ns"]), int(entry["entry_ns"]))
            frames.append(frame)
        self.assertLess(int(frames[0]["start_monotonic_ns"]),
                        int(frames[1]["start_monotonic_ns"]))

    def test_cli_empty_id_and_bare_deterministic_compatibility(self):
        request = self.request("")
        witnessed, bare = self.launch(request), self.launch(request, witnessed=False)
        self.assert_witnessed(request, witnessed, ["body"])
        self.assertEqual(bare.status, 0, bare.diagnostics)
        self.assertEqual(len(bare.entries), 1)
        self.assertEqual(bare.entries[0]["wire_hex"], "")
        observed = []
        for result in (witnessed, bare):
            report = json.loads(result.stdout)
            report.pop("elapsed_seconds")
            observed.append(json.dumps(report, ensure_ascii=True, separators=(",", ":")).encode())
        self.assertEqual(*observed)

    def test_cli_real_multicall_rich_options_and_input_sensitivity(self):
        hashes = []
        for count, offset in ((9, 0), (6, 120000)):
            request = self.request(f"rich-{count}", count=count, offset=offset, bench=True, rich=True)
            frames = self.assert_witnessed(
                request, self.launch(request), ["warmup", "repeat", "repeat", "body"])
            self.assertEqual(frames[1]["call_options_sha256"], frames[2]["call_options_sha256"])
            self.assertNotEqual(frames[0]["call_options_sha256"], frames[-1]["call_options_sha256"])
            hashes.append(frames[-1]["call_options_sha256"])
        self.assertNotEqual(*hashes)

    def test_cli_requested_metrics_legacy_policy_without_forced_receipt(self):
        request = self.request("metrics-policy", metrics_mode="legacy")
        self.assert_witnessed(request, self.launch(request), ["body"])

    def test_cli_parent_hash_mismatch_and_orphan_ingress_prevent_entry(self):
        request = self.request("reject-digest")
        request.manifest["call_options_sha256_by_role"]["body"] = digest(b"different options")
        result = self.launch(request)
        self.assert_start_refusal(result, "identity_mismatch", "call_options_sha256")
        request = self.request("orphan")
        for side in ("fd", "context"):
            with self.subTest(orphan=side):
                result = self.launch(request, orphan=side)
                self.assert_start_refusal(result, "incomplete_ingress")

    def test_cli_modified_request_is_not_certified_by_expected_hash(self):
        request = self.request("actual-options")
        request.argv += ["--inputs", '{"gate_bar":-1}']
        result = self.launch(request)
        self.assert_start_refusal(result, "identity_mismatch", "call_options_sha256")

    def test_cli_strict_manifest_and_static_identity_mismatches(self):
        import copy
        request = self.request("static-identities")
        original = request.manifest
        wrong_roles = dict(original["call_options_sha256_by_role"])
        del wrong_roles["metrics_recovery"]
        variants = [
            ({**original, "extra": 0}, "context_keys", None),
            ({**original, "nonce": os.urandom(16).hex()}, "nonce_mismatch", None),
            ({**original, "call_options_sha256_by_role": wrong_roles}, "role_options", None),
            ({**original, "run_id_sha256": digest(b"wrong run id")},
             "identity_mismatch", "run_id_sha256"),
            ({**original, "module_sha256": digest(b"wrong module")},
             "identity_mismatch", "module_sha256"),
            ({**original, "loaded_values_sha256": digest(b"wrong values")},
             "identity_mismatch", "loaded_values_sha256"),
            ({**original, "feed_bytes_sha256": digest(b"wrong file")},
             "identity_mismatch", "feed_bytes_sha256"),
            ({**original, "bar_count": len(request.rows) + 1}, "identity_mismatch", "bar_count"),
            ({**original, "first_timestamp": str(request.rows[0][0] + 1)},
             "identity_mismatch", "first_timestamp"),
        ]
        missing = copy.deepcopy(original)
        del missing["context_version"]
        variants.append((missing, "context_keys", None))
        for number, (manifest, reason, field) in enumerate(variants):
            with self.subTest(variant=number):
                request.manifest = manifest
                result = self.launch(request)
                self.assert_start_refusal(result, reason, field)

    def test_cli_start_and_phase_descriptor_cannot_alias(self):
        result = self.launch(self.request("separate-fds"), collide_phase=True)
        self.assert_start_refusal(result, "descriptor_collision")

    def test_cli_binary_consumed_identity_and_typed_failure_cleanup(self):
        request = self.request("binary-chart", binary=True)
        frames = self.assert_witnessed(request, self.launch(request), ["body"])
        self.assertNotEqual(frames[0]["loaded_values_sha256"], frames[0]["feed_bytes_sha256"])
        request.manifest["call_options_sha256_by_role"]["body"] = digest(b"wrong options")
        failed = self.launch(request)
        self.assert_start_refusal(failed, "identity_mismatch", "call_options_sha256")
        self.assertFalse(any("BufferError" in line for line in failed.diagnostics))

    def test_cli_binary_verification_mode_preserves_bare_sidecar_policy(self):
        for verify_bin in ("full", "sidecar"):
            with self.subTest(verify_bin=verify_bin):
                request = self.request("stale-sidecar-" + verify_bin, binary=True,
                                       verify_bin=verify_bin)
                binary = Path(request.argv[request.argv.index("--ohlcv-bin") + 1])
                sidecar = pf_bars.sidecar_path(binary)
                metadata = json.loads(sidecar.read_bytes())
                metadata["sha256"] = digest(b"stale sidecar data digest")
                self.assertNotEqual(metadata["sha256"], request.manifest["feed_bytes_sha256"])
                sidecar.write_text(json.dumps(metadata), encoding="utf-8")
                witnessed = self.launch(request)
                bare = self.launch(request, witnessed=False)
                if verify_bin == "full":
                    for name, result in (("witnessed", witnessed), ("bare", bare)):
                        with self.subTest(ingress=name):
                            self.assertNotEqual(result.status, 0)
                            self.assertEqual(result.entries, [])
                            failure = json.loads(result.stdout)
                            self.assertEqual(failure["code"], "chart_bars_unreadable")
                            self.assertEqual(failure["args"], {"reason": "sidecar"})
                            self.assertEqual(failure["error"],
                                             "sidecar does not match the bar/close files")
                else:
                    self.assert_witnessed(request, witnessed, ["body"])
                    self.assertEqual(bare.status, 0, bare.diagnostics)
                    self.assertEqual(bare.diagnostics, [])
                    self.assertEqual(len(bare.entries), 1)
                    self.assertEqual(bare.entries[0]["rows"], request.rows)
                    self.assertEqual(bare.entries[0]["reported_bars"], len(request.rows))
                    self.assertEqual(bare.entries[0]["wire_hex"], "")

    def test_cli_binary_parent_identity_mismatch_in_both_verification_modes(self):
        for verify_bin in ("full", "sidecar"):
            for field in ("feed_bytes_sha256", "loaded_values_sha256"):
                with self.subTest(verify_bin=verify_bin, field=field):
                    request = self.request(
                        "binary-parent-" + verify_bin + "-" + field, binary=True,
                        verify_bin=verify_bin, count=7 if verify_bin == "sidecar" else 9)
                    request.manifest[field] = digest(("wrong parent " + field).encode())
                    if verify_bin == "sidecar" and field == "feed_bytes_sha256":
                        binary = Path(request.argv[request.argv.index("--ohlcv-bin") + 1])
                        sidecar = pf_bars.sidecar_path(binary)
                        metadata = json.loads(sidecar.read_bytes())
                        # A sidecar agreeing with the wrong parent hash cannot
                        # substitute for hashing the actual consumed mapping.
                        metadata["sha256"] = request.manifest[field]
                        sidecar.write_text(json.dumps(metadata), encoding="utf-8")
                    failed = self.launch(request)
                    self.assert_start_refusal(failed, "identity_mismatch", field)
                    self.assertFalse(any("BufferError" in line for line in failed.diagnostics))

    def test_cli_native_integer_widths_refuse_wrap_before_entry(self):
        for option, value in (("--magnifier-samples", 1 << 31),
                              ("--trade-start-ms", 1 << 63)):
            with self.subTest(option=option):
                request = self.request("native-width")
                result = self.launch(request, extra=[option, str(value)])
                self.assert_start_refusal(result, "options_integer_range")

    def test_cli_supplied_empty_syminfo_keeps_file_identity_and_zero_calls(self):
        request = self.request("empty-syminfo")
        path = self.root / "empty-syminfo.json"
        path.write_bytes(b"{}")
        request.argv += ["--syminfo", str(path)]
        for role, document in request.documents.items():
            document["syminfo"] = {"file_bytes_sha256": digest(b"{}"), "calls": []}
            request.manifest["call_options_sha256_by_role"][role] = reference_options_digest(document)
        self.assert_witnessed(request, self.launch(request), ["body"])


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--cli-child":
        raise SystemExit(ordinary_cli_child(sys.argv[2:]))
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--recovery-module", type=Path, required=True)
    parser.add_argument("--baseline-host", type=Path)
    own, remaining = parser.parse_known_args()
    MODULE = own.module.resolve()
    RECOVERY_MODULE = own.recovery_module.resolve()
    BASELINE_HOST = own.baseline_host.resolve() if own.baseline_host is not None else None
    if not MODULE.is_file():
        parser.error("the existing whole-archive strategy fixture is required")
    if not RECOVERY_MODULE.is_file():
        parser.error("the real automatic-recovery strategy fixture is required")
    if BASELINE_HOST is not None and not BASELINE_HOST.is_file():
        parser.error("the parent-staged pinned baseline host is missing; this is not behavioral RED")
    unittest.main(argv=[sys.argv[0], *remaining])
