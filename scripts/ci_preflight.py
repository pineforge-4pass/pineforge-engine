#!/usr/bin/env python3
"""Fast CI wiring and source checks. Does not build or verify the engine.

Run this before the complete ci_verify.py profiles. Both local use and GitHub
Actions require actionlint 1.7.12; missing tools and failed checks fail closed.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

from ci_verify import ROOT, source_guard_commands

ACTIONLINT_VERSION = '1.7.12'


def check_commands(source: Path) -> list[tuple[str, list[str]]]:
    return [
        ('shellcheck-version', ['shellcheck', '--version']),
        ('workflow-lint', ['actionlint', '-color',
                           str(source / '.github/workflows/ci.yml'),
                           str(source / '.github/workflows/native-live.yml'),
                           str(source / '.github/workflows/corpus-parity.yml')]),
        *source_guard_commands(source),
        ('source-guard-market-admission',
         [sys.executable, str(source / 'scripts/check_market_admission_schema.py')]),
        ('source-guard-cancellation',
         [sys.executable, str(source / 'scripts/check_cancellation_hash_coverage.py')]),
        ('verifier-tests', [sys.executable, str(source / 'scripts/test_ci_verify.py')]),
        ('preflight-tests', [sys.executable, str(source / 'scripts/test_ci_preflight.py')]),
        ('native-c-surface-tests',
         [sys.executable, str(source / 'scripts/test_check_native_c_api_surface.py')]),
        ('kernel-residuals-tests',
         [sys.executable, str(source / 'scripts/test_check_kernel_residuals.py')]),
        ('corpus-parity-identity-tests',
         [sys.executable, str(source / 'scripts/test_corpus_trades_identity.py')]),
    ]


def run_checks(commands: list[tuple[str, list[str]]], output: Path, *, source: Path = ROOT) -> int:
    output.mkdir(parents=True, exist_ok=True)
    summary = {'schemaVersion': 'pineforge-ci-preflight/v1', 'status': 'incomplete',
               'scope': 'workflow and source checks only; full verification still required',
               'stages': []}
    summary_path = output / 'preflight-summary.json'

    def record() -> None:
        summary_path.write_text(json.dumps(summary, indent=2) + '\n')

    record()
    for name, argv in commands:
        print(f'ci_preflight: {name}: {shlex.join(argv)}', flush=True)
        stage = {'name': name, 'argv': argv, 'status': 'running', 'exitCode': None,
                 'log': name + '.log'}
        summary['stages'].append(stage)
        record()
        try:
            # 900 s: the verifier self-tests (test_ci_verify.py) now drive the real
            # literal-aware parity, receipt and submodule guards (L8d) and take
            # ~80 s locally, >180 s on the hosted runner.
            result = subprocess.run(argv, cwd=source, capture_output=True, timeout=900)
            code, log = result.returncode, result.stdout + result.stderr
        except subprocess.TimeoutExpired as error:
            code = 124
            log = (error.stdout or b'') + (error.stderr or b'') + b'\ncheck timed out\n'
        except OSError as error:
            code, log = 127, str(error).encode()
        (output / stage['log']).write_bytes(log)
        if log:
            print(log.decode('utf-8', 'replace'), end='', flush=True)
        stage.update(status='passed' if code == 0 else 'failed', exitCode=code)
        record()
    passed = bool(commands) and all(stage['exitCode'] == 0 for stage in summary['stages'])
    summary.update(status='passed' if passed else 'failed', exitCode=0 if passed else 1)
    record()
    return summary['exitCode']


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build-ci-preflight')
    args = parser.parse_args()
    # Pin the linter contract as well as its CI download. No optional lint lane.
    version_check = [sys.executable, '-c',
                     'import subprocess,sys; '
                     'v=subprocess.check_output(["actionlint","-version"],text=True).splitlines()[0]; '
                     f'print("actionlint "+v); sys.exit(0 if v == {ACTIONLINT_VERSION!r} else 1)']
    return run_checks([('actionlint-version', version_check), *check_commands(ROOT)],
                      args.output_dir.resolve())


if __name__ == '__main__':
    raise SystemExit(main())
