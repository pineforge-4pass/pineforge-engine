#!/usr/bin/env python3
"""Byte-identity of the corpus trades against a recorded parity baseline.

WHY NOT THE CORPUS TAPES THEMSELVES. Each
``corpus/validation/<probe>/engine_trades.csv`` committed in pineforge-corpus
was produced by the engine of its day, and pineforge-corpus 442d497 (the
R4-D regeneration) re-transpiled every ``generated.cpp`` WITHOUT re-running
the tapes. Against this engine those tapes no longer reproduce: the harness
appends an ``Engine range-end`` column (run_strategy.py 11e61d41) and prints
``Qty`` at full precision, and a minority of probes moved on a recorded cell.
So the committed tapes are a stale oracle; treating them as the byte oracle
would fail on every commit and gate nothing.

WHAT IS PINNED INSTEAD. ``scripts/corpus_parity_baseline.txt`` records the
sha256 of every ``engine_trades.csv`` this engine produces at the corpus
commit the ``corpus`` gitlink names. That is a byte oracle with a keeper: it
moves only when someone runs ``--update``, and the commit that does carries
the evidence. Re-deriving the trades and finding one probe's sha256 moved IS
"TradingView parity changed" for that probe.

The corpus tape stays in the picture as the locator, not the gate: for a
probe whose hash moved, this prints the first rows where its file differs
from the pinned tape. Two invariants ARE still taken from the tapes, because
they cannot go stale: every probe the corpus commits must produce a file, and
its header must be the tape's header, optionally plus a column named in
ALLOWED_EXTRA_COLUMNS. An undeclared column is drift, never an extension.

  python3 scripts/corpus_trades_identity.py [--corpus corpus] [--files 5] [--lines 20]
  python3 scripts/corpus_trades_identity.py --update    # re-record the baseline

Exit 0 when every probe matches the baseline, 1 on drift, 2 when the
comparison cannot be made.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / 'scripts' / 'corpus_parity_baseline.txt'

# Columns the current harness appends beyond what the pinned corpus recorded.
# Adding one here is a deliberate act: it declares that the corpus tape cannot
# speak about that column. The baseline still pins every byte of it.
ALLOWED_EXTRA_COLUMNS = ('Engine range-end',)

TRADES = 'engine_trades.csv'


def git(corpus: Path, *args: str) -> str:
    result = subprocess.run(['git', '-C', str(corpus), *args],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"corpus_trades_identity: git {' '.join(args)} failed: "
                         f'{result.stderr.strip()}')
    return result.stdout


def committed_paths(corpus: Path) -> list[str]:
    listing = git(corpus, 'ls-files', '--', f'validation/*/{TRADES}')
    return sorted(line for line in listing.splitlines() if line)


def rows_of(text: str) -> list[list[str]]:
    return list(csv.reader(io.StringIO(text)))


def header_problem(path: str, tape: str, produced: str) -> str | None:
    """The tape's header must survive verbatim; extras must be declared."""
    want = rows_of(tape)
    got = rows_of(produced)
    if not want or not got:
        return f'{path}: empty trades file'
    header, got_header = want[0], got[0]
    if got_header[:len(header)] != header:
        return (f'{path}: header changed\n'
                f"  tape:     {','.join(header)}\n"
                f"  produced: {','.join(got_header)}")
    for column in got_header[len(header):]:
        if column not in ALLOWED_EXTRA_COLUMNS:
            return (f'{path}: undeclared trailing column {column!r}\n'
                    f"  tape:     {','.join(header)}\n"
                    f"  produced: {','.join(got_header)}")
    return None


def first_differing_rows(path: str, tape: str, produced: str, lines: int) -> list[str]:
    """Where the produced file parts from the pinned corpus tape."""
    want, got = rows_of(tape), rows_of(produced)
    width = len(want[0]) if want else 0
    out: list[str] = []
    for line, (want_row, got_row) in enumerate(zip(want, got), start=1):
        if want_row[:width] != got_row[:width]:
            out.append(f'  {path}: row {line} (vs the pinned corpus tape)')
            out.append(f"    tape:     {','.join(want_row[:width])}")
            out.append(f"    produced: {','.join(got_row[:width])}")
        if len(out) >= lines:
            return out[:lines]
    if not out:
        if len(want) != len(got):
            out.append(f'  {path}: {len(want) - 1} tape rows, {len(got) - 1} produced')
        else:
            out.append(f'  {path}: identical to the pinned corpus tape on every '
                       'recorded column; the move is in a trailing column')
    return out[:lines]


def read_baseline() -> tuple[dict[str, str], str | None]:
    if not BASELINE.is_file():
        return {}, None
    pinned: dict[str, str] = {}
    gitlink = None
    for line in BASELINE.read_text().splitlines():
        if line.startswith('# corpus '):
            gitlink = line.split()[2]
        if not line or line.startswith('#'):
            continue
        digest, path = line.split(None, 1)
        pinned[path.strip()] = digest
    return pinned, gitlink


