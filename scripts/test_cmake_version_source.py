#!/usr/bin/env python3
"""Real CMake/Git fixtures for PINEFORGE_VERSION_SOURCE AUTO|FILE.

Does not build the engine, bump VERSION, retag the runtime, or invoke git in
the source worktree. Each case is an isolated temporary tree.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE_DIR = ROOT / "cmake"
MODULE = CMAKE_DIR / "PineForgeVersion.cmake"
PROBE_NAME = "version_probe.txt"

PROBE_CMAKELISTS = """cmake_minimum_required(VERSION 3.16)
list(APPEND CMAKE_MODULE_PATH "@CMAKE_DIR@")
include(PineForgeVersion)
pineforge_resolve_version()
file(WRITE "${CMAKE_BINARY_DIR}/version_probe.txt"
    "PINEFORGE_VERSION_MAJOR=${PINEFORGE_VERSION_MAJOR}\\n"
    "PINEFORGE_VERSION_MINOR=${PINEFORGE_VERSION_MINOR}\\n"
    "PINEFORGE_VERSION_PATCH=${PINEFORGE_VERSION_PATCH}\\n"
    "PINEFORGE_VERSION_MMP=${PINEFORGE_VERSION_MMP}\\n"
    "PINEFORGE_VERSION_FULL=${PINEFORGE_VERSION_FULL}\\n"
    "PINEFORGE_VERSION_GIT_SHA=${PINEFORGE_VERSION_GIT_SHA}\\n"
    "PINEFORGE_VERSION_DIRTY=${PINEFORGE_VERSION_DIRTY}\\n"
    "PINEFORGE_VERSION_SOURCE=${PINEFORGE_VERSION_SOURCE}\\n"
)
project(pineforge_version_probe VERSION ${PINEFORGE_VERSION_MMP} LANGUAGES NONE)
"""


def _which(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise RuntimeError(name + " is required for version-source fixtures")
    return path


def parse_probe(path: Path) -> dict[str, str]:
    found: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if not line or "=" not in line:
            raise AssertionError("unreadable probe line: " + line)
        key, value = line.split("=", 1)
        found[key] = value
    return found


def cache_value(build: Path, name: str) -> str | None:
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(name + ":") and "=" in line:
            return line.split("=", 1)[1]
    return None


class VersionSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake_bin = _which("cmake")
        cls.git_bin = _which("git")
        if not MODULE.is_file():
            raise RuntimeError("missing " + str(MODULE))

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="pf-version-source-")
        self.base = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def git_env(self) -> dict[str, str]:
        env = os.environ.copy()
        for key in (
            "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
            "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_DIR_CEILING",
        ):
            env.pop(key, None)
        env.update({
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_SYSTEM": os.devnull,
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_AUTHOR_NAME": "version-source-test",
            "GIT_AUTHOR_EMAIL": "version-source-test@pineforge.invalid",
            "GIT_COMMITTER_NAME": "version-source-test",
            "GIT_COMMITTER_EMAIL": "version-source-test@pineforge.invalid",
            "GIT_AUTHOR_DATE": "2026-01-01T00:00:00Z",
            "GIT_COMMITTER_DATE": "2026-01-01T00:00:00Z",
        })
        return env

    def git(self, repo: Path, *args: str) -> str:
        result = subprocess.run(
            [self.git_bin, "-c", "init.defaultBranch=main",
             "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", *args],
            cwd=repo, env=self.git_env(),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
        if result.returncode:
            raise RuntimeError(
                "git " + " ".join(args) + " failed (" + str(result.returncode) + "): "
                + result.stderr.strip())
        return result.stdout.strip()

    def write_tree(self, source: Path, version: str) -> None:
        source.mkdir(parents=True, exist_ok=True)
        source.joinpath("VERSION").write_text(version + "\n", encoding="utf-8")
        text = PROBE_CMAKELISTS.replace("@CMAKE_DIR@", CMAKE_DIR.as_posix())
        source.joinpath("CMakeLists.txt").write_text(text, encoding="utf-8")

    def init_repo(self, source: Path) -> None:
        self.git(source, "init")
        self.git(source, "add", "VERSION", "CMakeLists.txt")
        self.git(source, "commit", "-m", "fixture")

    def expected_describe(self, repo: Path) -> str:
        desc = self.git(repo, "describe", "--tags", "--match", "v*", "--abbrev=7", "--dirty")
        if desc.startswith("v"):
            desc = desc[1:]
        return desc

    def expected_sha(self, repo: Path) -> str:
        return self.git(repo, "rev-parse", "--short=7", "HEAD")

    def configure(self, source: Path, build: Path, *defs: str) -> subprocess.CompletedProcess[str]:
        build.mkdir(parents=True, exist_ok=True)
        return subprocess.run(
            [self.cmake_bin, "-S", str(source), "-B", str(build), *defs],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)

    def probe(self, source: Path, label: str, *defs: str) -> dict[str, str]:
        build = self.base / "build" / label
        result = self.configure(source, build, *defs)
        probe_path = build / PROBE_NAME
        if result.returncode:
            raise AssertionError(
                "cmake configure failed (" + str(result.returncode) + "):\n" + result.stdout)
        self.assertTrue(probe_path.is_file(), result.stdout)
        values = parse_probe(probe_path)
        values["_cache_source"] = cache_value(build, "PINEFORGE_VERSION_SOURCE") or ""
        values["_log"] = result.stdout
        values["_build"] = str(build)
        return values

    def configure_fail(self, source: Path, label: str, *defs: str) -> str:
        build = self.base / "build" / label
        result = self.configure(source, build, *defs)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse((build / PROBE_NAME).exists(), result.stdout)
        return result.stdout

    def test_default_auto_matches_explicit_auto(self) -> None:
        source = self.base / "auto-default"
        self.write_tree(source, "0.14.0")
        self.init_repo(source)
        self.git(source, "tag", "v0.13.1")
        omitted = self.probe(source, "omitted")
        explicit = self.probe(source, "explicit", "-DPINEFORGE_VERSION_SOURCE=AUTO")
        for key in (
            "PINEFORGE_VERSION_MAJOR", "PINEFORGE_VERSION_MINOR", "PINEFORGE_VERSION_PATCH",
            "PINEFORGE_VERSION_MMP", "PINEFORGE_VERSION_FULL", "PINEFORGE_VERSION_GIT_SHA",
            "PINEFORGE_VERSION_DIRTY", "PINEFORGE_VERSION_SOURCE",
        ):
            self.assertEqual(omitted[key], explicit[key], key)
        full = self.expected_describe(source)
        sha = self.expected_sha(source)
        self.assertEqual(omitted["PINEFORGE_VERSION_SOURCE"], "AUTO")
        self.assertEqual(omitted["_cache_source"], "AUTO")
        self.assertEqual(explicit["_cache_source"], "AUTO")
        self.assertEqual(omitted["PINEFORGE_VERSION_MMP"], "0.13.1")
        self.assertEqual(omitted["PINEFORGE_VERSION_FULL"], full)
        self.assertEqual(omitted["PINEFORGE_VERSION_GIT_SHA"], sha)
        self.assertEqual(omitted["PINEFORGE_VERSION_DIRTY"], "OFF")
        self.assertNotEqual(omitted["PINEFORGE_VERSION_MMP"], "0.14.0")

    def test_mismatched_tag_file_uses_version(self) -> None:
        source = self.base / "mismatch"
        self.write_tree(source, "0.14.0")
        self.init_repo(source)
        self.git(source, "tag", "v9.9.9")
        source.joinpath("extra.txt").write_text("second commit\n", encoding="utf-8")
        self.git(source, "add", "extra.txt")
        self.git(source, "commit", "-m", "ahead of tag")
        auto = self.probe(source, "mismatch-auto", "-DPINEFORGE_VERSION_SOURCE=AUTO")
        file_mode = self.probe(source, "mismatch-file", "-DPINEFORGE_VERSION_SOURCE=FILE")
        full = self.expected_describe(source)
        sha = self.expected_sha(source)
        self.assertEqual(auto["PINEFORGE_VERSION_MMP"], "9.9.9")
        self.assertEqual(auto["PINEFORGE_VERSION_FULL"], full)
        self.assertTrue(full.startswith("9.9.9-1-g"), full)
        self.assertEqual(file_mode["PINEFORGE_VERSION_SOURCE"], "FILE")
        self.assertEqual(file_mode["_cache_source"], "FILE")
        self.assertEqual(file_mode["PINEFORGE_VERSION_MMP"], "0.14.0")
        self.assertEqual(file_mode["PINEFORGE_VERSION_FULL"], "0.14.0")
        self.assertEqual(file_mode["PINEFORGE_VERSION_MAJOR"], "0")
        self.assertEqual(file_mode["PINEFORGE_VERSION_MINOR"], "14")
        self.assertEqual(file_mode["PINEFORGE_VERSION_PATCH"], "0")
        self.assertEqual(auto["PINEFORGE_VERSION_GIT_SHA"], sha)
        self.assertEqual(file_mode["PINEFORGE_VERSION_GIT_SHA"], sha)
        self.assertEqual(auto["PINEFORGE_VERSION_DIRTY"], "OFF")
        self.assertEqual(file_mode["PINEFORGE_VERSION_DIRTY"], "OFF")
        self.assertNotEqual(file_mode["PINEFORGE_VERSION_FULL"], auto["PINEFORGE_VERSION_FULL"])

    def test_shallow_no_tag(self) -> None:
        origin = self.base / "origin"
        self.write_tree(origin, "2.3.4")
        self.init_repo(origin)
        self.git(origin, "tag", "v1.0.0")
        origin.joinpath("later.txt").write_text("later\n", encoding="utf-8")
        self.git(origin, "add", "later.txt")
        self.git(origin, "commit", "-m", "untagged head")
        clone = self.base / "shallow"
        # Local clones otherwise copy all objects and are not shallow.
        self.git(self.base, "clone", "--depth=1", "--no-tags", "--no-local",
                 str(origin), str(clone))
        tags = self.git(clone, "tag", "-l")
        self.assertEqual(tags, "")
        self.assertEqual(self.git(clone, "rev-parse", "--is-shallow-repository"), "true")
        self.assertTrue((clone / ".git" / "shallow").is_file())
        describe = subprocess.run(
            [self.git_bin, "describe", "--tags", "--match", "v*", "--abbrev=7", "--dirty"],
            cwd=clone, env=self.git_env(),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertNotEqual(describe.returncode, 0, describe.stdout + describe.stderr)
        self.assertIn("No names found", describe.stderr)
        sha = self.expected_sha(clone)
        self.assertEqual(len(self.git(clone, "rev-list", "--all").splitlines()), 1)
        auto = self.probe(clone, "shallow-auto", "-DPINEFORGE_VERSION_SOURCE=AUTO")
        file_mode = self.probe(clone, "shallow-file", "-DPINEFORGE_VERSION_SOURCE=FILE")
        for values, source in ((auto, "AUTO"), (file_mode, "FILE")):
            self.assertEqual(values["PINEFORGE_VERSION_SOURCE"], source)
            self.assertEqual(values["PINEFORGE_VERSION_MMP"], "2.3.4")
            self.assertEqual(values["PINEFORGE_VERSION_FULL"], "2.3.4")
            self.assertEqual(values["PINEFORGE_VERSION_GIT_SHA"], sha)
            self.assertEqual(values["PINEFORGE_VERSION_DIRTY"], "OFF")

    def test_archive_no_git(self) -> None:
        source = self.base / "archive"
        self.write_tree(source, "0.14.0")
        self.assertFalse((source / ".git").exists())
        auto = self.probe(source, "archive-auto")
        file_mode = self.probe(source, "archive-file", "-DPINEFORGE_VERSION_SOURCE=FILE")
        for values in (auto, file_mode):
            self.assertEqual(values["PINEFORGE_VERSION_MMP"], "0.14.0")
            self.assertEqual(values["PINEFORGE_VERSION_FULL"], "0.14.0")
            self.assertEqual(values["PINEFORGE_VERSION_GIT_SHA"], "unknown")
            self.assertEqual(values["PINEFORGE_VERSION_DIRTY"], "OFF")
        self.assertEqual(auto["PINEFORGE_VERSION_SOURCE"], "AUTO")
        self.assertEqual(file_mode["PINEFORGE_VERSION_SOURCE"], "FILE")

    def test_invalid_mode_fails_clearly(self) -> None:
        source = self.base / "invalid"
        self.write_tree(source, "0.14.0")
        self.init_repo(source)
        self.git(source, "tag", "v0.14.0")
        for index, mode in enumerate(("BOGUS", "auto", "file", "GIT", "AUTO FILE", "")):
            with self.subTest(mode=mode):
                log = self.configure_fail(
                    source, "invalid-" + str(index),
                    "-DPINEFORGE_VERSION_SOURCE=" + mode)
                self.assertIn("PINEFORGE_VERSION_SOURCE must be AUTO or FILE", log)
                self.assertIn("got '" + mode + "'", log)
                self.assertNotIn("Configuring done", log)

    def test_file_preserves_dirty_without_retagging_full(self) -> None:
        source = self.base / "dirty"
        self.write_tree(source, "3.2.1")
        self.init_repo(source)
        self.git(source, "tag", "v8.8.8")
        source.joinpath("CMakeLists.txt").write_text(
            source.joinpath("CMakeLists.txt").read_text(encoding="utf-8") + "\n# dirty\n",
            encoding="utf-8")
        full = self.expected_describe(source)
        sha = self.expected_sha(source)
        self.assertTrue(full.endswith("-dirty"), full)
        auto = self.probe(source, "dirty-auto", "-DPINEFORGE_VERSION_SOURCE=AUTO")
        file_mode = self.probe(source, "dirty-file", "-DPINEFORGE_VERSION_SOURCE=FILE")
        self.assertEqual(auto["PINEFORGE_VERSION_MMP"], "8.8.8")
        self.assertEqual(auto["PINEFORGE_VERSION_FULL"], full)
        self.assertEqual(auto["PINEFORGE_VERSION_DIRTY"], "ON")
        self.assertEqual(file_mode["PINEFORGE_VERSION_MMP"], "3.2.1")
        self.assertEqual(file_mode["PINEFORGE_VERSION_FULL"], "3.2.1")
        self.assertFalse(file_mode["PINEFORGE_VERSION_FULL"].endswith("-dirty"))
        self.assertEqual(file_mode["PINEFORGE_VERSION_DIRTY"], "ON")
        self.assertEqual(file_mode["PINEFORGE_VERSION_GIT_SHA"], sha)
        self.assertEqual(auto["PINEFORGE_VERSION_GIT_SHA"], sha)


if __name__ == "__main__":
    unittest.main()
