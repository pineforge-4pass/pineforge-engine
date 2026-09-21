#!/usr/bin/env python3
"""Mutation self-test for examples/native/run_example.cmake (R5 gap lane P7).

Every example_* CTest row runs its host through that runner, and passes only
when the host exits 0 AND prints its summary line. The runner must therefore
fail a host that prints the line and then exits nonzero, dies on a signal or
hangs, and a host that exits 0 without the line, and it must pass a clean one.

With --examples-build-dir, the registered example_* rows are also read back
from CTest (--show-only=json-v1): each runs its own host through the runner
with a nonempty expected line, and none sets a property that would override
the runner's verdict -- PASS_REGULAR_EXPRESSION ignores the exit code again,
and the others re-grade, invert or skip it.

Stdlib only. The fixture hosts are /bin/sh scripts in a temporary directory.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "examples/native/run_example.cmake"
LINE = "closed trades: [1-9]"
OVERRIDING_PROPERTIES = frozenset((
    "PASS_REGULAR_EXPRESSION",
    "FAIL_REGULAR_EXPRESSION",
    "SKIP_REGULAR_EXPRESSION",
    "WILL_FAIL",
    "SKIP_RETURN_CODE",
))
FIXTURES = {
    "clean": 'echo "closed trades: 1"',
    "line_on_stderr": 'echo "closed trades: 1" >&2',
    "line_then_nonzero_exit": 'echo "closed trades: 1"\nexit 3',
    "line_then_abort": 'echo "closed trades: 1"\nkill -s ABRT $$',
    "line_then_hang": 'echo "closed trades: 1"\nexec sleep 30',
    "no_line": 'echo "closed trades: 0"',
}

TOOLS = argparse.Namespace(cmake=None, ctest=None, examples_build_dir=None)


def normalized(text: str) -> str:
    """CMake wraps message(FATAL_ERROR) text, so compare on single spaces."""
    return " ".join(text.split())


class Runner(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pf-example-runner-")
        self.addCleanup(directory.cleanup)
        self.hosts = Path(directory.name)

    def host(self, name: str) -> Path:
        path = self.hosts / name
        path.write_text("#!/bin/sh\n" + FIXTURES[name] + "\n")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def run_runner(self, *definitions: str) -> tuple[int, str]:
        result = subprocess.run([TOOLS.cmake, *definitions, "-P", str(RUNNER)],
                                capture_output=True, text=True, timeout=120)
        return result.returncode, normalized(result.stdout + result.stderr)

    def run_row(self, name: str, *extra: str) -> tuple[int, str]:
        return self.run_runner(f"-DEXAMPLE={self.host(name)}", f"-DEXPECT={LINE}", *extra)

    def test_a_clean_host_passes_and_its_output_is_echoed(self):
        code, text = self.run_row("clean")
        self.assertEqual(code, 0, text)
        self.assertIn("closed trades: 1", text)

    def test_the_line_on_stderr_counts_as_it_does_under_ctest(self):
        code, text = self.run_row("line_on_stderr")
        self.assertEqual(code, 0, text)

    def test_the_line_then_a_nonzero_exit_fails(self):
        # The case a PASS_REGULAR_EXPRESSION row let through.
        code, text = self.run_row("line_then_nonzero_exit")
        self.assertNotEqual(code, 0, text)
        self.assertIn("closed trades: 1", text)
        self.assertIn("did not exit 0 (3)", text)

    def test_the_line_then_a_signal_fails(self):
        code, text = self.run_row("line_then_abort")
        self.assertNotEqual(code, 0, text)
        self.assertIn("did not exit 0", text)

    def test_the_line_then_a_hang_fails_on_the_runner_timeout(self):
        code, text = self.run_row("line_then_hang", "-DTIMEOUT=1")
        self.assertNotEqual(code, 0, text)
        self.assertIn("did not exit 0", text)
        self.assertIn("timeout", text.lower())

    def test_exit_0_without_the_line_fails(self):
        code, text = self.run_row("no_line")
        self.assertNotEqual(code, 0, text)
        self.assertIn(f'printed no line matching "{LINE}"', text)

    def test_a_missing_host_or_line_is_refused(self):
        for definitions in ((), (f"-DEXAMPLE={self.host('clean')}",), (f"-DEXPECT={LINE}",),
                            (f"-DEXAMPLE={self.host('clean')}", "-DEXPECT=")):
            with self.subTest(definitions=definitions):
                code, text = self.run_runner(*definitions)
                self.assertNotEqual(code, 0, text)
                self.assertIn("pass -DEXAMPLE=<host> and -DEXPECT=<regex>", text)


class RegisteredRows(unittest.TestCase):
    def rows(self) -> list[dict]:
        result = subprocess.run(
            [TOOLS.ctest, "--show-only=json-v1", "--test-dir", str(TOOLS.examples_build_dir)],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return [test for test in json.loads(result.stdout)["tests"]
                if test["name"].startswith("example_")]

    def test_every_example_row_runs_its_own_host_through_the_runner(self):
        if TOOLS.examples_build_dir is None:
            self.skipTest("pass --examples-build-dir to read the registered rows back")
        rows = self.rows()
        self.assertTrue(rows, "no example_* row is registered")
        for row in rows:
            with self.subTest(row=row["name"]):
                command = row["command"]
                self.assertEqual(command[-2], "-P")
                self.assertEqual(Path(command[-1]).resolve(), RUNNER.resolve())
                hosts = [arg.split("=", 1)[1] for arg in command if arg.startswith("-DEXAMPLE=")]
                lines = [arg.split("=", 1)[1] for arg in command if arg.startswith("-DEXPECT=")]
                self.assertEqual(len(hosts), 1, command)
                self.assertEqual(len(lines), 1, command)
                self.assertEqual(Path(hosts[0]).name, row["name"][len("example_"):])
                self.assertTrue(lines[0].strip(), command)
                overriding = {prop["name"] for prop in row.get("properties", [])} & OVERRIDING_PROPERTIES
                self.assertFalse(overriding, f"{row['name']} sets {sorted(overriding)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    parser.add_argument("--ctest", default=shutil.which("ctest"))
    parser.add_argument("--examples-build-dir", type=Path, default=None,
                        help="the examples/native build directory whose example_* rows to read back")
    args, rest = parser.parse_known_args()
    if not args.cmake or (args.examples_build_dir is not None and not args.ctest):
        print("test_example_runner: cmake (and ctest, with --examples-build-dir) must be found",
              file=sys.stderr)
        return 2
    TOOLS.cmake, TOOLS.ctest, TOOLS.examples_build_dir = args.cmake, args.ctest, args.examples_build_dir
    program = unittest.main(argv=[sys.argv[0], *rest], exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
