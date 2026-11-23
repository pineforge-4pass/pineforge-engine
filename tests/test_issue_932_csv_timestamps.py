"""Exercise the CSV timestamp contract at the ctypes runner's input boundary."""

from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import run_strategy as runner

HEADER = "timestamp,open,high,low,close,volume\n"
FIRST = "1700000000000,100,101,99,100.5,10\n"
FRACTION = "1.7000000000605e12"
SUFFIX = ",100.5,101.5,100,101,12\n"
REPRO = HEADER + FIRST + FRACTION + SUFFIX
FIRST_MS = 1700000000000
TRUNCATED = 1700000000060
EXACT_MS = 9007199254740993  # the first positive integer binary64 cannot store


def verify_repro(text):
    rows = text.splitlines()
    if len(rows) != 3 or text != REPRO:
        reason = "expected 2 populated bars" if len(rows) < 3 else "wrong issue CSV"
        raise SystemExit(
            f"NOT COMPARED: {reason}; expected {REPRO!r}; got {text!r}")


def stamp_list(loaded):
    bars, count, *_ = loaded
    return [bars[i].timestamp for i in range(count)]


def require_stamps(label, got, expected):
    if not got:
        raise SystemExit(f"NOT COMPARED: {label}: no bars; expected {expected!r}; got {got!r}")
    if got != expected:
        raise SystemExit(f"FAIL: {label}: expected {expected!r}; got {got!r}")


def require_rejection(path, reader, label, cell, *, line=3, issue=False):
    expected = (
        f"OHLCV CSV {path} line {line}: "
        f"expected a decimal integer timestamp in the signed 64-bit range, got {cell!r}"
    )
    try:
        got = reader()
    except ValueError as error:
        if str(error) != expected:
            raise SystemExit(f"FAIL: {label}: expected {expected!r}; got {str(error)!r}") from None
    else:
        if not got:
            raise SystemExit(f"NOT COMPARED: {label}: no bars; expected {expected!r}; got {got!r}")
        if issue and got == [FIRST_MS, TRUNCATED]:
            raise SystemExit(
                "FAIL #932: chart loader accepted row 3 timestamp 1.7000000000605e12 "
                "as 1700000000060; expected rejection at line 3")
        raise SystemExit(f"FAIL: {label}: expected {expected!r}; got {got!r}")


def verify_input_guards():
    for label, text in (
            ("empty", HEADER),
            ("wrong input", REPRO.replace(FRACTION, str(TRUNCATED))),
    ):
        try:
            verify_repro(text)
        except SystemExit as error:
            if str(error.code).startswith("NOT COMPARED:"):
                print(error.code)
                continue
            got = error.code
        else:
            got = "comparison admitted"
        raise SystemExit(f"FAIL: {label} input guard: expected NOT COMPARED; got {got!r}")


def exercise(path, mode):
    readers = {
        f"chart/{mode}": lambda: stamp_list(runner._load_bars(path)),
        f"auxiliary/{mode}": lambda: stamp_list(runner._load_aux_bars_snapshot(path)),
        f"chart timestamps/{mode}": lambda: runner._feed_timestamps(path),
    }
    verify_repro(path.read_text(encoding="utf-8"))
    for label, read in readers.items():
        require_rejection(path, read, label, FRACTION, issue=label == "chart/numpy")
    for cell in ("1.700000000060e12", "1700000000060.0", "9223372036854775808", ""):
        path.write_text(HEADER + FIRST + "\n" + cell + SUFFIX, encoding="utf-8")
        for label, read in readers.items():
            require_rejection(path, read, label, cell, line=4)
    path.write_text(HEADER + FIRST + str(EXACT_MS) + SUFFIX, encoding="utf-8")
    for label, read in readers.items():
        require_stamps(label, read(), [FIRST_MS, EXACT_MS])

    # The integer just above 2**53 must be kept at its own inclusive bound,
    # and must be excluded when the end is the preceding millisecond.
    require_stamps(f"chart start/{mode}", stamp_list(runner._load_bars(
        path, ohlcv_start_ms=EXACT_MS)), [EXACT_MS])
    require_stamps(f"chart timestamp end/{mode}", runner._feed_timestamps(
        path, ohlcv_end_ms=EXACT_MS - 1), [FIRST_MS])

    max_ms = 9223372036854775807
    path.write_text(HEADER + FIRST + str(max_ms) + SUFFIX, encoding="utf-8")
    for label, read in readers.items():
        require_stamps(label, read(), [FIRST_MS, max_ms])
    require_stamps(f"chart timestamp wide end/{mode}", runner._feed_timestamps(
        path, ohlcv_end_ms=max_ms + 1), [FIRST_MS, max_ms])


def main():
    verify_input_guards()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "bars.csv"
        path.write_text(REPRO, encoding="utf-8")
        # NumPy is optional in the runner and is not installed by hosted CI.
        # The stdlib path is a real comparison, including when it runs alone.
        try:
            __import__("numpy")
        except ImportError:
            print("NOT COMPARED: expected NumPy for the issue path; got no NumPy")
        else:
            exercise(path, "numpy")
            print("PASS #932 (NumPy): invalid CSV timestamps refused by line; integer milliseconds exact")
            path.write_text(REPRO, encoding="utf-8")
        with patch.dict(sys.modules, {"numpy": None}):
            exercise(path, "stdlib")
        print("PASS #932 (stdlib): invalid CSV timestamps refused by line; integer milliseconds exact")


if __name__ == "__main__":
    main()
