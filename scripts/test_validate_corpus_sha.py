#!/usr/bin/env python3
"""Unit tests for corpus records; no engine parity oracle is used."""
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

import validate_corpus_sha as validator


class CorpusRecordsTests(unittest.TestCase):
    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        self.root = Path(resources.enter_context(
            tempfile.TemporaryDirectory(prefix="corpus-records-")))
        probe = self.root / "validation" / "sample"
        probe.mkdir(parents=True)
        (self.root / "data").mkdir()
        (self.root / "data" / "ohlcv_ETH-USDT-USDT_1m.csv").write_bytes(
            b"timestamp,open,high,low,close,volume\n0,1,1,1,1,1\n")
        for filename in validator.ARTIFACT_FILES.values():
            (probe / filename).write_bytes(b"recorded bytes\n")
        self.write_records()

    def write_records(self):
        probe = self.root / "validation" / "sample"
        digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        self.manifest = {
            "kind": "pineforge-corpus-run-manifest",
            "schemaVersion": 2,
            "feedSha256": digest(
                self.root / "data" / "ohlcv_ETH-USDT-USDT_1m.csv"),
            "tapes": {"validation/sample": digest(probe / "engine_trades.csv")},
        }
        self.owned = {
            "schema_version": "pineforge-owned-corpus-manifest-v1",
            "entries": [{
                "corpus_id": "sample",
                "artifacts": {key: digest(probe / filename)
                              for key, filename in validator.ARTIFACT_FILES.items()},
            }],
        }
        self.save()

    def save(self):
        (self.root / "run_manifest.json").write_text(json.dumps(self.manifest))
        (self.root / "owned_strategies.json").write_text(json.dumps(self.owned))

    def test_records_bind_each_artifact_and_feed_to_bytes(self):
        self.assertEqual(validator.check_records(self.root), [])
        for filename in (*validator.ARTIFACT_FILES.values(),
                         "../../data/ohlcv_ETH-USDT-USDT_1m.csv"):
            with self.subTest(filename=filename):
                path = self.root / "validation" / "sample" / filename
                previous = path.read_bytes()
                path.write_bytes(previous + b"x")
                errors = validator.check_records(self.root)
                self.assertTrue(errors)
                self.assertIn(path.resolve().name, "\n".join(errors))
                path.write_bytes(previous)

    def test_renamed_directory_reports_missing_and_unrecorded(self):
        (self.root / "validation" / "sample").rename(
            self.root / "validation" / "renamed")
        errors = "\n".join(validator.check_records(self.root))
        self.assertIn("validation/sample", errors)
        self.assertIn("validation/renamed", errors)

    def test_empty_population_and_wrong_schemas_are_invalid(self):
        for kind in ("tapes", "kind", "schemaVersion"):
            previous = self.manifest[kind]
            self.manifest[kind] = {} if kind == "tapes" else "wrong"
            self.save()
            self.assertTrue(validator.check_records(self.root))
            self.manifest[kind] = previous
        self.owned["entries"] = []
        self.save()
        self.assertTrue(validator.check_records(self.root))

    def test_lfs_pointer_is_checked_against_materialized_bytes(self):
        feed = self.root / "data" / "ohlcv_ETH-USDT-USDT_1m.csv"
        oid = hashlib.sha256(feed.read_bytes()).hexdigest()
        pointer = (f"version https://git-lfs.github.com/spec/v1\n"
                   f"oid sha256:{oid}\nsize {feed.stat().st_size}\n").encode()
        self.assertIsNone(validator.lfs_problem(feed, pointer))
        self.assertIn(str(feed), validator.lfs_problem(
            feed, pointer.replace(oid.encode(), b"0" * 64)))
        feed.write_bytes(pointer)
        self.assertIn("materialized", validator.lfs_problem(feed, pointer))

    def test_subset_rejects_empty_duplicate_and_unknown_probes(self):
        path = self.root / "subset.txt"
        for text in ("# empty\n", "validation/sample\nvalidation/sample\n",
                     "validation/missing\n", "../sample\n"):
            with self.subTest(text=text):
                path.write_text(text)
                with self.assertRaises(ValueError):
                    validator.read_subset(path, {"validation/sample"})
        path.write_text("validation/sample # mechanism\n")
        self.assertEqual(validator.read_subset(
            path, {"validation/sample"}), ["validation/sample"])


