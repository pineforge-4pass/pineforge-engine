#!/usr/bin/env python3
"""Re-anchor the doc citations ``check_doc_anchors.py --fix`` refuses as ambiguous.

Why this exists
---------------
Every lane that edits a header moves the lines the published pages cite, so each
lane re-anchors ``docs/pages/*.md``, ``docs/adr/0001-kernel-adapter-boundary.md``
and ``docs/design/native-feature-parity.md`` against *its own* tree.  When an
integration wave merges those lanes, the digits are a mixture: each citation is
correct for the lane that last wrote it and wrong for the merged tree.
``check_doc_anchors.py --fix`` repairs the citations whose symbol occurs on
exactly one line, and deliberately refuses the rest — picking between ten lines
that all say ``invoke_callback`` is editorial judgement, not arithmetic.

But it is not judgement when the *old* line number is known to be right for some
tree.  Then the repair is a line map: diff that tree's file against this one and
carry the citation across.  That is what this does, and it is why the wave's
ambiguous anchors do not have to be guessed one at a time.

How it decides
--------------
For each BAD anchor with a claimed symbol that ``--fix`` left alone:

1. for every reference given with ``--ref`` (the lane tips and the merge base),
   read that reference's copy of the cited file and ask whether the claimed
   symbol really is inside the cited window there.  A reference that does not
   place the symbol where the page says it is cannot be the tree the citation
   was written against, and is dropped;
2. map the cited first/last lines through
   ``difflib.SequenceMatcher`` against the working tree's copy.  A line the
   matcher deletes is carried by the nearest surviving line within
   ``--slack`` lines, at the same offset;
3. **verify**: the mapped citation must place the symbol inside its own window
   in the working tree.  A mapping that does not is dropped — so a wrong
   reference cannot produce a wrong anchor, only no anchor;
4. if the surviving references disagree, the anchor is reported and left alone —
   unless ``--decide`` is given, which then picks the candidate that points most
   directly AT the symbol: the smallest gap between the span's first line and an
   occurrence inside it, then the reading the most references share, then the
   order the references were given in.  Every candidate has already passed step
   3, so this chooses between correct anchors, not between right and wrong; the
   alternatives are printed either way.

Nothing is rewritten but the digits, and only after step 3 proves the result
resolves.  Run ``check_doc_anchors.py`` afterwards: it is the gate, this is only
a way to reach it.

Usage
-----
    python3 scripts/reanchor_doc_citations.py --ref origin/main --ref r5/E18 ...
    python3 scripts/reanchor_doc_citations.py --self-test
"""
from __future__ import annotations

import argparse
import difflib
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_doc_anchors as cda


def git_show(root: Path, ref: str, rel: str) -> list[str] | None:
    """The file as of ``ref``, or None when that tree does not carry it."""
    done = subprocess.run(['git', '-C', str(root), 'show', f'{ref}:{rel}'],
                          capture_output=True, text=True, errors='replace')
    if done.returncode:
        return None
    return done.stdout.splitlines()


def line_map(old: list[str], new: list[str]) -> dict[int, int]:
    """1-based old line -> 1-based new line, for the lines both files share."""
    out: dict[int, int] = {}
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(
            None, old, new, autojunk=False).get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1):
                out[i1 + k + 1] = j1 + k + 1
    return out


def carry(mapping: dict[int, int], line: int, slack: int) -> int | None:
    """``line`` through the map, or the nearest surviving neighbour's offset."""
    if line in mapping:
        return mapping[line]
    for distance in range(1, slack + 1):
        for neighbour in (line - distance, line + distance):
            if neighbour in mapping:
                return mapping[neighbour] + (line - neighbour)
    return None


def resolves(lines: list[str], first: int, last: int, symbol: str, mode: str) -> bool:
    """The check_doc_anchors OK test, on an arbitrary copy of the file."""
    if first < 1 or last < first or last > len(lines):
        return False
    window = range(first, min(last + 1, len(lines)) + 1)
    return any(n in window for n in cda.occurrences(lines, symbol, mode))


def rank(now: list[str], span: tuple[int, int], sources: list[str], symbol: str,
         mode: str, order: dict[str, int]) -> tuple[int, int, int]:
    """Smaller is better: gap to the symbol, then fewer references, then ref order."""
    first, last = span
    window = range(first, min(last + 1, len(now)) + 1)
    hits = [n for n in cda.occurrences(now, symbol, mode) if n in window]
    gap = min((n - first for n in hits), default=len(now))
    return (gap, -len(sources), min(order[r] for r in sources))


