#!/usr/bin/env python3
"""Unit tests for scripts/ci_docs_only.py, the rule that lets ci.yml skip its
proof jobs on a documentation-only change. A ci_preflight stage runs them.

The event cases run the script against throwaway git repositories: a pull
request is the merge GitHub tests, a push is its one commit, and every doubt
must answer "not docs-only". The workflow runs the base's copy of the script
with --root; scripts/test_ci_preflight.py runs that step's own shell."""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ci_docs_only import (PROOF_READS, classify_paths, decide, is_documentation,
                          read_paths_file)  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).with_name('ci_docs_only.py')
# The throwaway repositories read no user or system git configuration.
GIT_ENV = {'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1',
           'GIT_AUTHOR_NAME': 'ci', 'GIT_AUTHOR_EMAIL': 'ci@example.invalid',
           'GIT_COMMITTER_NAME': 'ci', 'GIT_COMMITTER_EMAIL': 'ci@example.invalid'}
# A CMake path reference: ${PROJECT_SOURCE_DIR}/docs/pages/x.md and the like.
CMAKE_PATH = re.compile(r'\$\{(PROJECT_SOURCE_DIR|CMAKE_SOURCE_DIR|CMAKE_CURRENT_SOURCE_DIR|'
                        r'CMAKE_CURRENT_LIST_DIR)\}/([^\s")}]+)')


def announce_fixture_paths(*paths: Path) -> None:
    """List command inputs and refuse protected paths before opening them."""
    forbidden = tuple(Path.home() / name for name in (
        '.config/zsh/keys', '.ssh', '.aws', '.codex', '.config/pineforge'))
    announced = []
    for path in paths:
        candidate = path.absolute()
        for resolved in (False, True):
            if resolved:
                candidate = candidate.resolve()
            if (any(part.endswith('.env') for part in candidate.parts)
                    or any(candidate == root or root in candidate.parents
                           for root in forbidden)):
                raise ValueError(f'refusing classifier fixture path by name: {candidate}')
        announced.append(str(candidate))
    print('classifier fixture follows: ' + ', '.join(announced), flush=True)


_saved_env: dict[str, str | None] = {}


def setUpModule() -> None:
    # decide() runs git itself; keep it off the host's configuration too.
    _saved_env.update({key: os.environ.get(key) for key in GIT_ENV})
    os.environ.update(GIT_ENV)


def tearDownModule() -> None:
    for key, value in _saved_env.items():
        if value is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = value


class Repo:
    """A throwaway repository on branch main."""

    def __init__(self, path: Path) -> None:
        self.path = path
        path.mkdir(parents=True, exist_ok=True)
        self.git('init', '--quiet', '--initial-branch=main')
        self.git('config', 'commit.gpgsign', 'false')
        self.git('config', 'uploadpack.allowReachableSHA1InWant', 'true')

    def git(self, *args: str) -> str:
        return subprocess.run(['git', '-C', str(self.path), *args], check=True,
                              capture_output=True, text=True, env={**os.environ, **GIT_ENV}).stdout

    def commit(self, message: str, write: dict[str, str] | None = None,
               remove: tuple[str, ...] = (), move: dict[str, str] | None = None) -> str:
        for path, text in (write or {}).items():
            (self.path / path).parent.mkdir(parents=True, exist_ok=True)
            (self.path / path).write_text(text)
        for path in remove:
            self.git('rm', '--quiet', path)
        for old, new in (move or {}).items():
            (self.path / new).parent.mkdir(parents=True, exist_ok=True)
            self.git('mv', old, new)
        self.git('add', '--all')
        self.git('commit', '--quiet', '--allow-empty', '-m', message)
        return self.head()

    def head(self) -> str:
        return self.git('rev-parse', 'HEAD').strip()


BASE_FILES = {'README.md': 'readme\n', 'docs/ci.md': 'ci\n', 'docs/build.sh': 'echo\n',
              'docs/pages/pine-to-native.md': 'page\n', 'src/engine.cpp': 'int x;\n',
              'src/util.cpp': 'int u;\n'}


