"""Tests of docker/run_execution_observer.py, the Python half of the run-only execution
observer bridge, and of the two receipt properties added to docker/run_phase_transport.py.

The native side is a stub. FakeNative stands in for the three exports of the strategy
library as Python functions and plays the engine's part around the callback: it copies the
descriptor's values, borrows the callback by its raw address only, presets a boundary and a
receipt the way the engine does, and calls the callback through the C function pointer.
What is real here is the ctypes layer (the structures, the callback prototype and
the call through libffi into the thunk), the RunPhaseWriter, and the AF_UNIX socketpair it
writes to. os.write is wrapped by a recording pass-through where a test counts writes, and
replaced by a fake only where a short write has to be made deterministic; the test that shows
the record is in the socket before the callback returns leaves os.write alone.
"""
import ast
import contextlib
import ctypes
import faulthandler
import gc
import importlib.util
import inspect
import json
import os
import socket
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
OBSERVER_PATH = ROOT / "docker" / "run_execution_observer.py"
TRANSPORT_PATH = ROOT / "docker" / "run_phase_transport.py"
CATALOG_PATH = ROOT / "docker" / "run_failure_codes.json"
REAL_WRITE = os.write


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


observer = load_module("run_execution_observer", OBSERVER_PATH)
transport = load_module("run_phase_transport", TRANSPORT_PATH)
RunPhaseWriter = transport.RunPhaseWriter
PhaseTransportError = transport.PhaseTransportError
ObserverBindingError = observer.ObserverBindingError
ExecutionObserverBinding = observer.ExecutionObserverBinding


def setUpModule():
    # A writer that blocked would hang the whole suite: dump the stacks and exit instead.
    faulthandler.dump_traceback_later(300, exit=True)


def tearDownModule():
    faulthandler.cancel_dump_traceback_later()


# What the frozen C declarations say, written out here and not read from the module under test.
U32, U64, VOIDP = ctypes.c_uint32, ctypes.c_uint64, ctypes.c_void_p
POINTER_SIZE = ctypes.sizeof(ctypes.c_void_p)
BOUNDARY_LAYOUT = [("struct_size", U32, 0), ("version", U32, 4),
                   ("run_generation", U64, 8), ("attempt_serial", U64, 16)]
RECEIPT_LAYOUT = [("struct_size", U32, 0), ("version", U32, 4), ("run_generation", U64, 8),
                  ("frame_bytes", U32, 16), ("handed_bytes", U32, 20),
                  ("export_requested", U32, 24), ("reserved", U32, 28)]
OBSERVER_LAYOUT = [("struct_size", U32, 0), ("version", U32, 4), ("context", VOIDP, 8),
                   ("before_results", observer.BeforeResultsFn, 8 + POINTER_SIZE)]
OBSERVATION_LAYOUT = [("struct_size", U32, 0), ("version", U32, 4),
                      ("run_generation", U64, 8), ("attempt_serial", U64, 16),
                      ("attempt_generation", U64, 24), ("phase", U32, 32),
                      ("fault_stage", U32, 36), ("attempt_outcome", U32, 40),
                      ("boundary_delivered", U32, 44)]
EXPORTS = ("pf_execution_observer_version", "strategy_set_execution_observer_v1",
           "strategy_execution_observation_v1")

UINT32_MAX = 0xFFFFFFFF
GENERATION = 7
SERIAL = 3
STATE = 4242  # the pf_strategy_t the binding borrows
# The receipt as the engine presets it before the callback:
# size 32, version 1, the generation, frame_bytes UINT32_MAX and every other field 0.
SENTINEL = (32, 1, GENERATION, UINT32_MAX, 0, 0, 0)
PHASES = ("preflight", "execution", "result_assembly", "results_digest", "serialization",
          "completed")


def kernel_boundary(generation=GENERATION, serial=SERIAL, size=24, version=1):
    """The boundary as the engine presets it; the size is the C literal, not read back from
    the mirror under test."""
    return observer.BoundaryC(size, version, generation, serial)


def kernel_receipt(generation=GENERATION, size=32, version=1):
    return observer.ReceiptC(size, version, generation, UINT32_MAX, 0, 0, 0)


def receipt_fields(receipt):
    return (receipt.struct_size, receipt.version, receipt.run_generation, receipt.frame_bytes,
            receipt.handed_bytes, receipt.export_requested, receipt.reserved)


def boundary_fields(boundary):
    return (boundary.struct_size, boundary.version, boundary.run_generation,
            boundary.attempt_serial)


def by_reference(value):
    """A struct passed to C by pointer; None is the NULL pointer."""
    return None if value is None else ctypes.byref(value)


def descriptor_count():
    """How many descriptors this process holds; None where /dev/fd cannot be listed."""
    gc.collect()
    try:
        return len(os.listdir("/dev/fd"))
    except OSError:
        return None


def prefix_write(cut):
    """A fake os.write: hands the first cut(len(data)) bytes to the real descriptor and
    reports exactly that count (0 hands nothing over and reports 0)."""
    def write(fd, data):
        count = cut(len(data))
        return REAL_WRITE(fd, data[:count]) if count else 0
    return write


class WriteSpy:
    """Stands in for os.write. Calls on `fd` (on every descriptor when fd is None) are
    recorded; `fake`, when given, decides what such a call does and returns; every other
    call passes through."""

    def __init__(self, fd, fake=None):
        self.fd = fd
        self.fake = fake
        self.calls = []

    def __call__(self, fd, data):
        if self.fd is not None and fd != self.fd:
            return REAL_WRITE(fd, data)
        self.calls.append(bytes(data))
        if self.fake is not None:
            return self.fake(fd, data)
        return REAL_WRITE(fd, data)


@contextlib.contextmanager
def spying_on_os_write(fd=None, fake=None):
    spy = WriteSpy(fd, fake)
    with mock.patch.object(os, "write", spy):
        yield spy


@contextlib.contextmanager
def unraisable_spy():
    """Records what sys.unraisablehook is called with: how ctypes reports an exception that
    escaped a callback. A contained failure leaves it empty."""
    seen = []
    previous = sys.unraisablehook
    sys.unraisablehook = seen.append
    try:
        yield seen
    finally:
        sys.unraisablehook = previous