def reanchor(root: Path, refs: list[str], selected: list[str], slack: int,
             dry_run: bool, decide: bool = False) -> tuple[int, int]:
    tree = cda.Tree(root)
    order = {ref: index for index, ref in enumerate(refs)}
    moved = stuck = 0
    for page in cda.pages(root, selected):
        if not page.is_file():
            continue
        text = page.read_text()
        fixes = []
        for anchor in cda.collect(page, text):
            cda.judge(anchor, tree)
            if anchor.verdict == 'OK' or anchor.symbol is None:
                continue
            if getattr(anchor, '_fix', None) is not None:
                continue                       # --fix can already prove this one
            path = tree.resolve(anchor.path)
            if path is None:
                continue
            rel = path.relative_to(root).as_posix()
            now = tree.lines(path)
            agreed: dict[tuple[int, int], list[str]] = {}
            for ref in refs:
                old = git_show(root, ref, rel)
                if old is None:
                    continue
                if not resolves(old, anchor.first, anchor.last, anchor.symbol, anchor.mode):
                    continue
                mapping = line_map(old, now)
                first = carry(mapping, anchor.first, slack)
                last = carry(mapping, anchor.last, slack)
                if first is None or last is None or last < first:
                    continue
                if not resolves(now, first, last, anchor.symbol, anchor.mode):
                    continue
                agreed.setdefault((first, last), []).append(ref)
            where = f'{page.relative_to(root)}:{anchor.doc_line}  {anchor.text}'
            if not agreed:
                print(f'  STUCK    {where}  `{anchor.symbol}` — no reference places it there')
                stuck += 1
                continue
            note = ''
            if len(agreed) > 1:
                shown = '; '.join(f'{a}-{b} from {",".join(r)}' for (a, b), r in agreed.items())
                if not decide:
                    print(f'  CONFLICT {where}  `{anchor.symbol}` — {shown}')
                    stuck += 1
                    continue
                best = min(agreed, key=lambda span: rank(now, span, agreed[span],
                                                         anchor.symbol, anchor.mode, order))
                agreed = {best: agreed[best]}
                note = f'   [decided among {shown}]'
            (first, last), sources = next(iter(agreed.items()))
            anchor._fix = (first, last)
            fixes.append(anchor)
            target = f'{anchor.path}:{first}' + (f'-{last}' if last != first else '')
            print(f'  {where}  ->  {target}   (via {",".join(sources)}){note}')
            moved += 1
        if fixes and not dry_run:
            cda.apply_fixes(page, text, fixes)
    return moved, stuck


# The self-test's header names ``use_widget`` on three lines, which is exactly
# what ``check_doc_anchors.py --fix`` refuses to choose between; the line map
# still carries each citation, which is the claim being tested.
SELF_TEST_OLD = """#pragma once
namespace demo {
struct Widget {
    int value = 0;
};
void use_widget();
void use_widget(int n);
void use_widget(double d);
}  // namespace demo
"""

SELF_TEST_NEW = """#pragma once
// five
// comment
// lines
// were
// inserted
namespace demo {
struct Widget {
    int value = 0;
};
void use_widget();
void use_widget(int n);
void use_widget(double d);
}  // namespace demo
"""

SELF_TEST_PAGE = """# Demo

The no-argument form is `use_widget` (widget.hpp:6).

The double form is `use_widget` (widget.hpp:8).
"""


def self_test() -> int:
    """A two-commit repository whose header shifts by three lines."""
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        (root / 'docs' / 'pages').mkdir(parents=True)
        (root / 'include' / 'pineforge').mkdir(parents=True)
        header = root / 'include' / 'pineforge' / 'widget.hpp'
        page = root / 'docs' / 'pages' / 'demo.md'
        header.write_text(SELF_TEST_OLD)
        page.write_text(SELF_TEST_PAGE)
        run = lambda *a: subprocess.run(['git', '-C', str(root), *a], check=True,
                                        capture_output=True)
        run('init', '-q')
        run('config', 'user.email', 'selftest@example.invalid')
        run('config', 'user.name', 'self test')
        run('add', '-A')
        run('commit', '-qm', 'base')
        header.write_text(SELF_TEST_NEW)

        saved_root, saved_globs = cda.ROOT, cda.PAGE_GLOBS
        cda.PAGE_GLOBS = ('docs/pages/*.md',)
        try:
            moved, stuck = reanchor(root, ['HEAD'], [], slack=3, dry_run=False)
        finally:
            cda.ROOT, cda.PAGE_GLOBS = saved_root, saved_globs

        got = page.read_text()
        want_moved, want_stuck = 2, 0
        problems = []
        if (moved, stuck) != (want_moved, want_stuck):
            problems.append(f'moved/stuck {moved}/{stuck}, want {want_moved}/{want_stuck}')
        if 'widget.hpp:11)' not in got:
            problems.append('the no-argument form was not carried from line 6 to line 11')
        if 'widget.hpp:13)' not in got:
            problems.append('the double form was not carried from line 8 to line 13')
        if problems:
            print('self-test FAILED:')
            for problem in problems:
                print('  ' + problem)
            print('--- page ---')
            print(got)
            return 1
        print('reanchor_doc_citations self-test: 2 ambiguous citations carried '
              'across a 5-line insertion, 0 stuck ... OK')
        return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ref', action='append', default=[],
                        help='a git ref whose line numbers a citation may have been '
                             'written against; repeatable, tried in order')
    parser.add_argument('--page', action='append', default=[],
                        help='limit to these pages (default: every published page)')
    parser.add_argument('--slack', type=int, default=3,
                        help='how far to look for a surviving neighbour when the cited '
                             'line itself was rewritten (default 3)')
    parser.add_argument('--decide', action='store_true',
                        help='resolve a disagreement between references by the anchor that '
                             'points most directly at the symbol (see the module docstring)')
    parser.add_argument('--dry-run', action='store_true', help='report without rewriting')
    parser.add_argument('--self-test', action='store_true',
                        help='run the built-in two-commit check and exit')
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.ref:
        parser.error('at least one --ref is required')
    moved, stuck = reanchor(cda.ROOT, args.ref, args.page, args.slack, args.dry_run,
                            args.decide)
    print(f'reanchor_doc_citations: {moved} citations re-anchored, {stuck} left for a human')
    return 1 if stuck else 0


if __name__ == '__main__':
    raise SystemExit(main())
