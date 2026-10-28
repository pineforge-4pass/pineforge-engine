#!/usr/bin/env python3
"""Run a unittest module across parallel subprocess shards.

Note: Each shard must execute all test IDs assigned to it (`Ran N` must equal
the assigned test count). Consequently, a module-level `SkipTest` (such as in
`setUpModule`, which causes `unittest` to report `Ran 0 tests` with exit status
0) counts as a failed shard, unlike serial `unittest`.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from typing import Iterable, Sequence, TextIO
import unittest

sys.dont_write_bytecode = True

from ci_verifier_classes import select_test_ids

ROOT = Path(__file__).resolve().parents[1]

_RAN_RE = re.compile(r'^Ran (\d+) tests? in ', re.MULTILINE)

# Relative scheduling estimates from the large mutation loops, not measured
# seconds. Keep whole methods: unittest's executed-count guard still applies.
# All other IDs have weight 1; modules without these rows retain round robin.
PREFLIGHT_TEST_WEIGHTS = {
    'test_ci_preflight.PreflightFailures.'
    'test_gcp_sites_reject_retired_and_void_labels': 120,
    'test_ci_preflight.PreflightFailures.'
    'test_light_jobs_refuse_other_routes_unlisted_images_arrays_and_fallbacks': 100,
    'test_ci_preflight.PreflightFailures.'
    'test_no_stage_runs_under_a_timeout_and_jobs_keep_the_platform_maximum': 90,
    'test_ci_preflight.PreflightFailures.'
    'test_pool_jobs_reject_hosted_runners_and_fallbacks_from_mutated_files': 60,
    'test_ci_preflight.PreflightFailures.'
    'test_ci_contract_pins_spot_runners_time_limits_and_parallelism': 50,
    'test_ci_preflight.PreflightFailures.'
    'test_ci_contract_pins_the_docs_only_skip': 45,
    'test_ci_preflight.PreflightFailures.'
    'test_gcp_sites_reject_cross_class_routes': 40,
    'test_ci_preflight.PreflightFailures.'
    'test_authz_and_every_dependency_are_required_from_mutated_files': 35,
}

_SHARD_BOOTSTRAP = """
import os
import sys
import threading
import unittest


def _watch_parent_stdin(pipe_fd: int) -> None:
    try:
        while os.read(pipe_fd, 1):
            pass
    except OSError:
        pass
    os._exit(143)


_watch_fd = os.dup(0)
_devnull_fd = os.open(os.devnull, os.O_RDONLY)
os.dup2(_devnull_fd, 0)
os.close(_devnull_fd)
threading.Thread(target=_watch_parent_stdin, args=(_watch_fd,), daemon=True).start()
sys.argv = ['unittest', '-v', *sys.argv[1:]]
unittest.main(module=None)
"""


def default_shard_count(cpu_count: int | None = None) -> int:
    """Return default shard count: len(os.sched_getaffinity(0)) when available, else os.cpu_count(), capped at 16, at least 1."""
    if cpu_count is None:
        getaffinity = getattr(os, 'sched_getaffinity', None)
        if callable(getaffinity):
            try:
                cpus: int | None = len(getaffinity(0))
            except (AttributeError, NotImplementedError, OSError):
                cpus = os.cpu_count()
        else:
            cpus = os.cpu_count()
    else:
        cpus = cpu_count
    return max(1, min(cpus or 1, 16))


def _iter_test_cases(suite: unittest.TestSuite) -> Iterable[unittest.TestCase]:
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _iter_test_cases(item)
        elif isinstance(item, unittest.TestCase):
            yield item


def discover_test_ids(module_path: Path | str) -> list[str]:
    """Load tests from module_path with unittest loader and return sorted test ids."""
    path = Path(module_path).resolve()
    if not path.is_file():
        raise FileNotFoundError(f'test module not found: {path}')
    module_dir = str(path.parent)
    module_name = path.stem
    sys.path.insert(0, module_dir)
    try:
        existing = sys.modules.get(module_name)
        if existing is not None:
            existing_file = getattr(existing, '__file__', None)
            if existing_file is None or Path(existing_file).resolve() != path:
                sys.modules.pop(module_name, None)
        suite = unittest.defaultTestLoader.loadTestsFromName(module_name)
    finally:
        if sys.path and sys.path[0] == module_dir:
            sys.path.pop(0)

    ids: list[str] = []
    for case in _iter_test_cases(suite):
        if case.__class__.__module__ == 'unittest.loader' and case.__class__.__name__ == '_FailedTest':
            exc = getattr(case, '_exception', None)
            raise RuntimeError(f'failed to load tests from {path}: {exc or case.id()}')
        ids.append(case.id())
    if not ids:
        raise ValueError(f'no unittest tests discovered in {path}')
    if len(set(ids)) != len(ids):
        raise ValueError(f'duplicate test ids discovered in {path}')
    return sorted(ids)


def split_into_shards(test_ids: Sequence[str], shard_count: int) -> list[list[str]]:
    """Balance known expensive preflight methods; otherwise use round robin."""
    if shard_count < 1:
        raise ValueError(f'shard_count must be at least 1, got {shard_count}')
    ordered = sorted(test_ids)
    if not any(test_id in PREFLIGHT_TEST_WEIGHTS for test_id in ordered):
        return [list(ordered[index::shard_count]) for index in range(shard_count)]
    shards: list[list[str]] = [[] for _ in range(shard_count)]
    loads = [0] * shard_count
    for test_id in sorted(ordered, key=lambda row: (-PREFLIGHT_TEST_WEIGHTS.get(row, 1), row)):
        index = min(range(shard_count), key=lambda slot: (loads[slot], slot))
        shards[index].append(test_id)
        loads[index] += PREFLIGHT_TEST_WEIGHTS.get(test_id, 1)
    return [sorted(shard) for shard in shards]


def validate_shard_partition(full_ids: Sequence[str], shards: Sequence[Sequence[str]]) -> None:
    """Verify that the shards' union equals full_ids with no lost or duplicated ids."""
    seen: set[str] = set()
    duplicates: list[str] = []
    for shard in shards:
        for test_id in shard:
            if test_id in seen and test_id not in duplicates:
                duplicates.append(test_id)
            seen.add(test_id)

    full_set = set(full_ids)
    lost = [test_id for test_id in full_ids if test_id not in seen]
    extra = sorted(seen - full_set)
    if lost or duplicates or extra:
        parts: list[str] = []
        if lost:
            parts.append(f'lost ids: {lost}')
        if duplicates:
            parts.append(f'duplicate ids: {duplicates}')
        if extra:
            parts.append(f'unexpected ids: {extra}')
        raise ValueError('; '.join(parts))