class RuleTests(unittest.TestCase):
    def test_markdown_and_the_docs_tree_are_documentation(self) -> None:
        for path in ('README.md', 'CONTRIBUTING.md', 'CHANGELOG.md', 'docs/ci.md',
                     'docs/pages/getting-started.md', 'docs/adr/0002-next-ruling.md',
                     'docs/design/native-feature-parity.md', 'docs/Doxyfile', 'docs/groups.dox',
                     'docs/_theme/custom.css', 'runner/README.md', 'tutorial/README.md',
                     'docker/README.md'):
            with self.subTest(path=path):
                self.assertTrue(is_documentation(path))

    def test_code_ci_and_the_excluded_trees_are_not(self) -> None:
        for path in ('src/engine.cpp', 'include/pineforge/engine.hpp', 'CMakeLists.txt',
                     'cmake/PineForgeConfig.cmake', 'docs/CMakeLists.txt', 'docs/pages/x.cmake',
                     '.github/workflows/ci.yml', '.github/pull_request_template.md',
                     'benchmarks/README.md', 'benchmarks/results/speed.md',
                     'tests/twin_parity_ledger.md', 'tests/fixtures/cash_fee_sizing/README.md',
                     'scripts/README.md', 'corpus', '.gitmodules', 'VERSION', 'LICENSE',
                     'README', 'README.MD', 'docs', 'documentation/x.md.txt',
                     # A git control file decides how the checkout writes its neighbours.
                     'docs/.gitattributes', 'docs/pages/.gitignore', '.gitattributes'):
            with self.subTest(path=path):
                self.assertFalse(is_documentation(path))

    def test_what_a_proof_job_reads_is_not_documentation(self) -> None:
        self.assertEqual(sorted(PROOF_READS), [
            'docs/adr/0001-kernel-adapter-boundary.md', 'docs/build.sh',
            'docs/pages/native-engine.md', 'docs/pages/pine-to-native.md',
            'docs/seo_postprocess.py'])
        for path in PROOF_READS:
            with self.subTest(path=path):
                self.assertFalse(is_documentation(path))
                # A rename would leave the new path documentation.
                self.assertTrue((ROOT / path).is_file(), f'{path} is gone; update PROOF_READS')

    def test_no_cmake_file_reads_documentation(self) -> None:
        """A CTest row that reads a page (the worked-example rows do) is a proof
        job reading documentation: the page belongs in PROOF_READS."""
        listed = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z', '--', '*CMakeLists.txt',
                                 '*.cmake'], check=True, capture_output=True, text=True).stdout
        read = set()
        for name in filter(None, listed.split('\0')):
            for line in (ROOT / name).read_text(encoding='utf-8', errors='replace').splitlines():
                if line.lstrip().startswith('#'):
                    continue
                for variable, path in CMAKE_PATH.findall(re.sub(r'\s#.*$', '', line)):
                    here = Path(name).parent if 'CURRENT' in variable else Path()
                    read.add(os.path.normpath(here / path))
        documentation = {path for path in read if path.startswith('docs/') or path.endswith('.md')}
        # The scan sees the rows it exists for, so a quiet parse cannot pass it.
        self.assertLessEqual({'docs/pages/pine-to-native.md', 'docs/pages/native-engine.md',
                              'docs/build.sh'}, documentation)
        self.assertEqual(sorted(path for path in documentation if is_documentation(path)), [])


class EventTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fresh()

    def fresh(self) -> None:
        """A new repository whose main holds BASE_FILES in one commit."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.tmp = Path(directory.name)
        self.repo = Repo(self.tmp / 'repo')
        self.base = self.repo.commit('base', BASE_FILES)

    def pull_request(self, **change) -> tuple[bool, list[str]]:
        """The merge GitHub tests: the head's change merged into a base that moved on."""
        repo = self.repo
        repo.git('checkout', '--quiet', '-b', 'topic', self.base)
        head = repo.commit('topic', **change)
        repo.git('checkout', '--quiet', 'main')
        repo.commit('main moved', {'src/other.cpp': 'int y;\n'})
        repo.git('merge', '--quiet', '--no-ff', '--no-edit', 'topic')
        return decide({'GITHUB_EVENT_NAME': 'pull_request', 'PR_HEAD_SHA': head}, repo.path)

    def push(self, forced: str = 'false', **change) -> tuple[bool, list[str]]:
        head = self.repo.commit('pushed', **change)
        self.assertNotEqual(head, self.base)
        return decide({'GITHUB_EVENT_NAME': 'push', 'PUSH_BEFORE': self.base,
                       'PUSH_FORCED': forced}, self.repo.path)

    def test_a_docs_pull_request_is_docs_only(self) -> None:
        docs_only, lines = self.pull_request(write={'docs/ci.md': 'new\n', 'README.md': 'new\n',
                                                    'docs/pages/new.md': 'new\n'})
        self.assertTrue(docs_only, lines)
        # Only the pull request's own change: main's src/other.cpp is the base.
        self.assertEqual(lines[-3:], ['  README.md', '  docs/ci.md', '  docs/pages/new.md'])

    def test_a_pull_request_touching_code_is_not(self) -> None:
        docs_only, lines = self.pull_request(write={'docs/ci.md': 'new\n',
                                                    'src/engine.cpp': 'int z;\n'})
        self.assertFalse(docs_only)
        self.assertIn('  src/engine.cpp', lines)

    def test_a_page_a_proof_job_reads_is_not(self) -> None:
        docs_only, lines = self.pull_request(write={'docs/pages/pine-to-native.md': 'new\n',
                                                    'docs/ci.md': 'new\n'})
        self.assertFalse(docs_only)
        self.assertEqual(lines[1:], ['not docs-only: 1 changed path(s) are not documentation',
                                     '  docs/pages/pine-to-native.md (test_pine_to_native_worked '
                                     'compiles and runs its worked migration)'])
        self.fresh()
        docs_only, lines = self.push(write={'docs/build.sh': 'echo new\n'})
        self.assertFalse(docs_only)
        self.assertIn('test_docs_doxygen_retry', lines[-1])

    def test_both_sides_of_a_rename_count(self) -> None:
        for move, docs_only_expected, not_documentation in (
                ({'docs/ci.md': 'src/ci.txt'}, False, ['  src/ci.txt']),
                # Any Markdown file is documentation, wherever it moves.
                ({'docs/ci.md': 'src/ci.md'}, True, []),
                ({'src/engine.cpp': 'docs/engine.cpp'}, False, ['  src/engine.cpp']),
                ({'docs/ci.md': 'docs/pages/ci.md'}, True, [])):
            with self.subTest(move=move):
                self.fresh()
                docs_only, lines = self.pull_request(move=move)
                self.assertEqual(docs_only, docs_only_expected, lines)
                if not_documentation:
                    self.assertEqual(lines[2:], not_documentation)

    def test_a_deleted_page_is_documentation(self) -> None:
        docs_only, lines = self.push(remove=('docs/ci.md',))
        self.assertTrue(docs_only, lines)

    def test_a_merge_of_another_head_is_a_doubt(self) -> None:
        docs_only, lines = self.pull_request(write={'docs/ci.md': 'new\n'})
        self.assertTrue(docs_only, lines)
        for pr_head in (self.base, '', 'f' * 40):
            with self.subTest(pr_head=pr_head):
                docs_only, lines = decide({'GITHUB_EVENT_NAME': 'pull_request',
                                           'PR_HEAD_SHA': pr_head}, self.repo.path)
                self.assertFalse(docs_only)
                self.assertIn('is not a merge of the pull request head', lines[0])

    def test_a_pull_request_head_that_is_not_a_merge_is_a_doubt(self) -> None:
        head = self.repo.commit('docs', {'docs/ci.md': 'new\n'})
        docs_only, lines = decide({'GITHUB_EVENT_NAME': 'pull_request', 'PR_HEAD_SHA': head},
                                  self.repo.path)
        self.assertFalse(docs_only)
        self.assertIn('is not a merge', lines[0])

    def test_an_empty_change_is_a_doubt(self) -> None:
        docs_only, lines = self.pull_request()
        self.assertFalse(docs_only)
        self.assertEqual(lines[-1], 'not docs-only: the change is empty')

    def test_a_docs_push_is_docs_only(self) -> None:
        docs_only, lines = self.push(write={'docs/ci.md': 'new\n'})
        self.assertTrue(docs_only, lines)
        self.assertEqual(lines[0], f'the pushed commit against the previous tip: '
                                   f'{self.base}..{self.repo.head()}, 1 changed path(s)')

    def test_a_push_touching_code_is_not(self) -> None:
        docs_only, _ = self.push(write={'README.md': 'new\n', 'CMakeLists.txt': 'x\n'})
        self.assertFalse(docs_only)

    def test_a_forced_or_unreadable_push_is_a_doubt(self) -> None:
        for forced in ('true', '', 'yes'):
            with self.subTest(forced=forced):
                self.fresh()
                docs_only, lines = self.push(forced=forced, write={'docs/ci.md': 'forced\n'})
                self.assertFalse(docs_only)
                self.assertIn('forced', lines[0])
        head = {'GITHUB_EVENT_NAME': 'push', 'PUSH_FORCED': 'false'}
        for before, reason in (('0' * 40, 'the push created the branch'),
                               ('', "before='' names no commit"),
                               ('HEAD~1', "before='HEAD~1' names no commit"),
                               ('e' * 40, 'is not the parent of HEAD')):
            with self.subTest(before=before):
                docs_only, lines = decide({**head, 'PUSH_BEFORE': before}, self.repo.path)
                self.assertFalse(docs_only)
                self.assertIn(reason, lines[0])

    def test_a_push_of_several_commits_is_a_doubt(self) -> None:
        # The checkout holds HEAD and its parent; a push whose before is older
        # carried commits the check cannot see one by one, so it runs everything.
        self.repo.commit('code', {'src/engine.cpp': 'int z;\n'})
        self.repo.commit('docs', {'docs/ci.md': 'new\n'})
        docs_only, lines = decide({'GITHUB_EVENT_NAME': 'push', 'PUSH_BEFORE': self.base,
                                   'PUSH_FORCED': 'false'}, self.repo.path)
        self.assertFalse(docs_only)
        self.assertIn('the push carried more than its one commit', lines[0])

    def test_every_other_event_runs_everything(self) -> None:
        for event in ('workflow_dispatch', 'schedule', 'merge_group', 'pull_request_target', ''):
            with self.subTest(event=event):
                docs_only, lines = decide({'GITHUB_EVENT_NAME': event, 'PUSH_BEFORE': self.base,
                                           'PUSH_FORCED': 'false'}, self.repo.path)
                self.assertFalse(docs_only)
                self.assertIn('runs every job', lines[0])


