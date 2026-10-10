"""Python half of the run-only execution observer bridge (observer ABI v1).

On the success path of a native run the engine calls one registered `before_results`
callback, synchronously and once: after every execution callback ended and the result gate
sealed, before results open. The callback fills a receipt for the run's result_assembly
boundary; anything but return 0 with a valid receipt keeps the generation sealed, with no
result work and no retry. This module is the harness side of
that call for a run that exports its phases with RunPhaseWriter (run_phase_transport.py):
when the engine calls, the callback enters result_assembly on the writer, exactly once, and
reports the bytes the writer really handed to the socket.

    writer = RunPhaseWriter(args.run_phase_fd)
    writer.advance("preflight")
    ...
    writer.advance("execution")              # the caller, before any native work
    binding = ExecutionObserverBinding(library, state, writer)
    binding.attach()                         # probe the version, bind, register once
    ... native run ...
    binding.raise_if_failed()                # after native returned
    binding.detach()                         # before the state is destroyed

attach() never advances the writer: the caller has already entered "execution". The receipt
is exactly what the writer did: without a descriptor it is 0/0/0/0 (frame_bytes,
handed_bytes, export_requested, reserved); an exported record of N whole bytes is N/N/1/0
with 1 <= N <= 1024. Handed bytes are not an acknowledgement: nothing here claims that the
parent received or validated the record.

Nothing may unwind through native code. The callback catches every BaseException, keeps the
first in `failure`, returns -1 and leaves the receipt as the engine preset it;
raise_if_failed() raises that exception once native code has returned (a PhaseTransportError
stays a PhaseTransportError). A second call of the callback is a failure too: a run has one
observer call, and the record is never written twice.

The registration borrows what the binding owns: the engine copies the descriptor's values
but keeps calling the callback until it is removed, so the binding holds the descriptor and
the callback strongly. detach() is explicit and comes before the state is destroyed (no
destructor calls native code): it unregisters with a NULL descriptor, checks the result,
never frees a registered callback and never touches the phase descriptor, which the caller
owns. A detach that fails keeps the ownership, for the caller to retry or to leave the
callback alive; only a successful one lets go. A binding is single use: attach() registers
once.

Errors are ObserverBindingError. `code` is window_mode_unsupported (a library without
execution_observer_v1: a missing export, or a version other than 1), out_of_memory (the
engine could not allocate its consumer storage: registration status -4) or
harness_internal_error (any other refusal, a violation of the callback contract, a misuse).
The code's arguments are `code_args` because BaseException.args is the exception's own
tuple. This module carries no failure catalogue and decides no billing.
"""
from __future__ import annotations

import ctypes
from typing import Any, Mapping, Optional, Tuple

__all__ = [
    "BeforeResultsFn",
    "BoundaryC",
    "CAPABILITY",
    "ExecutionObserverBinding",
    "MAX_FRAME_BYTES",
    "OBSERVER_VERSION",
    "ObservationC",
    "ObserverBindingError",
    "ObserverC",
    "ReceiptC",
]

OBSERVER_VERSION = 1                  # pf_execution_observer_version() and every struct's version
MAX_FRAME_BYTES = 1024                # one phase record, LF included: the transport's limit
CAPABILITY = "execution_observer_v1"  # the one argument of window_mode_unsupported

_STATUS_OUT_OF_MEMORY = -4            # registration: the consumer storage could not be allocated
_STATUS_MEANING = {
    -1: "invalid size, version, callback or handle",
    -2: "unsupported execution contract",
    -3: "not quiescent: a run or a callback is in progress",
    -4: "out of memory",
}


# --- ctypes mirror of <pineforge/execution_observer.h> ---------------------------------------
#
# Fixed-width scalars at natural alignment, no packing; the LP64 sizes are 24, 32, 24 and 48.

class BoundaryC(ctypes.Structure):
    """pf_execution_boundary_v1: the generation the callback is called for."""
    _fields_ = [
        ("struct_size",    ctypes.c_uint32),
        ("version",        ctypes.c_uint32),
        ("run_generation", ctypes.c_uint64),
        ("attempt_serial", ctypes.c_uint64),
    ]


class ReceiptC(ctypes.Structure):
    """pf_boundary_receipt_v1: what the callback fills; the engine presets every field."""
    _fields_ = [
        ("struct_size",      ctypes.c_uint32),
        ("version",          ctypes.c_uint32),
        ("run_generation",   ctypes.c_uint64),
        ("frame_bytes",      ctypes.c_uint32),
        ("handed_bytes",     ctypes.c_uint32),
        ("export_requested", ctypes.c_uint32),
        ("reserved",         ctypes.c_uint32),
    ]


# pf_before_results_fn_v1: int (*)(void *context, const pf_execution_boundary_v1 *boundary,
#                                  pf_boundary_receipt_v1 *receipt)
BeforeResultsFn = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(BoundaryC), ctypes.POINTER(ReceiptC))


