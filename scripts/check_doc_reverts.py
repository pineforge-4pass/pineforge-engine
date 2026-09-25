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
``--files`` the pages judged are limited to those given.

Without ``--head`` the gate walks the non-merge commits of ``base..HEAD`` one
by one, each against its parent and with its own message. ``base`` is
``--base``, else ``$PINEFORGE_DOC_REVERT_BASE``, else the merge base with
``origin/main`` or ``main``; where none resolves (a detached checkout with no
branch refs, as the lab's remote hosts have) the walk starts at ``HEAD`` and
stops at the first commit that predates this gate. A commit whose own tree
has no ``scripts/check_doc_reverts.py`` is never judged: the rule binds from
the commit that introduced it on. At most ``MAX_COMMITS`` commits are walked.

Exit 0 when every revert and deletion is named, 1 when one is not, 2 when git
or a named ref cannot be read.
"""
from __future__ import annotations

import argparse
from collections import Counter
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
    listed = git('ls-tree', '-r', '--name-only', ref, '--', *DOC_ROOTS, root=root).split('\n')
    return [p for p in listed if p.endswith('.md')]


def show(ref: str, path: str, root: Path) -> str:
    return git('show', f'{ref}:{path}', root=root, check=False)


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
                      root=self.root, check=False)
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


def judge_change(root: Path, parent: str, head_texts: dict[str, str], message: str,
                 history_tip: str) -> list[Finding]:
    """The unnamed deletions and reverts of one change parent -> head.

    `head_texts` maps each page to its text at the head (the pages changed);
    `history_tip` is the commit whose history the introducer and revert
    questions read (the parent, for a commit)."""
    findings: list[Finding] = []
    for path, head_text in sorted(head_texts.items()):
        before = units(show(parent, path, root))
        after = units(head_text)
        removed = before - after
        if not removed:
            continue
        added_texts = [unit.text for unit in (after - before).elements()]
        history = History(root, history_tip, path)
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
            if not named(message, finding, unit.row_key):
                findings.append(finding)
    return findings


def resolve(ref: str, root: Path) -> str:
    out = git('rev-parse', '--verify', '--quiet', ref + '^{commit}', root=root, check=False).strip()
    if not out:
        raise GitError(f'cannot resolve {ref!r}')
    return out


def default_base(root: Path) -> str | None:
    env = os.environ.get('PINEFORGE_DOC_REVERT_BASE')
    if env:
        return resolve(env, root)
    for ref in ('origin/main', 'main'):
        tip = git('rev-parse', '--verify', '--quiet', ref + '^{commit}', root=root,
                  check=False).strip()
        if tip:
            base = git('merge-base', 'HEAD', tip, root=root, check=False).strip()
            if base:
                return base
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


def changed_pages(root: Path, parent: str, commit: str) -> list[str]:
    out = git('diff', '--name-only', parent, commit, '--', *DOC_ROOTS, root=root)
    return [p for p in out.split() if p.endswith('.md')]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--base', help='judge from this commit (default: the merge base with main)')
    parser.add_argument('--head', help='judge the one change base -> HEAD-of-this-ref')
    parser.add_argument('--files', nargs='*', default=None,
                        help='with --head: judge only these pages')
    parser.add_argument('--message-from', action='append', default=[],
                        help='also read this commit\'s message (repeatable)')
    parser.add_argument('--message-file', type=Path, default=None,
                        help='also read this file as message text')
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        extra = ''.join(message_of(resolve(ref, root), root) + '\n' for ref in args.message_from)
        if args.message_file is not None:
            extra += args.message_file.read_text(encoding='utf-8') + '\n'
        if args.head:
            if not args.base:
                raise GitError('--head needs --base')
            base, head = resolve(args.base, root), resolve(args.head, root)
            pages = args.files if args.files is not None else sorted(
                set(changed_pages(root, base, head)))
            # The messages that may name a deletion: the ones given, or else
            # those of every commit the change consists of.
            messages = extra or ''.join(
                message_of(c, root) + '\n'
                for c in git('rev-list', f'{base}..{head}', root=root, check=False).split())
            texts = {path: show(head, path, root) for path in pages}
            findings = judge_change(root, base, texts, messages, base)
            judged = f'{base[:SHA_CHARS]} -> {head[:SHA_CHARS]} ({len(pages)} pages)'
        else:
            base = resolve(args.base, root) if args.base else default_base(root)
            commits = judged_commits(root, base)
            findings = []
            for commit in commits:
                parents = git('rev-list', '--parents', '-n', '1', commit, root=root).split()[1:]
                parent = parents[0] if parents else None
                if parent is None:
                    continue
                pages = changed_pages(root, parent, commit)
                if not pages:
                    continue
                texts = {path: show(commit, path, root) for path in pages}
                findings += judge_change(root, parent, texts, message_of(commit, root) + extra,
                                         parent)
            judged = (f'{len(commits)} commits since '
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
