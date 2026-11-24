#!/usr/bin/env python3
"""A documentation sentence may leave only when its commit says so.

Why this exists
---------------
The fourth claimed-vs-actual audit (AUDIT4-opus docs-a, finding (f) and N7)
walked every merge from fd785928 to 91d65ad6 and found that no silent doc
revert had reached ``main`` -- and that nothing would have caught one. Its
precedent was real: lane B-C-SURFACE's rebase kept its own base's pages, which
dropped V19-D's ``keep_handle`` / ``keep_binding`` row and put D2-C's
runtime-block wording back to its pre-D2-C text; the integration caught it by
reading, not by a gate. The anchor gate cannot see such a revert (the reverted
text cites nothing wrong), and the residual gate sees it only when the row
carries a name its vocabulary reads.

What it checks
--------------
For every commit it judges, the published documentation -- ``README.md``,
``CONTRIBUTING.md`` and ``docs/**/*.md`` -- is read at the commit and at its
parent and split into sentences: paragraphs, list items and table cells, cut at
sentence ends, with anchor digits and content pins normalised away (a
re-anchor is not an edit), and at least ``MIN_CHARS`` long. A sentence the
parent has and the commit does not is

* a **rewrite** when the commit adds a sentence at least ``REWRITE_RATIO``
  similar to it, and that sentence is new; nothing more is asked;
* a **revert** when that similar sentence is not new: the file held it in its
  history before the parent (within ``HISTORY_DEPTH`` versions) and the parent
  had dropped it -- older text coming back over newer text;
* a **deletion** otherwise.

Every revert and every deletion must be NAMED by the commit's message, which
names a sentence when it carries any of:

* the short hash (``SHA_CHARS`` hex digits or more) of the commit that
  introduced the sentence, or of the commit that one was cherry-picked from;
* a lane label of that commit's subject (``R5 lane V19-D`` -> ``V19-D``,
  ``INT23``, ``lane F6`` -> ``F6``);
* ``FRAGMENT_WORDS`` consecutive words of the sentence itself;
* the key of the table row the sentence sat in (``OL7``, ``SZ10a``, a
  backticked name in the row's first cell).

Which commits
-------------
``--base REF`` and ``--head REF`` judge the one change base -> head, with the
messages of every commit in ``base..head`` (``--message-from REF`` or
``--message-file PATH`` adds to them): that is the replay mode the audit's
acceptance uses (597a4367's pages replayed onto db98990c must fail; e3e20cb5,
V19-A's stated revert of PERF-P1's view, must pass with its own message). With
``--files`` the pages judged are limited to those given. ``--head REF`` without
``--base`` takes the base the walk below takes (``PINEFORGE_DOC_REVERT_BASE``,
else the merge base with main) and stops with exit 2 when none resolves: the
fast preflight runs it as ``doc-reverts-net`` with ``--head HEAD``, so the net
change of a branch, merge resolutions included, is judged beside the walk.

Without ``--head`` the gate walks the non-merge commits of ``base..HEAD`` one
by one, each against its parent and with its own message. ``base`` is
``--base``, else ``$PINEFORGE_DOC_REVERT_BASE``, else the merge base with
``refs/remotes/origin/main`` or ``refs/heads/main``, else -- in a checkout with
no branch refs, as the lab's remote hosts have -- the newest first-parent
ancestor of ``HEAD`` whose subject ends in a pull request number, `` (#N)``:
the squash or merge commit GitHub wrote on ``main``, which is the merge base
CI would find (its branch's commits were judged one by one, with their own
messages, before it merged; a squash commit's message is the pull request's
title and cannot name them again). Where none resolves, the walk starts at
``HEAD`` and stops at the first commit that predates this gate. A commit whose
own tree has no ``scripts/check_doc_reverts.py`` is never judged: the rule binds
from the commit that introduced it on. At most ``MAX_COMMITS`` commits are walked.

Exit 0 when every revert and deletion is named, 1 when one is not, and 2
when the revisions or pages cannot be compared. An implicit empty comparison
at main's own tip reports that its commits were judged before merge and exits 77.
"""
from __future__ import annotations