class ScriptedClock:
    """A clock that returns the given readings, one per call, and counts its calls."""

    def __init__(self, *readings):
        self.readings = list(readings)
        self.reads = 0

    def __call__(self):
        reading = self.readings[self.reads]
        self.reads += 1
        return reading


def drain(sock):
    """Everything a nonblocking socket holds right now."""
    chunks = []
    while True:
        try:
            chunk = sock.recv(65536)
        except BlockingIOError:
            break
        if not chunk:
            break
        chunks.append(chunk)
    return b"".join(chunks)


class Channel:
    """A real AF_UNIX stream socketpair. `fd` is the writer's end, the descriptor the parent
    would hand down; read() drains the far end as the parent would."""

    def __init__(self, test):
        self.near, self.far = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        test.addCleanup(self.near.close)
        test.addCleanup(self.far.close)
        self.far.setblocking(False)
        self.fd = self.near.fileno()
        test.assertGreaterEqual(self.fd, 3)

    def read(self):
        return drain(self.far)

    def fill(self):
        """Send filler until the near end takes no more, so that any further write finds
        the socket full. Returns how many filler bytes the socket holds."""
        sent = 0
        for size in (65536, 4096, 256, 1):
            while True:
                try:
                    sent += self.near.send(b"x" * size, socket.MSG_DONTWAIT)
                except BlockingIOError:
                    break
        return sent


class Export:
    """One export of the strategy library as ctypes shows it to the binding: a callable
    that takes argtypes and restype. `at_call` is what both were when it was last called."""

    def __init__(self, name, body):
        self.__name__ = name
        self.body = body
        self.argtypes = None
        self.restype = None
        self.at_call = None

    def __call__(self, *args):
        argtypes = None if self.argtypes is None else list(self.argtypes)
        self.at_call = (argtypes, self.restype)
        return self.body(*args)


class Registration:
    """What the engine keeps after it accepted a descriptor: the values it copied and the raw
    address of the callback, which it borrows. Nothing here references the Python callback,
    so a call through it works only while the binding keeps the callback alive."""

    def __init__(self, struct_size, version, context, address):
        self.struct_size = struct_size
        self.version = version
        self.context = context
        self.address = address

    def call(self, boundary, receipt):
        """The engine's call, through the C function pointer. The structs are passed by
        pointer; None is NULL."""
        function = observer.BeforeResultsFn(self.address)
        return function(self.context, by_reference(boundary), by_reference(receipt))


class FakeNative:
    """The three exports of the strategy library as Python functions, and the engine's
    memory of a registration. A UNIT control only: no native code runs."""

    def __init__(self, version=1, register_status=0, remove_status=0, missing=()):
        self.version = version
        self.register_status = register_status
        self.remove_status = remove_status
        self.registered = None
        self.set_calls = []  # (handle, removal) for every call of the setter
        bodies = {
            "pf_execution_observer_version": self._probe,
            "strategy_set_execution_observer_v1": self._set_observer,
            "strategy_execution_observation_v1": self._observation,
        }
        for name, body in bodies.items():
            if name not in missing:
                setattr(self, name, Export(name, body))

    def _probe(self):
        return self.version

    def _set_observer(self, handle, pointer):
        removal = pointer is None
        self.set_calls.append((handle, removal))
        if removal:
            if self.remove_status == 0:
                self.registered = None
            return self.remove_status
        if self.register_status == 0:
            descriptor = pointer.contents
            self.registered = Registration(
                descriptor.struct_size, descriptor.version, descriptor.context,
                ctypes.cast(descriptor.before_results, ctypes.c_void_p).value)
        return self.register_status

    def _observation(self, handle, pointer):
        return -1


class DrainingWriter(RunPhaseWriter):
    """Drains the far end the instant advance("result_assembly") returns, which is inside the
    observer callback and before it fills the receipt, and notes the receipt as it is then."""

    def __init__(self, fd, far, holder):
        super().__init__(fd)
        self.far = far
        self.holder = holder
        self.seen = None

    def advance(self, phase):
        super().advance(phase)
        if phase == "result_assembly":
            self.seen = (drain(self.far), receipt_fields(self.holder[0]))


class StubWriter:
    """A writer double for the receipt contract alone, since the real writer cannot report
    these values: advance succeeds and writes nothing, the reported handoff is the test's."""

    def __init__(self, export_requested, handed):
        self.phase = "execution"
        self.export_requested = export_requested
        self.last_handed_frame_bytes = handed
        self.advanced = []

    def advance(self, phase):
        self.advanced.append(phase)


class ExplodingWriter:
    """A writer double whose advance raises whatever it is given."""

    phase = "execution"
    export_requested = True
    last_handed_frame_bytes = 0

    def __init__(self, error):
        self.error = error
        self.calls = 0

    def advance(self, phase):
        self.calls += 1
        raise self.error


class Boom(BaseException):
    """An exception that is not an Exception."""


class BridgeCase(unittest.TestCase):
    def channel(self):
        return Channel(self)

    def writer_in_execution(self, fd=None, **kwargs):
        """A writer as the caller leaves it before native work starts: execution entered."""
        writer = RunPhaseWriter(fd, **kwargs)
        writer.advance("preflight")
        writer.advance("execution")
        return writer

    def attached(self, writer, native=None):
        native = FakeNative() if native is None else native
        binding = ExecutionObserverBinding(native, STATE, writer)
        binding.attach()
        return binding, native

    def records(self, data):
        self.assertTrue(data == b"" or data.endswith(b"\n"), "the stream ends inside a record")
        return [json.loads(line.decode("utf-8")) for line in data.split(b"\n") if line]

    def assert_refused(self, binding, status, receipt, before):
        """The callback returned -1, kept an ObserverBindingError as the failure and left the
        receipt as the engine preset it."""
        self.assertEqual(status, -1)
        failure = binding.failure
        self.assertIsInstance(failure, ObserverBindingError)
        self.assertEqual(failure.code, "harness_internal_error")
        self.assertEqual(failure.code_args, {})
        if receipt is not None:
            self.assertEqual(receipt_fields(receipt), before)
        with self.assertRaises(ObserverBindingError) as caught:
            binding.raise_if_failed()
        self.assertIs(caught.exception, failure)


