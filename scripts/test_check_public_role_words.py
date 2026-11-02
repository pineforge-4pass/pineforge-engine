#!/usr/bin/env python3
"""Self-tests for public wording, including a control that defeats detection."""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import check_public_role_words as guard

ROOT = Path(__file__).resolve().parent.parent
GATE = Path(guard.__file__).resolve()


def run_guard(root: Path) -> tuple[int, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = guard.main(['--root', str(root)])
    return code, out.getvalue() + err.getvalue()


class RepoCase(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix='pineforge-public-words-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.git('init', '-q')

    def git(self, *args: str) -> None:
        subprocess.run(['git', '-C', str(self.root), *args],
                       capture_output=True, text=True, check=True)

    def track(self, path: str, body: str | bytes) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(body.encode('utf-8') if isinstance(body, str) else body)
        self.git('add', '--', path)

    def assert_rejected(self, token: str, path: str | None = None) -> None:
        path = path or f'docs/{self._testMethodName}.md'
        self.track(path, f'# Public guide\n\nReference: {token}.\n')
        code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn(f'{path}:3: {token}', out.splitlines())
        self.assertIn('say it in plain public words; see docs/ci.md', out)


class MustFail(RepoCase):
    def test_core_top(self) -> None:
        self.assert_rejected('CORE-TOP')

    def test_other_top(self) -> None:
        self.assert_rejected('ORG-TOP')

    def test_uppercase_cx(self) -> None:
        self.assert_rejected('PERF-CX')

    def test_digit_cx(self) -> None:
        self.assert_rejected('X4-CX')

    def test_card_number(self) -> None:
        self.assert_rejected('CR-537')

    def test_lab_number(self) -> None:
        self.assert_rejected('LB-12')

    def test_run_id(self) -> None:
        self.assert_rejected('rj-20261030t060000-abc123')

    def test_later_run_id(self) -> None:
        self.assert_rejected('rj-20270101t000000-abc123')

    def test_two_components(self) -> None:
        self.assert_rejected('CX-SINK-B1')

    def test_single_digit_revision(self) -> None:
        self.assert_rejected('R5 lane')

    def test_multiple_digit_revision(self) -> None:
        self.assert_rejected('R12 lane')

    def test_performance_number(self) -> None:
        self.assert_rejected('PERF-D1')

    def test_integration_number(self) -> None:
        self.assert_rejected('INT23')

    def test_review_number(self) -> None:
        self.assert_rejected('TRIAGE1')

    def test_audit_number(self) -> None:
        self.assert_rejected('AUDIT4')

    def test_supervisor_call(self) -> None:
        self.assert_rejected('supervisor call')

    def test_owner_call(self) -> None:
        self.assert_rejected('owner call')

    def test_lane_brief(self) -> None:
        self.assert_rejected('lane brief')

    def test_owner_decision(self) -> None:
        self.assert_rejected('OWNER DECISION')

    def test_city_stamp(self) -> None:
        self.assert_rejected('Taipei')


class MustPass(RepoCase):
    def test_clean_tree(self) -> None:
        for path in ('docs/nested/x.md', 'README.md', 'CHANGELOG.md',
                     'src/nested/x.cpp', 'include/pineforge/x.hpp',
                     'scripts/arena_bound.py', 'tests/test_arena_bound.py'):
            self.track(path, 'The engine fills each order at its fill price.\n')
        code, out = run_guard(self.root)
        self.assertEqual(code, 0, out)
        self.assertIn('7 tracked files checked', out)

    def test_design_stages_and_partial_names(self) -> None:
        self.track('docs/design/native-feature-parity.md',
                   'L3 L4 L5 L6\nCR-12 CR-1234 CRYPTO-CX1 rj-2026 topology TOP\n'
                   'INT2 INT234 XINT23 INT23x\n')
        code, out = run_guard(self.root)
        self.assertEqual(code, 0, out)


class Scope(RepoCase):
    def test_every_public_root_and_retained_path(self) -> None:
        for path in ('docs/assets/word list.txt', 'README.md', 'CHANGELOG.md',
                     'src/nested/x.cpp', 'include/pineforge/x.h',
                     'scripts/arena_bound.py', 'tests/test_arena_bound.py'):
            with self.subTest(path=path):
                self.assert_rejected('R5 lane', path)

    def test_unrelated_files_and_untracked_output(self) -> None:
        self.track('docs/x.md', 'A public guide.\n')
        for path in ('tests/other.cpp', 'scripts/other.py',
                     'scripts/test_check_public_role_words.py'):
            self.track(path, 'CORE-TOP\n')
        (self.root / 'docs/untracked.md').write_text('CORE-TOP\n', encoding='utf-8')
        code, out = run_guard(self.root)
        self.assertEqual(code, 0, out)
        self.assertIn('1 tracked files checked', out)


class Reporting(RepoCase):
    def test_all_matches_and_line_numbers(self) -> None:
        self.track('docs/x.md', 'CORE-TOP, PERF-CX.\nA clean line.\nCR-537\n')
        code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertEqual(out.splitlines(), [
            'docs/x.md:1: CORE-TOP', 'docs/x.md:1: PERF-CX',
            'docs/x.md:3: CR-537', 'say it in plain public words; see docs/ci.md',
        ])

    def test_non_utf8_text_fails(self) -> None:
        self.track('docs/x.md', b'\xff\xfe')
        code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn('docs/x.md: cannot decode tracked text as UTF-8:', out)
        self.assertNotIn('no internal names found', out)

    def test_binary_asset_is_scanned_for_names(self) -> None:
        self.track('docs/image.bin', b'\0\xff\nPERF-D1\n')
        code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn('docs/image.bin:2: PERF-D1', out.splitlines())

    def test_clean_binary_asset_is_counted(self) -> None:
        self.track('docs/image.bin', b'\0\xff\xfe')
        code, out = run_guard(self.root)
        self.assertEqual(code, 0, out)
        self.assertIn('1 tracked files checked', out)

    def test_read_error_fails(self) -> None:
        self.track('docs/x.md', 'A public guide.\n')
        with mock.patch.object(Path, 'read_bytes', side_effect=PermissionError('denied')):
            code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn('docs/x.md: cannot read tracked file: denied', out)

    def test_missing_tracked_file_fails(self) -> None:
        self.track('docs/x.md', 'A public guide.\n')
        (self.root / 'docs/x.md').rename(self.root / 'docs/moved.md')
        code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn('docs/x.md: cannot read tracked file:', out)

    def test_empty_scope_fails(self) -> None:
        self.track('tests/x.cpp', 'A unit test.\n')
        code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn('no tracked files in the public scope', out)

    def test_listing_failure_fails(self) -> None:
        failed = subprocess.CompletedProcess(['git'], 1, b'', b'listing denied')
        with mock.patch.object(guard.subprocess, 'run', return_value=failed):
            code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn('git ls-files failed: listing denied', out)

    def test_missing_git_fails(self) -> None:
        with mock.patch.object(guard.subprocess, 'run', side_effect=OSError('git unavailable')):
            code, out = run_guard(self.root)
        self.assertEqual(code, 1, out)
        self.assertIn('git ls-files failed: git unavailable', out)


class RealRepository(unittest.TestCase):
    def test_default_root_from_another_directory(self) -> None:
        with tempfile.TemporaryDirectory(prefix='pineforge-public-words-cwd-') as directory:
            result = subprocess.run([sys.executable, str(GATE)], cwd=directory,
                                    capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class Mutation(unittest.TestCase):
    def test_never_matching_expression_breaks_all_twenty_red_rows(self) -> None:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(MustFail)
        result = unittest.TestResult()
        with mock.patch.object(guard, 'ROLE_WORD_RE', re.compile(r'(?!)')):
            suite.run(result)
        self.assertEqual(result.testsRun, 20)
        self.assertEqual(result.errors, [])
        self.assertEqual(result.skipped, [])
        self.assertEqual(len(result.failures), 20)


if __name__ == '__main__':
    unittest.main()
