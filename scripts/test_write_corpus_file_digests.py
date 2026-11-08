#!/usr/bin/env python3
"""Tests of digest writing on real temporary Git repositories."""

from contextlib import ExitStack, redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

import write_corpus_file_digests as writer


def git(root, *arguments):
    return subprocess.run(
        ["git", "-C", str(root), "-c", "user.name=Corpus tests",
         "-c", "user.email=corpus-tests@example.invalid",
         "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
         *arguments], check=True, capture_output=True, text=True,
    ).stdout


class CorpusFileDigestWriterTests(unittest.TestCase):
    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        self.directory = Path(resources.enter_context(
            tempfile.TemporaryDirectory(prefix="corpus-file-digests-")))
        self.root = self.directory / "corpus"
        self.root.mkdir()
        git(self.root, "init", "-q")
        (self.root / "nested").mkdir()
        (self.root / "alpha.txt").write_bytes(b"first tracked file\n")
        (self.root / "nested" / "second.bin").write_bytes(b"\x00second file\xff\n")
        (self.root / ".gitignore").write_text("untracked-*\n")
        self.output = self.root / "file_digests.json"
        self.commit("Record the writer fixture")

    def commit(self, message):
        git(self.root, "add", "--all")
        git(self.root, "commit", "-q", "-m", message)

    def invoke(self, *arguments, corpus=None):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            code = writer.main(["--corpus", str(corpus or self.root), *arguments])
        return code, output.getvalue()

    def assert_written(self, result):
        code, output = result
        self.assertEqual(code, 0, output)

    def assert_refused(self, reason, path):
        code, output = self.invoke()
        self.assertEqual(code, 1, output)
        self.assertIn(reason, output)
        self.assertIn(str(path), output)
        self.assertFalse(self.output.exists(), output)
        return output

    def test_deterministic_bytes_ignore_previous_manifest_contents(self):
        self.assert_written(self.invoke())
        first = self.output.read_bytes()
        record = json.loads(first)
        self.assertEqual(first, (json.dumps(record, sort_keys=True, indent=1) + "\n").encode())
        self.assert_written(self.invoke())
        self.assertEqual(self.output.read_bytes(), first)
        self.commit("Track the digest manifest")
        self.output.write_bytes(b"previous output must not be an input\n")
        self.assert_written(self.invoke())
        self.assertEqual(self.output.read_bytes(), first)

    def test_output_lists_exactly_tracked_files_without_itself(self):
        self.assert_written(self.invoke())
        self.commit("Track the digest manifest")
        self.assert_written(self.invoke())
        tracked = set(git(self.root, "ls-files", "-z").rstrip("\0").split("\0"))
        record = json.loads(self.output.read_bytes())
        self.assertEqual(record["schema_version"], "pineforge-corpus-file-digests-v1")
        self.assertEqual(set(record["files"]), tracked - {self.output.name})
        for name, expected in record["files"].items():
            with self.subTest(name=name):
                self.assertEqual(expected, hashlib.sha256((self.root / name).read_bytes()).hexdigest())
                self.assertRegex(expected, r"^[0-9a-f]{64}$")

    def test_ignored_untracked_file_is_not_listed(self):
        # A nonignored untracked file makes the checkout dirty. An ignored
        # untracked file leaves it clean and still must not enter the census.
        path = self.root / "untracked-not-an-input.txt"
        path.write_bytes(b"not tracked\n")
        self.assertEqual(git(self.root, "status", "--porcelain"), "")
        self.assert_written(self.invoke())
        self.assertNotIn(path.name, json.loads(self.output.read_bytes())["files"])

    def test_dirty_tracked_bytes_are_refused(self):
        path = self.root / "alpha.txt"
        path.write_bytes(path.read_bytes() + b"dirty\n")
        self.assert_refused("dirty", path)

    def test_nonignored_untracked_file_is_refused_as_dirty(self):
        path = self.root / "unexpected.txt"
        path.write_bytes(b"not committed\n")
        self.assert_refused("dirty", path)

    def test_symlink_is_refused_with_its_mode(self):
        path = self.root / "link.txt"
        path.symlink_to("alpha.txt")
        self.commit("Record a symlink")
        output = self.assert_refused("symlink", path)
        mode = git(self.root, "ls-files", "--stage", "--", path.name).split()[0]
        self.assertIn(mode, output)

    def test_executable_is_refused_with_its_mode(self):
        path = self.root / "alpha.txt"
        path.chmod(0o755)
        self.commit("Record an executable")
        output = self.assert_refused("executable", path)
        mode = git(self.root, "ls-files", "--stage", "--", path.name).split()[0]
        self.assertIn(mode, output)

    def test_gitlink_is_refused_with_its_mode(self):
        relative = "nested-repository"
        revision = git(self.root, "rev-parse", "HEAD").strip()
        git(self.root, "update-index", "--add", "--cacheinfo",
            "160000", revision, relative)
        git(self.root, "commit", "-q", "-m", "Record a gitlink")
        output = self.assert_refused("gitlink", self.root / relative)
        mode = git(self.root, "ls-files", "--stage", "--", relative).split()[0]
        self.assertIn(mode, output)

    def test_unmaterialized_lfs_pointer_is_refused(self):
        path = self.root / "pointer.csv"
        content = (self.root / "alpha.txt").read_bytes()
        oid = hashlib.sha256(content).hexdigest()
        path.write_text("version https://git-lfs.github.com/spec/v1\n"
                        f"oid sha256:{oid}\nsize {len(content)}\n")
        self.commit("Record an unmaterialized pointer")
        self.assert_refused("Git-LFS content is not materialized", path)

    def test_tracked_path_with_space_is_refused(self):
        path = self.root / "with space.txt"
        path.write_bytes(b"unusable path\n")
        self.commit("Record a path with a space")
        self.assert_refused("path", path)

    def test_explicit_output_outside_corpus_does_not_change_inputs(self):
        path = self.directory / "output.json"
        self.assert_written(self.invoke("--output", str(path)))
        first = path.read_bytes()
        path.write_bytes(b"previous output\n")
        self.assert_written(self.invoke("--output", str(path)))
        self.assertEqual(path.read_bytes(), first)
        self.assertEqual(git(self.root, "status", "--porcelain"), "")
        self.assertFalse(self.output.exists())

    def test_nonrepository_is_an_execution_error(self):
        plain = self.directory / "plain"
        plain.mkdir()
        code, output = self.invoke(corpus=plain)
        self.assertEqual(code, 2, output)
        self.assertIn(str(plain), output)
        self.assertFalse((plain / self.output.name).exists())

    def test_missing_required_argument_is_an_execution_error(self):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            code = writer.main([])
        self.assertEqual(code, 2, output.getvalue())
        self.assertIn("--corpus", output.getvalue())
        self.assertFalse(self.output.exists())

    def test_explicit_output_cannot_replace_a_tracked_input(self):
        path = self.root / "alpha.txt"
        previous = path.read_bytes()
        code, output = self.invoke("--output", str(path))
        self.assertEqual(code, 1, output)
        self.assertIn(str(path), output)
        self.assertIn("tracked", output)
        self.assertEqual(path.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main(verbosity=2)
