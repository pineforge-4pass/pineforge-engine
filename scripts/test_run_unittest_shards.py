#!/usr/bin/env python3
"""Unit tests for scripts/run_unittest_shards.py."""
from __future__ import annotations

import io
import os
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

import ci_preflight  # noqa: E402
import run_unittest_shards as shards_runner  # noqa: E402

SCRIPT = ROOT / 'scripts' / 'run_unittest_shards.py'

FAKE_MODULE_SOURCE = textwrap.dedent("""
    import os
    import signal
    import tempfile
    import unittest

    MODULE_SETUP_RAN = False


    def setUpModule():
        global MODULE_SETUP_RAN
        MODULE_SETUP_RAN = True


    class AlphaTests(unittest.TestCase):
        def test_one(self):
            self.assertTrue(MODULE_SETUP_RAN)
            print(f"TMPDIR_SEEN={tempfile.gettempdir()}")

        def test_two(self):
            self.assertTrue(MODULE_SETUP_RAN)
            print(f"TMPDIR_SEEN={tempfile.gettempdir()}")

        def test_three(self):
            self.assertTrue(MODULE_SETUP_RAN)
            if os.environ.get("FAKE_FAIL_TARGET") == "test_three":
                self.fail("deliberate failure in test_three")
            if os.environ.get("FAKE_SIGNAL_TARGET") == "test_three":
                os.kill(os.getpid(), signal.SIGTERM)


    class BetaTests(unittest.TestCase):
        def test_four(self):
            self.assertTrue(MODULE_SETUP_RAN)
            if os.environ.get("FAKE_FAIL_TARGET") == "test_four":
                self.fail("deliberate failure in test_four")

        def test_five(self):
            self.assertTrue(MODULE_SETUP_RAN)
            print(f"TMPDIR_SEEN={tempfile.gettempdir()}")
""")


class UnittestShardsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.module_path = self.root / 'fake_suite.py'
        self.module_path.write_text(FAKE_MODULE_SOURCE, encoding='utf-8')

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def run_cli(self, *args: str, extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            [sys.executable, '-B', str(SCRIPT), *args],
            capture_output=True,
            text=True,
            env=env,
        )

    def test_default_shard_count_caps_at_16_and_floors_at_1(self) -> None:
        self.assertEqual(shards_runner.default_shard_count(32), 16)
        self.assertEqual(shards_runner.default_shard_count(16), 16)
        self.assertEqual(shards_runner.default_shard_count(8), 8)
        self.assertEqual(shards_runner.default_shard_count(1), 1)
        self.assertEqual(shards_runner.default_shard_count(0), 1)
        self.assertGreaterEqual(shards_runner.default_shard_count(), 1)
        self.assertLessEqual(shards_runner.default_shard_count(), 16)

    def test_shards_union_equals_full_id_list_with_no_duplicates(self) -> None:
        full_ids = shards_runner.discover_test_ids(self.module_path)
        self.assertEqual(len(full_ids), 5)
        self.assertEqual(full_ids, sorted(full_ids))
        for count in (1, 2, 7, len(full_ids) + 4):
            with self.subTest(shards=count):
                shards = shards_runner.split_into_shards(full_ids, count)
                self.assertEqual(len(shards), count)
                flattened = [test_id for shard in shards for test_id in shard]
                self.assertEqual(len(flattened), len(full_ids))
                self.assertEqual(len(set(flattened)), len(full_ids))
                self.assertEqual(set(flattened), set(full_ids))
                shards_runner.validate_shard_partition(full_ids, shards)

                result = self.run_cli(str(self.module_path), '--shards', str(count))
                output = result.stdout + result.stderr
                self.assertEqual(result.returncode, 0, output)
                self.assertIn('5 tests', output)
                for index in range(count):
                    self.assertIn(f'shard {index}/{count}:', output)

    def test_lost_or_duplicated_test_ids_print_and_fail(self) -> None:
        full_ids = shards_runner.discover_test_ids(self.module_path)
        lost_shards = shards_runner.split_into_shards(full_ids, 2)
        removed_id = lost_shards[0].pop()
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = shards_runner.run_shards(self.module_path, shard_count=2, shards_override=lost_shards)
        self.assertNotEqual(code, 0)
        self.assertIn(removed_id, out.getvalue())
        self.assertIn('lost', out.getvalue().lower())

        dup_shards = shards_runner.split_into_shards(full_ids, 2)
        duplicated_id = dup_shards[0][0]
        dup_shards[1].append(duplicated_id)
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = shards_runner.run_shards(self.module_path, shard_count=2, shards_override=dup_shards)
        self.assertNotEqual(code, 0)
        self.assertIn(duplicated_id, out.getvalue())
        self.assertIn('duplicate', out.getvalue().lower())

    def test_failing_test_in_any_shard_exits_nonzero_and_names_the_shard(self) -> None:
        full_ids = shards_runner.discover_test_ids(self.module_path)
        shards = shards_runner.split_into_shards(full_ids, 2)
        for target_method in ('test_three', 'test_four'):
            target_id = next(test_id for test_id in full_ids if test_id.endswith('.' + target_method))
            expected_shard = next(index for index, shard in enumerate(shards) if target_id in shard)
            with self.subTest(target=target_method, expected_shard=expected_shard):
                result = self.run_cli(
                    str(self.module_path),
                    '--shards',
                    '2',
                    extra_env={'FAKE_FAIL_TARGET': target_method},
                )
                output = result.stdout + result.stderr
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(f'shard {expected_shard}/2: FAILED', output)
                self.assertIn(f'deliberate failure in {target_method}', output)
                self.assertIn('5 tests', output)

    def test_shard_killed_by_signal_fails_the_run_and_names_the_shard(self) -> None:
        full_ids = shards_runner.discover_test_ids(self.module_path)
        shards = shards_runner.split_into_shards(full_ids, 2)
        target_id = next(test_id for test_id in full_ids if test_id.endswith('.test_three'))
        expected_shard = next(index for index, shard in enumerate(shards) if target_id in shard)
        result = self.run_cli(
            str(self.module_path),
            '--shards',
            '2',
            extra_env={'FAKE_SIGNAL_TARGET': 'test_three'},
        )
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(f'shard {expected_shard}/2: FAILED', output)
        self.assertIn('signal', output.lower())
        self.assertIn('5 tests', output)

    def test_per_shard_tmpdir_is_isolated_and_outside_repo(self) -> None:
        in_repo_tmp = ROOT / 'build-ci-tmp-probe'
        result = self.run_cli(
            str(self.module_path),
            '--shards',
            '2',
            extra_env={'TMPDIR': str(in_repo_tmp)},
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        seen_tmpdirs = set(re.findall(r'TMPDIR_SEEN=(.+)', output))
        self.assertEqual(len(seen_tmpdirs), 2, output)
        repo_resolved = ROOT.resolve()
        for raw in seen_tmpdirs:
            resolved = Path(raw.strip()).resolve()
            self.assertNotEqual(resolved, repo_resolved)
            self.assertNotIn(repo_resolved, resolved.parents)

    def test_ci_preflight_wires_verifier_tests_and_registers_this_test_file(self) -> None:
        commands = {entry[0]: entry[1] for entry in ci_preflight.check_commands(ROOT)}
        self.assertIn('verifier-tests', commands)
        verifier_argv = commands['verifier-tests']
        self.assertEqual(
            verifier_argv,
            [
                sys.executable,
                str(ROOT / 'scripts/run_unittest_shards.py'),
                str(ROOT / 'scripts/test_ci_verify.py'),
            ],
        )
        registered_scripts = {Path(argv[-1]).name for argv in commands.values() if argv}
        self.assertIn('test_run_unittest_shards.py', registered_scripts)


if __name__ == '__main__':
    unittest.main()
