#!/usr/bin/env python3
"""Self-tests for the doc-revert gate: it must be able to FAIL.

Each case builds a throwaway git repository whose commits carry the gate
script (so the gate binds them), writes a page, and drives the real
``check_doc_reverts.main`` over it: a sentence deleted without a name fails,
and is passed once the commit's message names it by the introducing lane, by
its commit's hash or by its own words; older text restored over newer text is
a revert and fails the same way; a fresh rewrite and a re-anchor pass without
a name; a commit that predates the gate is not judged; and the replay mode the
audit's acceptance uses reads only the message it is given.

R5 lane H-DOCGATES (AUDIT4-opus X13, docs-a GAP-2).
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import check_doc_reverts as guard

GATE = Path(guard.__file__).resolve()

BASE_PAGE = """# Page

The kernel books every lot at its fill price and never rounds it.

| ID | Rule | Note |
|---|---|---|
| OL7 | The deferred form of a request waits for its owner to fill first. | kept |
"""
KEEP_HANDLE = "A replacement that keeps its handle keeps its lineage and its dependents too."


class Repo:
    """A git repository with the gate script in every commit."""

    def __init__(self) -> None:
        self.dir = tempfile.TemporaryDirectory(prefix='pineforge-doc-reverts-')
        self.root = Path(self.dir.name)
        self.git('init', '-q', '-b', 'lane')   # no `main`: the walk mode, as on a remote host
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('config', 'user.name', 'doc-revert test')
        (self.root / 'scripts').mkdir()
        shutil.copyfile(GATE, self.root / 'scripts' / 'check_doc_reverts.py')
        (self.root / 'docs' / 'pages').mkdir(parents=True)
        self.page = self.root / 'docs' / 'pages' / 'guide.md'

    def git(self, *args: str) -> str:
        return subprocess.run(['git', '-C', str(self.root), *args], capture_output=True,
                              text=True, check=True).stdout.strip()

    def commit(self, text: str, message: str, *, gate: bool = True) -> str:
        self.page.write_text(text)
        script = self.root / 'scripts' / 'check_doc_reverts.py'
        if not gate and script.exists():
            script.unlink()                     # `git add -A` records the removal
        elif gate and not script.exists():
            shutil.copyfile(GATE, script)
        self.git('add', '-A')
        self.git('commit', '-q', '-m', message)
        return self.git('rev-parse', 'HEAD')

    def run(self, *args: str) -> tuple[int, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = guard.main(['--root', str(self.root), *args])
        return code, out.getvalue() + err.getvalue()

    def close(self) -> None:
        self.dir.cleanup()


@contextlib.contextmanager
def repo():
    made = Repo()
    try:
        yield made
    finally:
        made.close()


def with_keep_handle(page: str = BASE_PAGE) -> str:
    return page + '\n' + KEEP_HANDLE + '\n'


class MustFail(unittest.TestCase):
    def setup_history(self, r: Repo) -> tuple[str, str]:
        base = r.commit(BASE_PAGE, 'docs: the guide')
        added = r.commit(with_keep_handle(), 'Docs: keep_handle on the guide (R5 lane V19-D)')
        return base, added

    def test_an_unnamed_deletion_fails(self) -> None:
        with repo() as r:
            base, added = self.setup_history(r)
            r.commit(BASE_PAGE, 'docs: tidy the guide')
            code, out = r.run('--base', added)
            self.assertEqual(code, 1, out)
            self.assertIn('unnamed deletion', out)
            self.assertIn('V19-D', out)             # the name that would have passed
            self.assertIn(added[:guard.SHA_CHARS], out)

    def test_restoring_older_text_is_a_revert(self) -> None:
        """The B-C-SURFACE precedent: a rebase that kept its own base's page put
        older wording back over newer wording. Similar text, so not a deletion,
        but the text is the page's own past."""
        with repo() as r:
            old = 'The ambient default is thread-local, and each scope restores it on exit.'
            new = 'The ambient default is the running pump\'s block, and each scope restores it on exit.'
            r.commit(BASE_PAGE + '\n' + old + '\n', 'docs: the ambient default')
            moved = r.commit(BASE_PAGE + '\n' + new + '\n', 'Docs: the pump block (R5 lane D2-C)')
            r.commit(BASE_PAGE + '\n' + old + '\n', 'docs: wording')
            code, out = r.run('--base', moved)
            self.assertEqual(code, 1, out)
            self.assertIn('unnamed revert', out)
            self.assertIn('D2-C', out)

    def test_a_deleted_table_row_fails(self) -> None:
        with repo() as r:
            base = r.commit(BASE_PAGE, 'docs: the guide')
            r.commit(BASE_PAGE.replace(
                '| OL7 | The deferred form of a request waits for its owner to fill first. | kept |\n',
                ''), 'docs: tidy the table')
            code, out = r.run('--base', base)
            self.assertEqual(code, 1, out)
            self.assertIn('deferred form of a request', out)

    def test_replay_reads_only_the_message_it_is_given(self) -> None:
        with repo() as r:
            base, added = self.setup_history(r)
            tidy = r.commit(BASE_PAGE, 'docs: drop the V19-D keep_handle sentence')
            message = r.root / 'message.txt'
            message.write_text('docs: tidy the guide\n')
            code, out = r.run('--base', added, '--head', tidy, '--message-file', str(message))
            self.assertEqual(code, 1, out)
            self.assertIn('unnamed deletion', out)
            code, out = r.run('--base', added, '--head', tidy)   # its own message names it
            self.assertEqual(code, 0, out)


