#!/usr/bin/env python3
"""Keep nine kinds of rot out of the published documentation.

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

The nine rules
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
   ``inline namespace <name>_v<n>``, every ``"pineforge-…/v<n>"`` hash
   domain and, since R5 lane H-DOCGATES, every ``"native-…/v<n>"`` domain
   (``native-consumer/v9``, ``native-driver/v5``) - and any *other* version of
   a live family is stale.  Deriving it
   beats a hardcoded range: it needs no edit when an epoch bumps, and it does
   not flag ``native_order_v1``, which is the live identity epoch even though
   ``native_order.hpp`` is at v6.  The same ``<!-- verified HEAD -->`` marker
   exempts a line that discusses an epoch's history on purpose.

4. A relative markdown link whose target file, or whose ``#anchor`` inside
   that file, does not exist.  Anchors are matched against both spellings the
   pages use: Doxygen's explicit ``{#label}`` and a GitHub-style slug of the
   heading text.  Doxygen ``@ref`` targets are deliberately *not* checked:
   they also name C++ symbols, which cannot be resolved without Doxygen's own
   index, so a checker here would guess.  An inline code span is not a link,
   for the same reason a fenced block is not one: ``Series<T>::operator[](k)``
   spelled in backticks is an operator signature, and reading its ``[](k)`` as
   a link to a file named ``k`` is the checker inventing a citation the page
   never made.  Only the span's *content* is masked, so the label of a real
   link (``[`docs/ci.md`](docs/ci.md)``) is still read as one.

5. A cited ``tests/…``, ``examples/…`` or ``scripts/…`` path that does not
   exist, anywhere the rules above look -- and, since R5 lane H-DOCGATES, a
   ``src/…`` or ``include/…`` path to a C/C++ source or header (a ``git show
   <ref>:path`` object path is history by its spelling, and a file the
   clause calls *generated* exists only in a build or an install).  A migration table's "Runs in"
   column and a reference page's "pinned by" clause are the reader's way to
   the witness; a file that never existed sends them nowhere (four of them on
   ``pine-to-native.md`` did).  A ``<placeholder>`` or ``*`` in a path is a
   pattern and must match at least one file.  A path that is gone on purpose
   may stay only where its sentence says so (``deleted``, ``retired``, ``was``
   …) *and* its line carries the ``<!-- verified HEAD -->`` marker.

6. A stated count the tree can derive, stated wrong.  Three families are read
   out of their one source each, never hardcoded here:

   * the ``PF_API`` declarations of ``include/pineforge/native_c_api.h`` (and,
     beside them, of ``pineforge.h``, the runtime set
     ``scripts/check_c_abi_runtime.py`` pins, and their sum);
   * ``KERNEL_MIN_TESTS`` / ``RELEASE_MIN_TESTS`` in ``scripts/ci_verify.py``;
   * the example manifest of ``scripts/check_native_include_independence.py``
     (how many sources, how many of them C).

   A sentence is read for the phrasings the pages use - ``34 further PF_API
   functions``, ``32 strategy_native_* functions``, ``native_c_api.h — 32``,
   ``KERNEL_MIN_TESTS … (193 rows)``, ``all fifteen sources``, ``thirteen C++
   and two C``, ``the C one`` - and the number must be the derived one.  When
   a source cannot be read, a sentence that states its count is an offender
   too: the claim can no longer be checked.  The marker exempts a line that
   records a count as history.

7. A ``<!-- verified HEAD -->`` marker on a line that states a non-live epoch
   as the tree's present.  The marker exempts rule 3 because a line may
   discuss an epoch's *history*; it was also sitting on sentences such as
   "its inline namespace is now ``engine_script_run_v4``" and "the current
   baseline uses ``native_order_v5``", which hid them from rule 3.  The
   clause around each non-live token (to the nearest sentence end, ``;`` or
   table bar) is read: a present-tense framing (``is``, ``are``, ``now``,
   ``uses``, ``current`` …) with no history framing (``was``, ``stood``,
   ``frozen``, ``until``, ``advanced`` …), or a ``file:line`` citation right
   after the token (which says the token lives in today's tree), keeps the
   marker from exempting it.  A version *above* every live one is a forward
   reference (``removed at lifecycle_v2``) and is judged by its framing only.

8. A stated ``PF_ABI_VERSION`` or ``PF_NATIVE_API_VERSION`` that is not the
   number the header defines (R5 lane H-DOCGATES).  A lower number is history
   and needs the marker and a history framing; a higher one is a forward
   reference (``removed at PF_ABI_VERSION 5``) unless the clause states it as
   today's (``is now``, ``current``).

9. A PineForge release stated as the current one (``the current release``,
   ``the latest release``, ``this tree's version``) that is not the ``VERSION``
   file's (R5 lane H-DOCGATES).  A version framed as history (``since``,
   ``was``, a release-notes link) is not a claim about this tree.

Exit status
-----------
0 when no offender is found, 1 otherwise.  ``ci_preflight`` runs it as a
binding stage.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

#: Where each rule looks.  ``lane L…`` is about the *published* migration
#: surface; the rest guard everything a reader can reach under ``docs/``.
PUBLISHED_GLOBS = ('README.md', 'CONTRIBUTING.md', 'docs/pages/*.md')
DOC_GLOBS = ('README.md', 'CONTRIBUTING.md', 'docs/*.md', 'docs/pages/*.md',
             'docs/adr/*.md', 'docs/design/*.md')

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
CODE_SPAN_RE = re.compile(r'(`+)(.+?)\1')
INLINE_NS_RE = re.compile(r'\binline namespace\s+([a-z][a-z0-9_]*_v[0-9]+)')
LITERAL_DOMAIN_RE = re.compile(r'"pineforge-([a-z-]+)/v([0-9]+)')
# Rule 3's second domain family (R5 lane H-DOCGATES): the kernel's own
# `native-<name>/v<n>` hash domains (market_driver.hpp).
NATIVE_DOMAIN_RE = re.compile(r'(?<![\w-])native-([a-z]+(?:-[a-z]+)*)/v([0-9]+)\b')
LITERAL_NATIVE_DOMAIN_RE = re.compile(r'"native-([a-z]+(?:-[a-z]+)*)/v([0-9]+)')
# Rule 8: the ABI numbers the C headers define, and a page stating one.
ABI_DEFINE_RE = re.compile(r'^\s*#\s*define\s+(PF_ABI_VERSION|PF_NATIVE_API_VERSION)\s+(\d+)\b',
                           re.MULTILINE)
ABI_STATED_RE = re.compile(r'\b(PF_ABI_VERSION|PF_NATIVE_API_VERSION)`?'
                           r'(?:\s*(?:==|=|:|is|was|stays|remains|still|now|currently|of|at)){0,3}'
                           r'\s*\(?`?\s*(\d+)\b')
# Rule 9: a release version on a page, and the words that make it the current one.
RELEASE_RE = re.compile(r'(?<![\w.])v?(\d+\.\d+\.\d+)(?![\w.]*\d)')
CURRENT_RELEASE_RE = re.compile(r'\b(?:current|latest|this tree|this release|newest)\b', re.I)
PINEFORGE_RE = re.compile(r'\b(?:PineForge|pineforge-engine|VERSION)\b')

TREE_GLOBS = ('include/pineforge/**/*.hpp', 'include/pineforge/**/*.h',
              'src/**/*.cpp', 'src/**/*.hpp')

# Rule 5.  A path starts at a token boundary, so `./build/examples/native/x`
# (a build output) and `runner/examples/strategy.cpp` are not read as rooted.
CITED_PATH_RE = re.compile(r'(?<![\w/.\-:])((?:tests|examples|scripts|src|include)/'
                           r'[A-Za-z0-9_./*<>+\-]*[A-Za-z0-9_*/>])')
# A src/ or include/ citation is this tree's only when it names a C/C++ file or
# a directory (`src/main.rs` on the Rust page is the example crate's).
TREE_SOURCE_RE = re.compile(r'^(?:src|include)/(?:.*\.(?:cpp|hpp|h|c|cc|inc|ipp)|.*/)$')
# Words that say a path is gone on purpose (rule 5), or that a clause is about
# the past (rule 7).
HISTORY_RE = re.compile(
    r'\b(?:was|were|used|stood|historical|history|frozen|immutable|previous(?:ly)?|'
    r'former(?:ly)?|until|removed|deleted|retired|superseded|advanced|bumped|moved|'
    r'old|older|earlier|originally|gone|no longer|predat\w*|pre-[a-z0-9]+)\b|→|->',
    re.IGNORECASE)
PRESENT_RE = re.compile(
    r'\b(?:is|are|now|uses?|current(?:ly)?|today|remains?|stays?|holds?|carries)\b',
    re.IGNORECASE)
# A `file:line` right after a token (rule 7): the page says the token is there.
TRAILING_ANCHOR_RE = re.compile(r'^[`"\s]*\(?`?[A-Za-z0-9_./+-]+\.(?:cpp|hpp|h|c|py|md):\d+')

NUMBER_WORDS = {w: n for n, w in enumerate(
    'zero one two three four five six seven eight nine ten eleven twelve thirteen '
    'fourteen fifteen sixteen seventeen eighteen nineteen twenty'.split())}
NUMBER_WORDS.update({'thirty': 30, 'forty': 40, 'fifty': 50, 'sixty': 60,
                     'seventy': 70, 'eighty': 80, 'ninety': 90})
_N = r'(?P<n>\d+|' + '|'.join(sorted(NUMBER_WORDS, key=len, reverse=True)) + r')'
_SENTENCE_BREAK = re.compile(r'\.\s|\n\s*\n|\|')

#: Rule 6: (family, phrasing, context the sentence must also name or None).
#: Matched against the sentence with `**` and backticks removed.
COUNT_PHRASINGS = (
    ('native-c-api', re.compile(r'\b' + _N + r'\s+(?:additive|further)\s+(?:PF_API\s+)?'
                                r'(?:symbols|functions)\b', re.I), None),
    ('native-prefix', re.compile(r'\b' + _N + r'\s+strategy_native_\*\s+(?:symbols|functions)\b',
                                re.I), None),
    ('native-c-api', re.compile(r'native_c_api\.h>?\s*(?:\([^)]*\))?\s*[—–]+\s*' + _N + r'\b',
                                re.I), None),
    ('pineforge-h', re.compile(r'pineforge\.h>?\s*[—–]+\s*' + _N + r'\b', re.I), None),
    ('pineforge-h', re.compile(r'\b' + _N + r'\s+public\s+(?:PF_API\s+)?'
                               r'(?:functions|declarations|symbols)\b', re.I), None),
    ('runtime', re.compile(r'\b' + _N + r'\s+(?:codegen-facing\s+|compiled-strategy\s+)?'
                           r'runtime\s+(?:PF_API\s+)?(?:symbols|implementations|exports)\b',
                           re.I), None),
    ('pf-api-total', re.compile(r'\b' + _N + r'\s+PF_API\s+declarations\s+across\s+two\s+'
                                r'headers\b', re.I), None),
    ('kernel-floor', re.compile(r'KERNEL_MIN_TESTS\b(?:[^.;|]|\.(?!\s)){0,80}?\b(?P<n>\d+)\s+rows\b'), None),
    ('kernel-floor', re.compile(r'KERNEL_MIN_TESTS\s*(?:=|is|of)\s*(?P<n>\d+)\b'), None),
    ('release-floor', re.compile(r'RELEASE_MIN_TESTS\b(?:[^.;|]|\.(?!\s)){0,80}?\b(?P<n>\d+)\s+rows\b'), None),
    ('release-floor', re.compile(r'RELEASE_MIN_TESTS\s*(?:=|is|of)\s*(?P<n>\d+)\b'), None),
    # The whole set, not a part of it: "three native examples added" and
    # "thirteen more hosts" count a subset (PARTIAL_COUNT_RE below).
    ('examples', re.compile(r'\b' + _N + r'\s+(?:native\s+|Pine-free\s+)?(?:example\s+)?'
                            r'(?:examples|hosts|sources)\b(?!\s+(?:added|landed|joined))', re.I),
     re.compile(r'examples/native|native examples?|example hosts|include.independence', re.I)),
    ('examples-cpp', re.compile(r'\b' + _N + r'\s+C\+\+\s+and\s+\w+\s+C\b', re.I),
     re.compile(r'examples/native|examples|hosts', re.I)),
    ('examples-c', re.compile(r'\b\w+\s+C\+\+\s+and\s+' + _N + r'\s+C\b', re.I),
     re.compile(r'examples/native|examples|hosts', re.I)),
    ('examples-c', re.compile(r'\bthe\s+' + _N + r'\s+C\s+(?:ones|sources|examples|hosts)\b',
                              re.I), None),
    ('examples-c', re.compile(r'\bthe\s+C\s+(?P<n>one)\b', re.I),
     re.compile(r'examples|sources|include.independence', re.I)),
)
# A count right after one of these words is a subset of the family, not the family.
PARTIAL_COUNT_RE = re.compile(r'\b(?:new|more|another|further|extra|other)\s*$', re.I)
COUNT_SOURCES = {
    'native-c-api': 'include/pineforge/native_c_api.h',
    'native-prefix': 'include/pineforge/native_c_api.h',
    'pineforge-h': 'include/pineforge/pineforge.h',
    'runtime': 'scripts/check_c_abi_runtime.py',
    'pf-api-total': 'include/pineforge/native_c_api.h + include/pineforge/pineforge.h',
    'kernel-floor': 'scripts/ci_verify.py',
    'release-floor': 'scripts/ci_verify.py',
    'examples': 'scripts/check_native_include_independence.py',
    'examples-cpp': 'scripts/check_native_include_independence.py',
    'examples-c': 'scripts/check_native_include_independence.py',
}


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
            domains.update(f'native-{family}/v{number}'
                           for family, number in LITERAL_NATIVE_DOMAIN_RE.findall(text))
    return namespaces, domains


def domain_tokens(line: str):
    """(token, family key, version) of every hash-domain token on a line."""
    for match in DOMAIN_RE.finditer(line):
        yield match, f'pineforge-{match.group(1)}', int(match.group(2))
    for match in NATIVE_DOMAIN_RE.finditer(line):
        yield match, f'native-{match.group(1)}', int(match.group(2))


def abi_numbers(root: Path) -> dict[str, int]:
    """Rule 8: the ABI numbers the C headers #define."""
    found: dict[str, int] = {}
    for rel in ('include/pineforge/pineforge.h', 'include/pineforge/native_c_api.h'):
        path = root / rel
        if path.is_file():
            for name, number in ABI_DEFINE_RE.findall(path.read_text(errors='replace')):
                found[name] = int(number)
    return found