class ObserverC(ctypes.Structure):
    """pf_execution_observer_v1: the descriptor the engine copies at registration."""
    _fields_ = [
        ("struct_size",    ctypes.c_uint32),
        ("version",        ctypes.c_uint32),
        ("context",        ctypes.c_void_p),
        ("before_results", BeforeResultsFn),
    ]


class ObservationC(ctypes.Structure):
    """pf_execution_observation_v1: the snapshot strategy_execution_observation_v1 fills;
    the caller presets struct_size and version."""
    _fields_ = [
        ("struct_size",        ctypes.c_uint32),
        ("version",            ctypes.c_uint32),
        ("run_generation",     ctypes.c_uint64),
        ("attempt_serial",     ctypes.c_uint64),
        ("attempt_generation", ctypes.c_uint64),
        ("phase",              ctypes.c_uint32),
        ("fault_stage",        ctypes.c_uint32),
        ("attempt_outcome",    ctypes.c_uint32),
        ("boundary_delivered", ctypes.c_uint32),
    ]


class ObserverBindingError(Exception):
    """The observer binding failed on the Python side, before or beside the native run: a
    library without the capability or a harness fault, never a strategy outcome. `code` is
    window_mode_unsupported (code_args {"capability": "execution_observer_v1"}),
    out_of_memory or harness_internal_error (code_args {} for both). The arguments are
    code_args because BaseException.args is the exception's own tuple."""

    def __init__(self, code: str, message: str,
                 code_args: Optional[Mapping[str, object]] = None) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.code_args = dict(code_args or {})


def _unsupported(message: str) -> ObserverBindingError:
    return ObserverBindingError("window_mode_unsupported", message, {"capability": CAPABILITY})


def _status_error(action: str, status: int) -> ObserverBindingError:
    """The error for a nonzero status of strategy_set_execution_observer_v1: -4 is the
    engine's allocation failure, every other status a harness fault."""
    meaning = _STATUS_MEANING.get(status, "unknown status")
    code = "out_of_memory" if status == _STATUS_OUT_OF_MEMORY else "harness_internal_error"
    return ObserverBindingError(
        code, f"execution observer {action} was refused with status {status} ({meaning})")


def _check_arguments(header: Any, slot: Any) -> None:
    """The boundary and the receipt as the engine presets them: this layout and version, a
    minted generation and attempt, and the one generation in both."""
    if header.struct_size != ctypes.sizeof(BoundaryC) or header.version != OBSERVER_VERSION:
        raise ObserverBindingError(
            "harness_internal_error",
            f"before_results got a boundary of size {header.struct_size} and version "
            f"{header.version}, expected size {ctypes.sizeof(BoundaryC)} and version "
            f"{OBSERVER_VERSION}")
    if header.run_generation == 0 or header.attempt_serial == 0:
        raise ObserverBindingError(
            "harness_internal_error",
            f"before_results got generation {header.run_generation} and attempt serial "
            f"{header.attempt_serial}: both are minted, so nonzero")
    if slot.struct_size != ctypes.sizeof(ReceiptC) or slot.version != OBSERVER_VERSION:
        raise ObserverBindingError(
            "harness_internal_error",
            f"before_results got a receipt of size {slot.struct_size} and version "
            f"{slot.version}, expected size {ctypes.sizeof(ReceiptC)} and version "
            f"{OBSERVER_VERSION}")
    if slot.run_generation != header.run_generation:
        raise ObserverBindingError(
            "harness_internal_error",
            f"before_results got a receipt for generation {slot.run_generation} and a "
            f"boundary for generation {header.run_generation}")


def _handoff(writer: Any) -> Tuple[int, bool]:
    """(bytes handed over, exported) of the advance() that just returned, as the writer
    reports them, held to the receipt contract: no export is 0 bytes, an export is one whole
    record of 1 to MAX_FRAME_BYTES bytes."""
    exported = writer.export_requested
    handed = writer.last_handed_frame_bytes
    if type(exported) is not bool or type(handed) is not int:
        raise ObserverBindingError(
            "harness_internal_error",
            f"the phase writer reports export_requested {exported!r} and "
            f"last_handed_frame_bytes {handed!r}: a bool and an int are required")
    valid = 1 <= handed <= MAX_FRAME_BYTES if exported else handed == 0
    if not valid:
        raise ObserverBindingError(
            "harness_internal_error",
            f"the phase writer reports {handed} bytes handed over with export_requested "
            f"{exported}: an export is 1 to {MAX_FRAME_BYTES} bytes, no export is 0")
    return handed, exported


