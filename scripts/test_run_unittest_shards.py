#!/usr/bin/env python3
"""Unit tests for scripts/run_unittest_shards.py."""
from __future__ import annotations

import io
import os
import re
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

import ci_preflight  # noqa: E402
import run_unittest_shards as shards_runner  # noqa: E402

SCRIPT = ROOT / 'scripts' / 'run_unittest_shards.py'

FAKE_MODULE_SOURCE = textwrap.dedent("""
    import os
    import signal
    import sys
    import tempfile
    import time
    import unittest

    MODULE_SETUP_RAN = False


    def setUpModule():
        global MODULE_SETUP_RAN
        MODULE_SETUP_RAN = True


    class AlphaTests(unittest.TestCase):
        def test_one(self):
            self.assertTrue(MODULE_SETUP_RAN)
            print("EARLY_OUTPUT_TEST_ONE", flush=True)
            print(f"TMPDIR_SEEN={tempfile.gettempdir()}", flush=True)
            if os.environ.get("FAKE_EARLY_EXIT_TARGET") == "test_one":
                os._exit(0)

        def test_two(self):
            self.assertTrue(MODULE_SETUP_RAN)
            print(f"TMPDIR_SEEN={tempfile.gettempdir()}", flush=True)
            sleep_sec = float(os.environ.get("FAKE_SLEEP_SECONDS", "0"))
            if sleep_sec > 0:
                print(f"SLEEPING_PID={os.getpid()}", flush=True)
                time.sleep(sleep_sec)

        def test_three(self):
            self.assertTrue(MODULE_SETUP_RAN)
            if os.environ.get("FAKE_FAIL_TARGET") == "test_three":
                self.fail("deliberate failure in test_three")
            if os.environ.get("FAKE_SIGNAL_TARGET") == "test_three":
                os.kill(os.getpid(), signal.SIGTERM)
            if os.environ.get("FAKE_EARLY_EXIT_TARGET") == "test_three":
                os._exit(0)


    class BetaTests(unittest.TestCase):
        def test_four(self):
            self.assertTrue(MODULE_SETUP_RAN)
            if os.environ.get("FAKE_FAIL_TARGET") == "test_four":
                self.fail("deliberate failure in test_four")

        def test_five(self):
            self.assertTrue(MODULE_SETUP_RAN)
            print(f"TMPDIR_SEEN={tempfile.gettempdir()}", flush=True)
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

    def test_default_shard_count_uses_sched_getaffinity_else_cpu_count_and_caps_at_16(self) -> None:
        self.assertEqual(shards_runner.default_shard_count(32), 16)
        self.assertEqual(shards_runner.default_shard_count(16), 16)
        self.assertEqual(shards_runner.default_shard_count(8), 8)
        self.assertEqual(shards_runner.default_shard_count(1), 1)
        self.assertEqual(shards_runner.default_shard_count(0), 1)
        self.assertGreaterEqual(shards_runner.default_shard_count(), 1)
        self.assertLessEqual(shards_runner.default_shard_count(), 16)

        with mock.patch.object(
            shards_runner.os,
            'sched_getaffinity',
            create=True,
            return_value={0, 1, 2, 3, 4, 5},
        ) as affinity_mock, mock.patch.object(shards_runner.os, 'cpu_count', return_value=32):
            self.assertEqual(shards_runner.default_shard_count(), 6)
            affinity_mock.assert_called_once_with(0)

        with mock.patch.object(
            shards_runner.os,
            'sched_getaffinity',
            create=True,
            return_value=set(range(24)),
        ), mock.patch.object(shards_runner.os, 'cpu_count', return_value=4):
            self.assertEqual(shards_runner.default_shard_count(), 16)

        with mock.patch.object(
            shards_runner.os,
            'sched_getaffinity',
            create=True,
            side_effect=AttributeError('no sched_getaffinity'),
        ), mock.patch.object(shards_runner.os, 'cpu_count', return_value=12):
            self.assertEqual(shards_runner.default_shard_count(), 12)

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
                self.assertNotIn(f'shard 0/{count}', output)
                for shard_num in range(1, count + 1):
                    self.assertIn(f'shard {shard_num}/{count}:', output)

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
            expected_shard = next(
                index + 1 for index, shard in enumerate(shards) if target_id in shard
            )
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
        expected_shard = next(
            index + 1 for index, shard in enumerate(shards) if target_id in shard
        )
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

    def test_shard_exiting_zero_with_fewer_tests_ran_fails_and_names_the_shard(self) -> None:
        full_ids = shards_runner.discover_test_ids(self.module_path)
        shards = shards_runner.split_into_shards(full_ids, 2)
        target_id = next(test_id for test_id in full_ids if test_id.endswith('.test_one'))
        expected_shard = next(
            index + 1 for index, shard in enumerate(shards) if target_id in shard
        )
        self.assertGreater(len(shards[expected_shard - 1]), 1)
        result = self.run_cli(
            str(self.module_path),
            '--shards',
            '2',
            extra_env={'FAKE_EARLY_EXIT_TARGET': 'test_one'},
        )
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn(f'shard {expected_shard}/2: FAILED', output)
        self.assertIn(f'failed: shard {expected_shard}', output)

    def test_live_shard_log_file_holds_earlier_output_while_shard_runs_and_progress_lines_flush(self) -> None:
        log_dir = self.root / 'shard-logs'
        env = os.environ.copy()
        env['FAKE_SLEEP_SECONDS'] = '1.5'
        proc = subprocess.Popen(
            [
                sys.executable,
                '-B',
                str(SCRIPT),
                str(self.module_path),
                '--shards',
                '1',
                '--log-dir',
                str(log_dir),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )
        try:
            assert proc.stdout is not None
            first_line = proc.stdout.readline()
            self.assertIn('shard 1/1: starting', first_line)
            self.assertIsNone(proc.poll(), 'runner should still be running after start line')

            deadline = time.monotonic() + 5.0
            shard_log: Path | None = None
            seen_early = False
            while time.monotonic() < deadline:
                if log_dir.is_dir():
                    candidates = sorted(log_dir.glob('*.log'))
                    if candidates:
                        shard_log = candidates[0]
                        content = shard_log.read_text(encoding='utf-8', errors='replace')
                        if (
                            'EARLY_OUTPUT_TEST_ONE' in content
                            and 'SLEEPING_PID=' in content
                            and 'test_two (fake_suite.AlphaTests' in content
                        ):
                            seen_early = True
                            break
                time.sleep(0.05)

            self.assertTrue(
                seen_early,
                f'shard log did not flush early output / verbose test name while still running; '
                f'log_dir={list(log_dir.glob("*")) if log_dir.is_dir() else []}',
            )
            assert shard_log is not None
            running_log = shard_log.read_text(encoding='utf-8', errors='replace')
            full_ids = shards_runner.discover_test_ids(self.module_path)
            for test_id in full_ids:
                self.assertIn(test_id, running_log.split('EARLY_OUTPUT_TEST_ONE')[0])
            self.assertIsNone(proc.poll(), 'runner should still be running while test_two sleeps')
            rest_output, _ = proc.communicate(timeout=10.0)
            output = first_line + (rest_output or '')
        finally:
            if proc.poll() is None:
                try:
                    proc.kill()
                except PermissionError:
                    pass
                try:
                    proc.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    pass

        self.assertEqual(proc.returncode, 0, output)
        self.assertIn('shard 1/1: starting', output)
        self.assertIn('EARLY_OUTPUT_TEST_ONE', output)
        self.assertIn('shard 1/1: ok (5 tests,', output)
        assert shard_log is not None
        final_log = shard_log.read_text(encoding='utf-8', errors='replace')
        self.assertIn('EARLY_OUTPUT_TEST_ONE', final_log)
        self.assertIn('Ran 5 tests', final_log)
        self.assertIn('test_one (fake_suite.AlphaTests', final_log)

    def test_sigterm_terminates_live_shard_processes_and_exits_nonzero(self) -> None:
        import fcntl

        sleeping_module = self.root / 'sleeping_suite.py'
        for sig, expected_rc in ((signal.SIGTERM, 143), (signal.SIGINT, 130)):
            with self.subTest(signal=sig.name):
                lock_dir = self.root / f'locks-{sig.name.lower()}'
                lock_dir.mkdir(parents=True, exist_ok=True)
                sleeping_module.write_text(
                    textwrap.dedent(f"""
                        import fcntl
                        import os
                        import time
                        import unittest
                        from pathlib import Path

                        LOCK_DIR = Path({str(lock_dir)!r})
                        _HELD_LOCKS = []

                        def _hold_lock(name: str) -> None:
                            lock_file = (LOCK_DIR / f"{{name}}.lock").open("w", encoding="utf-8")
                            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                            lock_file.write(f"{{os.getpid()}}\\n")
                            lock_file.flush()
                            _HELD_LOCKS.append(lock_file)

                        class SleepTests(unittest.TestCase):
                            def test_sleep_a(self):
                                _hold_lock("a")
                                time.sleep(30.0)

                            def test_sleep_b(self):
                                _hold_lock("b")
                                time.sleep(30.0)
                    """),
                    encoding='utf-8',
                )

                def _is_locked(lock_path: Path) -> bool:
                    if not lock_path.is_file():
                        return False
                    with lock_path.open('r+', encoding='utf-8') as probe:
                        try:
                            fcntl.flock(probe.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except BlockingIOError:
                            return True
                        fcntl.flock(probe.fileno(), fcntl.LOCK_UN)
                        return False

                driver_code = textwrap.dedent(f"""
                    import fcntl
                    import os
                    import signal
                    import sys
                    import threading
                    import time
                    from pathlib import Path

                    sys.path.insert(0, {str(ROOT / 'scripts')!r})
                    import run_unittest_shards

                    lock_dir = Path({str(lock_dir)!r})

                    def _locked(p: Path) -> bool:
                        if not p.is_file():
                            return False
                        with p.open("r+", encoding="utf-8") as f:
                            try:
                                fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                            except BlockingIOError:
                                return True
                            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
                            return False

                    def _trigger_signal() -> None:
                        deadline = time.monotonic() + 5.0
                        while time.monotonic() < deadline:
                            if _locked(lock_dir / "a.lock") and _locked(lock_dir / "b.lock"):
                                (lock_dir / "sig_sent.txt").write_text(
                                    f"{{time.monotonic():.6f}}\\n", encoding="utf-8"
                                )
                                os.kill(os.getpid(), {int(sig)})
                                return
                            time.sleep(0.02)

                    threading.Thread(target=_trigger_signal, daemon=True).start()
                    raise SystemExit(run_unittest_shards.main([{str(sleeping_module)!r}, "--shards", "2"]))
                """)
                proc = subprocess.Popen(
                    [sys.executable, '-B', '-c', driver_code],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                try:
                    try:
                        output, _ = proc.communicate(timeout=5.0)
                    except subprocess.TimeoutExpired:
                        self.fail(f'runner did not exit within 5s after {sig.name}')
                    exited_at = time.monotonic()
                finally:
                    if proc.stdout is not None:
                        proc.stdout.close()
                    if proc.poll() is None:
                        try:
                            proc.kill()
                        except OSError:
                            pass
                        try:
                            proc.wait(timeout=5.0)
                        except subprocess.TimeoutExpired:
                            pass

                sig_sent_file = lock_dir / 'sig_sent.txt'
                self.assertTrue(
                    sig_sent_file.is_file(),
                    f'both shards did not acquire locks before exit; output={output!r}',
                )
                sig_sent_at = float(sig_sent_file.read_text(encoding='utf-8').strip())
                elapsed = exited_at - sig_sent_at
                self.assertEqual(proc.returncode, expected_rc, output)
                self.assertLess(elapsed, 5.0, f'{sig.name} took {elapsed:.2f}s to exit (expected < 5s)')

                reap_deadline = time.monotonic() + 0.3
                alive_locks = ['a', 'b']
                while time.monotonic() < reap_deadline and alive_locks:
                    alive_locks = [
                        name for name in ('a', 'b') if _is_locked(lock_dir / f'{name}.lock')
                    ]
                    if alive_locks:
                        time.sleep(0.02)
                self.assertEqual(
                    alive_locks,
                    [],
                    f'shard processes for {alive_locks} remained alive after runner exited on {sig.name}',
                )

    def test_sig_ign_for_sigint_or_sigterm_is_preserved(self) -> None:
        observed_handlers: list[ tuple[object, object] ] = []
        original_popen = shards_runner.subprocess.Popen

        def _spy_popen(*args: object, **kwargs: object) -> subprocess.Popen[bytes]:
            observed_handlers.append(
                (signal.getsignal(signal.SIGINT), signal.getsignal(signal.SIGTERM))
            )
            return original_popen(*args, **kwargs)  # type: ignore[arg-type]

        prev_int = signal.getsignal(signal.SIGINT)
        prev_term = signal.getsignal(signal.SIGTERM)
        try:
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGTERM, signal.SIG_DFL)
            observed_handlers.clear()
            out = io.StringIO()
            with mock.patch.object(shards_runner.subprocess, 'Popen', side_effect=_spy_popen), \
                    redirect_stdout(out), redirect_stderr(out):
                rc = shards_runner.run_shards(self.module_path, shard_count=1)
            self.assertEqual(rc, 0)
            self.assertTrue(observed_handlers)
            self.assertEqual(observed_handlers[0][0], signal.SIG_IGN)
            self.assertNotEqual(observed_handlers[0][1], signal.SIG_DFL)
            self.assertEqual(signal.getsignal(signal.SIGINT), signal.SIG_IGN)

            signal.signal(signal.SIGINT, signal.SIG_DFL)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            observed_handlers.clear()
            out = io.StringIO()
            with mock.patch.object(shards_runner.subprocess, 'Popen', side_effect=_spy_popen), \
                    redirect_stdout(out), redirect_stderr(out):
                rc = shards_runner.run_shards(self.module_path, shard_count=1)
            self.assertEqual(rc, 0)
            self.assertTrue(observed_handlers)
            self.assertNotEqual(observed_handlers[0][0], signal.SIG_DFL)
            self.assertEqual(observed_handlers[0][1], signal.SIG_IGN)
            self.assertEqual(signal.getsignal(signal.SIGTERM), signal.SIG_IGN)
        finally:
            signal.signal(signal.SIGINT, prev_int)
            signal.signal(signal.SIGTERM, prev_term)

    def test_stale_shard_logs_in_log_dir_are_removed_at_start(self) -> None:
        log_dir = self.root / 'reused-logs'
        log_dir.mkdir(parents=True, exist_ok=True)
        for name in ('shard-01.log', 'shard-02.log', 'shard-03.log', 'shard-04.log', 'shard-99.log'):
            (log_dir / name).write_text('STALE_LOG_MARKER\n', encoding='utf-8')
        keep_file = log_dir / 'other-notes.txt'
        keep_file.write_text('keep this\n', encoding='utf-8')
        keep_log = log_dir / 'summary.log'
        keep_log.write_text('keep summary\n', encoding='utf-8')

        result = self.run_cli(str(self.module_path), '--shards', '2', '--log-dir', str(log_dir))
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertEqual(
            sorted(p.name for p in log_dir.iterdir()),
            ['other-notes.txt', 'shard-01.log', 'shard-02.log', 'summary.log'],
        )
        for active_name in ('shard-01.log', 'shard-02.log'):
            content = (log_dir / active_name).read_text(encoding='utf-8')
            self.assertNotIn('STALE_LOG_MARKER', content)
        self.assertEqual(keep_file.read_text(encoding='utf-8'), 'keep this\n')
        self.assertEqual(keep_log.read_text(encoding='utf-8'), 'keep summary\n')

    def test_docstring_notes_module_level_skiptest_ran_zero_fails_shard(self) -> None:
        combined_docs = '\n'.join(
            filter(
                None,
                [
                    shards_runner.__doc__,
                    shards_runner.run_shards.__doc__,
                ],
            )
        )
        self.assertIn('SkipTest', combined_docs)
        self.assertIn('Ran 0', combined_docs)

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

    def test_ci_preflight_wires_verifier_tests_with_log_dir_and_registers_this_test_file(self) -> None:
        out_dir = self.root / 'custom-preflight-out'
        commands = {entry[0]: entry[1] for entry in ci_preflight.check_commands(ROOT, out_dir)}
        self.assertIn('verifier-tests', commands)
        verifier_argv = commands['verifier-tests']
        self.assertEqual(
            verifier_argv,
            [
                sys.executable,
                str(ROOT / 'scripts/run_unittest_shards.py'),
                str(ROOT / 'scripts/test_ci_verify.py'),
                '--log-dir',
                str(out_dir / 'verifier-tests-shards'),
            ],
        )
        default_commands = {entry[0]: entry[1] for entry in ci_preflight.check_commands(ROOT)}
        self.assertEqual(
            default_commands['verifier-tests'],
            [
                sys.executable,
                str(ROOT / 'scripts/run_unittest_shards.py'),
                str(ROOT / 'scripts/test_ci_verify.py'),
                '--log-dir',
                str(ROOT / 'build-ci-preflight' / 'verifier-tests-shards'),
            ],
        )
        registered_scripts = {Path(argv[-1]).name for argv in commands.values() if argv}
        self.assertIn('test_run_unittest_shards.py', registered_scripts)


if __name__ == '__main__':
    unittest.main()