def release_version(root: Path) -> str | None:
    path = root / 'VERSION'
    return path.read_text().strip() if path.is_file() else None


def stale_numbers(page: Path, text: str, abi: dict[str, int],
                  version: str | None) -> list[Offender]:
    """Rules 8 and 9 over the page's prose."""
    out: list[Offender] = []
    for number, line in code_free(text):
        verified = VERIFIED in line
        for match in ABI_STATED_RE.finditer(line):
            name, stated = match.group(1), int(match.group(2))
            live = abi.get(name)
            if live is None or stated == live:
                continue
            clause = re.sub(r'<!--.*?-->', ' ', clause_around(line, match.start(), match.end()))
            if stated > live:
                if not re.search(r'\b(?:is now|current(?:ly)?)\b', clause, re.I):
                    continue                           # a forward reference
            elif verified and HISTORY_RE.search(clause):
                continue                               # history, said so and marked
            out.append(Offender(page, number, 'stale-abi', line.strip(),
                                f'`{name}` is {live} in the header; the line states {stated}'))
        if version is None:
            continue
        for match in RELEASE_RE.finditer(line):
            if match.group(1) == version:
                continue
            clause = re.sub(r'<!--.*?-->', ' ', clause_around(line, match.start(), match.end()))
            if not (CURRENT_RELEASE_RE.search(clause) and PINEFORGE_RE.search(clause)):
                continue
            if HISTORY_RE.search(clause) or re.search(r'\bsince\b', clause, re.I):
                continue
            out.append(Offender(page, number, 'stale-release', line.strip(),
                                f'{match.group(0)} is stated as the current release; '
                                f'`VERSION` is {version}'))
    return out


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


