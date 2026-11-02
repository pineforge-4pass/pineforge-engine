#!/usr/bin/env python3
"""Count real ci_verify provider-fetch failures in concurrent cold checkouts.

Each repetition shallow-clones HEAD, verifies the pin is absent, then releases
N workers together after their first real cat-file probe. All commands go
through ci_verify.default_runner; no Git result or lock file is fabricated.
The sparse checkout includes the real preparer, whose absent-manifest skip
avoids compiling providers: this measures fetching, not a full native profile.
There are no process, barrier, or lock deadlines.
"""
from __future__ import annotations

import argparse
import json
import multiprocessing
import os
from pathlib import Path
import subprocess
import sys
import traceback

import ci_verify


def git(*args: str) -> str:
    return subprocess.check_output(['git', *map(str, args)], stderr=subprocess.STDOUT).decode().strip()


def worker(source: Path, build: Path, index: int, count: int, barrier,
           provider: dict | None) -> None:
    first_probe = True

    def runner(argv, **kwargs):
        nonlocal first_probe
        result = ci_verify.default_runner(argv, **kwargs)
        if first_probe and argv[0] == 'git' and 'cat-file' in argv:
            first_probe = False
            # Synchronize the actual initial missing-object observations, not
            # synthetic exit codes. A locked re-check must not join this barrier.
            barrier.wait()
        return result

    try:
        cfg = ci_verify.VerifyConfig(
            profile=ci_verify.PROFILE['native'], source=source, build_dir=build,
            jobs=1, generator=None, curl_dir=None, ccache_path=None,
            require_websocket=False, runner=runner, stream_output=False,
            ctest_shard=ci_verify.CTestShard(index, count))
        driver = ci_verify.Driver(cfg)
        if provider is None:
            driver.ensure_abi_prior()
        else:
            driver.abi_prior_action = driver.ensure_prepared_provider(
                build / 'provider', provider['commit'], provider['tree'],
                extra_argv=['--commit', provider['commit']],
                stage='abi-prior', fetch_stage='abi-prior-fetch')
        driver.write_summary()
        raise SystemExit(int(bool(driver.failures)))
    except Exception:
        traceback.print_exc()
        raise


def run_repetitions(checkout: Path, origin: Path, work_dir: Path, *,
                    reps: int, processes: int, missing_origin: bool = False,
                    provider: dict | None = None) -> dict:
    """Keep every worker's real stage log and compute counts from its summary."""
    if os.environ.get('PINEFORGE_PRIVATE_FIXTURES'):
        raise RuntimeError('run this fetch-only fixture without PINEFORGE_PRIVATE_FIXTURES')
    work_dir.mkdir(parents=True, exist_ok=False)
    pin = provider or ci_verify.PROVIDERS['0e']
    context = multiprocessing.get_context('spawn')
    repetitions = []
    for repetition in range(1, reps + 1):
        directory = work_dir / f'rep-{repetition:03d}'
        directory.mkdir()
        source = directory / 'source'
        git('clone', '--no-checkout', '--no-tags', '--single-branch', '--depth=1',
            checkout.resolve().as_uri(), source)
        git('-C', source, 'sparse-checkout', 'set', '--no-cone',
            '/VERSION', '/scripts/prepare_settlement_cpp_abi_base.py')
        git('-C', source, 'checkout', '--force', 'HEAD')
        remote = origin
        if missing_origin:
            remote = directory / 'empty-origin.git'
            git('init', '--bare', remote)
        git('-C', source, 'remote', 'set-url', 'origin', remote.resolve())
        if ci_verify.pinned_object_present(source, ci_verify.default_runner, pin['commit']):
            raise RuntimeError(f'{source}: pin must be absent before each repetition')
        barrier = context.Barrier(processes)
        children = [context.Process(
            target=worker, args=(source, directory / f'worker-{index}', index,
                                 processes, barrier, provider))
                    for index in range(1, processes + 1)]
        for child in children:
            child.start()
        for child in children:
            child.join()
        rows = []
        for index, child in enumerate(children, 1):
            summary_path = directory / f'worker-{index}' / 'ci-summary.json'
            summary = json.loads(summary_path.read_text())
            failed = [stage['name'] for stage in summary['stages']
                      if stage['status'] == 'failed']
            fetches = [stage for stage in summary['stages']
                       if stage['name'] == 'abi-prior-fetch']
            row = {'process': index, 'exit': child.exitcode,
                   'failedStages': failed, 'fetches': len(fetches),
                   'fetchExitCodes': [stage['exitCode'] for stage in fetches],
                   'summary': str(summary_path)}
            rows.append(row)
            print(f'REP={repetition} PROC={index} EXIT={child.exitcode} '
                  f'FAILED_STAGE={",".join(failed) or "none"} '
                  f'FETCHES={len(fetches)} '
                  f'FETCH_EXIT={",".join(map(str, row["fetchExitCodes"])) or "none"}',
                  flush=True)
            if (set(failed) - {'abi-prior-fetch'}
                    or child.exitcode != int(bool(failed))):
                raise RuntimeError(f'worker failed outside the measured fetch stage: {row}')
            if not failed and not ci_verify.pinned_object_present(
                    source, ci_verify.default_runner, pin['commit']):
                raise RuntimeError('successful worker did not materialize the pin')
        repetitions.append(rows)
    failed_procs = sum('abi-prior-fetch' in row['failedStages']
                       for rows in repetitions for row in rows)
    failed_reps = sum(any('abi-prior-fetch' in row['failedStages'] for row in rows)
                      for rows in repetitions)
    result = {'reps': reps, 'processes': processes, 'failedReps': failed_reps,
              'failedProcs': failed_procs, 'repetitions': repetitions}
    (work_dir / 'counts.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'REPS={reps} N={processes} FAILED_REPS={failed_reps} FAILED_PROCS={failed_procs}',
          flush=True)
    return result


def positive(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError('must be positive')
    return number


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', type=Path, default=ci_verify.ROOT)
    parser.add_argument('--origin', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True, help='must not exist yet')
    parser.add_argument('--reps', type=positive, default=30)
    parser.add_argument('--processes', type=positive, default=4)
    parser.add_argument('--missing-origin', action='store_true',
                        help='negative control: each origin is an empty bare repository')
    args = parser.parse_args()
    result = run_repetitions(
        args.checkout, args.origin, args.work_dir, reps=args.reps,
        processes=args.processes, missing_origin=args.missing_origin)
    return int(result['failedProcs'] != 0)


if __name__ == '__main__':
    sys.exit(main())
