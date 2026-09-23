#!/usr/bin/env python3
"""Name the receipt-gated CTest rows of a build tree that will not run, and prepare what they read.

Four CTest rows pair this tree against historical ABI archives:
test_script_cpp_abi, test_settlement_cpp_abi,
test_aggregate_cpp_versions_runtime and test_l4g_runtime_budget. Each reads the
receipts of ABI providers prepared inside the build tree. A tree configured
with PINEFORGE_REQUIRE_ABI_RECEIPTS=OFF -- the default, so every plain
configure, CLAUDE.md's included -- registers them --skip-if-receipt-missing:
until the seven providers are prepared there, each exits 77, and CTest counts a
skipped row as passed, so it still prints "100% tests passed".
scripts/ci_verify.py prepares its providers itself and configures the option
ON, where a missing receipt fails the row; this script needs neither.

It reads the rows CTest registered (ctest --show-only=json-v1) and names every
receipt-gated row that will skip -- or, registered --require-receipts, fail --
for a missing input receipt. test_l4g_runtime_budget also reads the tree's
compile_commands.json once its receipt exists, so a tree configured without
CMAKE_EXPORT_COMPILE_COMMANDS=ON is named too. --prepare first prepares the
seven providers into the tree with the argv ci_verify uses, reusing a matching
prepared provider and refusing, without deleting it, one that does not match.

Exit status: 0 when every receipt-gated row runs (or none is registered), 1
when one will not, 2 when the directory is no configured build tree or ctest
cannot list its rows.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Callable

from prepare_settlement_cpp_abi_base import PROVIDERS, ROOT, reusable_prepared_base

# The seven providers, in the order scripts/ci_verify.py prepares them.
PROVIDER_ROLES = ('e60', '0e', 'v13', 'v14', 'v15-frozen', 'v16-frozen', 'v18-frozen')
MODES = {'--skip-if-receipt-missing': 'skip', '--require-receipts': 'require'}
# An input receipt: --base-receipt, --v16-frozen-receipt, ...; a bare
# --receipt names the row's own output.
RECEIPT_INPUT = re.compile(r'--(?:[a-z0-9]+-)+receipt')


@dataclass(frozen=True)
class GatedRow:
    name: str
    mode: str  # 'skip' (--skip-if-receipt-missing) or 'require' (--require-receipts)
    missing: tuple[Path, ...] = ()  # input receipts that do not exist
    database: Path | None = None  # a --compile-commands file that does not exist

    @property
    def verdict(self) -> str:
        """'runs', 'skips' or 'fails': what the row does in the next ctest run."""
        if self.missing:
            return 'skips' if self.mode == 'skip' else 'fails'
        return 'fails' if self.database is not None else 'runs'


def registered_tests(build_dir: Path, ctest: str = 'ctest') -> list[dict]:
    """The rows CTest registered in build_dir, as ctest --show-only=json-v1 lists them."""
    result = subprocess.run([ctest, '--show-only=json-v1', '--test-dir', str(build_dir)],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'ctest could not list the rows of {build_dir}:\n'
                           + result.stdout + result.stderr)
    return json.loads(result.stdout).get('tests', [])


def gated_rows(tests: list[dict]) -> list[GatedRow]:
    """The receipt-gated rows among tests, with the inputs each is missing."""
    rows = []
    for test in tests:
        command = test.get('command') or []
        modes = [MODES[argument] for argument in command if argument in MODES]
        if not modes:
            continue
        pairs = list(zip(command, command[1:]))
        missing = tuple(Path(value) for flag, value in pairs
                        if RECEIPT_INPUT.fullmatch(flag) and not Path(value).exists())
        database = next((Path(value) for flag, value in pairs
                         if flag == '--compile-commands' and not Path(value).exists()), None)
        rows.append(GatedRow(test['name'], modes[-1], missing, database))
    return rows


def report(rows: list[GatedRow], build_dir: Path, shown_dir: str) -> int:
    """Print what every gated row will do; 1 when one will not run."""
    def shown(path: Path) -> str:
        try:
            return str(path.resolve().relative_to(build_dir.resolve()))
        except ValueError:
            return str(path)

    head = f'check_abi_receipt_skips: {shown_dir}'
    if not rows:
        print(f'{head} registers no receipt-gated CTest row')
        return 0
    idle = [row for row in rows if row.verdict != 'runs']
    if not idle:
        print(f'{head} registers {len(rows)} receipt-gated CTest rows; every receipt they read '
              f'is present: all {len(rows)} run')
        return 0
    print(f'{head} registers {len(rows)} receipt-gated CTest rows')
    width = max(len(row.name) for row in rows)
    for row in rows:
        reasons = []
        if row.missing:
            noun = 'receipt' if len(row.missing) == 1 else 'receipts'
            reasons.append(f'{noun} missing: ' + ', '.join(shown(path) for path in row.missing))
        if row.database is not None:
            reasons.append(('then fails, ' if row.missing else '')
                           + 'compile database missing: ' + shown(row.database))
        print(f'  {row.verdict:<5}  {row.name:<{width}}  ' + ('; '.join(reasons) or 'ready'))
    print(f'{len(idle)} of {len(rows)} receipt-gated rows do not run. CTest counts a skipped '
          'row as passed, so its "100% tests passed" leaves them out.')
    if any(row.missing for row in rows):
        print('Prepare the seven ABI providers in this build tree (once per tree), then re-run '
              'ctest:\n  python3 scripts/check_abi_receipt_skips.py --build-dir '
              f'{shlex.quote(shown_dir)} --prepare')
    if any(row.database is not None for row in rows):
        print('Re-configure the tree with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON: '
              'test_l4g_runtime_budget reads compile_commands.json once its receipt exists.')
    return 1


def prepare_argv(source: Path, build_dir: Path, role: str, jobs: int) -> list[str]:
    """The argv scripts/ci_verify.py prepares the provider `role` with."""
    provider = PROVIDERS[role]
    argv = [sys.executable, str(source / 'scripts/prepare_settlement_cpp_abi_base.py'),
            '--source-repo', str(source), '--current-build', str(build_dir),
            '--output', str(build_dir / provider['default_output']), '--jobs', str(jobs)]
    if role != 'e60':
        argv += ['--commit', provider['commit'], '--tree', provider['tree'],
                 '--header-manifest', str(source / provider['manifest'].relative_to(ROOT))]
    return argv


def prepare(source: Path, build_dir: Path, jobs: int,
            run: Callable[..., subprocess.CompletedProcess] = subprocess.run) -> bool:
    """Prepare every provider build_dir lacks, as ci_verify does; False when one could not be."""
    ready = True
    for role in PROVIDER_ROLES:
        provider = PROVIDERS[role]
        output = build_dir / provider['default_output']
        if output.exists():
            try:
                reusable_prepared_base(output, build_dir, commit=provider['commit'],
                                       tree=provider['tree'])
            except Exception as error:
                print(f'  {role}: refused {output}: {error}', flush=True)
                ready = False
            else:
                print(f'  {role}: reused {output / "receipt.json"}', flush=True)
            continue
        present = run(['git', '-C', str(source), 'cat-file', '-e',
                       provider['commit'] + '^{commit}'], capture_output=True)
        if present.returncode:
            fetch = ['git', '-C', str(source), 'fetch', '--no-tags', '--depth=1', 'origin',
                     provider['commit']]
            print('  + ' + shlex.join(fetch), flush=True)
            if run(fetch).returncode:
                ready = False
                continue
        argv = prepare_argv(source, build_dir, role, jobs)
        print('  + ' + shlex.join(argv), flush=True)
        if run(argv).returncode:
            ready = False
    return ready


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--prepare', action='store_true',
                        help='first prepare the seven ABI providers into the build tree, '
                             'as scripts/ci_verify.py does')
    parser.add_argument('--jobs', type=int, default=4, help='build jobs per provider')
    parser.add_argument('--ctest', default='ctest')
    args = parser.parse_args(argv)
    build_dir = args.build_dir.resolve()
    if not (build_dir / 'CMakeCache.txt').is_file():
        print(f'check_abi_receipt_skips: {args.build_dir} is not a configured CMake build tree',
              file=sys.stderr)
        return 2
    prepared = True
    if args.prepare:
        print(f'check_abi_receipt_skips: preparing the seven ABI providers in {args.build_dir}',
              flush=True)
        prepared = prepare(ROOT, build_dir, args.jobs)
    try:
        rows = gated_rows(registered_tests(build_dir, args.ctest))
    except (OSError, RuntimeError, ValueError) as error:
        print(f'check_abi_receipt_skips: {error}', file=sys.stderr)
        return 2
    code = report(rows, build_dir, str(args.build_dir))
    return code if prepared else 1


if __name__ == '__main__':
    raise SystemExit(main())