def mask_code_spans(line: str) -> str:
    """The line with every inline code span's content blanked out.

    Column positions and every character outside a span are preserved, so a
    rule that reads the line afterwards still reports the same line and still
    sees a real link whose label happens to be code.
    """
    return CODE_SPAN_RE.sub(lambda m: m.group(1) + 'x' * len(m.group(2)) + m.group(1), line)


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


def derived_counts(root: Path) -> dict[str, int | None]:
    """Rule 6's numbers, each read out of its one source; None when unreadable."""
    def read(rel: str) -> str | None:
        path = root / rel
        return path.read_text(errors='replace') if path.is_file() else None

    counts: dict[str, int | None] = dict.fromkeys(COUNT_SOURCES)
    native = read('include/pineforge/native_c_api.h')
    public = read('include/pineforge/pineforge.h')
    if native is not None:
        counts['native-c-api'] = len(re.findall(r'^PF_API\b', native, re.MULTILINE))
        counts['native-prefix'] = len(re.findall(
            r'^PF_API[^\n]*\bstrategy_native_[A-Za-z0-9_]+\s*\(',
            native, re.MULTILINE))
    if public is not None:
        counts['pineforge-h'] = len(re.findall(r'^PF_API\b', public, re.MULTILINE))
    if native is not None and public is not None:
        counts['pf-api-total'] = counts['native-c-api'] + counts['pineforge-h']
    runtime = read('scripts/check_c_abi_runtime.py')
    if runtime is not None:
        block = re.search(r'^EXPECTED_RUNTIME\s*=\s*frozenset\(\{(.*?)\}\)', runtime,
                          re.MULTILINE | re.DOTALL)
        if block:
            counts['runtime'] = len(re.findall(r'"[A-Za-z_][A-Za-z0-9_]*"', block.group(1)))
    verify = read('scripts/ci_verify.py')
    if verify is not None:
        for family, name in (('kernel-floor', 'KERNEL_MIN_TESTS'),
                             ('release-floor', 'RELEASE_MIN_TESTS')):
            value = re.search(r'^' + name + r'\s*=\s*(\d+)\s*$', verify, re.MULTILINE)
            if value:
                counts[family] = int(value.group(1))
    manifest = read('scripts/check_native_include_independence.py')
    if manifest is not None:
        block = re.search(r'^NATIVE_EXAMPLES\s*=\s*\((.*?)^\)', manifest,
                          re.MULTILINE | re.DOTALL)
        if block:
            sources = re.findall(r'\(\s*"[^"]+"\s*,\s*"([^"]+)"\s*\)', block.group(1))
            counts['examples'] = len(sources)
            counts['examples-c'] = sum(1 for s in sources if s.endswith('.c'))
            counts['examples-cpp'] = len(sources) - counts['examples-c']
    return counts


