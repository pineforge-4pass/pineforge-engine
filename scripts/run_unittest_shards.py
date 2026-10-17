#!/usr/bin/env python3
"""Run a unittest module across parallel subprocess shards."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from typing import Iterable, Sequence
import unittest

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]


def default_shard_count(cpu_count: int | None = None) -> int:
    """Return default shard count: min(os.cpu_count(), 16), at least 1."""
    cpus = os.cpu_count() if cpu_count is None else cpu_count
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
    """Split test_ids deterministically into shard_count shards in round-robin order."""
    if shard_count < 1:
        raise ValueError(f'shard_count must be at least 1, got {shard_count}')
    ordered = sorted(test_ids)
    return [list(ordered[index::shard_count]) for index in range(shard_count)]


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
    existing_pythonpath = env.get('PYTHONPATH', '')
    env['PYTHONPATH'] = (
        f'{module_dir}{os.pathsep}{existing_pythonpath}'
        if existing_pythonpath
        else str(module_dir)
    )
    return env


def run_shards(
    module_path: Path | str,
    shard_count: int | None = None,
    *,
    shards_override: Sequence[Sequence[str]] | None = None,
) -> int:
    """Discover, partition, and run unittest shards in parallel subprocesses."""
    path = Path(module_path).resolve()
    count = default_shard_count() if shard_count is None else shard_count
    if count < 1:
        print(f'unittest-shards: shard count must be at least 1, got {count}', file=sys.stderr, flush=True)
        return 2

    try:
        full_ids = discover_test_ids(path)
    except Exception as exc:
        print(f'unittest-shards: discovery failed: {exc}', file=sys.stderr, flush=True)
        return 1

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
    temp_base = isolated_temp_base(ROOT)
    with tempfile.TemporaryDirectory(prefix='pf-unittest-shards-', dir=str(temp_base)) as temp_root_str:
        temp_root = Path(temp_root_str)
        running: list[tuple[int, list[str], subprocess.Popen[str] | None, float]] = []
        for index, shard_ids in enumerate(shards):
            shard_tmp = temp_root / f'shard-{index}'
            shard_tmp.mkdir(parents=True, exist_ok=True)
            started = time.monotonic()
            if not shard_ids:
                running.append((index, shard_ids, None, started))
                continue
            proc = subprocess.Popen(
                [sys.executable, '-B', '-m', 'unittest', *shard_ids],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors='replace',
                env=_shard_env(path.parent, shard_tmp),
            )
            running.append((index, shard_ids, proc, started))

        def _collect(entry: tuple[int, list[str], subprocess.Popen[str] | None, float]) -> tuple[int, list[str], int, str, float]:
            index, shard_ids, proc, started = entry
            if proc is None:
                return index, shard_ids, 0, '', time.monotonic() - started
            output, _ = proc.communicate()
            elapsed = time.monotonic() - started
            return index, shard_ids, proc.returncode, output or '', elapsed

        with ThreadPoolExecutor(max_workers=max(1, total_shards)) as pool:
            results = list(pool.map(_collect, running))

    failed_shards: list[int] = []
    for index, shard_ids, returncode, output, elapsed in results:
        if output:
            print(output.rstrip(), flush=True)
        if returncode == 0:
            print(
                f'shard {index}/{total_shards}: ok ({len(shard_ids)} tests, {elapsed:.2f}s)',
                flush=True,
            )
        elif returncode < 0:
            failed_shards.append(index)
            print(
                f'shard {index}/{total_shards}: FAILED '
                f'(killed by signal {-returncode}, {len(shard_ids)} tests, {elapsed:.2f}s)',
                flush=True,
            )
        else:
            failed_shards.append(index)
            print(
                f'shard {index}/{total_shards}: FAILED '
                f'(exit {returncode}, {len(shard_ids)} tests, {elapsed:.2f}s)',
                flush=True,
            )

    if failed_shards:
        names = ', '.join(f'shard {index}' for index in failed_shards)
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
        help='Number of parallel shards (default: min(os.cpu_count(), 16), at least 1).',
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    return run_shards(args.module, shard_count=args.shards)


if __name__ == '__main__':
    raise SystemExit(main())