import argparse
from collections import Counter
from collections.abc import Iterator
from dataclasses import dataclass, field
import difflib
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
SELF = 'scripts/check_doc_reverts.py'
DOC_ROOTS = ('README.md', 'CONTRIBUTING.md', 'docs')
MIN_CHARS = 30
REWRITE_RATIO = 0.7
HISTORY_DEPTH = 400
MAX_COMMITS = 400
FRAGMENT_WORDS = 6
SHA_CHARS = 7
MAIN_TIP_NOTICE = 'NOT COMPARED: HEAD is the tip of main; its commits were judged before the merge'

FENCE = re.compile(r'^\s*(```|~~~)')
BULLET = re.compile(r'^(?:[-*+]|\d+[.)])\s')
RULE_CELL = re.compile(r'\s*:?-{2,}:?\s*')
# Sentence ends: a terminal mark before a capital, a backtick, an emphasis or
# a bracket; and a colon before a capital (the pages' "Scope: ...").
SPLIT = re.compile(r'(?<=[.;!?])\s+(?=[A-Z`*(\["])|(?<=:)\s+(?=[A-Z`*])')
PIN = re.compile(r'(?i)sha256:[0-9a-f]{64}')
DIGITS = re.compile(r':\d+(?:-\d+)?(?!\d)')
LANE_LABELS = (
    re.compile(r'\bR5 (?:gap )?lane ([A-Za-z0-9][A-Za-z0-9-]*[A-Za-z0-9])'),
    re.compile(r'\blanes? ([A-Z][A-Za-z0-9]*(?:-[A-Za-z0-9]+)*)'),
    re.compile(r'\b(INT\d+[a-z]?)\b'),
)
CHERRY = re.compile(r'\(cherry picked from commit ([0-9a-f]{7,40})\)')
# The subject GitHub gives the commit a pull request merges as on main.
MERGED_PULL_REQUEST = re.compile(r' \(#\d+\)$')
ROW_KEY = re.compile(r'^(?:[A-Za-z]+[0-9]+[A-Za-z]?(?:-[A-Za-z0-9]+)?|`[^`]+`)$')


class GitError(Exception):
    """git could not answer; exit status 2."""


def git(*args: str, root: Path = ROOT, check: bool = True) -> str:
    result = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
    if check and result.returncode:
        raise GitError('git ' + ' '.join(args) + ': ' + result.stderr.strip())
    return result.stdout if result.returncode == 0 else ''


def normalise(sentence: str) -> str:
    sentence = PIN.sub('sha256:H', sentence)
    sentence = DIGITS.sub(':N', sentence)
    return re.sub(r'\s+', ' ', sentence).strip()


@dataclass(frozen=True)
class Unit:
    text: str        # normalised
    row_key: str     # the first cell of the table row it sat in, or ''


def units(text: str) -> Counter:
    """The sentence units of a page, as a multiset of Unit."""
    out: Counter = Counter()
    blocks: list[tuple[str, str]] = []
    buf: list[str] = []
    fenced = False

    def flush() -> None:
        if buf:
            blocks.append((' '.join(buf), ''))
            buf.clear()

    for line in text.split('\n'):
        if FENCE.match(line):
            fenced = not fenced
            flush()
            continue
        if fenced:
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith('#') or BULLET.match(stripped):
            flush()
            if stripped.startswith('#'):
                blocks.append((stripped, ''))
            elif stripped:
                buf.append(stripped)
            continue
        if stripped.startswith('|'):
            flush()
            cells = [cell.strip() for cell in stripped.strip('|').split('|')]
            key = cells[0].strip('* ') if cells else ''
            key = key if ROW_KEY.match(key) else ''
            for cell in cells:
                if cell and not RULE_CELL.fullmatch(cell):
                    blocks.append((cell, key))
            continue
        buf.append(stripped)
    flush()
    for block, key in blocks:
        for sentence in SPLIT.split(block):
            sentence = normalise(sentence)
            if len(sentence) >= MIN_CHARS:
                out[Unit(sentence, key)] += 1
    return out