class LayoutTests(unittest.TestCase):
    def check_layout(self, structure, layout, size):
        self.assertEqual(list(structure._fields_),
                         [(name, ctype) for name, ctype, _ in layout])
        for name, ctype, offset in layout:
            member = getattr(structure, name)
            self.assertEqual(member.offset, offset, name)
            self.assertEqual(member.size, ctypes.sizeof(ctype), name)
        self.assertEqual(ctypes.sizeof(structure), size)
        self.assertFalse(hasattr(structure, "_pack_"), "natural alignment, no packing")

    def test_boundary_matches_pf_execution_boundary_v1(self):
        self.check_layout(observer.BoundaryC, BOUNDARY_LAYOUT, 24)

    def test_receipt_matches_pf_boundary_receipt_v1(self):
        self.check_layout(observer.ReceiptC, RECEIPT_LAYOUT, 32)

    def test_observer_matches_pf_execution_observer_v1(self):
        self.check_layout(observer.ObserverC, OBSERVER_LAYOUT, 8 + 2 * POINTER_SIZE)

    def test_observation_matches_pf_execution_observation_v1(self):
        self.check_layout(observer.ObservationC, OBSERVATION_LAYOUT, 48)

    def test_lp64_sizes_are_the_frozen_ones(self):
        if POINTER_SIZE == 8:
            self.assertEqual([ctypes.sizeof(structure) for structure in (
                observer.BoundaryC, observer.ReceiptC, observer.ObserverC,
                observer.ObservationC)], [24, 32, 24, 48])

    def test_callback_prototype_is_pf_before_results_fn_v1(self):
        prototype = observer.BeforeResultsFn
        self.assertIs(prototype._restype_, ctypes.c_int)
        self.assertEqual(prototype._argtypes_, (
            ctypes.c_void_p, ctypes.POINTER(observer.BoundaryC),
            ctypes.POINTER(observer.ReceiptC)))
        self.assertIs(prototype, ctypes.CFUNCTYPE(
            ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(observer.BoundaryC),
            ctypes.POINTER(observer.ReceiptC)))

    def test_version_and_limits_are_the_frozen_numbers(self):
        self.assertEqual(observer.OBSERVER_VERSION, 1)
        self.assertEqual(observer.MAX_FRAME_BYTES, 1024)
        self.assertEqual(observer.MAX_FRAME_BYTES, transport.MAX_RECORD_BYTES)
        self.assertEqual(observer.CAPABILITY, "execution_observer_v1")


class ErrorTests(unittest.TestCase):
    def test_the_error_carries_code_message_and_a_code_args_dictionary(self):
        error = ObserverBindingError("window_mode_unsupported", "text",
                                     {"capability": "execution_observer_v1"})
        self.assertEqual(error.code, "window_mode_unsupported")
        self.assertEqual(error.message, "text")
        self.assertEqual(str(error), "text")
        self.assertEqual(error.code_args, {"capability": "execution_observer_v1"})
        self.assertEqual(error.args, ("text",), "BaseException.args stays the exception's own")
        plain = ObserverBindingError("out_of_memory", "text")
        self.assertEqual(plain.code_args, {})

    def test_the_arguments_are_copied(self):
        source = {"capability": "execution_observer_v1"}
        error = ObserverBindingError("window_mode_unsupported", "text", source)
        source["other"] = 1
        self.assertEqual(error.code_args, {"capability": "execution_observer_v1"})

    def test_it_is_not_an_oserror_and_its_codes_exist_in_the_catalogue(self):
        self.assertFalse(issubclass(ObserverBindingError, OSError))
        catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        # window_mode_unsupported is not in this tree's catalogue; no
        # code is introduced here.
        for code in ("harness_internal_error", "out_of_memory"):
            self.assertIn(code, catalog["codes"])


