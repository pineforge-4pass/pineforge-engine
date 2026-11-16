#!/usr/bin/env python3
"""Exercise added public text checks with committed repository fixtures."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

import check_doc_reverts as reverts
import check_public_role_words as previous
import check_public_text_added as guard

ROOT = Path(__file__).resolve().parent.parent
GATE = Path(guard.__file__).resolve()


def sample_token() -> str:
    return ''.join(('Z', '7')) + '-' + ''.join(('N', 'E', 'X', 'T'))


def city() -> str:
    return ''.join(chr(number) for number in (84, 97, 105, 112, 101, 105))


class RepoCase(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix='pineforge-added-text-')
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.root = self.directory / 'repo'
        self.root.mkdir()
        self.env = dict(os.environ)
        for key in ('PINEFORGE_DOC_REVERT_BASE', 'GITHUB_EVENT_PATH', 'GITHUB_EVENT_NAME'):
            self.env.pop(key, None)
        self.git('init', '-q', '--initial-branch=topic')
        self.write('README.md', 'Public guide.\n')
        self.base = self.commit('Initial text')
        self.digests = self.directory / 'digests.txt'
        self.digest_terms('violet-orbit')

    def git(self, *args: str) -> str:
        result = subprocess.run(
            ['git', '-C', str(self.root), '-c', 'user.name=Test',
             '-c', 'user.email=test@example.invalid', '-c', 'commit.gpgSign=false',
             '-c', f'core.hooksPath={os.devnull}', *args],
            capture_output=True, text=True, check=True, env=self.env,
        )
        return result.stdout.strip()

    def write(self, path: str, body: str | bytes) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(body.encode('utf-8') if isinstance(body, str) else body)
        self.git('add', '--', path)

    def commit(self, message: str) -> str:
        self.git('commit', '-q', '--allow-empty', '-m', message)
        return self.git('rev-parse', 'HEAD')

    def digest_terms(self, *terms: str) -> None:
        values = sorted({hashlib.sha256(term.encode('utf-8')).hexdigest()
                         for term in terms})
        self.digests.write_text('# Test digests.\n' + '\n'.join(values) + '\n',
                                encoding='ascii')

    def event_env(self, name: str, payload: object) -> dict[str, str]:
        path = self.directory / 'event.json'
        path.write_text(json.dumps(payload), encoding='utf-8')
        return {**self.env, 'GITHUB_EVENT_NAME': name, 'GITHUB_EVENT_PATH': str(path)}

    def run_guard(self, *options: str, default_base: bool = False,
                  digests: Path | None = None,
                  with_digests: bool = True,
                  env: dict[str, str] | None = None) -> tuple[int, str]:
        argv = [sys.executable, '-B', str(GATE), '--root', str(self.root)]
        if with_digests:
            argv += ['--digests', str(digests if digests is not None else self.digests)]
        if not default_base:
            argv += ['--base', self.base]
        result = subprocess.run(argv + list(options), capture_output=True, text=True,
                                env=self.env if env is None else env)
        return result.returncode, result.stdout + result.stderr

    def assert_hit(self, out: str, location: str, kind: str) -> None:
        self.assertIn(f'{location}: {kind}', out.splitlines())
        self.assertRegex(out, r'FAIL findings: [1-9][0-9]*')

    def assert_failure(self, out: str, kind: str) -> None:
        self.assertIn(f'FAIL {kind}: 1', out)
        self.assertNotIn('OK', out)


class Matching(RepoCase):
    def test_first_class_closes_the_previous_scope_gap(self) -> None:
        tokens = [sample_token(), 'Z7' + '-' + ''.join(('C', 'X')),
                  'Z7' + '-' + ''.join(('T', 'O', 'P'))]
        self.write('tests/sample.txt', '\n'.join(tokens) + '\n')
        self.commit('Add sample')
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            old_code = previous.main(['--root', str(self.root)])
        self.assertEqual(old_code, 0, stream.getvalue())
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        for number, token in enumerate(tokens, 1):
            self.assert_hit(out, f'tests/sample.txt:{number}', 'E1')
            self.assertNotIn(token, out)

    def test_second_class_covers_each_identifier_shape(self) -> None:
        prefixes = (('C', 'R'), ('H', 'O'), ('L', 'B'), ('S', 'M', 'P'),
                    ('C', 'G'), ('T', 'X'), ('L', 'I', 'V', 'E'), ('K',))
        tokens = [''.join(parts) + '-' + str(17) for parts in prefixes]
        tokens += [''.join(('C', 'G', 'I', 'N', 'T')) + str(9),
                   'S' + '-' + str(123)]
        self.write('config/sample.txt', '\n'.join(tokens) + '\n')
        self.commit('Add identifiers')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        for number in range(1, len(tokens) + 1):
            self.assert_hit(out, f'config/sample.txt:{number}', 'E2')
        for token in tokens:
            self.assertNotIn(token, out)

    def test_third_class_covers_both_identifier_shapes(self) -> None:
        tokens = [''.join(('r', 'j')) + '-' + str(20010203) + 't012345-ab12',
                  'tick' + '-' + str(3141)]
        self.write('fixtures/sample.txt', '\n'.join(tokens) + '\n')
        self.commit('Add receipt samples')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        for number in range(1, len(tokens) + 1):
            self.assert_hit(out, f'fixtures/sample.txt:{number}', 'E3')

    def test_fourth_class_normalizes_words_identifiers_and_paths(self) -> None:
        for atoms in (('violet',), ('violet', 'orbit'),
                      ('violet', 'orbit', 'pebble')):
            with self.subTest(length=len(atoms)):
                term = '-'.join(atoms)
                self.digest_terms(term)
                spellings = [separator.join(atoms) for separator in ('-', '_', '.')]
                spellings += ['prefix_' + term.upper().replace('-', '_') + '_suffix',
                              'prefix.' + term.title().replace('-', '.') + '.suffix']
                self.write('tests/sample.txt', '\n'.join(spellings) + '\n')
                line_head = self.commit('Add normalized line samples')
                code, out = self.run_guard()
                self.assertEqual(code, 1, out)
                self.assertNotIn(term, out)
                for number, spelling in enumerate(spellings, 1):
                    self.assert_hit(out, f'tests/sample.txt:{number}', 'E4')
                    self.assertNotIn(spelling, out)
                for separator in ('-', '_', '.'):
                    joined = separator.join(atoms)
                    self.assertNotIn(joined.upper(), out)
                    self.assertNotIn(joined.title(), out)
                paths = ['assets/' + term.upper() + '/sample.txt',
                         'assets/' + term.title().replace('-', '_') + '/other.txt',
                         'assets/' + ' '.join(atoms) + '/space.txt']
                for path in paths:
                    self.write(path, 'Public text.\n')
                self.commit('Add normalized path samples')
                code, out = self.run_guard('--base', line_head)
                self.assertEqual(code, 1, out)
                self.assertNotIn('tests/sample.txt:', out)
                for path in paths:
                    self.assert_hit(out, path, 'E4')

    def test_fourth_class_does_not_join_separate_words(self) -> None:
        atoms = ('violet', 'orbit', 'pebble')
        self.digest_terms('-'.join(atoms))
        self.write('tests/sample.txt', ' '.join(atoms) + '\n')
        self.commit('Add separate words')
        code, out = self.run_guard()
        self.assertEqual(code, 0, out)
        self.assertNotIn('E4', out)

    def test_fourth_class_requires_an_explicit_list_and_reports_its_absence(self) -> None:
        term = '-'.join(('violet', 'orbit', self.base))
        self.digest_terms(term)
        self.write('tests/sample.txt', term + '\n' + sample_token() + '\n')
        self.commit('Add samples')
        code, out = self.run_guard(with_digests=False)
        self.assertEqual(code, 1, out)
        self.assert_hit(out, 'tests/sample.txt:2', 'E1')
        self.assertNotIn(': E4', out)
        coverage = next(line for line in out.splitlines()
                        if line.startswith('public text added: coverage '))
        self.assertIn('files=1 added_lines=2 paths=1 commits=1', coverage)
        self.assertIn('e4=not-run (no private list)', coverage)
        code, out = self.run_guard(digests=self.digests)
        self.assertEqual(code, 1, out)
        self.assert_hit(out, 'tests/sample.txt:1', 'E4')
        self.assertNotIn(term, out)
        self.assert_hit(out, 'tests/sample.txt:2', 'E1')
        self.assertIn('e4=enabled', out)

    def test_fifth_class_uses_the_original_expression(self) -> None:
        self.assertIs(guard.ROLE_WORD_RE, previous.ROLE_WORD_RE)
        self.write('scripts/sample.txt', '12:34 ' + city() + '\n')
        self.commit('Add clock sample')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_hit(out, 'scripts/sample.txt:1', 'E5')
        self.assertNotIn(city(), out)

    def test_sixth_class_checks_exact_path_segments(self) -> None:
        segment = '.' + 'lane'
        path = '/'.join(('tests', segment, 'sample.txt'))
        self.write(path, 'Public text.\n')
        self.write('scripts/sample.txt', 'assets/' + segment + '/sample.txt\n')
        self.commit('Add path samples')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_hit(out, path, 'E6')
        self.assert_hit(out, 'scripts/sample.txt:1', 'E6')

    def test_ordinary_public_words_are_clean_with_test_digests(self) -> None:
        zone = 'Asia/' + city()
        self.assertEqual(zone, 'Asia/Taipei')
        lines = ['margin' + '-top', 'vs' + '-next',
                 'Margin' + '-Top', 'vS' + '-Next',
                 'k' + '-1', 'live' + '-0', zone,
                 'tick' + '-' + str(31415), 'S' + '-' + str(12),
                 'folder/.' + 'lane-extra/file.txt']
        self.write('tests/ordinary.txt', '\n'.join(lines) + '\n')
        self.commit('Add public wording')
        code, out = self.run_guard()
        self.assertEqual(code, 0, out)
        self.assertNotIn('FAIL', out)


class Coverage(RepoCase):
    def test_clean_diff_reports_real_coverage_and_explicit_head(self) -> None:
        self.write('cmake/sample.txt', 'First line.\nSecond line.\n')
        head = self.commit('Add clean sample')
        self.write('cmake/sample.txt', sample_token() + '\n')
        self.commit('Later sample')
        code, out = self.run_guard('--head', head)
        self.assertEqual(code, 0, out)
        self.assertIn(f'base={self.base}', out)
        self.assertIn(f'head={head}', out)
        self.assertIn('files=1 added_lines=2 paths=1 commits=1', out)

    def test_removal_only_does_not_scan_deleted_text(self) -> None:
        self.write('tests/sample.txt', 'Keep this line.\n' + sample_token() + '\n')
        self.base = self.commit('Existing text')
        self.write('tests/sample.txt', 'Keep this line.\n')
        self.commit('Remove a line')
        code, out = self.run_guard()
        self.assertEqual(code, 0, out)
        self.assertIn('files=1 added_lines=0 paths=0 commits=1', out)

    def test_all_tracked_roots_and_unusual_path_names_are_scanned(self) -> None:
        paths = ('tests/sample.txt', 'scripts/sample.txt', 'cmake/sample.txt',
                 'fixtures/sample.txt', 'config/settings.txt', '.github/sample.txt',
                 'assets/a space.txt', 'assets/a\ttab.txt', 'assets/a\nline.txt')
        for path in paths:
            self.write(path, sample_token() + '\n')
        self.commit('Add tracked samples')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        for path in paths:
            displayed = path.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')
            self.assert_hit(out, f'{displayed}:1', 'E1')

    def test_new_path_is_scanned_even_for_an_empty_file(self) -> None:
        path = 'tests/' + sample_token() + '.txt'
        self.write(path, '')
        self.commit('Add empty file')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_hit(out, path, 'E1')
        self.assertIn('added_lines=0 paths=1 commits=1', out)

    def test_rename_scans_new_path_without_rescanning_unchanged_text(self) -> None:
        self.write('tests/sample.txt', 'Existing text: ' + city() + '\n')
        self.base = self.commit('Existing sample')
        path = 'tests/' + sample_token() + '.txt'
        self.git('mv', '--', 'tests/sample.txt', path)
        self.commit('Rename sample')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_hit(out, path, 'E1')
        self.assertNotIn(': E5', out)
        self.assertIn('added_lines=0 paths=1 commits=1', out)

    def test_every_commit_message_is_scanned(self) -> None:
        first = self.commit('Add sample\n\n' + sample_token())
        self.write('tests/sample.txt', 'Public text.\n')
        self.commit('Add public text')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_hit(out, first[:8], 'E1')
        self.assertIn('commits=2', out)

    def test_branch_behind_main_does_not_readd_main_deletions(self) -> None:
        self.write('tests/shared.txt', sample_token() + '\n')
        common = self.commit('Shared text')
        self.write('tests/branch.txt', 'Branch text.\n')
        head = self.commit('Add branch text')
        self.git('checkout', '-q', '-b', 'main', common)
        self.write('tests/shared.txt', '')
        base = self.commit('Remove shared text')
        self.git('checkout', '-q', 'topic')
        code, out = self.run_guard('--base', base, '--head', head)
        self.assertEqual(code, 0, out)
        self.assertIn(f'merge_base={common}', out)
        self.assertIn('files=1 added_lines=1 paths=1 commits=1', out)
        self.assertNotIn('tests/shared.txt:', out)

    def test_default_base_is_imported_and_honors_the_existing_override(self) -> None:
        self.assertIs(guard.default_base, reverts.default_base)
        self.write('tests/sample.txt', 'Public text.\n')
        self.commit('Add public text')
        env = {**self.env, 'PINEFORGE_DOC_REVERT_BASE': self.base}
        code, out = self.run_guard(default_base=True, env=env)
        self.assertEqual(code, 0, out)
        self.assertIn(f'base={self.base}', out)

    def test_binary_attributes_do_not_hide_added_bytes(self) -> None:
        self.write('.gitattributes', '*.bin -diff\n')
        self.write('assets/sample.bin', b'\0\xff\nExisting bytes.\n')
        self.base = self.commit('Existing asset')
        self.write('assets/sample.bin',
                   b'\0\xff\nExisting bytes.\n' + sample_token().encode('ascii') + b'\n')
        self.commit('Extend asset')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_hit(out, 'assets/sample.bin:3', 'E1')
        self.assertIn('added_lines=1', out)


class Failures(RepoCase):
    def setUp(self) -> None:
        super().setUp()
        self.write('tests/sample.txt', 'Public text.\n')
        self.commit('Add public text')

    def test_missing_base_fails(self) -> None:
        code, out = self.run_guard(default_base=True)
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'base-resolution')

    def test_unknown_base_fails(self) -> None:
        code, out = self.run_guard('--base', 'refs/heads/missing')
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'base-resolution')

    def test_unknown_head_fails(self) -> None:
        code, out = self.run_guard('--head', 'refs/heads/missing')
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'head-resolution')

    def test_zero_commit_wrong_range_fails(self) -> None:
        code, out = self.run_guard('--base', 'HEAD', '--head', self.base)
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'empty-range')
        env = self.event_env('push', {'before': self.git('rev-parse', 'HEAD')})
        code, out = self.run_guard('--head', self.base, default_base=True, env=env)
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'empty-range')

    def test_identical_endpoints_report_no_added_range(self) -> None:
        code, out = self.run_guard('--base', 'HEAD')
        # expectation corrected: 1 -> 0, because equal commits have no added range.
        self.assertEqual(code, 0, out)
        self.assertIn('files=0 added_lines=0 paths=0 commits=0', out)
        self.assertIn('public text added: no added range (base equals head)', out.splitlines())
        self.assertNotIn('FAIL', out)

    def test_missing_digest_file_fails(self) -> None:
        code, out = self.run_guard(digests=self.directory / 'missing.txt')
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'digest-read')

    def test_empty_digest_file_fails(self) -> None:
        self.digests.write_bytes(b'')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'digest-format')

    def test_malformed_digest_files_fail(self) -> None:
        values = sorted(hashlib.sha256(word.encode()).hexdigest()
                        for word in ('violet', 'orbit'))
        bodies = ['# Header only.\n', 'short\n', 'A' * 64 + '\n',
                  values[0] + '\n' + values[0] + '\n',
                  '\n'.join(reversed(values)) + '\n', '\n' + values[0] + '\n']
        for body in bodies:
            with self.subTest(body=body):
                self.digests.write_text(body, encoding='ascii')
                code, out = self.run_guard()
                self.assertEqual(code, 1, out)
                self.assert_failure(out, 'digest-format')

    def test_malformed_digest_file_content_is_not_disclosed(self) -> None:
        lines = ['-'.join(('violet', 'orbit', self.base)),
                 '_'.join(('pebble', self.base[::-1]))]
        self.digests.write_text('\n'.join(lines) + '\n', encoding='ascii')
        code, out = self.run_guard(digests=self.digests)
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'digest-format')
        for line in lines:
            self.assertNotIn(line, out)

    def test_undecodable_blob_fails(self) -> None:
        self.write('tests/invalid.bin', b'\xff\xfe')
        self.commit('Add invalid text')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'blob-decode')

    def test_unreadable_blob_fails(self) -> None:
        oid = self.git('rev-parse', 'HEAD:tests/sample.txt')
        object_path = self.root / '.git/objects' / oid[:2] / oid[2:]
        object_path.chmod(0o644)
        object_path.write_bytes(b'broken object')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assertRegex(out, r'FAIL [a-z-]+: 1')
        self.assertNotIn('OK', out)

    def test_shallow_history_fails_even_with_both_endpoints_present(self) -> None:
        (self.root / '.git/shallow').write_text(self.base + '\n', encoding='ascii')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'shallow-history')


class Events(RepoCase):
    def test_push_event_checks_added_text_from_before(self) -> None:
        self.write('tests/sample.txt', sample_token() + '\n')
        head = self.commit('Add sample')
        self.git('update-ref', 'refs/remotes/origin/main', head)
        env = self.event_env('push', {'before': self.base})
        code, out = self.run_guard(default_base=True, env=env)
        self.assertEqual(code, 1, out)
        self.assert_hit(out, 'tests/sample.txt:1', 'E1')
        self.assertIn(f'base={self.base}', out)
        self.assertIn('commits=1', out)

    def test_push_event_with_clean_content_reports_one_commit(self) -> None:
        self.write('tests/sample.txt', 'Public text.\n')
        head = self.commit('Add public text')
        self.git('update-ref', 'refs/remotes/origin/main', head)
        env = self.event_env('push', {'before': self.base})
        code, out = self.run_guard(default_base=True, env=env)
        self.assertEqual(code, 0, out)
        self.assertIn(f'base={self.base}', out)
        self.assertIn(f'head={head}', out)
        self.assertIn('files=1 added_lines=1 paths=1 commits=1', out)
        self.assertNotIn('no added range', out)

    def test_push_event_with_zero_before_fails(self) -> None:
        self.git('update-ref', 'refs/remotes/origin/main', self.base)
        env = self.event_env('push', {'before': '0' * 40})
        code, out = self.run_guard(default_base=True, env=env)
        self.assertEqual(code, 1, out)
        self.assert_failure(out, 'event-before-zero')

    def test_pull_request_events_use_base_sha_and_the_fork_point(self) -> None:
        self.write('tests/shared.txt', sample_token() + '\n')
        common = self.commit('Shared text')
        self.write('tests/sample.txt', sample_token() + '\n')
        head = self.commit('Add branch sample')
        self.git('checkout', '-q', '-b', 'main', common)
        self.write('tests/shared.txt', '')
        base = self.commit('Remove shared text')
        self.git('checkout', '-q', 'topic')
        for name in ('pull_request', 'pull_request_target'):
            with self.subTest(event=name):
                env = self.event_env(name, {'pull_request': {'base': {'sha': base}}})
                env['PINEFORGE_DOC_REVERT_BASE'] = head
                code, out = self.run_guard(default_base=True, env=env)
                self.assertEqual(code, 1, out)
                self.assert_hit(out, 'tests/sample.txt:1', 'E1')
                self.assertIn(f'base={base}', out)
                self.assertIn(f'merge_base={common}', out)
                self.assertNotIn('tests/shared.txt:', out)
                self.assertIn('files=1 added_lines=1 paths=1 commits=1', out)

    def test_schedule_and_dispatch_on_main_report_no_added_range(self) -> None:
        self.git('checkout', '-q', '-b', 'main')
        self.git('update-ref', 'refs/remotes/origin/main', self.base)
        for name in ('schedule', 'workflow_dispatch'):
            with self.subTest(event=name):
                env = self.event_env(name, {})
                code, out = self.run_guard(default_base=True, env=env)
                self.assertEqual(code, 0, out)
                self.assertIn(f'base={self.base}', out)
                self.assertIn(f'head={self.base}', out)
                self.assertIn('files=0 added_lines=0 paths=0 commits=0', out)
                self.assertIn('public text added: no added range (base equals head)',
                              out.splitlines())
                self.assertNotIn('FAIL', out)

    def test_missing_or_unreadable_event_file_fails_without_fallback(self) -> None:
        for path in (self.directory / 'missing.json', self.directory):
            with self.subTest(path=path):
                env = {**self.env, 'GITHUB_EVENT_NAME': 'push',
                       'GITHUB_EVENT_PATH': str(path),
                       'PINEFORGE_DOC_REVERT_BASE': self.base}
                code, out = self.run_guard(default_base=True, env=env)
                self.assertEqual(code, 1, out)
                self.assert_failure(out, 'event-read')

    def test_malformed_event_sha_fails_without_fallback(self) -> None:
        for value in (self.base[:-1], 'A' * 40, None, 17):
            for name in ('push', 'pull_request_target'):
                with self.subTest(value=value, event=name):
                    payload = ({'before': value} if name == 'push'
                               else {'pull_request': {'base': {'sha': value}}})
                    env = self.event_env(name, payload)
                    env['PINEFORGE_DOC_REVERT_BASE'] = self.base
                    code, out = self.run_guard(default_base=True, env=env)
                    self.assertEqual(code, 1, out)
                    self.assert_failure(out, 'event-base')

    def test_missing_event_base_keys_fail_without_fallback(self) -> None:
        for name, payload in (('push', {}), ('pull_request', {}),
                              ('pull_request_target', {'pull_request': {'base': {}}}),
                              ('pull_request_target', {'pull_request': []})):
            with self.subTest(event=name, payload=payload):
                env = self.event_env(name, payload)
                env['PINEFORGE_DOC_REVERT_BASE'] = self.base
                code, out = self.run_guard(default_base=True, env=env)
                self.assertEqual(code, 1, out)
                self.assert_failure(out, 'event-base')

    def test_malformed_event_json_fails_without_fallback(self) -> None:
        for data in (b'{', b'\xff', b'[]'):
            with self.subTest(data=data):
                env = self.event_env('push', {})
                Path(env['GITHUB_EVENT_PATH']).write_bytes(data)
                env['PINEFORGE_DOC_REVERT_BASE'] = self.base
                code, out = self.run_guard(default_base=True, env=env)
                self.assertEqual(code, 1, out)
                self.assert_failure(out, 'event-format')

    def test_explicit_base_takes_precedence_over_event_data(self) -> None:
        self.write('tests/sample.txt', 'Public text.\n')
        self.commit('Add public text')
        env = {**self.env, 'GITHUB_EVENT_NAME': 'push',
               'GITHUB_EVENT_PATH': str(self.directory / 'missing.json')}
        code, out = self.run_guard(env=env)
        self.assertEqual(code, 0, out)
        self.assertIn(f'base={self.base}', out)
        self.assertIn('commits=1', out)


class Frozen(RepoCase):
    def test_tree_exemption_depends_on_actual_blob_bytes(self) -> None:
        path, expected = next(iter(guard.FROZEN_BLOBS.items()))
        data = (ROOT / path).read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), expected)
        word = re.search(rb'[0-9A-Za-z]{6,}', data)
        self.assertIsNotNone(word)
        self.digest_terms(word.group().decode('ascii').lower())
        self.write(path, data)
        self.commit('Add fixed fixture')
        code, out = self.run_guard('--tree', default_base=True)
        self.assertEqual(code, 0, out)
        self.assertIn('exempt=1', out)
        self.write(path, data + b'\n')
        self.commit('Change one byte')
        code, out = self.run_guard('--tree', default_base=True)
        self.assertEqual(code, 1, out)
        self.assertRegex(out, re.escape(path) + r':[1-9][0-9]*: E4')
        self.assertIn('exempt=0', out)

    def test_matching_frozen_blob_is_not_exempt_in_added_diff(self) -> None:
        path = next(iter(guard.FROZEN_BLOBS))
        data = (ROOT / path).read_bytes()
        word = re.search(rb'[0-9A-Za-z]{6,}', data)
        self.assertIsNotNone(word)
        self.digest_terms(word.group().decode('ascii').lower())
        self.write(path, data)
        self.commit('Add fixed fixture')
        code, out = self.run_guard()
        self.assertEqual(code, 1, out)
        self.assertRegex(out, re.escape(path) + r':[1-9][0-9]*: E4')
        self.assertIn('exempt=0', out)


if __name__ == '__main__':
    unittest.main()