def as_number(text: str) -> int:
    return int(text) if text.isdigit() else NUMBER_WORDS[text.lower()]


def sentences(body: str):
    """(start, end) of each sentence of a masked page: to '. ', a blank line or a bar."""
    start = 0
    for match in _SENTENCE_BREAK.finditer(body):
        yield start, match.start() + 1
        start = match.end()
    yield start, len(body)


def stale_counts(page: Path, body: str, counts: dict[str, int | None]) -> list[Offender]:
    out: list[Offender] = []
    lines = body.splitlines()
    for start, end in sentences(body):
        raw = body[start:end]
        flat = re.sub(r'\*\*|`', '', raw)
        flat = re.sub(r'\s+', ' ', flat)
        for family, phrasing, context in COUNT_PHRASINGS:
            if context is not None and not context.search(flat):
                continue
            for match in phrasing.finditer(flat):
                if family.startswith('examples') and PARTIAL_COUNT_RE.search(flat[:match.start()]):
                    continue
                first = body.count('\n', 0, start) + 1
                last = min(body.count('\n', 0, end) + 1, len(lines))
                if any(VERIFIED in lines[n - 1] for n in range(first, last + 1)):
                    continue
                # Report the line that carries the number, not the sentence's first.
                number = re.compile(r'\b' + re.escape(match.group('n')) + r'\b', re.I)
                line = next((n for n in range(first, last + 1) if number.search(lines[n - 1])),
                            first)
                stated = as_number(match.group('n'))
                derived = counts.get(family)
                where = COUNT_SOURCES[family]
                if derived is None:
                    out.append(Offender(page, line, 'stale-count', match.group(0),
                                        f'states a {family} count, but {where} cannot be read '
                                        'to derive it'))
                elif stated != derived:
                    out.append(Offender(page, line, 'stale-count', match.group(0),
                                        f'states {stated}; {where} derives {derived} '
                                        f'({family})'))
    return out


