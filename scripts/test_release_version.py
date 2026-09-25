#!/usr/bin/env python3
"""Unit tests for scripts/release_version.py, the release workflow's version
arithmetic (lane REL10). A ci_preflight stage runs them."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from release_version import (  # noqa: E402
    ReleaseError, check_install, next_version, parse_tags, parse_version, plan,
    previous_tag)

SCRIPT = Path(__file__).with_name("release_version.py")

# The engine's tags as of lane REL10 (no v0.14.0: VERSION went 0.13.1 ->
# 0.14.0 without a release), plus the candidates and finals of a 1.0 cycle.
TAGS = ["v0.10.14", "v0.11.0", "v0.12.3", "v0.13.0", "v0.13.1"]


class ParseTests(unittest.TestCase):
    def test_accepts_finals_and_release_candidates(self) -> None:
        self.assertEqual(str(parse_version("0.14.0")), "0.14.0")
        self.assertEqual(str(parse_version("1.0.0-rc.1")), "1.0.0-rc.1")
        self.assertEqual(parse_version("1.0.0-rc.12").rc, 12)
        self.assertEqual(parse_version("10.20.30").numeric, "10.20.30")

    def test_refuses_every_other_spelling(self) -> None:
        for bad in ("v1.0.0", "01.0.0", "1.00.0", "1.0.0-rc.0", "1.0.0-rc.01",
                    "1.0.0-rc", "1.0.0rc1", "1.0.0-beta.1", "1.0.0-rc.1.1",
                    "1.0.0+build", "1.0", "1.0.0 ", "1.0.0\n", "1.0.0;true",
                    "$(id)", ""):
            with self.subTest(bad=bad), self.assertRaises(ReleaseError):
                parse_version(bad)

    def test_semver_precedence(self) -> None:
        order = ["0.13.1", "0.14.0", "1.0.0-rc.1", "1.0.0-rc.2", "1.0.0-rc.10",
                 "1.0.0", "1.0.1-rc.1", "1.0.1", "1.1.0"]
        keys = [parse_version(v).key() for v in order]
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(len(set(keys)), len(keys))

    def test_tags_that_are_not_releases_are_ignored(self) -> None:
        found = parse_tags(["v1.0.0", "1.0.0", "vnext", "v1.0", "v1.0.0-beta.1", " v0.13.1 "])
        self.assertEqual([str(v) for v in found], ["1.0.0", "0.13.1"])


class NextVersionTests(unittest.TestCase):
    def test_bumps_a_final(self) -> None:
        self.assertEqual(str(next_version("0.14.0", "patch")), "0.14.1")
        self.assertEqual(str(next_version("0.14.0", "minor")), "0.15.0")
        self.assertEqual(str(next_version("0.14.0", "major")), "1.0.0")

    def test_override_wins_over_bump(self) -> None:
        self.assertEqual(str(next_version("0.14.0", "patch", "1.0.0-rc.1")), "1.0.0-rc.1")
        self.assertEqual(str(next_version("1.0.0-rc.1", "patch", "1.0.0-rc.2")), "1.0.0-rc.2")
        self.assertEqual(str(next_version("1.0.0-rc.2", "major", "1.0.0")), "1.0.0")

    def test_a_candidate_has_no_bump(self) -> None:
        for bump in ("patch", "minor", "major"):
            with self.subTest(bump=bump), self.assertRaisesRegex(
                    ReleaseError, r"release candidate 1\.0\.0-rc\.1: pass override"):
                next_version("1.0.0-rc.1", bump)

    def test_refuses_what_does_not_sort_above(self) -> None:
        for current, override in (("1.0.0", "1.0.0"), ("1.0.0", "1.0.0-rc.3"),
                                  ("1.0.0-rc.2", "1.0.0-rc.1"), ("1.0.0-rc.1", "1.0.0-rc.1"),
                                  ("0.14.0", "0.13.2")):
            with self.subTest(current=current, override=override), \
                    self.assertRaisesRegex(ReleaseError, "does not sort above"):
                next_version(current, "patch", override)

    def test_refuses_a_malformed_current_or_bump(self) -> None:
        with self.assertRaises(ReleaseError):
            next_version("0.14", "patch")
        with self.assertRaises(ReleaseError):
            next_version("0.14.0", "invented")


class PreviousTagTests(unittest.TestCase):
    def tags(self, *extra: str) -> list:
        return parse_tags(TAGS + list(extra))

    def test_first_candidate_starts_at_the_last_release(self) -> None:
        self.assertEqual(previous_tag(parse_version("1.0.0-rc.1"), self.tags()), "v0.13.1")

    def test_a_later_candidate_starts_at_the_candidate_before(self) -> None:
        self.assertEqual(previous_tag(parse_version("1.0.0-rc.2"), self.tags("v1.0.0-rc.1")),
                         "v1.0.0-rc.1")

    def test_a_final_starts_at_the_last_final(self) -> None:
        tags = self.tags("v1.0.0-rc.1", "v1.0.0-rc.2")
        self.assertEqual(previous_tag(parse_version("1.0.0"), tags), "v0.13.1")

    def test_candidates_of_a_release_sort_below_it(self) -> None:
        # git's --sort=-v:refname would answer v1.0.0-rc.2 here.
        tags = self.tags("v1.0.0-rc.1", "v1.0.0-rc.2", "v1.0.0")
        self.assertEqual(previous_tag(parse_version("1.0.1"), tags), "v1.0.0")
        self.assertEqual(previous_tag(parse_version("1.0.1-rc.1"), tags), "v1.0.0")

    def test_no_earlier_tag(self) -> None:
        self.assertEqual(previous_tag(parse_version("0.1.0"), []), "")


class PlanTests(unittest.TestCase):
    def test_the_release_candidate_plan(self) -> None:
        self.assertEqual(plan("0.14.0", "patch", "1.0.0-rc.1", TAGS), {
            "current": "0.14.0", "next": "1.0.0-rc.1", "tag": "v1.0.0-rc.1",
            "prerelease": "true", "previous": "v0.13.1"})

    def test_the_final_plan(self) -> None:
        self.assertEqual(plan("1.0.0-rc.1", "patch", "1.0.0", TAGS + ["v1.0.0-rc.1"]), {
            "current": "1.0.0-rc.1", "next": "1.0.0", "tag": "v1.0.0",
            "prerelease": "false", "previous": "v0.13.1"})

    def test_refuses_an_existing_tag(self) -> None:
        with self.assertRaisesRegex(ReleaseError, r"tag v1\.0\.0-rc\.1 already exists"):
            plan("0.14.0", "patch", "1.0.0-rc.1", TAGS + ["v1.0.0-rc.1"])


class CheckInstallTests(unittest.TestCase):
    def stage(self, root: Path, header_full: str, config_full: str, package: str) -> Path:
        header = root / "include" / "pineforge" / "version.h"
        header.parent.mkdir(parents=True)
        header.write_text(
            "#define PINEFORGE_VERSION_MAJOR   1\n"
            "#define PINEFORGE_VERSION_MINOR   0\n"
            "#define PINEFORGE_VERSION_PATCH   0\n"
            '#define PINEFORGE_VERSION_STRING  "1.0.0"\n'
            f'#define PINEFORGE_VERSION_FULL    "{header_full}"\n'
            '#define PINEFORGE_GIT_SHA         "abc1234"\n', encoding="utf-8")
        cmake = root / "lib" / "cmake" / "PineForge"
        cmake.mkdir(parents=True)
        (cmake / "PineForgeConfig.cmake").write_text(
            f'set(PineForge_VERSION_FULL "{config_full}")\n', encoding="utf-8")
        (cmake / "PineForgeConfigVersion.cmake").write_text(
            f'set(PACKAGE_VERSION "{package}")\n', encoding="utf-8")
        return root

    def test_a_candidate_install(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.stage(Path(directory), "1.0.0-rc.1", "1.0.0-rc.1", "1.0.0")
            self.assertEqual(check_install(root, "1.0.0-rc.1"), [])

    def test_a_dropped_prerelease_is_named(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.stage(Path(directory), "1.0.0", "1.0.0", "1.0.0")
            problems = check_install(root, "1.0.0-rc.1")
            self.assertEqual(len(problems), 2, problems)
            self.assertIn("PINEFORGE_VERSION_FULL", problems[0])
            self.assertIn("PineForge_VERSION_FULL", problems[1])

    def test_a_git_descriptor_is_not_the_release(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.stage(Path(directory), "1.0.0-rc.1-1-gabc1234", "1.0.0-rc.1", "1.0.0")
            self.assertEqual(len(check_install(root, "1.0.0-rc.1")), 1)

    def test_a_missing_install(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(len(check_install(Path(directory), "1.0.0")), 2)


class CommandLineTests(unittest.TestCase):
    def run_next(self, base: Path, *argv: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "next", "--version-file", str(base / "VERSION"),
             "--tags-file", str(base / "tags"), "--github-output", str(base / "output"),
             *argv], capture_output=True, text=True, check=False)

    def test_writes_the_outputs_the_workflow_reads(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "VERSION").write_text("0.14.0\n", encoding="utf-8")
            (base / "tags").write_text("\n".join(TAGS) + "\n", encoding="utf-8")
            result = self.run_next(base, "--bump", "patch", "--override", "1.0.0-rc.1")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((base / "output").read_text(encoding="utf-8"),
                             "current=0.14.0\nnext=1.0.0-rc.1\ntag=v1.0.0-rc.1\n"
                             "prerelease=true\nprevious=v0.13.1\n")
            self.assertEqual((base / "VERSION").read_text(encoding="utf-8"), "0.14.0\n")

    def test_a_refusal_exits_1_and_writes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "VERSION").write_text("1.0.0-rc.1\n", encoding="utf-8")
            (base / "tags").write_text("v1.0.0-rc.1\n", encoding="utf-8")
            result = self.run_next(base, "--bump", "minor")
            self.assertEqual(result.returncode, 1)
            self.assertIn("pass override", result.stderr)
            self.assertFalse((base / "output").exists())


if __name__ == "__main__":
    unittest.main()