class ExecutionObserverBinding:
    """Registers the before_results callback of one strategy handle and answers it from a
    RunPhaseWriter (see the module text). `library` is the loaded strategy library, or
    anything with its three observer exports; `state` is the pf_strategy_t it was created
    with, which the binding borrows; `writer` is the run's RunPhaseWriter, already in phase
    "execution" when native work starts. Not thread safe: the thread that calls into native
    code drives it."""

    def __init__(self, library: Any, state: Any, writer: Any) -> None:
        self._library = library
        self._state = state
        self._writer = writer
        self._attach_called = False
        self._attached = False                       # the engine may hold the callback
        self._entered = False                        # before_results was called
        self._failure: Optional[BaseException] = None
        self._set_observer: Any = None
        self._callback: Optional[BeforeResultsFn] = None
        self._descriptor: Optional[ObserverC] = None

    @property
    def failure(self) -> Optional[BaseException]:
        """The first exception the callback met, for which it returned -1; None while the
        callback met none."""
        return self._failure

    def attach(self) -> None:
        """Probe the library, bind its three observer exports and register the callback,
        once. The writer is not advanced. Raises ObserverBindingError: window_mode_unsupported
        for a library without execution_observer_v1 (nothing is registered), out_of_memory
        for registration status -4, harness_internal_error for any other nonzero status and
        for a second attach(), which is refused without touching the library."""
        if self._attach_called:
            raise ObserverBindingError(
                "harness_internal_error",
                "execution observer attach() was already called: a binding registers once")
        self._attach_called = True
        library = self._library
        try:
            probe = library.pf_execution_observer_version
            set_observer = library.strategy_set_execution_observer_v1
            observation = library.strategy_execution_observation_v1
        except AttributeError as error:
            raise _unsupported(
                f"the strategy library lacks an execution observer export: {error}") from error
        probe.argtypes = []
        probe.restype = ctypes.c_uint32
        set_observer.argtypes = [ctypes.c_void_p, ctypes.POINTER(ObserverC)]
        set_observer.restype = ctypes.c_int
        observation.argtypes = [ctypes.c_void_p, ctypes.POINTER(ObservationC)]
        observation.restype = ctypes.c_int
        version = probe()
        if version != OBSERVER_VERSION:
            raise _unsupported(
                f"the strategy library reports execution observer version {version}, "
                f"not {OBSERVER_VERSION}")
        callback = BeforeResultsFn(self._before_results)   # the closure captures self
        descriptor = ObserverC(ctypes.sizeof(ObserverC), OBSERVER_VERSION, None, callback)
        # From here the engine may borrow both, whatever happens next: they stay with the
        # binding, and so does the right to unregister.
        self._set_observer = set_observer
        self._callback = callback
        self._descriptor = descriptor
        self._attached = True
        status = set_observer(self._state, ctypes.pointer(descriptor))
        if status != 0:
            self._attached = False  # refused: the descriptor the engine had is unchanged
            raise _status_error("registration", status)

    def detach(self) -> None:
        """Unregister with a NULL descriptor, before the state is destroyed. A binding that
        is not registered (never attached, refused, already detached) does nothing. A
        nonzero status raises ObserverBindingError and keeps the descriptor and the callback
        with the binding: the engine may still hold them, and detach() may be called again.
        Only a status 0 lets them go. No phase descriptor is closed or touched."""
        if not self._attached:
            return
        status = self._set_observer(self._state, None)
        if status != 0:
            raise _status_error("removal", status)
        self._attached = False
        self._callback = None
        self._descriptor = None

    def raise_if_failed(self) -> None:
        """Raise the first exception the callback met, the very object (a
        PhaseTransportError stays one). Call it after native code has returned: raised
        inside the callback, ctypes would only print it."""
        if self._failure is not None:
            raise self._failure

    def _before_results(self, context: Any, boundary: Any, receipt: Any) -> int:
        """The native callback: 0 once the receipt is filled, -1 for anything else. Nothing
        may unwind through the C caller, so every BaseException is kept, the first one only,
        and refused here."""
        try:
            self._deliver(boundary, receipt)
        except BaseException as error:
            if self._failure is None:
                self._failure = error
            return -1
        return 0

    def _deliver(self, boundary: Any, receipt: Any) -> None:
        if self._entered:
            raise ObserverBindingError(
                "harness_internal_error",
                "before_results was called again: a run has one observer call")
        self._entered = True
        if not boundary or not receipt:
            raise ObserverBindingError(
                "harness_internal_error", "before_results got a null boundary or receipt")
        # Only the two pointers the engine passed are read, and only when nonnull: a pointer
        # that is not what the engine passed is outside the C lifetime contract.
        header, slot = boundary.contents, receipt.contents
        _check_arguments(header, slot)
        writer = self._writer
        if writer.phase != "execution":
            raise ObserverBindingError(
                "harness_internal_error",
                f"before_results arrived with the phase writer in phase {writer.phase!r}, "
                "not 'execution': the caller enters execution before any native work")
        writer.advance("result_assembly")  # the one handoff; a refusal raises, typed
        handed, exported = _handoff(writer)
        # Only now, with the record handed over, is the receipt filled. The boundary and the
        # receipt header, generation included, are never written.
        slot.frame_bytes = handed
        slot.handed_bytes = handed
        slot.export_requested = 1 if exported else 0
        slot.reserved = 0