class MustPass(unittest.TestCase):
    def deleted_with(self, message: str) -> tuple[int, str]:
        with repo() as r:
            r.commit(BASE_PAGE, 'docs: the guide')
            added = r.commit(with_keep_handle(), 'Docs: keep_handle on the guide (R5 lane V19-D)')
            r.commit(BASE_PAGE, message.replace('<sha>', added[:guard.SHA_CHARS]))
            return r.run('--base', added)

    def test_named_by_the_introducing_lane(self) -> None:
        code, out = self.deleted_with('docs: V19-D\'s keep_handle sentence moves to the ADR')
        self.assertEqual(code, 0, out)

    def test_named_by_the_introducing_commit(self) -> None:
        code, out = self.deleted_with('docs: drop the sentence <sha> added')
        self.assertEqual(code, 0, out)

    def test_named_by_its_own_words(self) -> None:
        code, out = self.deleted_with('docs: "keeps its handle keeps its lineage and" is gone')
        self.assertEqual(code, 0, out)

    def test_a_table_row_named_by_its_key(self) -> None:
        with repo() as r:
            base = r.commit(BASE_PAGE, 'docs: the guide')
            r.commit(BASE_PAGE.replace(
                '| OL7 | The deferred form of a request waits for its owner to fill first. | kept |\n',
                ''), 'docs: design row OL7 is retired')
            code, out = r.run('--base', base)
            self.assertEqual(code, 0, out)

    def test_a_fresh_rewrite_and_a_reanchor_need_no_name(self) -> None:
        with repo() as r:
            base = r.commit(BASE_PAGE + '\nThe host reads `x` engine.hpp:12 for the price.\n',
                            'docs: the guide')
            r.commit(BASE_PAGE.replace('never rounds it', 'never rounds the price')
                     + '\nThe host reads `x` engine.hpp:40 for the price.\n', 'docs: wording')
            code, out = r.run('--base', base)
            self.assertEqual(code, 0, out)

    def test_a_commit_before_the_gate_is_not_judged(self) -> None:
        with repo() as r:
            r.commit(BASE_PAGE, 'docs: the guide', gate=False)
            r.commit(with_keep_handle(), 'Docs: keep_handle (R5 lane V19-D)', gate=False)
            r.commit(BASE_PAGE, 'docs: tidy', gate=False)            # predates the gate
            r.commit(BASE_PAGE + '\nA new sentence that the gate itself brings along.\n',
                     'gates: the doc-revert gate lands')
            code, out = r.run()                                      # no base: walk to the gate
            self.assertEqual(code, 0, out)
            self.assertIn('1 commits since this gate landed', out)


if __name__ == '__main__':
    unittest.main()