def dead_paths(root: Path, page: Path, text: str) -> list[Offender]:
    out: list[Offender] = []
    for number, line in code_free(text):
        for match in CITED_PATH_RE.finditer(line):
            cited = match.group(1)
            if cited.startswith(('src/', 'include/')) and not TREE_SOURCE_RE.match(cited):
                continue                               # not a C/C++ file of this tree
            if cited.endswith('/') and (root / cited).is_dir():
                continue
            if re.search(r'\bgenerated\b', clause_around(line, match.start(), match.end()),
                         re.I):
                continue                               # exists in a build or an install
            pattern = re.sub(r'<[^<>]*>', '*', cited)
            if '*' in pattern:
                if any(root.glob(pattern.rstrip('/'))):
                    continue
                problem = f'no file matches the pattern `{cited}`'
            elif (root / cited.rstrip('/')).exists():
                continue
            else:
                problem = f'`{cited}` does not exist'
            if VERIFIED in line and HISTORY_RE.search(clause_around(line, match.start(), match.end())):
                continue                               # gone on purpose, and said so
            out.append(Offender(page, number, 'dead-path', line.strip(),
                                problem + ': cite the file that pins it, or say it is gone'))
    return out


def clause_around(text: str, start: int, end: int) -> str:
    """The clause holding text[start:end]: to the nearest sentence end, ';' or bar."""
    left = max(text.rfind(token, 0, start) for token in ('. ', '; ', '|', '\n\n'))
    left = 0 if left < 0 else left + 1
    rights = [i for i in (text.find(token, end) for token in ('. ', '; ', '|', '\n\n'))
              if i >= 0]
    return text[left:min(rights) if rights else len(text)]


