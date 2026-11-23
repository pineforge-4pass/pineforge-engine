"""Witness the strategy load boundary through the real harness request path."""

import hashlib
import io
import json
import os
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch


DIGEST_ENV = "PINEFORGE_STRATEGY_SO_SHA256"
PAYLOAD = b"issue 956 strategy bytes\n"
LOADER_WITNESS = "issue 956 loader reached"


def diagnostic(result, label, observed, expected):
    print(f"{result} {label}: observed={observed!r}, expected={expected!r}", file=sys.stderr)
    return 1


def failure_line(message, code, args):
    return json.dumps({"engine": "pineforge", "error": message, "code": code, "args": args},
                      separators=(",", ":")) + "\n"


def attempt(harness, path, digest):
    # No native execution. Reaching the strategy CDLL call is observable
    # even if the loader reports a failure and the harness emits a typed line.
    calls = []

    def observe(name):
        calls.append(name)
        raise OSError(LOADER_WITNESS)

    report, errors = io.StringIO(), io.StringIO()
    with patch.dict(os.environ, {DIGEST_ENV: digest, "PINEFORGE_RUN_JSON_PROFILE": "unjailed"}), \
            patch.object(harness.ctypes, "CDLL", side_effect=observe), redirect_stdout(report), \
            redirect_stderr(errors):
        try:
            status = harness.main(["--so", str(path), "--capabilities-json"])
        except SystemExit as error:
            status = error.code
    return calls, status, report.getvalue(), errors.getvalue()


def check(harness, directory):
    path = directory / "strategy.so"
    try:
        path.write_bytes(PAYLOAD)
        stored = path.read_bytes()
    except OSError as error:
        return diagnostic("NOT COMPARED", "strategy fixture read/write", str(error), PAYLOAD)
    if stored != PAYLOAD:
        return diagnostic("NOT COMPARED", "strategy fixture bytes", stored, PAYLOAD)

    matching = hashlib.sha256(PAYLOAD).hexdigest()
    wrong = hashlib.sha256(b"issue 956 different strategy bytes\n").hexdigest()
    if wrong == matching:
        return diagnostic("NOT COMPARED", "distinct digest precondition", wrong,
                          "a digest different from " + matching)

    library_error = "strategy_library_incompatible"
    load_failed = {"reason": "load_failed"}
    control = ([str(path)], 1, failure_line(
        "cannot load the strategy library: " + LOADER_WITNESS, library_error, load_failed), "")
    observed = attempt(harness, path, matching)
    if observed != control:
        return diagnostic("NOT COMPARED", "matching digest loader control", observed, control)

    bad_request = failure_line(
        f"error: {DIGEST_ENV} must be 64 hexadecimal digits",
        "run_request_invalid", {"option": "arguments"})
    empty = directory / "empty.so"
    try:
        empty.write_bytes(b"")
        length = empty.stat().st_size
    except OSError as error:
        return diagnostic("NOT COMPARED", "empty fixture creation", str(error), 0)
    if length != 0:
        return diagnostic("NOT COMPARED", "empty fixture byte count", length, 0)
    cases = (
        ("mismatch", path, wrong, failure_line(
            "cannot load the strategy library: SHA-256 mismatch", library_error, load_failed)),
        ("empty digest", path, "", bad_request),
        ("nonhex digest", path, "z" * 64, bad_request),
        ("empty file", empty, hashlib.sha256(b"").hexdigest(), failure_line(
            "cannot load the strategy library: SHA-256 verification requires a nonempty regular file",
            library_error, load_failed)),
    )
    for label, artifact, digest, line in cases:
        calls, status, output, errors = attempt(harness, artifact, digest)
        if calls:
            return diagnostic("FAIL", label + " reached ctypes.CDLL", len(calls), 0)
        if (status, output, errors) != (1, line, ""):
            return diagnostic("FAIL", label + " typed refusal", (status, output, errors),
                              (1, line, ""))

    # A bare dlopen name would search for a strategy other than the file
    # hashed in the current directory.
    observed = attempt(harness, Path(path.name), matching)
    if observed != control:
        return diagnostic("FAIL", "relative name uses the verified path", observed, control)
    print("PASS strategy library digest refusals precede ctypes.CDLL")
    return 0


def main():
    source = Path(__file__).resolve().parents[1] / "docker"
    sys.path.insert(0, str(source))
    try:
        import run_json
        actual = Path(run_json.__file__).resolve()
    except Exception as error:
        return diagnostic("NOT COMPARED", "harness import", str(error), "importable run_json")
    expected = source / "run_json.py"
    if actual != expected:
        return diagnostic("NOT COMPARED", "harness source path", str(actual), str(expected))
    try:
        temporary = tempfile.TemporaryDirectory()
    except OSError as error:
        return diagnostic("NOT COMPARED", "fixture directory", str(error), "writable directory")
    with temporary as directory:
        directory = Path(directory).resolve()
        original = Path.cwd()
        try:
            os.chdir(directory)
        except OSError as error:
            return diagnostic("NOT COMPARED", "fixture directory cwd", str(error), str(directory))
        try:
            return check(run_json, directory)
        finally:
            os.chdir(original)


if __name__ == "__main__":
    sys.exit(main())