class AttachTests(BridgeCase):
    def test_a_missing_export_is_unsupported_and_registers_nothing(self):
        for name in EXPORTS:
            with self.subTest(missing=name):
                native = FakeNative(missing=(name,))
                binding = ExecutionObserverBinding(native, STATE, RunPhaseWriter(None))
                with self.assertRaises(ObserverBindingError) as caught:
                    binding.attach()
                self.assertEqual(caught.exception.code, "window_mode_unsupported")
                self.assertEqual(caught.exception.code_args,
                                 {"capability": "execution_observer_v1"})
                self.assertEqual(native.set_calls, [])
                self.assertIsNone(native.registered)
                binding.detach()
                self.assertEqual(native.set_calls, [], "nothing registered: no native call")

    def test_a_version_other_than_one_is_unsupported_and_registers_nothing(self):
        for version in (0, 2, 7, UINT32_MAX):
            with self.subTest(version=version):
                native = FakeNative(version=version)
                binding = ExecutionObserverBinding(native, STATE, RunPhaseWriter(None))
                with self.assertRaises(ObserverBindingError) as caught:
                    binding.attach()
                self.assertEqual(caught.exception.code, "window_mode_unsupported")
                self.assertEqual(caught.exception.code_args,
                                 {"capability": "execution_observer_v1"})
                self.assertIn(str(version), str(caught.exception))
                self.assertEqual(native.set_calls, [])
                self.assertIsNone(native.registered)

    def test_registration_statuses_map_to_their_codes(self):
        expected = {-4: "out_of_memory", -1: "harness_internal_error",
                    -2: "harness_internal_error", -3: "harness_internal_error",
                    1: "harness_internal_error", 7: "harness_internal_error",
                    -5: "harness_internal_error"}
        for status, code in expected.items():
            with self.subTest(status=status):
                native = FakeNative(register_status=status)
                binding = ExecutionObserverBinding(native, STATE, RunPhaseWriter(None))
                with self.assertRaises(ObserverBindingError) as caught:
                    binding.attach()
                self.assertEqual(caught.exception.code, code)
                self.assertEqual(caught.exception.code_args, {})
                self.assertIn(str(status), str(caught.exception))
                self.assertEqual(native.set_calls, [(STATE, False)])
                self.assertIsNone(native.registered)
                binding.detach()
                self.assertEqual(native.set_calls, [(STATE, False)],
                                 "refused: nothing to unregister, no native call")

    def test_it_binds_argtypes_and_restype_before_the_first_call(self):
        binding, native = self.attached(RunPhaseWriter(None))
        probe = native.pf_execution_observer_version
        setter = native.strategy_set_execution_observer_v1
        getter = native.strategy_execution_observation_v1
        self.assertEqual(probe.at_call, ([], ctypes.c_uint32))
        self.assertEqual(setter.at_call, ([ctypes.c_void_p,
                                           ctypes.POINTER(observer.ObserverC)], ctypes.c_int))
        self.assertEqual(list(getter.argtypes),
                         [ctypes.c_void_p, ctypes.POINTER(observer.ObservationC)])
        self.assertIs(getter.restype, ctypes.c_int)
        self.assertIsNone(getter.at_call, "the observation getter is bound, not called")

    def test_it_registers_one_descriptor_for_the_handle(self):
        binding, native = self.attached(RunPhaseWriter(None))
        registration = native.registered
        self.assertEqual(native.set_calls, [(STATE, False)])
        self.assertEqual(registration.struct_size, 8 + 2 * POINTER_SIZE)
        self.assertEqual(registration.version, 1)
        self.assertIsNone(registration.context, "context is NULL: the closure captures self")
        self.assertTrue(registration.address)

    def test_a_repeated_attach_is_refused_locally(self):
        binding, native = self.attached(RunPhaseWriter(None))
        with self.assertRaises(ObserverBindingError) as caught:
            binding.attach()
        self.assertEqual(caught.exception.code, "harness_internal_error")
        self.assertEqual(caught.exception.code_args, {})
        self.assertIn("already", str(caught.exception))
        self.assertEqual(native.set_calls, [(STATE, False)], "the library is not touched again")
        self.assertIsNotNone(native.registered)

    def test_a_binding_is_single_use_even_after_a_refusal_or_a_detach(self):
        refused = FakeNative(register_status=-2)
        binding = ExecutionObserverBinding(refused, STATE, RunPhaseWriter(None))
        with self.assertRaises(ObserverBindingError):
            binding.attach()
        with self.assertRaises(ObserverBindingError) as caught:
            binding.attach()
        self.assertIn("already", str(caught.exception))
        self.assertEqual(refused.set_calls, [(STATE, False)])
        done, native = self.attached(RunPhaseWriter(None))
        done.detach()
        with self.assertRaises(ObserverBindingError):
            done.attach()
        self.assertEqual(native.set_calls, [(STATE, False), (STATE, True)])

    def test_attach_never_advances_the_writer(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        writer.advance("preflight")
        channel.read()
        with spying_on_os_write() as spy:
            self.attached(writer)
        self.assertEqual(spy.calls, [])
        self.assertEqual((writer.phase, writer.sequence), ("preflight", 1))
        self.assertEqual(channel.read(), b"")


class DeliveryTests(BridgeCase):
    def test_the_whole_result_assembly_record_is_in_the_socket_before_the_callback_returns(self):
        channel = self.channel()
        holder = []
        writer = DrainingWriter(channel.fd, channel.far, holder)
        writer.advance("preflight")
        writer.advance("execution")
        binding, native = self.attached(writer)
        boundary, receipt = kernel_boundary(), kernel_receipt()
        holder.append(receipt)
        with unraisable_spy() as unraisable:
            status = native.registered.call(boundary, receipt)  # the real os.write, untouched
        self.assertEqual(status, 0)
        self.assertEqual(unraisable, [])
        self.assertIsNone(binding.failure)
        binding.raise_if_failed()
        # What the far end held at the moment advance("result_assembly") returned, which was
        # inside the callback and before it filled the receipt: nothing came after it.
        data, receipt_then = writer.seen
        self.assertEqual(channel.read(), b"", "nothing reached the socket after advance returned")
        self.assertEqual(receipt_then, SENTINEL, "the receipt is filled only after the handoff")
        records = self.records(data)
        self.assertEqual([record["phase"] for record in records],
                         ["preflight", "execution", "result_assembly"])
        lines = data.split(b"\n")
        self.assertEqual(lines[-1], b"", "the stream ends with a whole record")
        frame = lines[-2] + b"\n"  # the result_assembly record as it reached the socket
        self.assertTrue(1 <= len(frame) <= 1024)
        self.assertEqual(json.loads(frame.decode("utf-8")), records[-1])
        self.assertEqual(receipt_fields(receipt),
                         (32, 1, GENERATION, len(frame), len(frame), 1, 0))
        self.assertEqual((writer.phase, writer.sequence), ("result_assembly", 3))

    def test_a_writer_without_a_descriptor_hands_over_nothing_and_the_receipt_says_zero(self):
        writer = self.writer_in_execution(None)
        binding, native = self.attached(writer)
        receipt = kernel_receipt()
        with unraisable_spy() as unraisable:
            with spying_on_os_write() as spy:
                status = native.registered.call(kernel_boundary(), receipt)
        self.assertEqual(status, 0)
        self.assertEqual(unraisable, [])
        self.assertEqual(spy.calls, [], "no descriptor: no os.write at all")
        self.assertEqual(receipt_fields(receipt), (32, 1, GENERATION, 0, 0, 0, 0))
        self.assertEqual((writer.phase, writer.sequence), ("result_assembly", 3))
        self.assertIsNone(binding.failure)
        binding.raise_if_failed()

    def test_boundary_identity_and_receipt_header_are_never_written(self):
        for generation, serial in ((1, 1), (GENERATION, SERIAL), (2 ** 64 - 1, 2 ** 63)):
            with self.subTest(generation=generation, serial=serial):
                writer = self.writer_in_execution(None)
                binding, native = self.attached(writer)
                boundary = kernel_boundary(generation=generation, serial=serial)
                receipt = kernel_receipt(generation=generation)
                status = native.registered.call(boundary, receipt)
                self.assertEqual(status, 0)
                self.assertEqual(boundary_fields(boundary), (24, 1, generation, serial))
                self.assertEqual(receipt_fields(receipt), (32, 1, generation, 0, 0, 0, 0))

    def test_the_callback_reads_no_clock_and_encodes_and_writes_one_record(self):
        channel = self.channel()
        clock = ScriptedClock(0.0, 0.5, 1.25)
        writer = self.writer_in_execution(channel.fd, clock=clock)
        binding, native = self.attached(writer)
        self.assertEqual(clock.reads, 2, "attach reads no clock")
        with mock.patch.object(transport, "encode_record",
                               wraps=transport.encode_record) as encode:
            with spying_on_os_write(channel.fd) as spy:
                status = native.registered.call(kernel_boundary(), kernel_receipt())
        self.assertEqual(status, 0)
        self.assertEqual(encode.call_count, 1, "one record encoded")
        self.assertEqual(len(spy.calls), 1, "one record written")
        self.assertEqual(clock.reads, 3, "the writer's one reading, nothing else")
        records = self.records(channel.read())
        self.assertEqual([record["sequence"] for record in records], [1, 2, 3])
        self.assertEqual(records[-1]["elapsed_ms"], 1250)

    def test_a_second_callback_is_a_failure_and_writes_nothing(self):
        channel = self.channel()
        writer = self.writer_in_execution(channel.fd)
        binding, native = self.attached(writer)
        first = kernel_receipt()
        self.assertEqual(native.registered.call(kernel_boundary(), first), 0)
        filled = receipt_fields(first)
        self.assertIsNone(binding.failure)
        second = kernel_receipt()
        with unraisable_spy() as unraisable:
            with spying_on_os_write() as spy:
                status = native.registered.call(kernel_boundary(), second)
        self.assert_refused(binding, status, second, SENTINEL)
        self.assertIn("again", str(binding.failure))
        self.assertEqual(unraisable, [])
        self.assertEqual(spy.calls, [], "the record is never written twice")
        self.assertEqual(receipt_fields(first), filled)
        self.assertEqual(len(self.records(channel.read())), 3)
        self.assertEqual((writer.phase, writer.sequence), ("result_assembly", 3))


class RefusalTests(BridgeCase):
    CASES = [
        ("null boundary", lambda: None, kernel_receipt),
        ("null receipt", kernel_boundary, lambda: None),
        ("both null", lambda: None, lambda: None),
        ("boundary size 0", lambda: kernel_boundary(size=0), kernel_receipt),
        ("boundary size 23", lambda: kernel_boundary(size=23), kernel_receipt),
        ("boundary size 25", lambda: kernel_boundary(size=25), kernel_receipt),
        ("boundary size 32", lambda: kernel_boundary(size=32), kernel_receipt),
        ("boundary size UINT32_MAX", lambda: kernel_boundary(size=UINT32_MAX), kernel_receipt),
        ("boundary version 0", lambda: kernel_boundary(version=0), kernel_receipt),
        ("boundary version 2", lambda: kernel_boundary(version=2), kernel_receipt),
        ("generation 0", lambda: kernel_boundary(generation=0),
         lambda: kernel_receipt(generation=0)),
        ("attempt serial 0", lambda: kernel_boundary(serial=0), kernel_receipt),
        ("receipt size 0", kernel_boundary, lambda: kernel_receipt(size=0)),
        ("receipt size 31", kernel_boundary, lambda: kernel_receipt(size=31)),
        ("receipt size 33", kernel_boundary, lambda: kernel_receipt(size=33)),
        ("receipt size 24", kernel_boundary, lambda: kernel_receipt(size=24)),
        ("receipt version 0", kernel_boundary, lambda: kernel_receipt(version=0)),
        ("receipt version 2", kernel_boundary, lambda: kernel_receipt(version=2)),
        ("receipt generation 0", kernel_boundary, lambda: kernel_receipt(generation=0)),
        ("receipt generation differs", kernel_boundary,
         lambda: kernel_receipt(generation=GENERATION + 1)),
    ]

    def test_malformed_arguments_are_refused_without_a_handoff_or_a_receipt(self):
        for label, make_boundary, make_receipt in self.CASES:
            with self.subTest(case=label):
                channel = self.channel()
                writer = self.writer_in_execution(channel.fd)
                binding, native = self.attached(writer)
                boundary, receipt = make_boundary(), make_receipt()
                before = None if receipt is None else receipt_fields(receipt)
                with unraisable_spy() as unraisable:
                    with spying_on_os_write() as spy:
                        status = native.registered.call(boundary, receipt)
                self.assert_refused(binding, status, receipt, before)
                self.assertEqual(unraisable, [])
                self.assertEqual(spy.calls, [], "nothing is handed over for a malformed call")
                self.assertEqual((writer.phase, writer.sequence), ("execution", 2))
                self.assertIsNone(writer.failure)

    def test_the_callback_needs_the_writer_in_execution(self):
        prefixes = {
            "no phase yet": (),
            "preflight": ("preflight",),
            "result_assembly": ("preflight", "execution", "result_assembly"),
            "serialization": ("preflight", "execution", "result_assembly", "serialization"),
        }
        for label, prefix in prefixes.items():
            with self.subTest(phase=label):
                writer = RunPhaseWriter(None)
                for phase in prefix:
                    writer.advance(phase)
                binding, native = self.attached(writer)
                receipt = kernel_receipt()
                status = native.registered.call(kernel_boundary(), receipt)
                self.assert_refused(binding, status, receipt, SENTINEL)
                self.assertEqual(writer.sequence, len(prefix), "the writer was not advanced")
                self.assertEqual(writer.phase, prefix[-1] if prefix else None)
                self.assertIsNone(writer.failure)

    def test_a_handoff_the_receipt_contract_cannot_hold_is_refused(self):
        cases = [(True, 0), (True, -1), (True, 1025), (True, 2 ** 32), (False, 1),
                 (False, 1024), (False, -1), (True, 1.0), (True, True), (True, "5"),
                 (True, None), (1, 5), (0, 0), (None, 0), ("yes", 5)]
        for exported, handed in cases:
            with self.subTest(exported=exported, handed=handed):
                writer = StubWriter(exported, handed)
                binding, native = self.attached(writer)
                receipt = kernel_receipt()
                status = native.registered.call(kernel_boundary(), receipt)
                self.assert_refused(binding, status, receipt, SENTINEL)
                self.assertEqual(writer.advanced, ["result_assembly"],
                                 "one advance, then the check")

    def test_the_handoffs_the_receipt_contract_holds_are_filled_as_reported(self):
        cases = [(True, 1, (1, 1, 1, 0)), (True, 87, (87, 87, 1, 0)),
                 (True, 1024, (1024, 1024, 1, 0)), (False, 0, (0, 0, 0, 0))]
        for exported, handed, filled in cases:
            with self.subTest(exported=exported, handed=handed):
                writer = StubWriter(exported, handed)
                binding, native = self.attached(writer)
                receipt = kernel_receipt()
                status = native.registered.call(kernel_boundary(), receipt)
                self.assertEqual(status, 0)
                self.assertEqual(receipt_fields(receipt), (32, 1, GENERATION) + filled)
                self.assertEqual(writer.advanced, ["result_assembly"])
                self.assertIsNone(binding.failure)


class ContainmentTests(BridgeCase):
    def test_a_full_socket_returns_minus_one_and_keeps_the_first_failure(self):
        channel = self.channel()
        writer = self.writer_in_execution(channel.fd)
        binding, native = self.attached(writer)
        channel.fill()
        receipt = kernel_receipt()
        with unraisable_spy() as unraisable:
            with spying_on_os_write(channel.fd) as spy:
                status = native.registered.call(kernel_boundary(), receipt)
        self.assertEqual(status, -1)
        self.assertEqual(unraisable, [])
        self.assertEqual(len(spy.calls), 1, "one write attempt, no retry")
        failure = binding.failure
        self.assertIsInstance(failure, PhaseTransportError)
        self.assertEqual(failure.reason, "would_block")
        self.assertEqual(failure.phase, "result_assembly")
        self.assertEqual(receipt_fields(receipt), SENTINEL, "a failed handoff supplies no receipt")
        self.assertEqual(writer.failure, "would_block")
        self.assertEqual((writer.phase, writer.sequence), ("execution", 2))
        with self.assertRaises(PhaseTransportError) as caught:
            binding.raise_if_failed()
        self.assertIs(caught.exception, failure, "the retained exception itself, still typed")
        self.assertEqual(caught.exception.code, "harness_internal_error")
        again = kernel_receipt()
        self.assertEqual(native.registered.call(kernel_boundary(), again), -1)
        self.assertIs(binding.failure, failure, "a later refusal does not replace the first")
        self.assertEqual(receipt_fields(again), SENTINEL)

    def test_a_short_write_returns_minus_one_with_the_typed_cause(self):
        cuts = {"nothing taken": lambda size: 0, "one byte": lambda size: 1,
                "half": lambda size: size // 2, "all but the LF": lambda size: size - 1}
        for name, cut in cuts.items():
            with self.subTest(cut=name):
                channel = self.channel()
                writer = self.writer_in_execution(channel.fd)
                binding, native = self.attached(writer)
                receipt = kernel_receipt()
                with unraisable_spy() as unraisable:
                    with spying_on_os_write(channel.fd, fake=prefix_write(cut)) as spy:
                        status = native.registered.call(kernel_boundary(), receipt)
                self.assertEqual(status, -1)
                self.assertEqual(unraisable, [])
                self.assertEqual(len(spy.calls), 1, "one write, the rest is not retried")
                failure = binding.failure
                self.assertIsInstance(failure, PhaseTransportError)
                self.assertEqual(failure.reason, "short_write")
                self.assertEqual(receipt_fields(receipt), SENTINEL)
                self.assertEqual(writer.failure, "short_write")
                with self.assertRaises(PhaseTransportError) as caught:
                    binding.raise_if_failed()
                self.assertIs(caught.exception, failure)

    def test_a_disconnected_parent_returns_minus_one_with_the_typed_cause(self):
        channel = self.channel()
        writer = self.writer_in_execution(channel.fd)
        binding, native = self.attached(writer)
        channel.far.close()
        receipt = kernel_receipt()
        status = native.registered.call(kernel_boundary(), receipt)
        self.assertEqual(status, -1)
        self.assertEqual(binding.failure.reason, "disconnected")
        self.assertEqual(receipt_fields(receipt), SENTINEL)

    def test_no_exception_escapes_ctypes_whatever_the_writer_raises(self):
        errors = [RuntimeError("boom"), MemoryError(), Boom(), KeyboardInterrupt(),
                  SystemExit(3)]
        for error in errors:
            with self.subTest(error=type(error).__name__):
                writer = ExplodingWriter(error)
                binding, native = self.attached(writer)
                receipt = kernel_receipt()
                with unraisable_spy() as unraisable:
                    status = native.registered.call(kernel_boundary(), receipt)
                self.assertEqual(status, -1)
                self.assertEqual(unraisable, [], "ctypes was handed no exception")
                self.assertEqual(writer.calls, 1)
                self.assertIs(binding.failure, error)
                self.assertEqual(receipt_fields(receipt), SENTINEL)
                with self.assertRaises(type(error)) as caught:
                    binding.raise_if_failed()
                self.assertIs(caught.exception, error)
                self.assertEqual(native.registered.call(kernel_boundary(), kernel_receipt()), -1)
                self.assertIs(binding.failure, error, "the first failure is the one kept")
                self.assertEqual(writer.calls, 1, "a second call does not reach the writer")

    def test_a_failure_does_not_stop_detach(self):
        writer = ExplodingWriter(RuntimeError("boom"))
        binding, native = self.attached(writer)
        self.assertEqual(native.registered.call(kernel_boundary(), kernel_receipt()), -1)
        binding.detach()
        self.assertIsNone(native.registered)
        self.assertIsInstance(binding.failure, RuntimeError, "detach keeps the failure")
        with self.assertRaises(RuntimeError):
            binding.raise_if_failed()


class DetachTests(BridgeCase):
    def test_detach_unregisters_with_a_null_descriptor_and_lets_go_only_then(self):
        writer = self.writer_in_execution(None)
        binding, native = self.attached(writer)
        self.assertEqual(native.set_calls, [(STATE, False)])
        binding.detach()
        self.assertEqual(native.set_calls, [(STATE, False), (STATE, True)])
        self.assertIsNone(native.registered)
        self.assertIsNone(binding._callback, "released after a successful removal")
        self.assertIsNone(binding._descriptor)
        binding.detach()
        self.assertEqual(len(native.set_calls), 2, "a detached binding calls native code no more")

    def test_detach_without_a_registration_does_nothing(self):
        native = FakeNative()
        binding = ExecutionObserverBinding(native, STATE, RunPhaseWriter(None))
        binding.detach()
        self.assertEqual(native.set_calls, [])

    def test_a_failed_detach_keeps_the_callback_alive_and_can_be_retried(self):
        channel = self.channel()
        writer = self.writer_in_execution(channel.fd)
        native = FakeNative(remove_status=-3)
        binding, _ = self.attached(writer, native)
        registration = native.registered
        with self.assertRaises(ObserverBindingError) as caught:
            binding.detach()
        self.assertEqual(caught.exception.code, "harness_internal_error")
        self.assertEqual(caught.exception.code_args, {})
        self.assertIn("-3", str(caught.exception))
        self.assertIsNotNone(binding._callback, "ownership is retained")
        self.assertIsNotNone(binding._descriptor)
        self.assertIsNotNone(native.registered, "the engine still holds the registration")
        # The engine still holds the callback, so it must still be callable through the raw
        # address: nothing but the binding keeps it alive. (A freed callback would crash here.)
        gc.collect()
        receipt = kernel_receipt()
        self.assertEqual(registration.call(kernel_boundary(), receipt), 0)
        self.assertEqual(receipt.export_requested, 1)
        native.remove_status = 0
        binding.detach()
        self.assertIsNone(native.registered)
        self.assertEqual(native.set_calls, [(STATE, False), (STATE, True), (STATE, True)])
        self.assertIsNone(binding._callback)

    def test_removal_statuses_map_like_registration_statuses(self):
        for status, code in ((-4, "out_of_memory"), (-3, "harness_internal_error"),
                             (-1, "harness_internal_error"), (9, "harness_internal_error")):
            with self.subTest(status=status):
                native = FakeNative(remove_status=status)
                binding, _ = self.attached(RunPhaseWriter(None), native)
                with self.assertRaises(ObserverBindingError) as caught:
                    binding.detach()
                self.assertEqual(caught.exception.code, code)
                self.assertEqual(caught.exception.code_args, {})

    def test_the_binding_keeps_the_registered_callback_alive(self):
        channel = self.channel()
        writer = self.writer_in_execution(channel.fd)
        binding, native = self.attached(writer)
        registration = native.registered  # the engine's view: values and a raw address only
        gc.collect()
        gc.collect()
        receipt = kernel_receipt()
        # Called through the borrowed address, as the engine does; this works only while the
        # binding holds the callback (a freed one would crash here, not fail cleanly).
        self.assertEqual(registration.call(kernel_boundary(), receipt), 0)
        self.assertEqual(receipt.export_requested, 1)
        self.assertGreaterEqual(receipt.frame_bytes, 1)

    def test_nothing_here_closes_or_duplicates_the_phase_descriptor(self):
        channel = self.channel()
        writer = self.writer_in_execution(channel.fd)
        before = descriptor_count()
        binding, native = self.attached(writer)
        self.assertEqual(native.registered.call(kernel_boundary(), kernel_receipt()), 0)
        binding.raise_if_failed()
        binding.detach()
        del binding, native
        gc.collect()
        os.fstat(channel.fd)
        self.assertEqual(channel.near.fileno(), channel.fd)
        channel.read()
        self.assertEqual(channel.near.send(b"!"), 1, "still the live socket")
        self.assertEqual(channel.far.recv(1), b"!")
        if before is not None:
            self.assertEqual(descriptor_count(), before)


class WriterReceiptTests(BridgeCase):
    def test_before_any_advance(self):
        channel = self.channel()
        exporting, silent = RunPhaseWriter(channel.fd), RunPhaseWriter(None)
        self.assertIs(exporting.export_requested, True)
        self.assertIs(silent.export_requested, False)
        self.assertEqual(exporting.last_handed_frame_bytes, 0)
        self.assertEqual(silent.last_handed_frame_bytes, 0)

    def test_every_successful_advance_reports_the_one_record_it_handed_over(self):
        channel = self.channel()
        writer = RunPhaseWriter(channel.fd)
        for phase in PHASES:
            with self.subTest(phase=phase):
                with spying_on_os_write(channel.fd) as spy:
                    writer.advance(phase)
                data = channel.read()
                self.assertEqual(len(spy.calls), 1)
                self.assertEqual(spy.calls[0], data)
                self.assertEqual(data.count(b"\n"), 1)
                self.assertEqual(writer.last_handed_frame_bytes, len(data))
                self.assertTrue(1 <= writer.last_handed_frame_bytes <= 1024)
                self.assertIs(writer.export_requested, True)

    def test_a_writer_without_a_descriptor_stores_zero_after_every_advance(self):
        writer = RunPhaseWriter(None)
        with spying_on_os_write() as spy:
            for phase in PHASES:
                writer.advance(phase)
                self.assertEqual(writer.last_handed_frame_bytes, 0, phase)
                self.assertIs(writer.export_requested, False)
        self.assertEqual(spy.calls, [])

    def test_a_refused_advance_leaves_what_the_writer_reports(self):
        channel = self.channel()
        writer = self.writer_in_execution(channel.fd)
        handed = writer.last_handed_frame_bytes
        self.assertGreater(handed, 0)
        channel.read()
        channel.fill()
        with self.assertRaises(PhaseTransportError):
            writer.advance("result_assembly")
        self.assertEqual(writer.last_handed_frame_bytes, handed, "nothing was handed over")
        other = self.channel()
        short = self.writer_in_execution(other.fd)
        handed = short.last_handed_frame_bytes
        with spying_on_os_write(other.fd, fake=prefix_write(lambda size: size // 2)):
            with self.assertRaises(PhaseTransportError):
                short.advance("result_assembly")
        self.assertEqual(short.last_handed_frame_bytes, handed)
        misordered = RunPhaseWriter(None)
        with self.assertRaises(PhaseTransportError):
            misordered.advance("execution")
        self.assertEqual(misordered.last_handed_frame_bytes, 0)

    def test_the_properties_are_read_only(self):
        for name in ("export_requested", "last_handed_frame_bytes"):
            with self.subTest(name=name):
                member = getattr(RunPhaseWriter, name)
                self.assertIsInstance(member, property)
                self.assertIsNone(member.fset)
                self.assertIsNone(member.fdel)
                with self.assertRaises(AttributeError):
                    setattr(RunPhaseWriter(None), name, 1)

    def test_one_encode_one_clock_reading_and_one_write_per_advance(self):
        channel = self.channel()
        clock = ScriptedClock(0.0, 1.0, 2.0, 3.0, 4.0, 5.0)
        writer = RunPhaseWriter(channel.fd, clock=clock)
        for count, phase in enumerate(PHASES, start=1):
            with self.subTest(phase=phase):
                with mock.patch.object(transport, "encode_record",
                                       wraps=transport.encode_record) as encode:
                    with spying_on_os_write(channel.fd) as spy:
                        writer.advance(phase)
                self.assertEqual(encode.call_count, 1)
                self.assertEqual(len(spy.calls), 1)
                self.assertEqual(clock.reads, count)
        silent = RunPhaseWriter(None, clock=ScriptedClock(0.0, 1.0))
        with mock.patch.object(transport, "encode_record",
                               wraps=transport.encode_record) as encode:
            silent.advance("preflight")
            silent.advance("execution")
        self.assertEqual(encode.call_count, 0, "nothing is encoded when nothing is exported")


def called_names(root):
    names = []
    for node in ast.walk(root):
        if isinstance(node, ast.Call):
            if isinstance(node.func, ast.Name):
                names.append(node.func.id)
            elif isinstance(node.func, ast.Attribute):
                names.append(node.func.attr)
    return names


def find_class(tree, name):
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == name:
            return node
    raise AssertionError("class %s not found" % name)


def find_method(class_node, name):
    for item in class_node.body:
        if isinstance(item, ast.FunctionDef) and item.name == name:
            return item
    raise AssertionError("method %s not found" % name)


class SourceContractTests(unittest.TestCase):
    FORBIDDEN_CALLS = {"print", "sleep", "close", "write", "dup", "dup2", "fromfd", "fdopen",
                       "makefile", "send", "sendall", "recv", "setblocking", "settimeout",
                       "set_blocking", "monotonic", "perf_counter", "time", "encode_record",
                       "_hand_off", "_read_clock"}
    FORBIDDEN_IMPORTS = {"os", "time", "socket", "select", "selectors", "threading", "sys",
                         "logging", "subprocess", "signal", "fcntl", "atexit", "weakref", "gc"}

    @classmethod
    def setUpClass(cls):
        cls.tree = ast.parse(OBSERVER_PATH.read_text(encoding="utf-8"))
        cls.transport_tree = ast.parse(TRANSPORT_PATH.read_text(encoding="utf-8"))

    def test_the_bridge_advances_the_writer_exactly_once_and_only_to_result_assembly(self):
        advances = [node for node in ast.walk(self.tree)
                    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "advance"]
        self.assertEqual(len(advances), 1, "one call site, one boundary")
        self.assertEqual([ast.literal_eval(argument) for argument in advances[0].args],
                         ["result_assembly"])
        self.assertEqual(advances[0].keywords, [])

    def test_the_bridge_writes_no_bytes_reads_no_clock_and_has_no_destructor(self):
        self.assertEqual(set(called_names(self.tree)) & self.FORBIDDEN_CALLS, set())
        imported = set()
        for node in ast.walk(self.tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name.split(".")[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                imported.add((node.module or "").split(".")[0])
        self.assertEqual(imported & self.FORBIDDEN_IMPORTS, set())
        functions = {node.name for node in ast.walk(self.tree)
                     if isinstance(node, ast.FunctionDef)}
        self.assertNotIn("__del__", functions)

    def test_the_callback_catches_every_base_exception_and_answers_minus_one_or_zero(self):
        callback = find_method(find_class(self.tree, "ExecutionObserverBinding"),
                               "_before_results")
        self.assertEqual([type(node).__name__ for node in callback.body],
                         ["Expr", "Try", "Return"], "docstring, the guarded call, the answer")
        guarded = callback.body[1]
        self.assertEqual([handler.type.id for handler in guarded.handlers], ["BaseException"])
        returns = sorted((node for node in ast.walk(callback) if isinstance(node, ast.Return)),
                         key=lambda node: node.lineno)
        self.assertEqual([ast.literal_eval(node.value) for node in returns], [-1, 0])

    def test_the_writer_enters_a_phase_with_one_clock_reading_one_encode_and_one_handoff(self):
        enter = find_method(find_class(self.transport_tree, "RunPhaseWriter"), "_enter")
        names = called_names(enter)
        self.assertEqual(names.count("_read_clock"), 1)
        self.assertEqual(names.count("encode_record"), 1)
        self.assertEqual(names.count("_hand_off"), 1)
        self.assertEqual(names.count("_record"), 1)
        self.assertEqual(called_names(self.transport_tree).count("encode_record"), 1,
                         "no other call site encodes a record")


class ApiContractTests(unittest.TestCase):
    def test_the_binding_has_exactly_the_fixed_api(self):
        public = {name for name in dir(ExecutionObserverBinding) if not name.startswith("_")}
        self.assertEqual(public, {"attach", "detach", "failure", "raise_if_failed"})
        self.assertEqual(list(inspect.signature(ExecutionObserverBinding.__init__).parameters),
                         ["self", "library", "state", "writer"])
        for method in (ExecutionObserverBinding.attach, ExecutionObserverBinding.detach,
                       ExecutionObserverBinding.raise_if_failed):
            self.assertEqual(list(inspect.signature(method).parameters), ["self"])
        self.assertIsInstance(ExecutionObserverBinding.failure, property)
        self.assertIsNone(ExecutionObserverBinding.failure.fset)

    def test_the_module_exports_what_it_names(self):
        self.assertEqual(set(observer.__all__), {
            "BeforeResultsFn", "BoundaryC", "CAPABILITY", "ExecutionObserverBinding",
            "MAX_FRAME_BYTES", "OBSERVER_VERSION", "ObservationC", "ObserverBindingError",
            "ObserverC", "ReceiptC"})
        for name in observer.__all__:
            self.assertTrue(hasattr(observer, name), name)

    def test_the_writer_gained_exactly_two_public_members(self):
        # tests/test_run_phase_transport.py still pins the earlier six-name set and needs the
        # two names added; that file is outside the scope of this test and is not edited here.
        public = {name for name in dir(RunPhaseWriter) if not name.startswith("_")}
        self.assertEqual(public, {"advance", "failure", "phase", "phase_timing", "sequence",
                                  "unsent", "export_requested", "last_handed_frame_bytes"})
        self.assertEqual(list(inspect.signature(RunPhaseWriter.__init__).parameters),
                         ["self", "fd", "clock"])
        self.assertEqual(list(inspect.signature(RunPhaseWriter.advance).parameters),
                         ["self", "phase"])


if __name__ == "__main__":
    unittest.main()
