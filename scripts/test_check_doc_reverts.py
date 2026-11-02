#!/usr/bin/env python3
"""Self-tests for the doc-revert gate: it must be able to FAIL.

Each case builds a throwaway git repository whose commits carry the gate
script (so the gate binds them), writes a page, and drives the real
``check_doc_reverts.main`` over it: a sentence deleted without a name fails,
and is passed once the commit's message names it by the introducing lane, by
its commit's hash or by its own words; older text restored over newer text is
a revert and fails the same way; a fresh rewrite and a re-anchor pass without
a name; a commit that predates the gate is not judged; a checkout with no main
ref starts after the newest merged pull request, as CI's merge base does, and
still judges what came after it; and the replay mode the audit's acceptance uses
reads only the message it is given.

R5 lane H-DOCGATES (AUDIT4-opus X13, docs-a GAP-2).
"""
from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import check_doc_reverts as guard
from ci_preflight import check_commands

GATE = Path(guard.__file__).resolve()

BASE_PAGE = """# Page

The kernel books every lot at its fill price and never rounds it.

| ID | Rule | Note |
|---|---|---|
| OL7 | The deferred form of a request waits for its owner to fill first. | kept |
"""
KEEP_HANDLE = "A replacement that keeps its handle keeps its lineage and its dependents too."

_inherited_doc_revert_base: str | None = None


def setUpModule() -> None:
    """Fixture repositories cannot resolve the caller's engine base."""
    global _inherited_doc_revert_base
    _inherited_doc_revert_base = os.environ.pop('PINEFORGE_DOC_REVERT_BASE', None)


def tearDownModule() -> None:
    if _inherited_doc_revert_base is None:
        os.environ.pop('PINEFORGE_DOC_REVERT_BASE', None)
    else:
        os.environ['PINEFORGE_DOC_REVERT_BASE'] = _inherited_doc_revert_base


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

    def run_ci(self) -> tuple[int, str]:
        """Run the checker arguments CI really selects, only changing its root."""
        plan = check_commands(GATE.parent.parent, env={}, tier='fast')
        results = [self.run(*entry[1][2:]) for entry in plan
                   if len(entry[1]) > 1 and Path(entry[1][1]) == GATE]
        if not results:
            raise AssertionError('fast preflight has no doc-reverts checker invocation')
        return max(code for code, _ in results), ''.join(out for _, out in results)

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