def isolated_temp_base(repo_root: Path = ROOT) -> Path:
    """Return a temporary directory base outside repo_root."""
    repository = repo_root.resolve()
    current = Path(tempfile.gettempdir()).resolve()
    if current != repository and repository not in current.parents:
        return current
    saved = tempfile.tempdir
    masked = {
        name: os.environ.pop(name)
        for name in ('TMPDIR', 'TEMP', 'TMP')
        if name in os.environ
    }
    try:
        tempfile.tempdir = None
        return Path(tempfile.gettempdir()).resolve()
    finally:
        os.environ.update(masked)
        tempfile.tempdir = saved


def _shard_env(module_dir: Path, shard_tmp: Path) -> dict[str, str]:
    env = os.environ.copy()
    tmp_str = str(shard_tmp)
    env['TMPDIR'] = tmp_str
    env['TEMP'] = tmp_str
    env['TMP'] = tmp_str
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PYTHONUNBUFFERED'] = '1'
    existing_pythonpath = env.get('PYTHONPATH', '')
    env['PYTHONPATH'] = (
        f'{module_dir}{os.pathsep}{existing_pythonpath}'
        if existing_pythonpath
        else str(module_dir)
    )
    return env


def parse_ran_count(output: str) -> int | None:
    """Parse unittest's `Ran N test(s) in ...` summary line from shard output."""
    matches = _RAN_RE.findall(output)
    if not matches:
        return None
    return int(matches[-1])


def _terminate_live_shards(
    running: Sequence[tuple[int, list[str], subprocess.Popen[bytes] | None, Path, TextIO | None, float]],
) -> None:
    for _, _, proc, _, _, _ in running:
        if proc is None:
            continue
        if proc.stdin is not None:
            try:
                proc.stdin.close()
            except Exception:
                pass
        if proc.poll() is None:
            try:
                proc.terminate()
            except OSError:
                pass
    deadline = time.monotonic() + 2.0
    for _, _, proc, _, _, _ in running:
        if proc is None or proc.poll() is not None:
            continue
        remaining = max(0.0, deadline - time.monotonic())
        try:
            proc.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            try:
                proc.kill()
            except OSError:
                pass
            try:
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                pass