def doc_paths(ref: str | None, root: Path) -> list[str]:
    """The published pages at `ref` (None: the working tree)."""
    if ref is None:
        found = []
        for top in DOC_ROOTS:
            path = root / top
            if path.is_file():
                found.append(top)
            elif path.is_dir():
                found.extend(p.relative_to(root).as_posix() for p in sorted(path.rglob('*.md')))
        return found
    listed = git('ls-tree', '-r', '-z', '--name-only', ref, '--', *DOC_ROOTS, root=root).split('\0')
    return [p for p in listed if p.endswith('.md')]


def show(ref: str, path: str, root: Path) -> str:
    try:
        return git('show', f'{ref}:{path}', root=root)
    except (GitError, UnicodeDecodeError) as error:
        # A page can legitimately be absent before its addition or after deletion.
        if not git('--literal-pathspecs', 'ls-tree', '-z', '--name-only', ref, '--', path,
                   root=root):
            return ''
        raise GitError(f'NOT COMPARED: cannot read {ref[:SHA_CHARS]}:{path}') from error


@dataclass
class Finding:
    path: str
    kind: str            # "deletion" or "revert"
    sentence: str
    introduced: str      # short hash of the commit that introduced it, or ''
    subject: str
    names: list[str] = field(default_factory=list)
    partner: str = ''

    def __str__(self) -> str:
        what = (f'{self.kind} of {self.sentence[:150]!r}'
                + (f' (older text restored: {self.partner[:90]!r})' if self.partner else ''))
        who = (f'introduced by {self.introduced} "{self.subject[:90]}"'
               if self.introduced else 'introducer not found in the history read')
        return (f'{self.path}: unnamed {what}; {who}; the message names it with any of: '
                + ', '.join(self.names + [f'{FRAGMENT_WORDS} consecutive words of it']))


class History:
    """A page's versions reachable from one commit, newest first, read lazily."""

    def __init__(self, root: Path, tip: str, path: str) -> None:
        self.root, self.tip, self.path = root, tip, path
        self._commits: list[str] | None = None
        self._units: dict[str, Counter] = {}

    def commits(self) -> list[str]:
        if self._commits is None:
            out = git('rev-list', f'--max-count={HISTORY_DEPTH}', self.tip, '--', self.path,
                      root=self.root)
            self._commits = out.split()
        return self._commits

    def units_at(self, commit: str) -> Counter:
        if commit not in self._units:
            self._units[commit] = units(show(commit, self.path, self.root))
        return self._units[commit]

    def texts_at(self, commit: str) -> set[str]:
        return {unit.text for unit in self.units_at(commit)}

    def introducer(self, text: str) -> str | None:
        """The commit that last brought `text` into the page (newest first walk:
        the oldest version of the unbroken run of versions that hold it)."""
        found = None
        for commit in self.commits():
            if text in self.texts_at(commit):
                found = commit
            elif found is not None:
                return found
        return found

    def seen_before(self, text: str) -> bool:
        """Whether a version older than the tip held `text`."""
        return any(text in self.texts_at(commit) for commit in self.commits()[1:])


def labels(message: str) -> list[str]:
    subject = message.split('\n', 1)[0]
    found: list[str] = []
    for pattern in LANE_LABELS:
        for label in pattern.findall(subject):
            if label not in found and label.lower() not in ('lane', 'lanes'):
                found.append(label)
    return found


def message_of(commit: str, root: Path) -> str:
    return git('log', '-1', '--format=%B', commit, root=root)


def names_for(commit: str | None, root: Path) -> list[str]:
    if not commit:
        return []
    message = message_of(commit, root)
    names = [commit[:SHA_CHARS]] + labels(message)
    names += [sha[:SHA_CHARS] for sha in CHERRY.findall(message)]
    return names


def fold(text: str) -> str:
    return re.sub(r'\s+', ' ', re.sub(r'[`*_]', '', text)).strip().lower()


def named(message: str, finding: Finding, row_key: str) -> bool:
    folded = fold(message)
    for name in finding.names:
        if re.fullmatch(r'[0-9a-f]+', name):
            if re.search(r'\b' + name + r'[0-9a-f]*\b', message):
                return True
        elif re.search(r'(?<![A-Za-z0-9-])' + re.escape(name) + r'(?![A-Za-z0-9])', message):
            return True
    if row_key:
        bare = fold(row_key)
        if re.search(r'(?<![a-z0-9])' + re.escape(bare) + r'(?![a-z0-9])', folded):
            return True
    words = fold(finding.sentence).split()
    for start in range(0, max(1, len(words) - FRAGMENT_WORDS + 1)):
        window = ' '.join(words[start:start + FRAGMENT_WORDS])
        if len(words) >= FRAGMENT_WORDS and window in folded:
            return True
    return False


