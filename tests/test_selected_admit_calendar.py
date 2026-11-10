#!/usr/bin/env python3
"""Check the calendar chain of stage selected in pineforge-selected-admit.

Named zones are classified against the zone database root, then libc applies
their rules for each date. Check aligned windows and precise refusals when a
boundary is unaligned or a named zone's database cannot be read.

Four-hour script bars are anchored at the session open, so the UTC instant of
an aligned boundary changes with the daylight-saving rules. Hourly bars repeat
at the same minute in both seasons and cannot distinguish those rules.
Every expected instant, count and boundary is derived here from civil times,
known UTC offsets and arithmetic.
"""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


def utc_ms(year, month, day, hour, minute):
    """Convert a UTC civil time to integer milliseconds since the epoch."""
    civil = datetime(year, month, day, hour, minute, tzinfo=timezone.utc)
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    return (civil - epoch) // timedelta(milliseconds=1)


OPEN_HOUR = 9
OPEN_MINUTE = 30
MINUTE_MS = 60 * 1000
HOUR_MS = 60 * MINUTE_MS
FOUR_HOURS_MS = 4 * HOUR_MS


def grid_neighbours(start_ms, offset_hours):
    """Return the UTC bar opens at or before an instant and strictly after it."""
    offset_ms = timedelta(hours=offset_hours) // timedelta(milliseconds=1)
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    local = epoch + timedelta(milliseconds=start_ms + offset_ms)
    local_open_ms = utc_ms(local.year, local.month, local.day, OPEN_HOUR, OPEN_MINUTE)
    open_ms = local_open_ms - offset_ms
    # Floor division includes the earlier bar opens before the session open.
    previous_ms = open_ms + ((start_ms - open_ms) // FOUR_HOURS_MS) * FOUR_HOURS_MS
    return previous_ms, previous_ms + FOUR_HOURS_MS


def aligned(start_ms, offset_hours):
    """Whether an instant is itself a bar open at the given UTC offset."""
    return grid_neighbours(start_ms, offset_hours)[0] == start_ms


def engine_reads_tzdir():
    """Whether the engine build of this host reads TZDIR: only a glibc build does."""
    try:
        return os.confstr("CS_GNU_LIBC_VERSION").startswith("glibc")
    except (AttributeError, ValueError, OSError):
        return False


DATE_OFFSETS = (
    ((2024, 1, 17), -5, "Wednesday in winter standard time."),
    ((2024, 7, 17), -4, "Wednesday in summer daylight time."),
    ((2024, 3, 8), -5, "Friday before Sunday 2024-03-10 at 02:00 local."),
    ((2024, 3, 11), -4, "Monday after Sunday 2024-03-10 at 02:00 local."),
    ((2024, 11, 1), -4, "Friday before Sunday 2024-11-03 at 02:00 local."),
    ((2024, 11, 4), -5, "Monday after Sunday 2024-11-03 at 02:00 local."),
)
ZONE = "America/New_York"
ZONE_ROOT = "/usr/share/zoneinfo"
HEADER = b"timestamp,open,high,low,close,volume\n"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def primary_rows(fed_start, end):
    return HEADER + b"".join(
        f"{stamp},1,1,1,1,1\n".encode("ascii")
        for stamp in range(fed_start, end, MINUTE_MS)
    )


class HelperCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = os.environ.get("PINEFORGE_SELECTED_ADMIT_HELPER")
        cls.identity = os.environ.get("PINEFORGE_SELECTED_ADMIT_EXPECT_GIT_SHA")
        if not cls.helper:
            raise AssertionError(
                "PINEFORGE_SELECTED_ADMIT_HELPER must be supplied by the test registration"
            )
        if not cls.identity:
            raise AssertionError(
                "PINEFORGE_SELECTED_ADMIT_EXPECT_GIT_SHA must be supplied by the test registration"
            )
        if len(cls.identity) != 40 or any(c not in "0123456789abcdef" for c in cls.identity):
            raise AssertionError("invalid PINEFORGE_SELECTED_ADMIT_EXPECT_GIT_SHA")
        if not Path(cls.helper).is_file():
            raise AssertionError(f"PINEFORGE_SELECTED_ADMIT_HELPER file is missing: {cls.helper}")
        cls.root = Path(tempfile.mkdtemp(prefix="selected-admit-calendar-", dir=WORK_DIR))
        cls.serial = 0

    def file(self, data):
        type(self).serial += 1
        path = self.root / ("input-" + str(self.serial))
        path.write_bytes(data)
        return os.open(path, os.O_RDONLY)

    def descriptor(self, stream_fd, data, start_ms, zone):
        return {
            "schema": "pineforge-selected-capacity-request/v1",
            "expect": {"git_sha": self.identity},
            "identity": {"run_id": "calendar", "attempt_id": "selected"},
            "bounds": {"script_bars": "100"},
            "limits": {"max_record_bytes": "4096"},
            "streams": [{
                "id": "primary", "role": "primary", "kind": "file",
                "fd": str(stream_fd), "bytes": str(len(data)), "sha256": digest(data),
            }],
            "calendar": {
                "start_ms": str(start_ms), "end_ms": str(start_ms + FOUR_HOURS_MS),
                "fed_start_ms": str(start_ms - FOUR_HOURS_MS),
                "preroll_bars": "1", "input_tf": "1", "script_tf": "240",
                "chart_timezone": zone, "engine_timezone": zone,
                "session": "0930-1600", "feed_tolerant": True,
            },
        }

    def process(self, start_ms, zone, env):
        data = primary_rows(start_ms - FOUR_HOURS_MS, start_ms + FOUR_HOURS_MS)
        stream_fd = self.file(data)
        try:
            document = self.descriptor(stream_fd, data, start_ms, zone)
            raw = json.dumps(document, separators=(",", ":"), ensure_ascii=True).encode("ascii")
            descriptor_hash = digest(raw)
            descriptor_fd = self.file(raw)
            try:
                result = subprocess.run(
                    [
                        self.helper, "--stage", "selected",
                        "--descriptor-fd", str(descriptor_fd),
                        "--descriptor-sha256", descriptor_hash,
                    ],
                    pass_fds=(stream_fd, descriptor_fd), env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=120,
                )
            finally:
                os.close(descriptor_fd)
        finally:
            os.close(stream_fd)
        self.assertTrue(result.stdout.endswith(b"\n"), result.stdout)
        self.assertEqual(result.stdout.count(b"\n"), 1, result.stdout)
        self.assertTrue(result.stdout.isascii(), result.stdout)
        decoded = json.loads(result.stdout)
        self.assertEqual(decoded["exit"], result.returncode)
        self.assertEqual(decoded["descriptor_sha256"], descriptor_hash)
        self.assertEqual(decoded["identity"], document["identity"])
        self.assertEqual(decoded["helper"]["git_sha"], self.identity)
        return result, decoded


class SelectedCalendar(HelperCase):
    def assert_admitted(self, process, document, start_ms):
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(document["outcome"], "admitted")
        self.assertEqual(document["status"], "completed")
        self.assertEqual(document["reason"], "none")
        fed_start = start_ms - FOUR_HOURS_MS
        end = start_ms + FOUR_HOURS_MS
        expected_bars = (end - fed_start) // FOUR_HOURS_MS
        self.assertEqual(document["state"]["script_bars"], str(expected_bars))

    def assert_refused(self, process, document, reason, option, bound, value_ms,
                       previous_ms, next_ms):
        self.assertEqual(process.returncode, 11, process.stderr)
        self.assertEqual(document["outcome"], "not_admitted")
        self.assertEqual(document["status"], "not_admitted")
        self.assertEqual(document["reason"], reason)
        where = document["where"]
        self.assertEqual(where["option"], option)
        self.assertEqual(where["bound"], str(bound))
        self.assertEqual(where["value_ms"], str(value_ms))
        self.assertEqual(where["previous_boundary_ms"], str(previous_ms))
        self.assertEqual(where["next_boundary_ms"], str(next_ms))

    def test_zone_database_file(self):
        """The named zone requires an installed zone database file."""
        path = Path(ZONE_ROOT) / ZONE
        self.assertTrue(path.exists(), f"missing zone database file: {path}")
        self.assertTrue(path.is_file(), f"zone database path is not a file: {path}")

    def test_oracle(self):
        """Civil-time arithmetic gives both neighbours and exact alignment."""
        pairs = (
            (
                utc_ms(2024, 1, 17, 13, 30), -5,
                (utc_ms(2024, 1, 17, 10, 30), utc_ms(2024, 1, 17, 14, 30)),
            ),
            (
                utc_ms(2024, 7, 17, 14, 30), -4,
                (utc_ms(2024, 7, 17, 13, 30), utc_ms(2024, 7, 17, 17, 30)),
            ),
        )
        for instant, offset, expected in pairs:
            with self.subTest(instant=instant, offset=offset):
                self.assertEqual(grid_neighbours(instant, offset), expected)
        with self.subTest(alignment=True):
            self.assertTrue(aligned(utc_ms(2024, 1, 17, 14, 30), -5))
        with self.subTest(alignment=False):
            self.assertFalse(aligned(utc_ms(2024, 1, 17, 13, 30), -5))

    def test_session_open_each_date(self):
        """The named zone admits its own session open under each date's rules."""
        for date, offset, reason in DATE_OFFSETS:
            with self.subTest(date=date, offset=offset, reason=reason):
                start_ms = utc_ms(*date, OPEN_HOUR - offset, OPEN_MINUTE)
                process, document = self.process(start_ms, ZONE, env={})
                self.assert_admitted(process, document, start_ms)

    def test_other_season_open_each_date(self):
        """The other season's UTC open is refused with the true grid neighbours."""
        for date, offset, reason in DATE_OFFSETS:
            with self.subTest(date=date, offset=offset, reason=reason):
                other_offset = -4 if offset == -5 else -5
                start_ms = utc_ms(*date, OPEN_HOUR - other_offset, OPEN_MINUTE)
                previous_ms, next_ms = grid_neighbours(start_ms, offset)
                process, document = self.process(start_ms, ZONE, env={})
                self.assert_refused(
                    process, document, "boundary_unaligned", "start_ms", 0,
                    start_ms, previous_ms, next_ms,
                )

    def test_zone_database_root(self):
        """Named zones require their database root and override the process zone."""
        offset = -4
        start_ms = utc_ms(2024, 7, 17, OPEN_HOUR - offset, OPEN_MINUTE)
        missing = self.root / "missing-zoneinfo"
        empty = self.root / "empty-zoneinfo"
        self.assertFalse(missing.exists(), f"zone database directory must be absent: {missing}")
        empty.mkdir()
        # Only a glibc build of the engine reads TZDIR (src/native_calendar.cpp,
        # tzdir_canonical): Darwin and any other libc use a fixed zone database
        # root and ignore it, so only a glibc host refuses a root that holds no zone.
        root_honoured = engine_reads_tzdir()
        cases = (
            ({"TZDIR": str(missing)}, not root_honoured),
            ({"TZDIR": str(empty)}, not root_honoured),
            ({"TZDIR": ZONE_ROOT}, True),
            ({"TZ": "Asia/Tokyo"}, True),
        )
        for env, admitted in cases:
            with self.subTest(env=env):
                process, document = self.process(start_ms, ZONE, env=env)
                if admitted:
                    self.assert_admitted(process, document, start_ms)
                else:
                    # Zone classification precedes any dated boundary lookup.
                    self.assert_refused(
                        process, document, "calendar_unsupported", "engine_timezone", -1,
                        0, 0, 0,
                    )

    def test_utc_calendar(self):
        """UTC keeps its session grid when the zone database root is missing."""
        open_ms = utc_ms(2024, 7, 17, OPEN_HOUR, OPEN_MINUTE)
        missing = self.root / "missing-utc-zoneinfo"
        self.assertFalse(missing.exists(), f"zone database directory must be absent: {missing}")
        cases = (
            (open_ms + FOUR_HOURS_MS, True),
            (open_ms + FOUR_HOURS_MS + HOUR_MS, False),
        )
        for start_ms, admitted in cases:
            with self.subTest(start_ms=start_ms):
                process, document = self.process(start_ms, "UTC", env={"TZDIR": str(missing)})
                if admitted:
                    self.assert_admitted(process, document, start_ms)
                else:
                    previous_ms, next_ms = grid_neighbours(start_ms, 0)
                    self.assert_refused(
                        process, document, "boundary_unaligned", "start_ms", 0,
                        start_ms, previous_ms, next_ms,
                    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--work-dir", required=True)
    options = parser.parse_args()
    WORK_DIR = options.work_dir
    unittest.main(argv=[__file__])
