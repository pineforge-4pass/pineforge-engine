"""Sequential timezone switches through separately linked engine modules."""

import argparse
import ctypes
import subprocess
import sys


CASES = ("same-module", "two-modules")
STAMP_MS = 1730730600000  # 2024-11-04 14:30 UTC.
THIRTY_DAYS_MS = 30 * 86_400_000


def run_case(module_a, module_b, case):
    # RTLD_LOCAL is ctypes' default: each helper contains its own engine image.
    a = ctypes.CDLL(module_a)
    b = ctypes.CDLL(module_b)
    for module in (a, b):
        module.module_local_hour.argtypes = (ctypes.c_longlong, ctypes.c_char_p)
        module.module_local_hour.restype = ctypes.c_int

    other = a if case == "same-module" else b
    # London must switch libc's TZ; UTC would take the arithmetic fast path.
    # The final stamp misses A's first interval without discarding valid caches.
    actual = [
        a.module_local_hour(STAMP_MS, b"America/New_York"),
        other.module_local_hour(STAMP_MS, b"Europe/London"),
        a.module_local_hour(STAMP_MS + THIRTY_DAYS_MS, b"America/New_York"),
    ]
    expected = [9, 14, 9]
    print(f"{case}: actual={actual}, expected={expected}", flush=True)
    if actual != expected:
        raise AssertionError(f"{case}: expected {expected}, got {actual}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module-a", required=True)
    parser.add_argument("--module-b", required=True)
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()
    if args.case is not None:
        run_case(args.module_a, args.module_b, args.case)
        return

    # A fresh interpreter per case prevents historical thread-local intervals
    # from turning the final call into a cache hit that masks the old defect.
    for case in CASES:
        subprocess.run(
            [
                sys.executable,
                "-B",
                __file__,
                "--module-a",
                args.module_a,
                "--module-b",
                args.module_b,
                "--case",
                case,
            ],
            check=True,
        )


if __name__ == "__main__":
    main()
