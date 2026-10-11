#!/usr/bin/env python3
"""L5 control for the four private-fixture probe guards.

scripts/test_symbol_calendar.py, scripts/test_run_strategy_mincontract.py,
scripts/test_run_strategy_magnifier.py and scripts/test_run_strategy_emit_window.py
each compute a ``tests/fixtures/...`` path from their own location and, when it
is absent, print ``SKIP private fixtures not present: <path>`` and exit 77. This
control proves BOTH branches of each guard, so a reviewer reads the proof in the
tree instead of trusting one run of one tree:

* RED (fixture absent): the probe is copied to ``<tree>/scripts/`` with no
  fixture anywhere under ``<tree>``; running the copy must exit 77 AND print the
  exact skip literal naming the probe's repo-relative path.
* GREEN (fixture present): a second tree holds the same copy and a minimal
  stand-in at the fixture path; running the copy must NOT exit 77 and must NOT
  print the skip literal (the guard did not trigger). The stand-in is not real
  data, so the probe is expected to fail afterwards -- the control asserts only
  that it did not skip, never that it passed.

The control builds both worlds itself, one script run, one assertion per branch,
per probe: it branches on nothing in the real tree and carries no fixture.

Environment knobs (for reproduction only; both default to the repo):

* ``PF_GUARD_PROBES``   directory holding the probe sources to copy (default
  ``<repo>/scripts``). Point it at a scratch copy to prove the control is real:
  break one probe's guard path there and the RED branch fails.
* ``PF_GUARD_SCRATCH``  directory to build the worlds under (default a fresh
  temporary directory). The control removes each world when its test ends.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROBE_SOURCES = Path(os.environ.get("PF_GUARD_PROBES") or (REPO / "scripts"))

# Probe file -> (the repo-relative fixture its guard tests, a minimal stand-in).
# The paths are read off each probe's own guard; each is the payload of the
# probe's own "SKIP private fixtures not present: ..." line.
PROBES = {
    "test_symbol_calendar.py": (
        "tests/fixtures/symbol_calendar/xc-cal-aapl15/calendar.json",
        {"schema": "pineforge-symbol-calendar/v1", "sessions": []},
    ),
    "test_run_strategy_mincontract.py": (
        "tests/fixtures/syminfo_mincontract/aapl-15/meta.json",
        {"symbol": "NASDAQ:AAPL", "interval": "15"},
    ),
    "test_run_strategy_magnifier.py": (
        "tests/fixtures/magnifier_intrabars/mi-ltf-f-1D/meta.json",
        {"symbol": "BINANCE:ETHUSDT.P", "interval": "1"},
    ),
    "test_run_strategy_emit_window.py": (
        "tests/fixtures/run_harness_window/rh-diag-firstbar2-es115/meta.json",
        {"symbol": "CME_MINI:ES1!", "interval": "15"},
    ),
}


def _skip_literal(repo_relative: str) -> str:
    return f"SKIP private fixtures not present: {repo_relative}"


def _probe_env() -> dict:
    """A clean environment for the probe copy: no PYTHONPATH to leak the repo."""
    env = dict(os.environ)
    for name in ("PYTHONPATH", "PYTHONHOME", "PINEFORGE_PRIVATE_FIXTURES"):
        env.pop(name, None)
    return env


class PrivateFixtureGuardControl(unittest.TestCase):
    """One test per probe; each proves the absent and present branches."""

    def setUp(self) -> None:
        base = os.environ.get("PF_GUARD_SCRATCH")
        if base:
            Path(base).mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix="pf-guard-", dir=base or None))
        self.addCleanup(shutil.rmtree, self.root, True)

    def _world(self, name: str, probe: str):
        """A tree holding only a copy of ``probe`` under its own scripts/."""
        tree = self.root / name
        (tree / "scripts").mkdir(parents=True)
        source = PROBE_SOURCES / probe
        self.assertTrue(source.is_file(), f"probe source missing: {source}")
        copy = tree / "scripts" / probe
        shutil.copyfile(source, copy)
        return tree, copy

    def _run(self, probe_copy: Path):
        return subprocess.run(
            [sys.executable, "-I", str(probe_copy)],
            cwd=str(probe_copy.parent), capture_output=True, text=True,
            env=_probe_env(),
        )

    def _check(self, probe: str) -> None:
        repo_relative, stand_in = PROBES[probe]
        literal = _skip_literal(repo_relative)

        # RED: no fixture anywhere under the tree -- the guard must fire.
        red, red_copy = self._world(f"red-{probe}", probe)
        self.assertFalse((red / repo_relative).exists(),
                         "the RED tree must not carry the fixture")
        got = self._run(red_copy)
        self.assertEqual(got.returncode, 77,
                         f"RED {probe}: exit {got.returncode}, expected 77\n"
                         f"stdout:\n{got.stdout}\nstderr:\n{got.stderr}")
        self.assertIn(literal, got.stdout.splitlines(),
                      f"RED {probe}: the exact skip line is missing\n"
                      f"stdout:\n{got.stdout}")

        # GREEN: a minimal stand-in at the fixture path -- the guard must not fire.
        green, green_copy = self._world(f"green-{probe}", probe)
        fixture = green / repo_relative
        fixture.parent.mkdir(parents=True, exist_ok=True)
        fixture.write_text(json.dumps(stand_in), encoding="utf-8")
        got = self._run(green_copy)
        self.assertNotEqual(got.returncode, 77,
                            f"GREEN {probe}: exit 77, the guard skipped a present fixture\n"
                            f"stdout:\n{got.stdout}\nstderr:\n{got.stderr}")
        self.assertNotIn(literal, got.stdout,
                         f"GREEN {probe}: the skip literal leaked with the fixture present\n"
                         f"stdout:\n{got.stdout}")

    def test_symbol_calendar_guard(self) -> None:
        self._check("test_symbol_calendar.py")

    def test_mincontract_guard(self) -> None:
        self._check("test_run_strategy_mincontract.py")

    def test_magnifier_guard(self) -> None:
        self._check("test_run_strategy_magnifier.py")

    def test_emit_window_guard(self) -> None:
        self._check("test_run_strategy_emit_window.py")


if __name__ == "__main__":
    unittest.main()