class DefaultBase(unittest.TestCase):
    def setUp(self) -> None:
        env = mock.patch.dict(os.environ, {'PINEFORGE_DOC_REVERT_BASE': ''})
        env.start()
        self.addCleanup(env.stop)

    def test_tags_cannot_shadow_branch_refs(self) -> None:
        for ref, tag in (('refs/remotes/origin/main', 'origin/main'),
                         ('refs/heads/main', 'main')):
            with self.subTest(ref=ref), repo() as r:
                common = r.commit(BASE_PAGE, 'docs: common ancestor')
                r.git('checkout', '-q', '-b', 'tag-history')
                tag_tip = r.commit(with_keep_handle(), 'docs: unrelated tag history')
                r.git('tag', tag, tag_tip)
                r.git('checkout', '-q', 'lane')
                base = r.commit(BASE_PAGE + '\nThe main branch adds its own sentence.\n',
                                'docs: main branch')
                r.git('update-ref', ref, base)
                r.commit(with_keep_handle(), 'docs: lane after main')
                self.assertEqual(r.git('merge-base', 'HEAD', f'refs/tags/{tag}'), common)
                self.assertEqual(r.git('merge-base', 'HEAD', ref), base)
                self.assertNotEqual(common, base)
                self.assertEqual(guard.default_base(r.root), base)

    def test_remote_tracking_main_precedes_local_main(self) -> None:
        with repo() as r:
            remote = r.commit(BASE_PAGE, 'docs: remote main')
            r.git('update-ref', 'refs/remotes/origin/main', remote)
            local = r.commit(with_keep_handle(), 'docs: local main')
            r.git('update-ref', 'refs/heads/main', local)
            r.commit(BASE_PAGE + '\nThe lane adds a new sentence after local main.\n',
                     'docs: lane')
            self.assertEqual(guard.default_base(r.root), remote)

    def test_local_main_is_used_without_remote_tracking_main(self) -> None:
        with repo() as r:
            base = r.commit(BASE_PAGE, 'docs: local main')
            r.git('update-ref', 'refs/heads/main', base)
            r.commit(with_keep_handle(), 'docs: lane')
            self.assertEqual(guard.default_base(r.root), base)

    def test_environment_base_precedes_branch_refs(self) -> None:
        with repo() as r:
            override = r.commit(BASE_PAGE, 'docs: explicit base')
            remote = r.commit(with_keep_handle(), 'docs: remote main')
            r.git('update-ref', 'refs/remotes/origin/main', remote)
            local = r.commit(BASE_PAGE + '\nLocal main adds another sentence.\n',
                             'docs: local main')
            r.git('update-ref', 'refs/heads/main', local)
            r.commit(with_keep_handle(), 'docs: lane')
            with mock.patch.dict(os.environ, {'PINEFORGE_DOC_REVERT_BASE': override}):
                self.assertEqual(guard.default_base(r.root), override)

    def test_merged_pull_request_is_used_without_branch_refs(self) -> None:
        with repo() as r:
            r.commit(BASE_PAGE, 'docs: initial guide')
            base = r.commit(with_keep_handle(), 'docs: merged guide (#12)')
            r.commit(BASE_PAGE + '\nThe lane adds a new sentence after the merge.\n',
                     'docs: lane')
            self.assertEqual(guard.default_base(r.root), base)

    def test_no_branch_ref_or_merged_pull_request_has_no_base(self) -> None:
        with repo() as r:
            r.commit(BASE_PAGE, 'docs: initial guide')
            self.assertIsNone(guard.default_base(r.root))


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

    def test_a_deletion_after_a_merged_pull_request_fails(self) -> None:
        with repo() as r:
            r.commit(BASE_PAGE, 'docs: the guide')
            r.commit(with_keep_handle(), 'Docs: keep_handle on the guide (R5 lane V19-D) (#12)')
            r.commit(BASE_PAGE, 'docs: tidy the guide')           # no main ref: walk mode
            code, out = r.run()
            self.assertEqual(code, 1, out)
            self.assertIn('unnamed deletion', out)

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

    def test_explicit_base_precedes_environment_base(self) -> None:
        with repo() as r:
            base = r.commit(BASE_PAGE, 'docs: the guide')
            r.commit(with_keep_handle(), 'docs: the next lane adds a sentence')
            with mock.patch.dict(os.environ, {'PINEFORGE_DOC_REVERT_BASE': '0' * 40}):
                code, out = r.run('--base', base)
            self.assertEqual(code, 0, out)
            self.assertIn('1 commits since ' + base[:guard.SHA_CHARS], out)

    def test_a_merged_pull_request_is_the_base_without_a_main_ref(self) -> None:
        """A squash merge on main carries a whole branch's deletions under the pull
        request's title; its branch was judged commit by commit before it merged, so
        a checkout with no main ref (a lab remote host) starts after it, as CI's merge
        base with main does -- and still judges what came after."""
        with repo() as r:
            r.commit(BASE_PAGE, 'docs: the guide', gate=False)
            r.commit(with_keep_handle(), 'Docs: keep_handle (R5 lane V19-D)')
            squash = r.commit(BASE_PAGE, 'INT26: wave H and the late picks (#290)')
            r.commit(BASE_PAGE + '\nA new sentence the next lane writes on the guide.\n',
                     'Docs: the next lane')
            code, out = r.run()
            self.assertEqual(code, 0, out)
            self.assertIn('1 commits since ' + squash[:guard.SHA_CHARS], out)
            code, out = r.run('--base', squash + '~1')             # the squash judged alone
            self.assertEqual(code, 1, out)
            self.assertIn('unnamed deletion', out)

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


