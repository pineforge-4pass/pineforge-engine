#!/usr/bin/env python3
"""Real Git regression for provider check/fetch serialization (K=3, N=4).

Only the provider's post-fetch compiler work is replaced by a tiny fixture
preparer: it checks that the requested commit really exists. The production
Driver, object probes, fetch argv, stage recording and process barriers run
unchanged. No network, fabricated shallow.lock, or deadline is involved.
"""
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from repro_ci_verify_fetch_race import git, run_repetitions


class ProviderFetchLock(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ci-verify-fetch-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.checkout = self.root / 'checkout'
        git('init', self.checkout)
        git('-C', self.checkout, 'config', 'user.name', 'Fetch regression')
        git('-C', self.checkout, 'config', 'user.email', 'fetch-test@example.invalid')
        (self.checkout / 'VERSION').write_text('0.0.0\n')
        scripts = self.checkout / 'scripts'
        scripts.mkdir()
        (scripts / 'prepare_settlement_cpp_abi_base.py').write_text(
            'import argparse, subprocess\n'
            'p = argparse.ArgumentParser()\n'
            'p.add_argument("--source-repo")\n'
            'p.add_argument("--commit")\n'
            'a, _ = p.parse_known_args()\n'
            'raise SystemExit(subprocess.call(["git", "-C", a.source_repo,\n'
            '    "cat-file", "-e", a.commit + "^{commit}"]))\n')
        git('-C', self.checkout, 'add', '.')
        git('-C', self.checkout, 'commit', '-m', 'provider fixture')
        self.provider = {
            'commit': git('-C', self.checkout, 'rev-parse', 'HEAD'),
            'tree': git('-C', self.checkout, 'rev-parse', 'HEAD^{tree}')}
        git('-C', self.checkout, 'commit', '--allow-empty', '-m', 'cold clone head')
        self.origin = self.root / 'origin.git'
        git('clone', '--bare', self.checkout, self.origin)

    def run_fixture(self, name, *, reps, processes, missing_origin=False):
        return run_repetitions(
            self.checkout, self.origin, self.root / name, reps=reps,
            processes=processes, missing_origin=missing_origin, provider=self.provider)

    def test_concurrent_cold_fetch_runs_once_per_checkout(self):
        result = self.run_fixture('concurrent', reps=3, processes=4)
        self.assertEqual(result['failedReps'], 0, result)
        self.assertEqual(result['failedProcs'], 0, result)
        for rows in result['repetitions']:
            # Also catches a missing locked re-check even on a lucky Git run.
            self.assertEqual(sum(row['fetches'] for row in rows), 1, rows)

    def test_real_fetch_failure_is_recorded_for_every_worker(self):
        result = self.run_fixture('missing', reps=1, processes=4, missing_origin=True)
        self.assertEqual(result['failedReps'], result['reps'])
        self.assertEqual(result['failedProcs'], result['processes'])
        for row in result['repetitions'][0]:
            self.assertEqual(row['failedStages'], ['abi-prior-fetch'])
            self.assertEqual(row['fetches'], 1)  # No retry, even on real failure.
            self.assertEqual(row['fetchExitCodes'], [128])

    def test_single_process_fetch_is_unchanged(self):
        result = self.run_fixture('single', reps=1, processes=1)
        self.assertEqual(result['failedProcs'], 0, result)
        self.assertEqual(result['repetitions'][0][0]['fetches'], 1)


if __name__ == '__main__':
    unittest.main()
