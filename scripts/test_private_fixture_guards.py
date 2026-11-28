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

import importlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock
from test_run_pytest_no_skips import declared_inputs

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


class NestedFixtureGuards(unittest.TestCase):
    def test_unittest_cannot_skip_a_required_input_inside_a_passing_row(self):
        from check_settlement_cpp_abi import MANIFEST

        # Exercise the actual decorators during collection. The same missing
        # input must refuse a private job even on a fully overlaid checkout.
        regular_exists = Path.exists
        for name, missing, case in (
                ('test_native_cpp_versions', REPO / 'tests/fixtures/native_cpp_abi',
                 'FixtureAuthentication'),
                ('test_settlement_cpp_abi', MANIFEST, 'SettlementAbi.test_current_include_tree'),
                ('test_verify_corpus_metrics',
                 REPO / 'tests/fixtures/coof_cascade_identity/w7-coof-cascade-identity',
                 'CoofCascadeIdentityTapeTests')):
            declared_inputs((REPO / 'scripts' / (name + '.py'), missing))
            with self.subTest(module=name), mock.patch.object(
                    Path, 'exists', lambda p: False if p == missing else regular_exists(p)):
                with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': ''}):
                    spec = importlib.util.find_spec(name)
                    absent = importlib.util.module_from_spec(spec)
                    with mock.patch.dict(sys.modules, {name: absent}):
                        spec.loader.exec_module(absent)
                    marked = absent
                    for component in case.split('.'):
                        marked = getattr(marked, component)
                    self.assertTrue(marked.__unittest_skip__)
                with mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': 'b2' * 20}):
                    private = importlib.util.module_from_spec(spec)
                    with mock.patch.dict(sys.modules, {name: private}), \
                            self.assertRaisesRegex(FileNotFoundError, 'NOT COMPARED') as caught:
                        spec.loader.exec_module(private)
                    self.assertIn(str(missing.relative_to(REPO)), str(caught.exception))

    def test_missing_default_native_report_host_skips_in_overlay_mode(self):
        import pytest
        declared_inputs((REPO / 'tests/test_run_json_nonfinite_report.py',
                         REPO / 'tests/test_private_marker_rows_registration.py',
                         REPO / 'tests/private_fixture_paths.txt'))
        with mock.patch.object(sys, 'path', [str(REPO / 'tests'), *sys.path]):
            from test_private_marker_rows_registration import removed_files
            from test_run_json_nonfinite_report import native_host_library
        default = 'build/lib/report_nonfinite_host.so'
        self.assertNotIn(default, removed_files(),
                         'a promised host must fail if missing in private mode')
        fixture = native_host_library.__wrapped__
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.dict(fixture.__globals__, ROOT=Path(temporary)), \
                mock.patch.dict(os.environ, PINEFORGE_REPORT_NONFINITE_HOST_LIBRARY='',
                                PINEFORGE_PRIVATE_FIXTURES=''):
            with self.assertRaises(pytest.skip.Exception):
                fixture()
            with mock.patch.dict(os.environ, PINEFORGE_PRIVATE_FIXTURES='c2' * 20):
                with self.assertRaisesRegex(pytest.skip.Exception, r'report_nonfinite_host\.so'):
                    fixture()
                supplied = Path(temporary) / default
                with mock.patch.dict(os.environ,
                                     PINEFORGE_REPORT_NONFINITE_HOST_LIBRARY=str(supplied)):
                    with self.assertRaisesRegex(pytest.fail.Exception,
                                                'configured native report fixture is missing'):
                        fixture()
                supplied.parent.mkdir(parents=True)
                supplied.touch()  # the fixture returns its path, it does not load it
                self.assertEqual(fixture(), supplied)


class TapeRegistration(unittest.TestCase):
    def test_partial_input_sets_register_placeholders_naming_the_missing_file(self):
        # Configure the shipping registration blocks with invented files and a
        # no-language CMake project. CTest supplies the registered command.
        with mock.patch.object(sys, 'path', [str(REPO / 'tests'), *sys.path]):
            import test_private_marker_rows_registration as registration
        grid = 'tests/fixtures/requested_bar_grid/btc60/'
        tail = 'tests/fixtures/series_origin/tailh/'
        rows = {
            'test_requested_bar_grid_tapes': [
                grid + slug + '/tv_trades.csv' for slug in (
                    'bi-300', 'bi-420', 'bi-600', 'open-300', 'open-420', 'trace-300',
                    'bi-120', 'bi-180', 'bi-240', 'bi-360', 'bi-480', 'open-120')],
            'test_series_origin_tailh_tape': [tail + name for name in
                                              ('tv_trades.csv', 'bounds.csv', 'bars.csv')],
        }
        declared_inputs((REPO / 'tests/CMakeLists.txt', registration.REMOVED_PATHS))
        promised = registration.removed_files()
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(registration.TOOLS, 'cmake', 'cmake'), \
                mock.patch.object(registration.TOOLS, 'ctest', 'ctest'):
            for name, paths in rows.items():
                # A public chart feed is supplied even in fixture-absent builds.
                public = [grid + 'feed/bars.csv'] if name == 'test_requested_bar_grid_tapes' else []
                for index, missing in enumerate([*paths, None]):
                    with self.subTest(row=name, missing=missing):
                        present = public + [p for p in paths if p != missing]
                        work = Path(temporary) / name / str(index)
                        declared_inputs([work / 'tree/CMakeLists.txt', work / 'tree/tests/CMakeLists.txt',
                                         *(work / 'tree' / p for p in public + paths)])
                        probe = registration.Probe(work, name, present)
                        row = probe.rows()[name]
                        if missing:
                            self.assertIn(registration.PRIVATE_LABEL, registration.labels(row))
                            self.assertEqual(row['command'][-1], registration.SKIP_LITERAL + missing)
                        else:
                            self.assertEqual(row['command'][1:], registration.real_unit_test(name))
                with self.subTest(row=name, inventory=True):
                    self.assertEqual(set(paths) - promised, set(), 'undeclared tape inputs')


if __name__ == "__main__":
    unittest.main()