def judge_change(root: Path, parent: str, head: str, pages: list[tuple[str, str]],
                 messages: list[str]) -> list[Finding]:
    """Judge each page at its respective paths in the parent and head."""
    findings: list[Finding] = []
    for parent_path, path in sorted(pages):
        parent_text = show(parent, parent_path, root)
        head_text = show(head, path, root)
        if parent_text == head_text:
            continue
        before = units(parent_text)
        after = units(head_text)
        removed = before - after
        if not removed:
            continue
        added_texts = [unit.text for unit in (after - before).elements()]
        history = History(root, parent, parent_path)
        for unit in removed.elements():
            partner, ratio = None, 0.0
            for candidate in added_texts:
                matcher = difflib.SequenceMatcher(None, unit.text, candidate, autojunk=False)
                if matcher.quick_ratio() <= ratio:
                    continue
                exact = matcher.ratio()
                if exact > ratio:
                    partner, ratio = candidate, exact
            kind = 'deletion'
            if partner is not None and ratio >= REWRITE_RATIO:
                if not history.seen_before(partner):
                    continue                            # a fresh rewrite
                kind = 'revert'
            introduced = history.introducer(unit.text)
            finding = Finding(path, kind, unit.text,
                              introduced[:SHA_CHARS] if introduced else '',
                              message_of(introduced, root).split('\n', 1)[0] if introduced else '',
                              names_for(introduced, root),
                              partner if kind == 'revert' else '')
            if not any(named(message, finding, unit.row_key) for message in messages):
                findings.append(finding)
    return findings


def resolve(ref: str, root: Path) -> str:
    out = git('rev-parse', '--verify', '--quiet', ref + '^{commit}', root=root, check=False).strip()
    if not out:
        raise GitError(f'cannot resolve {ref!r}')
    return out


def main_tips(root: Path) -> Iterator[str]:
    for ref in ('refs/remotes/origin/main', 'refs/heads/main'):
        tip = git('rev-parse', '--verify', '--quiet', ref + '^{commit}', root=root,
                  check=False).strip()
        if tip:
            yield tip


def default_base(root: Path) -> str | None:
    env = os.environ.get('PINEFORGE_DOC_REVERT_BASE')
    if env:
        return resolve(env, root)
    for tip in main_tips(root):
        base = git('merge-base', 'HEAD', tip, root=root, check=False).strip()
        if base:
            return base
    return merged_pull_request(root)


def merged_pull_request(root: Path) -> str | None:
    """With no branch ref, the newest first-parent ancestor of HEAD a pull request
    merged as (subject `... (#N)`), HEAD itself included: the merge base with main."""
    log = git('log', '--first-parent', f'--max-count={MAX_COMMITS}', '--format=%H %s', 'HEAD',
              root=root, check=False)
    for line in log.splitlines():
        sha, _, subject = line.partition(' ')
        if MERGED_PULL_REQUEST.search(subject):
            return sha
    return None


def judged_commits(root: Path, base: str | None) -> list[str]:
    spec = [f'{base}..HEAD'] if base else ['HEAD']
    commits = git('rev-list', '--no-merges', f'--max-count={MAX_COMMITS}', *spec,
                  root=root).split()
    kept = []
    for commit in commits:
        if not git('cat-file', '-e', f'{commit}:{SELF}', root=root, check=False) and \
                subprocess.run(['git', '-C', str(root), 'cat-file', '-e', f'{commit}:{SELF}'],
                               capture_output=True).returncode:
            if base is None:
                break                   # the walk reached the commits before this gate
            continue
        kept.append(commit)
    return kept


