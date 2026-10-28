"""Compare the real Python descriptor with C++ manifests pinned by the new test.

Run by CTest with --native pointing to test_scratch_manifest_sizing. No fake
library or native allocation is needed: the executable emits policy values.
"""
from __future__ import annotations

import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
import run_json

FIELDS = ("K_q", "B_q", "K_e", "B_e", "K_c", "B_c",
          "U", "B_U", "V", "B_V", "R")
FLOOR, CAP, MAXIMUM = 65536, 1048576, (1 << 64) - 1


def expected(bars, fresh_per_bar=1, literals=0, fixed_fresh=0):
    # Independent derivation: S=max(65536,min(CAP,literals+bars*fresh+fixed)).
    # K_e=4S, K_c=floor(S/2); byte factors are 96,129,64, respectively.
    # For the three native pins S is 65536, 200000, 1048576. Never transcribe
    # the byte totals from C++ output or use the producer's policy constants.
    need = min(MAXIMUM, literals + bars * fresh_per_bar + fixed_fresh)
    rows = max(FLOOR, min(CAP, need))
    erased, consumed = 4 * rows, rows // 2
    return (0, 0, erased, 96 * erased, consumed, 129 * consumed,
            rows, 64 * rows, 27, 6912, 2 * bars)


def values(manifest):
    assert (manifest.struct_size, manifest.version,
            manifest.backend, manifest.reserved) == (104, 1, 1, 0)
    assert ctypes.sizeof(manifest) == 104
    return tuple(getattr(manifest, name) for name in FIELDS)


def compare_native(native):
    result = subprocess.run([str(native), "--manifest-values"],
                            check=True, capture_output=True, text=True)
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert [row[0] for row in rows] == [0, 200000, CAP], result.stdout
    for bars, *native_fields in rows:
        python_fields = values(run_json.default_scratch_manifest(bars))
        assert python_fields == tuple(native_fields) == expected(bars), bars


def check_boundaries():
    previous = (0,) * len(FIELDS)
    for bars in (0, 1, FLOOR - 1, FLOOR, FLOOR + 1, 100000, 200000,
                 CAP, CAP + 1, 2_000_000, MAXIMUM // 2):
        current = values(run_json.default_scratch_manifest(bars))
        assert current == expected(bars), bars
        assert all(a <= b for a, b in zip(previous, current)), bars
        previous = current
    stream = list(expected(0))
    stream[-1] = 65536
    assert values(run_json.default_scratch_manifest(None)) == tuple(stream)

    for terms in ((100000, 2, 7, 11), (CAP, 0, 7, 11),
                  (MAXIMUM // 2, MAXIMUM, 7, 11)):
        assert values(run_json.default_scratch_manifest(*terms)) == expected(*terms)
    assert values(run_json.default_scratch_manifest(100000, 2, 7, 11, True)) == (
        expected(100000, 2, 7, 11))
    for terms, need in (((CAP + 1, 1, 0, 0), CAP + 1),
                        ((MAXIMUM // 2, MAXIMUM, 7, 11), MAXIMUM)):
        try:
            run_json.default_scratch_manifest(*terms, declared=True)
        except run_json.RunFailure as error:
            assert (error.code, error.code_args) == (
                "scratch_capacity_exceeded", {"resource": "cohort"})
            assert f"{CAP:,}" in str(error) and f"{need:,}" in str(error)
        else:
            raise AssertionError("declared demand above CAP did not refuse")
    for bars in (-1, MAXIMUM // 2 + 1, MAXIMUM):
        try:
            run_json.default_scratch_manifest(bars)
        except run_json.RunFailure as error:
            assert (error.code, error.code_args) == (
                "scratch_capacity_exceeded", {"resource": "layout"})
        else:
            raise AssertionError("root overflow did not refuse")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--admission-only", action="store_true")
    args = parser.parse_args()
    if args.admission_only:
        # Distinct CTest row for the same TU, following the test registry's
        # script-launcher rule; preserve the native process's exit status.
        return subprocess.run([str(args.native.resolve()), "--admission"]).returncode
    compare_native(args.native.resolve())
    check_boundaries()
    print("Python/native scratch manifest sizing: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
