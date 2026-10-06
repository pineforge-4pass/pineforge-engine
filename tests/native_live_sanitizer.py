"""Startup support for ctypes witnesses using the build's sanitizer runtime."""

import atexit
import ctypes
import gc
import os
import sys


def prepare_sanitizer(sanitizer):
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
        # Run the full LSan check after the witness releases its native handles
        # and Python collects cycles, while CPython's own globals remain roots.
        # CPython teardown otherwise leaves interpreter allocations orphaned.
        # The runtime's one-shot API performs the check here instead of repeating
        # it during libc exit. No sanitizer option or suppression is changed;
        # an unreachable native allocation still terminates the test.
        check = runtime.__lsan_do_leak_check
        check.argtypes = []
        check.restype = None

        @atexit.register
        def check_native_leaks():
            gc.collect()
            check()
            print("PASS LeakSanitizer check after native cleanup, before CPython teardown", flush=True)

        receipt["leak_check"] = "__lsan_do_leak_check at Python atexit"
    # Native runner children already link their selected runtime. Only Python
    # needs the preload; retain every sanitizer option for all native children.
    os.environ.pop("LD_PRELOAD", None)
    os.environ.pop("DYLD_INSERT_LIBRARIES", None)
    return receipt
