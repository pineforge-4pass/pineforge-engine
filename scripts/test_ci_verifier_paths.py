#!/usr/bin/env python3
"""Controls for the verifier's conservative PR-only SLOW deferral."""
from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import ci_docs_only
import ci_verifier_paths as rule


class VerifierPaths(unittest.TestCase):
    def test_only_explicitly_deferred_paths_can_skip_slow(self):
        for paths in (
                ['README.md', 'CHANGELOG.md'],
                ['LICENSE', 'NOTICE'],
                ['tools/inspect.py', 'tutorial/run.py'],
                ['benchmarks/README.md', '.claude/notes.md']):
            with self.subTest(paths=paths):
                self.assertFalse(rule.needs_slow(paths))

    def test_all_direct_and_transitive_inputs_and_unknown_paths_run_slow(self):
        for path in (
                'include/pineforge/engine.hpp', 'include/pineforge/pineforge.h',
                'src/c_abi.cpp', 'src/otherwise_unread_by_the_test.cpp',
                'tests/private_fixture_rows.txt', 'tests/CMakeLists.txt',
                'tests/confirmed_capabilities_refusals.cmake',
                'tests/fixtures/settlement_cpp_abi/e60e571/headers.tar',
                'tests/fixtures/native_cpp_abi/host-fc7aad6/manifest.json',
                'runner/transport.cpp', 'runner/CMakeLists.txt',
                'scripts/collect_ci_diagnostics.py', 'scripts/ci_verify.py',
                'scripts/ci_preflight.py', 'scripts/test_ci_verify.py',
                'scripts/run_unittest_shards.py', 'scripts/check_twin_parity.py',
                'scripts/cxxlex.py', '.github/workflows/ci.yml',
                '.github/workflows/new.yml', '.github/actions/setup-live-curl/action.yml',
                'examples/native/host.cpp', 'docker/run_failure_codes.json',
                'docs/ci.md', 'docs/pages/contributing-llm.md',
                'docs/pages/native-engine.md', 'docs/otherwise_unread.md',
                'CMakeLists.txt', 'cmake/consumer.cmake', 'CONTRIBUTING.md',
                'VERSION', '.gitmodules', 'corpus', 'corpus/data/tape.csv',
                'unknown/new-input.txt', 'unknown/new-page.md', 'README',
                'tools', 'tutorial', 'benchmarks', '.claude', '.gitattributes'):
            with self.subTest(path=path):
                self.assertTrue(rule.needs_slow([path]))
                self.assertTrue(rule.needs_slow(['README.md', path]))

    def test_empty_unreadable_and_malformed_diffs_run_slow(self):
        for paths in (None, [], [''], ['/README.md'], ['../README.md'],
                      ['tools/../src/c_abi.cpp'], ['./README.md'],
                      ['tools//inspect.py'], ['tools\\inspect.py']):
            with self.subTest(paths=paths):
                self.assertTrue(rule.needs_slow(paths))

    def test_real_diff_parser_checks_the_tested_merge_and_both_rename_sides(self):
        # compared() and changed_paths() are real; only Git's byte stream is
        # supplied. This tests the same merge-parent and NUL-path parsing the
        # existing docs-only helper uses, without borrowing its path verdict.
        head, base, pr_head = ('a' * 40, 'b' * 40, 'c' * 40)
        env = {'GITHUB_EVENT_NAME': 'pull_request', 'PR_HEAD_SHA': pr_head}
        root = Path('/verifier-fixture')
        for listed, expected in (
                ('README.md\0CHANGELOG.md\0', False),
                ('tools/new.cpp\0src/old.cpp\0', True),
                ('', True)):
            with self.subTest(listed=listed), mock.patch.object(
                    ci_docs_only, 'git',
                    side_effect=[f'{head} {base} {pr_head}\n', listed]) as git:
                self.assertEqual(rule.decide(env, root)[0], expected)
                self.assertEqual(git.call_args_list, [
                    mock.call(root, 'rev-list', '--parents', '--max-count=1', 'HEAD'),
                    mock.call(root, 'diff', '--no-renames', '--no-ext-diff',
                              '--name-only', '-z', base, head, '--')])

    def test_unknown_merge_missing_head_and_git_errors_default_to_slow(self):
        env = {'GITHUB_EVENT_NAME': 'pull_request', 'PR_HEAD_SHA': 'c' * 40}
        with mock.patch.object(ci_docs_only, 'git', return_value='a' * 40):
            self.assertTrue(rule.decide(env)[0])
        for error in (ci_docs_only.Doubt('missing objects'), OSError('unreadable'),
                      UnicodeError('bad filename encoding')):
            with self.subTest(error=error), mock.patch.object(
                    rule, 'compared', side_effect=error):
                self.assertTrue(rule.decide(env)[0])

    def test_non_pr_events_run_all_without_attempting_a_diff(self):
        for event in ('push', 'workflow_dispatch', 'schedule', 'release', ''):
            with self.subTest(event=event), mock.patch.object(rule, 'compared') as compare:
                self.assertTrue(rule.decide({'GITHUB_EVENT_NAME': event})[0])
                compare.assert_not_called()

    def test_cli_output_is_the_helpers_verdict(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / 'github-output'
            env = {'GITHUB_EVENT_NAME': 'pull_request'}
            for changed, expected in ((['README.md'], 'false'),
                                      (['include/pineforge/engine.hpp'], 'true'),
                                      ([], 'true')):
                with self.subTest(changed=changed), mock.patch.dict(
                        os.environ, env, clear=True), mock.patch.object(
                        rule, 'compared', return_value=('base', 'head', 'fixture')), \
                        mock.patch.object(rule, 'changed_paths', return_value=changed), \
                        contextlib.redirect_stdout(io.StringIO()):
                    before = output.read_text() if output.exists() else ''
                    self.assertEqual(rule.main(['--root', str(root),
                                                '--github-output', str(output)]), 0)
                    self.assertEqual(output.read_text(),
                                     before + f'verifier_slow={expected}\n')


if __name__ == '__main__':
    unittest.main()