class MergeCommits(unittest.TestCase):
    PAGE = 'docs/pages/guide.md'
    OLD = 'The ambient default is thread-local, and each scope restores it on exit.'
    NEW = "The ambient default is the running pump's block, and each scope restores it on exit."
    NOTE = 'The lane adds a separate sentence about its review notes.'
    TOP_NOTE = 'The upper lane adds another sentence about its review notes.'
    MAIN_NOTE = 'Only the later main successor carries this additional sentence.'

    def assert_page(self, r: Repo, text: str, ref: str = 'HEAD') -> None:
        self.assertEqual(r.page.read_text(), text)
        self.assertEqual(r.git('show', f'{ref}:{self.PAGE}'), text.strip())

    def commit_page(self, r: Repo, text: str, message: str,
                    parents: tuple[str, ...] = ()) -> str:
        commit = r.commit(text, message)
        self.assert_page(r, text, commit)
        self.assertEqual(r.git('rev-list', '--parents', '-n', '1', commit).split(),
                         [commit, *parents])
        return commit

    def conflict_with_main(self, r: Repo, lane: str, main: str) -> None:
        result = subprocess.run(
            ['git', '-C', str(r.root), 'merge', '--no-ff', '--no-commit', 'main'],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('CONFLICT (content)', result.stdout + result.stderr)
        self.assertEqual(r.git('rev-parse', 'HEAD'), lane)
        self.assertEqual(r.git('rev-parse', 'MERGE_HEAD'), main)
        self.assertEqual(r.git('diff', '--name-only', '--diff-filter=U'), self.PAGE)

    def wording_page(self, wording: str, *, note: bool = False) -> str:
        line = wording + (' ' + self.NOTE if note else '')
        return with_keep_handle(BASE_PAGE + '\n' + line + '\n')

    def wording_merge(self, r: Repo, *, keep_older: bool = True,
                      named: bool = False) -> tuple[str, str, str]:
        old_page = self.wording_page(self.OLD)
        main_page = self.wording_page(self.NEW)
        lane_page = self.wording_page(self.OLD, note=True)
        common = self.commit_page(
            r, old_page, 'Docs: ambient default and keep_handle (R5 lane V19-D)')
        r.git('checkout', '-q', '-b', 'main')
        main = self.commit_page(r, main_page, 'Docs: the pump block (R5 lane D2-C)',
                                (common,))
        r.git('checkout', '-q', 'lane')
        lane_message = ("docs: keep the older default instead of D2-C's wording"
                        if named else 'docs: add the lane note')
        lane = self.commit_page(r, lane_page, lane_message, (common,))
        self.assertEqual(r.git('merge-base', 'HEAD', 'main'), common)
        self.conflict_with_main(r, lane, main)
        resolved = self.wording_page(self.OLD if keep_older else self.NEW, note=True)
        merged = self.commit_page(r, resolved, 'Merge main into lane', (lane, main))
        self.assertEqual(r.git('show', f'{merged}^1:{self.PAGE}'), lane_page.strip())
        self.assertEqual(r.git('show', f'{merged}^2:{self.PAGE}'), main_page.strip())
        self.assertEqual(r.git('merge-base', 'HEAD', 'main'), main)
        self.assertIn(self.OLD if keep_older else self.NEW, r.page.read_text())
        self.assertNotIn(self.NEW if keep_older else self.OLD, r.page.read_text())
        return main, lane, merged

    def assert_unnamed_revert(self, code: int, out: str, main: str) -> None:
        self.assertEqual(code, 1, out)
        self.assertIn(self.PAGE + ': unnamed revert', out)
        self.assertIn(self.NEW, out)
        self.assertIn('older text restored:', out)
        self.assertIn(self.OLD, out)
        self.assertIn('D2-C', out)
        self.assertIn(main[:7], out)

    def test_ci_rejects_a_merge_that_keeps_older_wording_over_main(self) -> None:
        """Row 1: RED until CI judges the net main-to-HEAD change as well."""
        with repo() as r:
            main, lane, merged = self.wording_merge(r)
            self.assertEqual(r.git('log', '-1', '--format=%B', lane),
                             'docs: add the lane note')
            self.assertEqual(r.git('log', '-1', '--format=%B', merged),
                             'Merge main into lane')
            code, out = r.run('--base', main, '--head', merged)
            self.assert_unnamed_revert(code, out, main)  # Existing replay control.
            code, out = r.run_ci()
            self.assert_unnamed_revert(code, out, main)  # Sole intended RED.

    def test_ci_accepts_a_merge_revert_named_by_a_branch_commit(self) -> None:
        """Row 2: a non-merge branch message may name main's replaced sentence."""
        with repo() as r:
            main, lane, merged = self.wording_merge(r, named=True)
            self.assertIn('D2-C', r.git('log', '-1', '--format=%B', lane))
            self.assertEqual(r.git('log', '-1', '--format=%B', merged),
                             'Merge main into lane')
            code, out = r.run_ci()
            self.assertEqual(code, 0, out)
            code, out = r.run('--base', main, '--head', merged)
            self.assertEqual(code, 0, out)

    def test_ci_accepts_a_merge_that_takes_main_wording(self) -> None:
        """Row 3: resolving the same real conflict to main's text is allowed."""
        with repo() as r:
            main, lane, merged = self.wording_merge(r, keep_older=False)
            code, out = r.run_ci()
            self.assertEqual(code, 0, out)
            code, out = r.run('--base', main, '--head', merged)
            self.assertEqual(code, 0, out)

    def test_ci_still_rejects_an_unnamed_non_merge_deletion(self) -> None:
        """Row 4: the existing per-commit judgment must remain binding."""
        with repo() as r:
            main, lane, merged = self.wording_merge(r, keep_older=False)
            deleted_page = self.wording_page(self.NEW, note=True).replace(KEEP_HANDLE + '\n', '')
            deleted = self.commit_page(r, deleted_page, 'docs: tidy the guide', (merged,))
            self.assertEqual(set(guard.judged_commits(r.root, main)), {lane, deleted})
            code, out = r.run_ci()
            self.assertEqual(code, 1, out)
            self.assertIn(self.PAGE + ': unnamed deletion', out)
            self.assertIn(KEEP_HANDLE, out)
            self.assertIn('V19-D', out)

    def test_ci_keeps_inherited_main_out_when_a_stack_base_moves(self) -> None:
        """Row 5: moving the base and then advancing only main preserve the verdict."""
        with repo() as r:
            common_page = with_keep_handle()
            lane_page = BASE_PAGE + '\n' + KEEP_HANDLE + ' ' + self.NOTE + '\n'
            lower_page = BASE_PAGE + '\n' + self.NOTE + '\n'
            top_page = lower_page + '\n' + self.TOP_NOTE + '\n'
            common = self.commit_page(
                r, common_page, 'Docs: keep_handle on the guide (R5 lane V19-D) (#12)')
            lane = self.commit_page(r, lane_page, 'docs: add the lower lane note', (common,))
            r.git('checkout', '-q', '-b', 'main', common)
            main = self.commit_page(r, BASE_PAGE, 'docs: tidy the main guide (#13)', (common,))
            r.git('checkout', '-q', 'lane')
            self.assert_page(r, lane_page)
            self.conflict_with_main(r, lane, main)
            lower = self.commit_page(r, lower_page, 'Merge main into the lower lane', (lane, main))
            self.assertEqual(r.git('show', f'{lower}^1:{self.PAGE}'), lane_page.strip())
            self.assertEqual(r.git('show', f'{lower}^2:{self.PAGE}'), BASE_PAGE.strip())
            r.git('checkout', '-q', '-b', 'stack')
            top = self.commit_page(r, top_page, 'docs: add the upper lane note', (lower,))

            # Main's inherited deletion really would fail if it were judged.
            code, out = r.run('--base', common, '--head', main)
            self.assertEqual(code, 1, out)
            self.assertIn(self.PAGE + ': unnamed deletion', out)
            self.assertIn(KEEP_HANDLE, out)
            self.assertIn('V19-D', out)
            self.assertEqual(r.git('merge-base', 'HEAD', 'main'), main)
            self.assertEqual(guard.default_base(r.root), main)
            self.assertEqual(set(guard.judged_commits(r.root, main)), {lane, top})
            before_code, before_out = r.run_ci()
            self.assertEqual(before_code, 0, before_out)

            # Absorbing the lower layer causes a genuine merge-base shift.
            r.git('checkout', '-q', 'main')
            r.git('merge', '--ff-only', 'lane')
            self.assertEqual(r.git('rev-parse', 'HEAD'), lower)
            self.assert_page(r, lower_page)
            r.git('checkout', '-q', 'stack')
            self.assert_page(r, top_page)
            self.assertEqual(r.git('merge-base', 'HEAD', 'main'), lower)
            self.assertEqual(guard.default_base(r.root), lower)
            self.assertEqual(set(guard.judged_commits(r.root, lower)), {top})
            shifted_code, shifted_out = r.run_ci()
            self.assertEqual(shifted_code, before_code, shifted_out)

            # A later main-only successor leaves that shared base unchanged.
            r.git('checkout', '-q', 'main')
            successor_page = lower_page + '\n' + self.MAIN_NOTE + '\n'
            successor = self.commit_page(r, successor_page, 'docs: main advances alone', (lower,))
            r.git('checkout', '-q', 'stack')
            self.assert_page(r, top_page)
            self.assertNotEqual(successor, lower)
            self.assertEqual(r.git('merge-base', 'HEAD', 'main'), lower)
            self.assertEqual(guard.default_base(r.root), lower)
            self.assertEqual(set(guard.judged_commits(r.root, lower)), {top})
            code, out = r.run_ci()
            self.assertEqual(code, shifted_code, out)
            self.assertEqual(out, shifted_out)


if __name__ == '__main__':
    unittest.main()
