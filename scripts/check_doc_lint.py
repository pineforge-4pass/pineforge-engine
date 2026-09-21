#!/usr/bin/env python3
"""Keep three kinds of rot out of the published documentation.

Why a second script rather than a mode of ``check_doc_anchors.py``
------------------------------------------------------------------
The two guards have opposite failure semantics and opposite lifetimes.  The
anchor gate is mechanical: its ``--fix`` repairs most of what it finds, and it
is meant to reach zero and stay there.  This one reads *prose* - a roadmap
label, a negative claim, an epoch number - and nothing here can be repaired
mechanically, because every offender is a sentence somebody has to rewrite.
Keeping them apart means the anchor gate can go binding on its own schedule
and this one's offender list reads as a work list rather than as noise inside
another report.

The four rules
--------------
1. ``lane L<n>`` in a published page (``docs/pages/*.md``, ``README.md``).
   A roadmap label is a promise about the future.  Fifteen of them in
   ``pine-to-native.md`` describe work that landed campaigns ago, so a reader
   is told a shipped feature does not exist.  Say what the tree does; the
   roadmap lives in the campaign, not in the migration guide.

2. ``there is no … yet / in this slice / today`` anywhere under ``docs/`` or
   in ``README.md``.  These sentences were true when written and are the first
   thing a gap wave falsifies.  A line may keep one by carrying an explicit
   ``<!-- verified HEAD -->`` marker: a claim that somebody checked the line
   against this tree, either because it is still true or because it is
   deliberately historical.  The marker shows up in the diff when they did not.

3. A stale epoch or hash-domain token anywhere under ``docs/`` or in
   ``README.md``.  The live set is read out of the tree itself - every
   ``inline namespace <name>_v<n>`` and every ``"pineforge-…/v<n>"`` hash
   domain - and any *other* version of a live family is stale.  Deriving it
   beats a hardcoded range: it needs no edit when an epoch bumps, and it does
   not flag ``native_order_v1``, which is the live identity epoch even though
   ``native_order.hpp`` is at v6.  The same ``<!-- verified HEAD -->`` marker
   exempts a line that discusses an epoch's history on purpose.

4. A relative markdown link whose target file, or whose ``#anchor`` inside
   that file, does not exist.  Anchors are matched against both spellings the
   pages use: Doxygen's explicit ``{#label}`` and a GitHub-style slug of the
   heading text.  Doxygen ``@ref`` targets are deliberately *not* checked:
   they also name C++ symbols, which cannot be resolved without Doxygen's own
   index, so a checker here would guess.

Exit status
-----------
0 when no offender is found.  On HEAD this guard FAILS by design: lanes L14-B
and L14-C are the ones that remove today's offenders, so ``ci_preflight`` runs
it advisory until then (see ``--strict-docs`` there and ``docs/ci.md``).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

#: Where each rule looks.  ``lane L…`` is about the *published* migration
#: surface; the rest guard everything a reader can reach under ``docs/``.
PUBLISHED_GLOBS = ('README.md', 'docs/pages/*.md')
DOC_GLOBS = ('README.md', 'docs/*.md', 'docs/pages/*.md', 'docs/adr/*.md', 'docs/design/*.md')

VERIFIED = '<!-- verified HEAD -->'

LANE_RE = re.compile(r'\blane[s]?\s+L[0-9]')
# Tempered: the claim may wrap over a line and may contain a dotted spelling
# (`request.security`), but it stops at a real sentence end or a table bar.
NO_CLAIM_RE = re.compile(r'\bthere (?:is|are) no\b(?:(?!\.\s)[^;|]){0,140}?'
                         r'\b(?:yet|in\s+this\s+slice|today)\b', re.IGNORECASE)
EPOCH_RE = re.compile(r'\b([a-z][a-z0-9_]*?)_v([0-9]+)\b')
DOMAIN_RE = re.compile(r'\bpineforge-([a-z-]+)/v([0-9]+)\b')
LINK_RE = re.compile(r'(?<!\!)\[[^\]]*\]\(([^)\s]+)\)')
HEADING_RE = re.compile(r'^\s{0,3}#{1,6}\s+(.*?)\s*$')
EXPLICIT_ID_RE = re.compile(r'\{#([A-Za-z0-9_.-]+)\}')
FENCE_RE = re.compile(r'^\s*(```|~~~)')
INLINE_NS_RE = re.compile(r'\binline namespace\s+([a-z][a-z0-9_]*_v[0-9]+)')
LITERAL_DOMAIN_RE = re.compile(r'"pineforge-([a-z-]+)/v([0-9]+)')

TREE_GLOBS = ('include/pineforge/**/*.hpp', 'include/pineforge/**/*.h',
              'src/**/*.cpp', 'src/**/*.hpp')


class Offender:
    def __init__(self, page: Path, line: int, rule: str, text: str, why: str) -> None:
        self.page, self.line, self.rule, self.text, self.why = page, line, rule, text, why


def live_epochs(root: Path) -> tuple[set[str], set[str]]:
    """Every epoch namespace and hash domain the tree actually declares."""
    namespaces: set[str] = set()
    domains: set[str] = set()
    for glob in TREE_GLOBS:
        for path in root.glob(glob):
            text = path.read_text(errors='replace')
            namespaces.update(INLINE_NS_RE.findall(text))
            domains.update(f'pineforge-{family}/v{number}'
                           for family, number in LITERAL_DOMAIN_RE.findall(text))
    return namespaces, domains


def families(tokens: set[str], splitter) -> dict[str, set[str]]:
    out: dict[str, set[str]] = {}
    for token in tokens:
        family, _ = splitter(token)
        out.setdefault(family, set()).add(token)
    return out


def code_free(text: str):
    """Yield (line number, line) outside fenced code blocks."""
    fenced = False
    for number, line in enumerate(text.splitlines(), 1):
        if FENCE_RE.match(line):
            fenced = not fenced
            continue
        if not fenced:
            yield number, line


def masked(text: str) -> str:
    """The page with fenced code blocked out, line numbering preserved.

    The "there is no …" claims wrap over a line break, so they have to be read
    from the whole page rather than line by line.
    """
    fenced, out = False, []
    for line in text.splitlines():
        if FENCE_RE.match(line):
            fenced = not fenced
            out.append('')
        else:
            out.append('' if fenced else line)
    return '\n'.join(out)


def headings(text: str) -> set[str]:
    """Every anchor a link may target: explicit ``{#id}`` and heading slugs."""
    found = set(EXPLICIT_ID_RE.findall(text))
    for _, line in code_free(text):
        match = HEADING_RE.match(line)
        if not match:
            continue
        title = EXPLICIT_ID_RE.sub('', match.group(1)).strip()
        slug = re.sub(r'[^a-z0-9 _-]', '', title.lower()).replace(' ', '-').strip('-')
        if slug:
            found.add(slug)
            found.add('autotoc_md' + slug)
    return found


def pages(root: Path, globs: tuple[str, ...]) -> list[Path]:
    seen, out = set(), []
    for glob in globs:
        for path in sorted(root.glob(glob)):
            if path not in seen:
                seen.add(path)
                out.append(path)
    return out


def check(root: Path) -> list[Offender]:
    found: list[Offender] = []
    namespaces, domains = live_epochs(root)
    live_ns_families = families(namespaces, lambda t: t.rsplit('_v', 1))
    live_domain_families = families(domains, lambda t: t.rsplit('/v', 1))
    published = set(pages(root, PUBLISHED_GLOBS))
    anchors_cache: dict[Path, set[str]] = {}

    for page in pages(root, DOC_GLOBS):
        text = page.read_text(errors='replace')
        anchors_cache[page] = headings(text)
        for number, line in code_free(text):
            verified = VERIFIED in line
            if page in published and LANE_RE.search(line):
                found.append(Offender(page, number, 'lane-label', line.strip(),
                                      'a roadmap label on a published page: say what the '
                                      'tree does, not which lane was going to do it'))
            if not verified:
                for family, number_text in EPOCH_RE.findall(line):
                    token = f'{family}_v{number_text}'
                    if family in live_ns_families and token not in live_ns_families[family]:
                        found.append(Offender(
                            page, number, 'stale-epoch', line.strip(),
                            f'`{token}` is not live; the tree declares '
                            + ', '.join(sorted(live_ns_families[family]))))
                for family, number_text in DOMAIN_RE.findall(line):
                    token = f'pineforge-{family}/v{number_text}'
                    key = f'pineforge-{family}'
                    if key in live_domain_families and token not in live_domain_families[key]:
                        found.append(Offender(
                            page, number, 'stale-epoch', line.strip(),
                            f'`{token}` is not live; the tree hashes under '
                            + ', '.join(sorted(live_domain_families[key]))))

    for page in pages(root, DOC_GLOBS):
        body = masked(page.read_text(errors='replace'))
        lines = body.splitlines()
        for match in NO_CLAIM_RE.finditer(body):
            first = body.count('\n', 0, match.start()) + 1
            last = body.count('\n', 0, match.end()) + 1
            if any(VERIFIED in lines[n - 1] for n in range(first, last + 1)):
                continue
            found.append(Offender(page, first, 'stale-negative',
                                  ' '.join(match.group(0).split()),
                                  'a "there is no …" claim with no ' + VERIFIED
                                  + ' marker: the next gap wave falsifies it silently'))

    for page in pages(root, DOC_GLOBS):
        text = page.read_text(errors='replace')
        for number, line in code_free(text):
            for target in LINK_RE.findall(line):
                problem = dead_link(root, page, target, anchors_cache)
                if problem:
                    found.append(Offender(page, number, 'dead-link', line.strip(), problem))
    found.sort(key=lambda o: (str(o.page), o.line, o.rule))
    return found


def dead_link(root: Path, page: Path, target: str,
              cache: dict[Path, set[str]]) -> str | None:
    if re.match(r'^[a-z][a-z0-9+.-]*:', target) or target.startswith('//'):
        return None                                   # absolute URL, not ours to resolve
    path_part, _, anchor = target.partition('#')
    if not path_part:
        resolved = page
    else:
        resolved = (page.parent / path_part).resolve()
        if not resolved.exists():
            return f'link target does not exist: {target}'
        if resolved.is_dir() or resolved.suffix != '.md':
            return None                               # a directory or source file holds no anchors
    if not anchor:
        return None
    if resolved not in cache:
        cache[resolved] = headings(resolved.read_text(errors='replace'))
    if anchor not in cache[resolved]:
        where = 'this page' if resolved == page else resolved.name
        return f'no `#{anchor}` heading or {{#id}} in {where}'
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='Lint the published documentation prose.')
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    found = check(root)

    by_rule: dict[str, int] = {}
    by_page: dict[str, int] = {}
    for offender in found:
        by_rule[offender.rule] = by_rule.get(offender.rule, 0) + 1
        rel = str(offender.page.relative_to(root))
        by_page[rel] = by_page.get(rel, 0) + 1

    width = max((len(p) for p in by_page), default=4)
    print(f'{"page":<{width}} {"offenders":>10}')
    for rel, count in sorted(by_page.items()):
        print(f'{rel:<{width}} {count:>10}')
    print(f'{"TOTAL":<{width}} {len(found):>10}')
    print('by rule: ' + (', '.join(f'{rule}={count}' for rule, count in sorted(by_rule.items()))
                         or 'none'))

    if not found:
        print('\nno lane labels, stale negatives, stale epochs or dead links')
        return 0
    print(f'\n{len(found)} offenders:')
    for offender in found:
        excerpt = offender.text if len(offender.text) <= 150 else offender.text[:147] + '...'
        print(f'  {offender.page.relative_to(root)}:{offender.line}  [{offender.rule}] '
              f'{offender.why}\n      {excerpt}')
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
