#!/usr/bin/env python3
"""Deterministic controls for source clones racing with pytest cache removal."""
from pathlib import Path
import re
import shutil
import tempfile
import unittest

import source_clone


ROOT = Path(__file__).resolve().parents[1]


def add_test_statements(text):
    """Read balanced add_test calls, respecting CMake comments and literals."""
    tokens = re.compile(
        r'(?P<comment>#\[(?P<comment_equals>=*)\[.*?\](?P=comment_equals)\]|#[^\n]*)'
        r'|"(?:\\.|[^"\\])*"'
        r'|\[(?P<equals>=*)\[.*?\](?P=equals)\]'
        r'|\\.'
        r'|(?P<add>\badd_test\s*\()'
        r'|[()]',
        re.DOTALL | re.IGNORECASE,
    )
    start = None
    depth = 0
    pieces = []
    cursor = 0
    for token in tokens.finditer(text):
        if token.group("comment") is not None:
            if start is not None:
                pieces.extend((text[cursor:token.start()], " "))
                cursor = token.end()
            continue
        if token.group("add") is not None:
            if start is None:
                start = token.start()
                cursor = start
                pieces = []
            depth += 1
        elif start is not None and token.group() == "(":
            depth += 1
        elif start is not None and token.group() == ")":
            depth -= 1
            if depth == 0:
                pieces.append(text[cursor:token.end()])
                yield "".join(pieces)
                start = None
    if start is not None:
        raise AssertionError("unterminated add_test statement")


class SourceCloneVolatile(unittest.TestCase):
    def make_source(self, parent):
        source = parent / "source"
        ordinary = {
            "README.txt": b"ordinary source text\n",
            "main.py": b"print('source')\n",
            "subdir/payload.bin": b"\x00\x01\x7f\x80\xff\n",
        }
        caches = {
            "pytest-cache-files-x1/first": b"first temporary cache file\n",
            "pytest-cache-files-x1/second": b"second temporary cache file\n",
            ".pytest_cache/v/cache/nodeids": b"[]\n",
            "subdir/pytest-cache-files-x2/entry": b"nested temporary cache\n",
            "subdir/.pytest_cache/v/cache/nodeids": b"[]\n",
        }
        for relative, data in {**ordinary, **caches}.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return source, ordinary

    @staticmethod
    def cache_directories(source):
        return sorted(
            (path for path in source.rglob("*")
             if path.is_dir() and (path.name == ".pytest_cache"
                                  or path.match("pytest-cache-files-*"))),
            key=lambda path: len(path.parts), reverse=True,
        )

    def vanishing_copy(self, source):
        calls = []
        removed = []

        def copy_file(src, dst):
            calls.append(Path(src))
            if len(calls) == 1:
                # copytree has already listed the source entries. If it entered
                # a cache first, deleting this very file still forces the control
                # to fail; traversal order cannot decide whether the race occurs.
                directories = self.cache_directories(source)
                self.assertTrue(directories, "race fixture has no cache directories")
                for directory in directories:
                    shutil.rmtree(directory)
                    removed.append(directory)
                self.assertTrue(all(not path.exists() for path in removed))
                self.assertEqual(self.cache_directories(source), [])
            return shutil.copy2(src, dst)

        return copy_file, calls, removed

    def assert_vanished(self, source, calls, removed, original_caches):
        self.assertGreater(len(calls), 0, "the vanishing copy function never ran")
        self.assertEqual(set(removed), set(original_caches))
        self.assertTrue(all(not path.exists() for path in original_caches))
        self.assertEqual(self.cache_directories(source), [])

    def assert_ordinary_files(self, destination, expected):
        actual = {
            path.relative_to(destination).as_posix(): path.read_bytes()
            for path in destination.rglob("*") if path.is_file()
        }
        self.assertEqual(actual, expected)
        self.assertEqual(self.cache_directories(destination), [])

    def test_vanishing_pytest_cache_dir_is_survived(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            source, ordinary = self.make_source(parent)
            original_caches = self.cache_directories(source)
            copy_file, calls, removed = self.vanishing_copy(source)
            destination = parent / "clone"
            source_clone.clone_source_tree(
                source, destination, copy_function=copy_file)
            self.assert_vanished(source, calls, removed, original_caches)
            self.assert_ordinary_files(destination, ordinary)

    def test_control_the_old_list_fails_the_same_race(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            source, _ = self.make_source(parent)
            original_caches = self.cache_directories(source)
            copy_file, calls, removed = self.vanishing_copy(source)
            with self.assertRaises(
                    shutil.Error,
                    msg="the race harness does not reproduce the failure") as caught:
                shutil.copytree(
                    source, parent / "clone",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
                    copy_function=copy_file)
            self.assert_vanished(source, calls, removed, original_caches)
            self.assertTrue(any(
                ("pytest-cache-files-" in src or ".pytest_cache" in src)
                and "[Errno 2]" in diagnostic
                for src, _, diagnostic in caught.exception.args[0]),
                str(caught.exception))

    def test_ordinary_files_are_not_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory)
            source = parent / "source"
            near_names = {
                "pytest_cache_notes.md": b"cache documentation\n",
                "my-pytest-cache-files.txt": b"ordinary notes\n",
                "cache.py": b"VALUE = 7\n",
                "pytest_cache_files_data/entry": b"ordinary directory data\n",
            }
            for relative, data in near_names.items():
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            destination = parent / "clone"
            source_clone.clone_source_tree(source, destination)
            self.assert_ordinary_files(destination, near_names)

    def test_every_tree_cloning_guard_uses_the_helper(self):
        # A fifth guard that clones ROOT must be added to this list.
        guards = (
            "test_pending_intent_view_mutations.py",
            "test_broker_state_hash_coverage.py",
            "test_market_admission_schema.py",
            "test_aggregate_cpp_versions.py",
        )
        for guard in guards:
            with self.subTest(guard=guard):
                text = (ROOT / "scripts" / guard).read_text()
                self.assertIn("clone_source_tree(", text)
                self.assertIn("import source_clone", text)
                self.assertNotRegex(text, r"shutil\s*\.\s*copytree\s*\(\s*ROOT\b")
        raw_clones = {
            path.name for path in (ROOT / "scripts").glob("test_*.py")
            if re.search(r"\bcopytree\s*\(\s*ROOT\s*,", path.read_text())
        }
        self.assertEqual(raw_clones, set(), "whole-tree clones must use source_clone")

    def test_every_pytest_ctest_row_disables_the_cache_provider(self):
        paths = [ROOT / "tests" / "CMakeLists.txt"]
        paths.extend(sorted((ROOT / "tests" / "cr150.d").glob("*.cmake")))
        pytest_rows = []
        for path in paths:
            for statement in add_test_statements(path.read_text()):
                if not re.search(r"(?<!\S)-m\s+pytest\b", statement):
                    continue
                name = re.search(r"\bNAME\s+([^\s)]+)", statement, re.IGNORECASE)
                self.assertIsNotNone(name, f"pytest row has no NAME: {statement}")
                row = name.group(1)
                pytest_rows.append(row)
                with self.subTest(path=str(path.relative_to(ROOT)), row=row):
                    self.assertIn(
                        "no:cacheprovider", statement,
                        f"{row}: pytest must disable its source-tree cache provider")
        self.assertGreaterEqual(
            len(pytest_rows), 20, "the CMake parser missed the pytest registrations")


if __name__ == "__main__":
    unittest.main()