def git(root, *arguments):
    return subprocess.run(
        ["git", "-C", str(root), "-c", "user.name=Corpus tests",
         "-c", "user.email=corpus-tests@example.invalid",
         "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
         *arguments], check=True, capture_output=True, text=True,
    ).stdout.strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class CorpusValidatorMainTests(unittest.TestCase):
    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        self.directory = Path(resources.enter_context(
            tempfile.TemporaryDirectory(prefix="corpus-main-")))
        self.root = self.directory / "corpus"
        self.engine = self.directory / "engine"
        self.root.mkdir()
        self.engine.mkdir()
        git(self.root, "init", "-q")
        git(self.root, "lfs", "install", "--local", "--skip-smudge", "--skip-repo")
        feed = self.root / validator.FEED
        feed.parent.mkdir()
        feed.write_bytes(b"timestamp,open,high,low,close,volume\n0,1,1,1,1,1\n")
        (self.root / ".gitattributes").write_text(
            f"{validator.FEED} filter=lfs diff=lfs merge=lfs -text\n")
        self.lfs_files = [feed]
        self.owned_probe = self.root / "validation" / "sample"
        self.owned_probe.mkdir(parents=True)
        for filename in validator.ARTIFACT_FILES.values():
            (self.owned_probe / filename).write_bytes(b"recorded bytes\n")
        self.unowned_probe = self.root / "validation" / "additional"
        self.unowned_probe.mkdir()
        (self.unowned_probe / "engine_trades.csv").write_bytes(b"additional tape\n")
        self.manifest = {
            "kind": "pineforge-corpus-run-manifest",
            "schemaVersion": 2,
            "feedSha256": digest(feed),
            "tapes": {probe.relative_to(self.root).as_posix():
                      digest(probe / "engine_trades.csv")
                      for probe in (self.owned_probe, self.unowned_probe)},
        }
        self.owned = {
            "schema_version": "pineforge-owned-corpus-manifest-v1",
            "entries": [{
                "corpus_id": self.owned_probe.name,
                "artifacts": {key: digest(self.owned_probe / filename)
                              for key, filename in validator.ARTIFACT_FILES.items()},
            }],
        }
        (self.root / "run_manifest.json").write_text(json.dumps(self.manifest))
        (self.root / "owned_strategies.json").write_text(json.dumps(self.owned))
        self.commit("Record corpus fixture")

    def commit(self, message):
        git(self.root, "add", "--all")
        git(self.root, "commit", "-q", "-m", message)

    def invoke(self, *arguments, corpus=None):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            code = validator.main([
                "--corpus", str(corpus or self.root), "--engine", str(self.engine),
                *arguments,
            ])
        return code, output.getvalue()

    def assert_outcome(self, result, expected):
        code, output = result
        self.assertEqual(code, expected, output)
        self.assertEqual(
            [line for line in output.splitlines() if line.startswith("RESULT ")],
            [f"RESULT {('valid', 'invalid', 'error')[code]}"])
        self.assertNotIn("Traceback", output)
        if code:
            self.assertTrue(any(line.startswith("FINDING ")
                                for line in output.splitlines()), output)
        return output

    def test_records_only_valid_checkout(self):
        output = self.assert_outcome(self.invoke("--records-only"), 0)
        expected = {
            "tapes": len(self.manifest["tapes"]),
            "owned_entries": len(self.owned["entries"]),
            "owned_hashes": sum(len(entry["artifacts"]) for entry in self.owned["entries"]),
            "lfs_pointers": len(self.lfs_files),
            "feed": len(self.lfs_files),
        }
        self.assertIn("COUNT " + " ".join(f"{key}={value}"
                                         for key, value in expected.items()), output)
        self.assertIn("CLEAN corpus tree unchanged", output)
        self.assertNotIn("FINDING ", output)

    def test_unowned_tape_flipped_byte_is_invalid(self):
        self.assertNotIn(self.unowned_probe.name,
                         [entry["corpus_id"] for entry in self.owned["entries"]])
        path = self.unowned_probe / "engine_trades.csv"
        previous = path.read_bytes()
        path.write_bytes(bytes([previous[0] ^ 1]) + previous[1:])
        self.commit("Change an unowned tape byte")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("differs from recorded", output)

    def test_owned_artifact_flipped_byte_is_invalid(self):
        path = self.owned_probe / "strategy.pine"
        previous = path.read_bytes()
        path.write_bytes(bytes([previous[0] ^ 1]) + previous[1:])
        self.commit("Change an owned artifact byte")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("differs from recorded", output)

    def test_work_is_required_for_builds(self):
        output = self.assert_outcome(self.invoke(), 2)
        self.assertIn("--work is required", output)

    def test_work_inside_either_tree_is_refused(self):
        for tree in (self.root, self.engine):
            with self.subTest(tree=tree.name):
                output = self.assert_outcome(self.invoke("--work", str(tree / "work")), 2)
                self.assertIn("--work must be outside", output)

    def test_jobs_outside_worker_limit_are_errors(self):
        for jobs in (0, 9):
            with self.subTest(jobs=jobs):
                output = self.assert_outcome(
                    self.invoke("--records-only", "--jobs", str(jobs)), 2)
                self.assertIn("--jobs must be between 1 and 8", output)

    def test_corpus_must_be_a_git_checkout(self):
        plain = self.directory / "plain-corpus"
        plain.mkdir()
        output = self.assert_outcome(self.invoke("--records-only", corpus=plain), 2)
        self.assertIn("FINDING setup", output)

    def test_checkout_dirty_before_validation_is_an_error(self):
        path = self.owned_probe / "README.md"
        path.write_bytes(path.read_bytes() + b"dirty\n")
        output = self.assert_outcome(self.invoke("--records-only"), 2)
        self.assertIn("FINDING clean ", output)
        self.assertIn("dirty before validation", output)
        self.assertIn(path.relative_to(self.root).as_posix(), output)

    def test_checkout_dirtied_during_validation_is_an_error(self):
        marker = self.directory / "status-seen"
        changed = self.root / "changed-during-validation"
        hook = self.directory / "filesystem-monitor"
        hook.write_text(
            "#!/bin/sh\n"
            f"if [ -f {shlex.quote(str(marker))} ]; then\n"
            f"  : > {shlex.quote(str(changed))}\n"
            "else\n"
            f"  : > {shlex.quote(str(marker))}\n"
            "fi\n"
            "printf 'corpus-token\\000/\\000'\n")
        hook.chmod(0o755)
        git(self.root, "config", "core.fsmonitorHookVersion", "2")
        git(self.root, "config", "core.fsmonitor", str(hook))
        output = self.assert_outcome(self.invoke("--records-only"), 2)
        self.assertIn("FINDING clean ", output)
        self.assertNotIn("dirty before validation", output)
        self.assertIn("dirty after validation", output)
        self.assertIn(changed.name, output)
        self.assertTrue(changed.is_file())

    def test_tracked_file_and_directory_symlinks_are_invalid(self):
        file_link = self.owned_probe / "extra-file"
        directory_link = self.owned_probe / "extra-directory"
        file_link.symlink_to("README.md")
        directory_link.symlink_to("../../data", target_is_directory=True)
        self.commit("Record file and directory symlinks")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        for path in (file_link, directory_link):
            with self.subTest(path=path.name):
                self.assertIn(f"FINDING records {path}: tracked symlink", output)
        self.assertTrue(file_link.is_symlink())
        self.assertTrue(directory_link.is_symlink())

    def test_engine_commit_is_reported(self):
        git(self.engine, "init", "-q")
        (self.engine / "version.txt").write_text("engine fixture\n")
        git(self.engine, "add", "version.txt")
        git(self.engine, "commit", "-q", "-m", "Record engine fixture")
        expected = git(self.engine, "rev-parse", "HEAD")
        output = self.assert_outcome(self.invoke("--records-only"), 0)
        lines = output.splitlines()
        self.assertEqual([line for line in lines if line.startswith("ENGINE ")],
                         [f"ENGINE {expected}"])
        self.assertLess(lines.index(f"ENGINE {expected}"),
                        next(index for index, line in enumerate(lines)
                             if line.startswith("COUNT ")))

    def test_engine_plain_directory_is_reported_unknown(self):
        output = self.assert_outcome(self.invoke("--records-only"), 0)
        lines = output.splitlines()
        self.assertEqual([line for line in lines if line.startswith("ENGINE ")],
                         ["ENGINE unknown"])
        self.assertLess(lines.index("ENGINE unknown"),
                        next(index for index, line in enumerate(lines)
                             if line.startswith("COUNT ")))


class ProbeWorkerTests(unittest.TestCase):
    def test_worker_exception_preserves_other_results(self):
        items = ["first", "broken", "last", "clean"]
        normal_findings = {item: f"header {item}: mismatch"
                           for item in items if item not in ("broken", "clean")}

        def work(item):
            if item == "broken":
                raise RuntimeError("probe work interrupted")
            return normal_findings.get(item)

        findings, error = validator.collect_probe_results(work, items, jobs=2)
        self.assertTrue(error)
        for finding in normal_findings.values():
            self.assertIn(finding, findings)
        self.assertEqual(len(findings), len(normal_findings) + 1)
        self.assertTrue(any("broken" in finding
                            and "RuntimeError: probe work interrupted" in finding
                            for finding in findings), findings)


if __name__ == "__main__":
    unittest.main(verbosity=2)