def write_baseline(gitlink: str, digests: dict[str, str]) -> None:
    body = [
        '# The sha256 of every engine_trades.csv this engine produces over the',
        '# validation corpus at the gitlink below. Regenerate deliberately:',
        '#   ./scripts/check_corpus_parity.sh && \\',
        '#     python3 scripts/corpus_trades_identity.py --update',
        '# and carry the evidence in the commit that moves it.',
        f'# corpus {gitlink}',
        '',
    ]
    body += [f'{digests[path]}  {path}' for path in sorted(digests)]
    BASELINE.write_text('\n'.join(body) + '\n')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--corpus', default=ROOT / 'corpus', type=Path)
    ap.add_argument('--files', type=int, default=5)
    ap.add_argument('--lines', type=int, default=20)
    ap.add_argument('--update', action='store_true',
                    help='re-record the baseline from the trades on disk')
    args = ap.parse_args()

    corpus: Path = args.corpus
    if not (corpus / 'CMakeLists.txt').is_file():
        print(f'corpus_trades_identity: {corpus} is not a corpus checkout', file=sys.stderr)
        return 2
    paths = committed_paths(corpus)
    if not paths:
        print('corpus_trades_identity: the corpus commits no validation trades', file=sys.stderr)
        return 2
    gitlink = git(corpus, 'rev-parse', 'HEAD').strip()

    digests: dict[str, str] = {}
    missing: list[str] = []
    schema: list[str] = []
    same_as_tape = 0
    same_on_recorded = 0
    for path in paths:
        produced_path = corpus / path
        if not produced_path.is_file():
            missing.append(path)
            continue
        produced = produced_path.read_text()
        digests[path] = hashlib.sha256(produced.encode()).hexdigest()
        tape = git(corpus, 'show', f'HEAD:{path}')
        if tape == produced:
            same_as_tape += 1
        want, got = rows_of(tape), rows_of(produced)
        width = len(want[0]) if want else 0
        if len(want) == len(got) and all(a[:width] == b[:width] for a, b in zip(want, got)):
            same_on_recorded += 1
        problem = header_problem(path, tape, produced)
        if problem:
            schema.append(problem)

    if args.update:
        if missing or schema:
            for line in missing[:args.files]:
                print(f'  MISSING {line}', file=sys.stderr)
            for line in schema[:args.files]:
                print(f'  {line}', file=sys.stderr)
            print('corpus_trades_identity: refusing to record a baseline over a '
                  'missing or schema-broken sweep', file=sys.stderr)
            return 1
        write_baseline(gitlink, digests)
        print(f'corpus_trades_identity: recorded {len(digests)} probes at corpus {gitlink} '
              f'-> {BASELINE.relative_to(ROOT)}')
        return 0

    pinned, pinned_gitlink = read_baseline()
    if not pinned:
        print(f'corpus_trades_identity: no baseline at {BASELINE.relative_to(ROOT)}; '
              'record one with --update', file=sys.stderr)
        return 2
    if pinned_gitlink != gitlink:
        print(f'corpus_trades_identity: the baseline was recorded at corpus '
              f'{pinned_gitlink}, the checkout is at {gitlink}', file=sys.stderr)
        return 2

    moved = [path for path, digest in digests.items()
             if pinned.get(path) not in (None, digest)]
    unknown = [path for path in digests if path not in pinned]
    dropped = [path for path in pinned if path not in digests]

    print(f'corpus_trades_identity: {len(paths)} probes at corpus {gitlink} — '
          f'baseline match={len(digests) - len(moved) - len(unknown)}, moved={len(moved)}, '
          f'unrecorded={len(unknown)}, missing={len(missing) + len(dropped)}, '
          f'schema violations={len(schema)}')
    print(f'  (against the pinned corpus tape: byte-identical={same_as_tape}, '
          f'identical on every recorded column={same_on_recorded} of {len(paths)}; '
          'the rest is the harness generation the tape predates)')

    for line in schema[:args.files]:
        print(f'\n  {line}', file=sys.stderr)
    for path in (missing + dropped)[:args.files]:
        print(f'\n  MISSING {path} — the sweep produced no trades', file=sys.stderr)
    for path in unknown[:args.files]:
        print(f'\n  UNRECORDED {path} — the corpus commits a probe the baseline '
              'does not name', file=sys.stderr)
    for path in moved[:args.files]:
        print(f'\n  MOVED {path}', file=sys.stderr)
        print(f'    baseline: {pinned[path]}', file=sys.stderr)
        print(f'    produced: {digests[path]}', file=sys.stderr)
        tape = git(corpus, 'show', f'HEAD:{path}')
        for line in first_differing_rows(path, tape, (corpus / path).read_text(), args.lines):
            print(f'  {line}', file=sys.stderr)
    extra = len(moved) - args.files
    if extra > 0:
        print(f'\n  (… {extra} more moved; re-run with --files {len(moved)})', file=sys.stderr)

    return 1 if (moved or unknown or missing or dropped or schema) else 0


if __name__ == '__main__':
    sys.exit(main())