def run_shards(
    module_path: Path | str,
    shard_count: int | None = None,
    *,
    log_dir: Path | str | None = None,
    shards_override: Sequence[Sequence[str]] | None = None,
    class_tier: str = 'all',
) -> int:
    """Discover, partition, and run unittest shards in parallel subprocesses.

    A module-level `SkipTest` (reporting `Ran 0` for a non-empty shard) counts
    as a failed shard, unlike serial `unittest`, because each shard verifies
    that `Ran N` matches its assigned test count.
    """
    path = Path(module_path).resolve()
    count = default_shard_count() if shard_count is None else shard_count
    if count < 1:
        print(f'unittest-shards: shard count must be at least 1, got {count}', file=sys.stderr, flush=True)
        return 2

    try:
        discovered_ids = discover_test_ids(path)
        full_ids = select_test_ids(discovered_ids, class_tier)
        if not full_ids:
            raise ValueError(f'class tier {class_tier} selected no tests')
    except Exception as exc:
        print(f'unittest-shards: discovery failed: {exc}', file=sys.stderr, flush=True)
        return 1

    if class_tier != 'all':
        print(f'unittest-shards: class tier {class_tier}: selected {len(full_ids)} '
              f'of {len(discovered_ids)} discovered tests', flush=True)
    shards = (
        [list(shard) for shard in shards_override]
        if shards_override is not None
        else split_into_shards(full_ids, count)
    )
    try:
        validate_shard_partition(full_ids, shards)
    except ValueError as exc:
        print(f'unittest-shards: partition mismatch ({exc})', file=sys.stderr, flush=True)
        return 1

    total_shards = len(shards)
    width = max(2, len(str(total_shards)))
    temp_base = isolated_temp_base(ROOT)
    print_lock = threading.Lock()
    interrupted_signum: list[int] = []

    def _handle_signal(signum: int, _frame: object) -> None:
        interrupted_signum.append(signum)
        raise KeyboardInterrupt(f'received signal {signum}')

    use_signals = threading.current_thread() is threading.main_thread()
    prev_sigint = signal.getsignal(signal.SIGINT) if use_signals else None
    prev_sigterm = signal.getsignal(signal.SIGTERM) if use_signals else None
    installed_sigint = False
    installed_sigterm = False
    if use_signals:
        if prev_sigint != signal.SIG_IGN:
            signal.signal(signal.SIGINT, _handle_signal)
            installed_sigint = True
        if prev_sigterm != signal.SIG_IGN:
            signal.signal(signal.SIGTERM, _handle_signal)
            installed_sigterm = True

    try:
        with tempfile.TemporaryDirectory(prefix='pf-unittest-shards-', dir=str(temp_base)) as temp_root_str:
            temp_root = Path(temp_root_str)
            resolved_log_dir = Path(log_dir).resolve() if log_dir is not None else (temp_root / 'logs')
            resolved_log_dir.mkdir(parents=True, exist_ok=True)
            planned_log_names = {
                f'shard-{index + 1:0{width}d}.log' for index in range(total_shards)
            }
            for stale_log in resolved_log_dir.glob('shard-*.log'):
                if stale_log.name not in planned_log_names and stale_log.is_file():
                    stale_log.unlink()

            running: list[tuple[int, list[str], subprocess.Popen[bytes] | None, Path, TextIO | None, float]] = []
            try:
                for index, shard_ids in enumerate(shards):
                    shard_num = index + 1
                    shard_tmp = temp_root / f'shard-{shard_num}'
                    shard_tmp.mkdir(parents=True, exist_ok=True)
                    log_path = resolved_log_dir / f'shard-{shard_num:0{width}d}.log'
                    started = time.monotonic()
                    with print_lock:
                        print(
                            f'shard {shard_num}/{total_shards}: starting '
                            f'({len(shard_ids)} tests, log: {log_path})',
                            flush=True,
                        )
                    header = ''.join(
                        [
                            f'# shard {shard_num}/{total_shards} ({len(shard_ids)} tests)\n',
                            *(f'#   {test_id}\n' for test_id in shard_ids),
                            '\n',
                        ]
                    )
                    if not shard_ids:
                        log_path.write_text(header, encoding='utf-8')
                        running.append((shard_num, shard_ids, None, log_path, None, started))
                        continue
                    log_handle = log_path.open('w', encoding='utf-8', buffering=1)
                    log_handle.write(header)
                    log_handle.flush()
                    proc = subprocess.Popen(
                        [sys.executable, '-B', '-u', '-c', _SHARD_BOOTSTRAP, *shard_ids],
                        stdin=subprocess.PIPE,
                        stdout=log_handle,
                        stderr=subprocess.STDOUT,
                        env=_shard_env(path.parent, shard_tmp),
                    )
                    running.append((shard_num, shard_ids, proc, log_path, log_handle, started))

                def _collect(
                    entry: tuple[int, list[str], subprocess.Popen[bytes] | None, Path, TextIO | None, float],
                ) -> tuple[int, list[str], int, int | None, str, float, str]:
                    shard_num, shard_ids, proc, log_path, log_handle, started = entry
                    if proc is None:
                        elapsed = time.monotonic() - started
                        status_line = f'shard {shard_num}/{total_shards}: ok (0 tests, {elapsed:.2f}s)'
                        with print_lock:
                            print(status_line, flush=True)
                        return shard_num, shard_ids, 0, 0, '', elapsed, status_line
                    try:
                        returncode = proc.wait()
                    finally:
                        if proc.stdin is not None:
                            try:
                                proc.stdin.close()
                            except Exception:
                                pass
                        if log_handle is not None:
                            try:
                                log_handle.flush()
                                log_handle.close()
                            except Exception:
                                pass
                    elapsed = time.monotonic() - started
                    output = (
                        log_path.read_text(encoding='utf-8', errors='replace')
                        if log_path.is_file()
                        else ''
                    )
                    ran_count = parse_ran_count(output)
                    expected_count = len(shard_ids)
                    if returncode < 0:
                        status_line = (
                            f'shard {shard_num}/{total_shards}: FAILED '
                            f'(killed by signal {-returncode}, {expected_count} tests, {elapsed:.2f}s)'
                        )
                    elif returncode != 0:
                        status_line = (
                            f'shard {shard_num}/{total_shards}: FAILED '
                            f'(exit {returncode}, {expected_count} tests, {elapsed:.2f}s)'
                        )
                    elif ran_count != expected_count:
                        ran_desc = 'no Ran line' if ran_count is None else f'ran {ran_count}'
                        status_line = (
                            f'shard {shard_num}/{total_shards}: FAILED '
                            f'({ran_desc} of {expected_count} assigned tests, {elapsed:.2f}s)'
                        )
                    else:
                        status_line = (
                            f'shard {shard_num}/{total_shards}: ok '
                            f'({expected_count} tests, {elapsed:.2f}s)'
                        )
                    with print_lock:
                        print(status_line, flush=True)
                    return shard_num, shard_ids, returncode, ran_count, output, elapsed, status_line

                pool = ThreadPoolExecutor(max_workers=max(1, total_shards))
                try:
                    results = list(pool.map(_collect, running))
                except BaseException:
                    _terminate_live_shards(running)
                    raise
                finally:
                    pool.shutdown(wait=True)
            finally:
                _terminate_live_shards(running)
                for _, _, _, _, log_handle, _ in running:
                    if log_handle is not None and not log_handle.closed:
                        try:
                            log_handle.close()
                        except Exception:
                            pass
    except KeyboardInterrupt:
        sig = interrupted_signum[-1] if interrupted_signum else signal.SIGINT
        print(
            f'unittest-shards: interrupted by signal {sig}; terminated live shards',
            file=sys.stderr,
            flush=True,
        )
        return 128 + sig
    finally:
        if use_signals:
            if installed_sigint and prev_sigint is not None:
                signal.signal(signal.SIGINT, prev_sigint)
            if installed_sigterm and prev_sigterm is not None:
                signal.signal(signal.SIGTERM, prev_sigterm)

    failed_shards: list[int] = []
    for shard_num, shard_ids, returncode, ran_count, output, elapsed, status_line in results:
        if output:
            print(output.rstrip(), flush=True)
        print(status_line, flush=True)
        if returncode != 0 or ran_count != len(shard_ids):
            failed_shards.append(shard_num)

    if failed_shards:
        names = ', '.join(f'shard {shard_num}' for shard_num in failed_shards)
        print(
            f'unittest-shards: FAILED ({len(full_ids)} tests across {total_shards} shards; '
            f'failed: {names})',
            flush=True,
        )
        return 1

    print(
        f'unittest-shards: {len(full_ids)} tests across {total_shards} shards passed',
        flush=True,
    )
    return 0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path, help='Path to the unittest module file.')
    parser.add_argument(
        '-n',
        '--shards',
        type=int,
        default=None,
        help='Number of parallel shards (default: min(sched_getaffinity or cpu_count, 16), at least 1).',
    )
    parser.add_argument(
        '--log-dir',
        type=Path,
        default=None,
        help='Directory where each shard writes its live output file (shard-XX.log).',
    )
    parser.add_argument('--class-tier', choices=('all', 'fast'), default='all',
                        help='Verifier tier: fast defers only named SLOW classes; '
                             'unclassified classes still run (default: all).')
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    return run_shards(args.module, shard_count=args.shards, log_dir=args.log_dir,
                      class_tier=args.class_tier)


if __name__ == '__main__':
    raise SystemExit(main())
