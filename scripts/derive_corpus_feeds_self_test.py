#!/usr/bin/env python3
"""Focused regression tests for deterministic corpus-feed derivation."""
from __future__ import annotations

import hashlib
import os
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

import derive_corpus_feeds as derive


def _legacy_resample_15m(lines: list[str]):
    """Previous open=first algorithm, used only for no-prefix equality."""
    buckets = []
    cur_key = None
    for line in lines:
        ts_s, o, h, l, c, v = line.rstrip("\n").split(",")
        ts = int(ts_s)
        key = ts - ts % 900_000
        if key != cur_key:
            buckets.append([key, o, float(h), float(l), c, float(v)])
            cur_key = key
        else:
            bucket = buckets[-1]
            bucket[2] = max(bucket[2], float(h))
            bucket[3] = min(bucket[3], float(l))
            bucket[4] = c
            bucket[5] += float(v)
    return buckets


class ResampleOpenTests(unittest.TestCase):
    def test_positive_first_row_is_unchanged(self) -> None:
        lines = [
            "60000,100,102,99,101,2\n",
            "120000,200,203,98,202,3\n",
        ]
        bucket = derive._resample_15m(lines)[0]
        self.assertEqual(bucket, [0, "100", 203.0, 98.0, "202", 5.0])

    def test_zero_volume_lower_prefix_uses_first_real_open(self) -> None:
        lines = [
            "0,90,91,89,90,0\n",
            "60000,100,101,99,100,4\n",
            "120000,105,106,104,105,2\n",
        ]
        bucket = derive._resample_15m(lines)[0]
        self.assertEqual(bucket, [0, "100", 106.0, 89.0, "105", 6.0])

    def test_zero_volume_higher_prefix_uses_first_real_open(self) -> None:
        lines = [
            "0,110,111,109,110,0\n",
            "60000,100,101,99,100,4\n",
            "120000,95,96,94,95,0\n",
        ]
        bucket = derive._resample_15m(lines)[0]
        self.assertEqual(bucket, [0, "100", 111.0, 94.0, "95", 4.0])

    def test_all_zero_bucket_falls_back_to_first_row(self) -> None:
        lines = [
            "0,90,91,89,90,0\n",
            "60000,110,111,109,110,0\n",
        ]
        bucket = derive._resample_15m(lines)[0]
        self.assertEqual(bucket, [0, "90", 111.0, 89.0, "110", 0.0])

    def test_no_synthetic_prefix_is_exactly_legacy_equal(self) -> None:
        lines = [
            "0,100.125,102,99,101,1.25\n",
            "60000,101,103,98,102,0\n",
            "900000,200,201,199,200.5,2.125\n",
            "960000,201,202,198,199.5,3.25\n",
        ]
        actual = derive._resample_15m(lines)
        expected = _legacy_resample_15m(lines)
        self.assertEqual(actual, expected)
        self.assertEqual(
            [derive._format_bucket(bucket) for bucket in actual],
            [
                "0,100.125,103,98,102,1.25",
                "900000,200,202,198,199.5,5.375",
            ],
        )


class InputPolicyTests(unittest.TestCase):
    def test_malformed_and_nonfinite_rows_fail_closed(self) -> None:
        cases = {
            "field count": ("0,1,2\n", "expected 6"),
            "timestamp": ("bad,1,1,1,1,1\n", "invalid timestamp"),
            "volume text": ("0,1,1,1,1,nope\n", "invalid volume"),
            "volume nan": ("0,1,1,1,1,nan\n", "volume must be finite"),
            "volume positive infinity": (
                "0,1,1,1,1,inf\n", "volume must be finite"),
            "volume negative infinity": (
                "0,1,1,1,1,-inf\n", "volume must be finite"),
            "negative volume": (
                "0,1,1,1,1,-0.001\n", "volume must be non-negative"),
            "nonfinite price": ("0,1,nan,1,1,1\n", "high must be finite"),
        }
        for name, (line, message) in cases.items():
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, message):
                    derive._resample_15m([line])


class CacheVersionTests(unittest.TestCase):
    def test_old_version_cannot_preserve_newer_stale_feeds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_s:
            root = Path(tmp_s)
            source = root / "source.csv"
            derived_dir = root / "derived"
            full = derived_dir / "full.csv"
            window = derived_dir / "window.csv"
            stamp = derived_dir / ".version"
            derived_dir.mkdir()

            # ensure_derived deliberately rejects tiny/LFS-pointer-like inputs.
            # A large ignored header keeps this fixture cheap and exercises the
            # real materialization path without weakening that production guard.
            source.write_text(
                "h" * 1_000_001
                + "\n"
                + f"{derive.WINDOW_START_MS},90,91,89,90,0\n"
                + f"{derive.WINDOW_START_MS + 60_000},100,101,99,100,2\n"
            )
            source_before = hashlib.sha256(source.read_bytes()).hexdigest()
            full.write_text("stale-full\n")
            window.write_text("stale-window\n")
            stamp.write_text("v1:first-row-open\n")

            # Outputs are newer than the source, so only the rule-version stamp
            # can force regeneration.
            os.utime(source, (100, 100))
            for path in (full, window, stamp):
                os.utime(path, (200, 200))

            with mock.patch.multiple(
                derive,
                SOURCE_1M=source,
                DERIVED_DIR=derived_dir,
                DERIVED_15M=full,
                DERIVED_15M_WINDOW=window,
                DERIVATION_STAMP=stamp,
            ):
                derive.ensure_derived()
                expected = (
                    derive.HEADER
                    + "\n"
                    + f"{derive.WINDOW_START_MS},100,101,89,100,2\n"
                )
                self.assertEqual(full.read_text(), expected)
                self.assertEqual(window.read_text(), expected)
                self.assertEqual(
                    stamp.read_text(), derive.DERIVATION_CACHE_VERSION + "\n")
                self.assertEqual(
                    hashlib.sha256(source.read_bytes()).hexdigest(),
                    source_before,
                )

                # A current stamp and fresh outputs are a true byte-preserving
                # no-op, including file mtimes.
                before = {
                    path: (path.read_bytes(), path.stat().st_mtime_ns)
                    for path in (full, window, stamp)
                }
                derive.ensure_derived()
                after = {
                    path: (path.read_bytes(), path.stat().st_mtime_ns)
                    for path in (full, window, stamp)
                }
                self.assertEqual(after, before)


class AtomicWriteTests(unittest.TestCase):
    def test_parallel_writers_do_not_share_a_temporary_path(self) -> None:
        """Mutation-kill the former fixed ``<target>.new`` race."""
        with tempfile.TemporaryDirectory() as tmp_s:
            root = Path(tmp_s)
            target = root / "derived.csv"
            barrier = threading.Barrier(2)
            seen_sources: list[Path] = []
            seen_lock = threading.Lock()
            original_replace = Path.replace

            def synchronized_replace(source: Path, destination: Path):
                with seen_lock:
                    seen_sources.append(source)
                barrier.wait(timeout=5)
                return original_replace(source, destination)

            with mock.patch.object(Path, "replace", synchronized_replace):
                with ThreadPoolExecutor(max_workers=2) as pool:
                    futures = [
                        pool.submit(derive._write_atomic, target, value)
                        for value in ("alpha\n", "beta\n")
                    ]
                    for future in futures:
                        future.result(timeout=5)

            self.assertEqual(len(seen_sources), 2)
            self.assertEqual(len(set(seen_sources)), 2)
            self.assertIn(target.read_text(), {"alpha\n", "beta\n"})
            self.assertEqual(list(root.glob(".derived.csv.*.new")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
