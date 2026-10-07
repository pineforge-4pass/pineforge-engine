"""Startup support for ctypes witnesses using the build's sanitizer runtime."""

import ctypes
import gc
import os
import sys


_native_leak_check = None


def run_with_finalizer(main, finalize):
    """Unwind the witness before finalization; neither failure can exit green."""
    try:
        return main()
    finally:
        finalize()


def finalize_sanitizer():
    if _native_leak_check is not None:
        gc.collect()
        _native_leak_check()
        print("PASS LeakSanitizer check after native cleanup, before CPython teardown", flush=True)


def prepare_sanitizer(sanitizer):
    global _native_leak_check
    if sanitizer is None:
        return {"sanitizer": None}
    runtime = ctypes.CDLL(None)
    try:
        getattr(runtime, {"asan": "__asan_init", "tsan": "__tsan_init"}[sanitizer])
    except AttributeError as error:
        raise RuntimeError("sanitizer runtime must be preloaded before Python starts") from error
    receipt = {"sanitizer": sanitizer, "runtime_initialized_before_strategy": True,
        "startup_environment": {key: os.environ.get(key) for key in (
            "LD_PRELOAD", "DYLD_INSERT_LIBRARIES", "ASAN_OPTIONS", "UBSAN_OPTIONS",
            "TSAN_OPTIONS", "PYTHONMALLOC")}}
    if sanitizer == "asan" and sys.platform != "darwin":
        # Run LSan after the witness releases its native handles
        # and Python collects cycles, while CPython's own globals remain roots.
        # CPython teardown otherwise leaves interpreter allocations orphaned.
        # The runtime's one-shot API performs the check here instead of repeating
        # it during libc exit. This checks pre-teardown reachability, not leaks
        # introduced by later teardown. No sanitizer option or suppression is
        # changed; an unreachable native allocation still terminates the test.
        check = runtime.__lsan_do_leak_check
        check.argtypes = []
        check.restype = None

        _native_leak_check = check
        receipt["leak_check"] = "__lsan_do_leak_check in status-bearing outer epilogue"
    # Native runner children already link their selected runtime. Only Python
    # needs the preload; retain every sanitizer option for all native children.
    os.environ.pop("LD_PRELOAD", None)
    os.environ.pop("DYLD_INSERT_LIBRARIES", None)
    return receipt