class CommandLineTests(unittest.TestCase):
    """The script as the workflow runs it: its own checkout, its output file."""

    def run_script(self, repo: Path, env: dict[str, str], output: Path,
                   outside: Path | None = None) -> subprocess.CompletedProcess:
        """Run a copy of the script: inside the checkout, or, as the workflow
        does, from a directory of its own with --root naming the checkout."""
        home = outside if outside is not None else repo / 'scripts'
        announce_fixture_paths(SCRIPT, home / 'ci_docs_only.py', repo, output,
                               Path(sys.executable))
        home.mkdir(exist_ok=True)
        shutil.copyfile(SCRIPT, home / 'ci_docs_only.py')
        environment = {key: value for key, value in os.environ.items()
                       if key not in ('GITHUB_EVENT_NAME', 'PR_HEAD_SHA', 'PUSH_BEFORE', 'PUSH_FORCED')}
        root = ['--root', '.'] if outside is not None else []
        return subprocess.run([sys.executable, str(home / 'ci_docs_only.py'), *root,
                               '--github-output', str(output)], cwd=repo, capture_output=True,
                              text=True, env={**environment, **GIT_ENV, **env})

    def test_appends_the_answer_and_exits_zero(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        repo = Repo(Path(directory.name) / 'repo')
        base = repo.commit('base', BASE_FILES)
        repo.commit('docs', {'docs/ci.md': 'new\n'})
        output = Path(directory.name) / 'github_output'
        outside = Path(directory.name) / 'runner-temp'
        for env, answer in (({'GITHUB_EVENT_NAME': 'push', 'PUSH_BEFORE': base,
                              'PUSH_FORCED': 'false'}, 'true'),
                            ({'GITHUB_EVENT_NAME': 'workflow_dispatch'}, 'false'),
                            ({'GITHUB_EVENT_NAME': 'push', 'PUSH_BEFORE': base,
                              'PUSH_FORCED': 'true'}, 'false')):
            for where in (None, outside):
                with self.subTest(env=env, outside=where is not None):
                    output.write_text('earlier=1\n')
                    result = self.run_script(repo.path, env, output, where)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(output.read_text(),
                                     f'earlier=1\ndocs_only={answer}\ngithub_only=false\n'
                                     f'arm_relevant={"false" if answer == "true" else "true"}\n')
                    self.assertIn('docs-only', result.stdout)


class PathListTests(unittest.TestCase):
    """The SHA-bound compare list is data, including rename and hostile names."""

    def test_exact_prefixes_and_classifier_script_are_not_overbroad(self) -> None:
        self.assertEqual(classify_paths(['.github/workflows/new.yml']), (False, True, False))
        self.assertEqual(classify_paths(['.github_evil/workflows/new.yml']), (False, False, True))
        self.assertEqual(classify_paths(['docs-evil/readme.txt']), (False, False, True))
        self.assertEqual(classify_paths(['scripts/ci_docs_only.py']), (False, False, True))
        self.assertEqual(classify_paths(['docs/ci.md']), (True, False, False))
        self.assertEqual(classify_paths([]), (False, False, True))

    def test_nul_names_are_data_and_renames_supply_both_sides(self) -> None:
        paths = ['docs/old name "quoted"\n..', '.github/workflows/new.yml']
        self.assertEqual(classify_paths(paths), (False, False, False))
        self.assertEqual(classify_paths(['.github/old', '.github/new']), (False, True, False))

    def test_arm_relevant_source_and_test_paths(self) -> None:
        paths = ('tests/foo.cpp', 'tests/fixture.json', 'runner/main.cpp', 'runner/config.json',
                 'include/pineforge/api.hpp', 'include/generated.inc', 'src/engine.cpp',
                 'src/generated.inc', 'docker/x.c', 'docker/Dockerfile', 'CMakeLists.txt',
                 'scripts/test_x.py', 'third-party/lib/x.c', 'third_party/lib/CMakeLists.txt')
        paths += tuple(f'elsewhere/file{suffix}' for suffix in (
            '.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.cmake'))
        for path in paths:
            with self.subTest(path=path):
                self.assertEqual(classify_paths([path]), (False, False, True))
        # Mixed known classes exercise explicit source rules as well as the
        # conservative fallback for paths outside both known classes.
        for suffix in ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.cmake'):
            with self.subTest(mixed_suffix=suffix):
                self.assertEqual(classify_paths(['docs/ci.md', f'.github/helper{suffix}']),
                                 (False, False, True))
        for directory in ('runner', 'include', 'src', 'docker'):
            with self.subTest(mixed_directory=directory):
                self.assertEqual(classify_paths(
                    ['.github/workflows/ci.yml', f'{directory}/README.md']),
                    (False, False, True))

    def test_arm_relevant_unknown_path_fails_closed(self) -> None:
        for paths in (['weird/blob.bin'], ['docs/ci.md', 'weird/blob.bin'],
                      ['.github/workflows/ci.yml', 'weird/blob.bin'], [], ['']):
            with self.subTest(paths=paths):
                self.assertEqual(classify_paths(paths), (False, False, True))

    def test_arm_relevant_docs_and_github_only_counter_control(self) -> None:
        for paths, docs_only, github_only in (
                (['README.md', 'docs/ci.md'], True, False),
                (['docs/example.cpp', 'docker/README.md', 'runner/README.md'], True, False),
                (['.github/workflows/ci.yml', '.github/test.cpp'], False, True)):
            with self.subTest(paths=paths):
                self.assertEqual(classify_paths(paths), (docs_only, github_only, False))
        self.assertEqual(classify_paths(['docs/ci.md', '.github/workflows/ci.yml']),
                         (False, False, False))
        self.assertEqual(classify_paths(['tests/foo.cpp', 'docs/ci.md']),
                         (False, False, True))
        self.assertEqual(classify_paths(['.github/workflows/ci.yml'], pull_request=False),
                         (False, False, False))

    def test_path_cli_publishes_arm_relevant_and_fails_closed(self) -> None:
        cases = (
            ('source', b'tests/foo.cpp\0', ('false', 'false', 'true')),
            ('runner', b'runner/main.cpp\0', ('false', 'false', 'true')),
            ('docker', b'docker/x.c\0', ('false', 'false', 'true')),
            ('cmake', b'CMakeLists.txt\0', ('false', 'false', 'true')),
            ('script-test', b'scripts/test_x.py\0', ('false', 'false', 'true')),
            ('unknown', b'weird/blob.bin\0', ('false', 'false', 'true')),
            ('empty', b'', ('false', 'false', 'true')),
            ('missing', None, ('false', 'false', 'true')),
            ('truncated', b'docs/ci.md', ('false', 'false', 'true')),
            ('invalid', b'\xff\0', ('false', 'false', 'true')),
            ('empty-path', b'\0', ('false', 'false', 'true')),
            ('docs', b'README.md\0docs/ci.md\0', ('true', 'false', 'false')),
            ('github', b'.github/workflows/ci.yml\0', ('false', 'true', 'false')),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, data, expected in cases:
                with self.subTest(case=name):
                    paths, output = root / name, root / f'{name}-output'
                    announce_fixture_paths(SCRIPT, paths, output, Path(sys.executable))
                    if data is not None:
                        paths.write_bytes(data)
                    output.write_text('earlier=1\n')
                    result = subprocess.run(
                        [sys.executable, '-I', str(SCRIPT), '--root', str(root),
                         '--paths', str(paths), '--github-output', str(output)],
                        capture_output=True, text=True,
                        env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    docs_only, github_only, arm_relevant = expected
                    self.assertEqual(
                        output.read_text(),
                        f'earlier=1\ndocs_only={docs_only}\ngithub_only={github_only}\n'
                        f'arm_relevant={arm_relevant}\n')

    def test_path_file_requires_a_complete_utf8_nul_list(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            complete = root / 'complete'
            complete.write_bytes(b'.github/workflows/a.yml\0docs/ci.md\0')
            self.assertEqual(read_paths_file(complete),
                             (['.github/workflows/a.yml', 'docs/ci.md'],
                              'path list contains 2 path(s)'))
            empty = root / 'empty'
            empty.write_bytes(b'')
            self.assertEqual(read_paths_file(empty), ([], 'path list is empty'))
            for name, data in (('truncated', b'.github/workflows/a.yml'),
                               ('invalid', b'\xff\0')):
                path = root / name
                path.write_bytes(data)
                paths, reason = read_paths_file(path)
                self.assertIsNone(paths)
                self.assertIn('path list', reason)

    def test_unreadable_path_file_is_neither(self) -> None:
        paths, reason = read_paths_file(Path('/path/that/does/not/exist'))
        self.assertIsNone(paths)
        self.assertIn('unreadable', reason)


if __name__ == '__main__':
    unittest.main()