def verified_live_claims(page: Path, body: str, live_ns_families: dict[str, set[str]],
                         live_domain_families: dict[str, set[str]]) -> list[Offender]:
    """Rule 7: a marker may not exempt a non-live epoch stated as today's."""
    out: list[Offender] = []
    lines = body.splitlines()
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line) + 1)
    for index, line in enumerate(lines):
        if VERIFIED not in line:
            continue
        tokens = []
        for match in EPOCH_RE.finditer(line):
            family, version = match.group(1), int(match.group(2))
            live = live_ns_families.get(family)
            if live and match.group(0) not in live:
                newest = max(int(t.rsplit('_v', 1)[1]) for t in live)
                tokens.append((match, version > newest))
        for match, key, version in domain_tokens(line):
            live = live_domain_families.get(key)
            if live and match.group(0) not in live:
                newest = max(int(t.rsplit('/v', 1)[1]) for t in live)
                tokens.append((match, version > newest))
        for match, forward in tokens:
            at, until = offsets[index] + match.start(), offsets[index] + match.end()
            clause = re.sub(r'<!--.*?-->', ' ', clause_around(body, at, until))
            cited_here = bool(TRAILING_ANCHOR_RE.match(body[until:until + 120]))
            present = PRESENT_RE.search(clause) and not HISTORY_RE.search(clause)
            if forward:
                present = present and re.search(r'\b(?:is now|current(?:ly)?)\b', clause, re.I)
            if present or cited_here:
                why = ('cites it at a file:line of today\'s tree' if cited_here
                       else 'states it in the present tense')
                out.append(Offender(page, index + 1, 'verified-stale-epoch', line.strip(),
                                    f'`{match.group(0)}` is not live, and the line {why}: '
                                    f'the {VERIFIED} marker exempts history, not a false '
                                    'present'))
    return out


def check(root: Path) -> list[Offender]:
    found: list[Offender] = []
    namespaces, domains = live_epochs(root)
    live_ns_families = families(namespaces, lambda t: t.rsplit('_v', 1))
    live_domain_families = families(domains, lambda t: t.rsplit('/v', 1))
    published = set(pages(root, PUBLISHED_GLOBS))
    anchors_cache: dict[Path, set[str]] = {}
    counts = derived_counts(root)
    abi, version = abi_numbers(root), release_version(root)

    for page in pages(root, DOC_GLOBS):
        text = page.read_text(errors='replace')
        anchors_cache[page] = headings(text)
        found.extend(stale_numbers(page, text, abi, version))
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
                for match, key, _ in domain_tokens(line):
                    token = match.group(0)
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
            for target in LINK_RE.findall(mask_code_spans(line)):
                problem = dead_link(root, page, target, anchors_cache)
                if problem:
                    found.append(Offender(page, number, 'dead-link', line.strip(), problem))
        found.extend(dead_paths(root, page, text))
        body = masked(text)
        found.extend(stale_counts(page, body, counts))
        found.extend(verified_live_claims(page, body, live_ns_families, live_domain_families))
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
        print('\nno lane labels, stale negatives, stale epochs, dead links, dead paths, '
              'stale counts, stale ABI numbers or releases, or markers on a stale present')
        return 0
    print(f'\n{len(found)} offenders:')
    for offender in found:
        excerpt = offender.text if len(offender.text) <= 150 else offender.text[:147] + '...'
        print(f'  {offender.page.relative_to(root)}:{offender.line}  [{offender.rule}] '
              f'{offender.why}\n      {excerpt}')
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