def changed_pages(root: Path, parent: str, commit: str) -> list[tuple[str, str]]:
    out = git('diff', '--name-status', '--find-renames', '-z', parent, commit, '--',
              *DOC_ROOTS, root=root)
    fields = iter(out.split('\0')[:-1])
    pages = []
    for status in fields:
        path = next(fields)
        if status.startswith('R'):
            destination = next(fields)
            if path.endswith('.md') and destination.endswith('.md'):
                pages.append((path, destination))
                continue
            # Moving into or out of the published pages is an addition or deletion.
            if not path.endswith('.md'):
                path = destination
        if path.endswith('.md'):
            pages.append((path, path))
    return pages


def select_pages(root: Path, base: str, head: str,
                 selected: list[str] | None) -> list[tuple[str, str]]:
    if selected == []:
        raise GitError('NOT COMPARED: no pages to compare')
    changed = changed_pages(root, base, head)
    if selected is None and changed:
        return changed
    published = set(doc_paths(base, root)) | set(doc_paths(head, root))
    if selected is not None:
        for path in selected:
            if path not in published:
                raise GitError(f'NOT COMPARED: no published page {path}')
        wanted = set(selected)
        matches = [pair for pair in changed if not wanted.isdisjoint(pair)]
        unchanged = wanted.difference(path for pair in matches for path in pair)
        return matches + [(path, path) for path in sorted(unchanged)]
    if not published:
        raise GitError('NOT COMPARED: no pages to compare')
    return [(path, path) for path in sorted(published)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--base', help='judge from this commit (default: the merge base with main)')
    parser.add_argument('--head', help='judge the one change base -> HEAD-of-this-ref')
    parser.add_argument('--files', nargs='*', default=None,
                        help='with --head: judge only these pages')
    parser.add_argument('--message-from', action='append', default=[],
                        help='with --head: also read this commit\'s message (repeatable)')
    parser.add_argument('--message-file', type=Path, default=None,
                        help='with --head: also read this file as message text')
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        base = resolve(args.base, root) if args.base is not None else default_base(root)
        if args.head is not None and base is None:
            raise GitError('NOT COMPARED: --head needs --base')
        head = resolve('HEAD' if args.head is None else args.head, root)
        if (args.base is None and args.head in (None, 'HEAD') and base == head
                and head in main_tips(root)):
            print(MAIN_TIP_NOTICE)
            return 77
        if args.head is not None:
            if base == head:
                raise GitError('NOT COMPARED: base and head are the same commit')
            pages = select_pages(root, base, head, args.files)
            commits = git('rev-list', f'{base}..{head}', root=root).split()
            messages = [message_of(c, root) for c in commits]
            messages.extend(message_of(resolve(ref, root), root) for ref in args.message_from)
            if args.message_file is not None:
                try:
                    messages.append(args.message_file.read_text(encoding='utf-8'))
                except (OSError, UnicodeError) as error:
                    raise GitError('NOT COMPARED: cannot read message file') from error
            findings = judge_change(root, base, head, pages, messages)
            judged = f'{base[:SHA_CHARS]} -> {head[:SHA_CHARS]} ({len(pages)} pages)'
        else:
            commits = judged_commits(root, base)
            findings = []
            compared = []
            saw_page = False
            for commit in commits:
                parents = git('rev-list', '--parents', '-n', '1', commit, root=root).split()[1:]
                parent = parents[0] if parents else None
                if parent is None:
                    continue
                compared.append(commit)
                pages = changed_pages(root, parent, commit)
                if not pages:
                    continue
                saw_page = True
                findings += judge_change(root, parent, commit, pages, [message_of(commit, root)])
            if not compared:
                raise GitError('NOT COMPARED: no commits to compare')
            if not saw_page and not any(doc_paths(commit, root) for commit in compared):
                raise GitError('NOT COMPARED: no pages to compare')
            judged = (f'{len(compared)} commits since '
                      + (base[:SHA_CHARS] if base else 'this gate landed'))
    except GitError as error:
        print(f'check_doc_reverts: {error}', file=sys.stderr)
        return 2
    for finding in findings:
        print('check_doc_reverts: ' + str(finding), file=sys.stderr)
    verdict = 'FAIL' if findings else 'OK'
    print(f'doc reverts: {judged}: {len(findings)} unnamed sentence deletions or reverts '
          f'... {verdict}')
    return 1 if findings else 0


if __name__ == '__main__':
    raise SystemExit(main())
