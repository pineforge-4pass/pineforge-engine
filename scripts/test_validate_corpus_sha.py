#!/usr/bin/env python3
"""Unit tests for corpus records; no engine parity oracle is used."""
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import validate_corpus_sha as validator
import write_corpus_file_digests as writer


class CorpusRecordsTests(unittest.TestCase):
    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        self.root = Path(resources.enter_context(
            tempfile.TemporaryDirectory(prefix="corpus-records-")))
        git(self.root, "init", "-q")
        probe = self.root / "validation" / "sample"
        probe.mkdir(parents=True)
        (self.root / "data").mkdir()
        (self.root / "data" / "ohlcv_ETH-USDT-USDT_1m.csv").write_bytes(
            b"timestamp,open,high,low,close,volume\n0,1,1,1,1,1\n")
        for filename in validator.ARTIFACT_FILES.values():
            (probe / filename).write_bytes(b"recorded bytes\n")
        self.write_records()
        self.rewrite_file_digests()

    def rewrite_file_digests(self):
        git(self.root, "add", "--all")
        git(self.root, "commit", "-q", "-m", "Record corpus records fixture")
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            code = writer.main(["--corpus", str(self.root)])
        self.assertEqual(code, 0, output.getvalue())
        # Only the first refresh introduces a new tracked file. Later refreshes
        # need not commit an identical manifest.
        git(self.root, "add", "--all")
        if git(self.root, "diff", "--cached", "--name-only"):
            git(self.root, "commit", "-q", "-m", "Record file digests")

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
                self.rewrite_file_digests()
                errors = validator.check_records(self.root)
                self.assertTrue(errors)
                self.assertIn(path.resolve().name, "\n".join(errors))
                self.assertTrue(any(
                    error.startswith(f"records {path.resolve()}: sha256 ")
                    and "differs from recorded" in error for error in errors), errors)
                path.write_bytes(previous)
                self.rewrite_file_digests()

    def test_renamed_directory_reports_missing_and_unrecorded(self):
        (self.root / "validation" / "sample").rename(
            self.root / "validation" / "renamed")
        self.rewrite_file_digests()
        errors = "\n".join(validator.check_records(self.root))
        self.assertIn("validation/sample", errors)
        self.assertIn("validation/renamed", errors)
        self.assertIn(
            f"records {self.root / 'validation/sample'}: recorded probe directory is missing",
            errors)
        self.assertIn(
            f"records {self.root / 'validation/renamed'}: directory is unrecorded in run_manifest.json",
            errors)

    def test_empty_population_and_wrong_schemas_are_invalid(self):
        for kind in ("tapes", "kind", "schemaVersion"):
            previous = self.manifest[kind]
            self.manifest[kind] = {} if kind == "tapes" else "wrong"
            self.save()
            self.rewrite_file_digests()
            errors = validator.check_records(self.root)
            self.assertTrue(errors)
            reason = ("tapes must be a nonempty map" if kind == "tapes"
                      else f"unsupported {kind}")
            self.assertIn(f"records {self.root / 'run_manifest.json'}: {reason}", errors)
            self.manifest[kind] = previous
        self.owned["entries"] = []
        self.save()
        self.rewrite_file_digests()
        errors = validator.check_records(self.root)
        self.assertTrue(errors)
        self.assertIn(
            f"records {self.root / 'owned_strategies.json'}: entries must be a nonempty list",
            errors)

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
        (self.root / ".gitignore").write_text("untracked-*\n")
        self.lfs_files = [feed]
        self.owned_probe = self.root / "validation" / "sample"
        self.owned_probe.mkdir(parents=True)
        for filename in validator.ARTIFACT_FILES.values():
            (self.owned_probe / filename).write_bytes(b"recorded bytes\n")
        self.unowned_probe = self.root / "validation" / "additional"
        self.unowned_probe.mkdir()
        (self.unowned_probe / "engine_trades.csv").write_bytes(b"additional tape\n")
        (self.unowned_probe / "tv_trades.csv").write_bytes(b"additional export\n")
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
        self.rewrite_file_digests()

    def commit(self, message):
        git(self.root, "add", "--all")
        git(self.root, "commit", "-q", "-m", message)

    def rewrite_file_digests(self):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            code = writer.main(["--corpus", str(self.root)])
        self.assertEqual(code, 0, output.getvalue())
        self.commit("Record file digests")

    def file_digests(self):
        return json.loads((self.root / "file_digests.json").read_text())

    def save_file_digests(self, record):
        (self.root / "file_digests.json").write_text(json.dumps(record))
        self.commit("Change the file digest record")

    def count_fields(self, output):
        lines = [line for line in output.splitlines() if line.startswith("COUNT ")]
        self.assertEqual([line.split()[0] for line in lines], ["COUNT"], output)
        return {key: int(value) for key, value in
                (field.split("=", 1) for field in lines[0].split()[1:])}

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
        tracked = set(git(self.root, "ls-files", "-z").rstrip("\0").split("\0"))
        expected = {
            "tapes": len(self.manifest["tapes"]),
            "owned_entries": len(self.owned["entries"]),
            "owned_hashes": sum(len(entry["artifacts"]) for entry in self.owned["entries"]),
            "lfs_pointers": len(self.lfs_files),
            "feed": len(self.lfs_files),
            "tracked": len(tracked),
            "bound": len(tracked - {"file_digests.json"}),
        }
        self.assertIn("COUNT " + " ".join(f"{key}={value}"
                                         for key, value in expected.items()), output)
        self.assertEqual(self.count_fields(output), expected)
        self.assertIn("CLEAN corpus tree unchanged", output)
        self.assertNotIn("FINDING ", output)

    def test_unowned_tape_flipped_byte_is_invalid(self):
        self.assertNotIn(self.unowned_probe.name,
                         [entry["corpus_id"] for entry in self.owned["entries"]])
        path = self.unowned_probe / "engine_trades.csv"
        previous = path.read_bytes()
        path.write_bytes(bytes([previous[0] ^ 1]) + previous[1:])
        self.commit("Change an unowned tape byte")
        self.rewrite_file_digests()
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("differs from recorded", output)

    def test_owned_artifact_flipped_byte_is_invalid(self):
        path = self.owned_probe / "strategy.pine"
        previous = path.read_bytes()
        path.write_bytes(bytes([previous[0] ^ 1]) + previous[1:])
        self.commit("Change an owned artifact byte")
        self.rewrite_file_digests()
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("differs from recorded", output)

    def test_unowned_export_flipped_byte_is_invalid(self):
        self.assertNotIn(self.unowned_probe.name,
                         [entry["corpus_id"] for entry in self.owned["entries"]])
        path = self.unowned_probe / "tv_trades.csv"
        recorded_paths = {f"{name}/engine_trades.csv" for name in self.manifest["tapes"]}
        recorded_paths.update(
            f"validation/{entry['corpus_id']}/{filename}"
            for entry in self.owned["entries"] for filename in validator.ARTIFACT_FILES.values())
        self.assertNotIn(path.relative_to(self.root).as_posix(), recorded_paths)
        previous = path.read_bytes()
        path.write_bytes(bytes([previous[0] ^ 1]) + previous[1:])
        self.commit("Change an export outside the owned records")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("differs from recorded", output)

    def test_new_tracked_file_without_a_digest_is_invalid(self):
        path = self.root / "additional.txt"
        path.write_bytes(b"new tracked bytes\n")
        self.commit("Add a file without rewriting the manifest")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("tracked but unlisted", output)

    def test_listed_file_removed_from_repository_is_invalid_and_not_counted(self):
        path = self.unowned_probe / "tv_trades.csv"
        recorded = self.file_digests()["files"]
        relative = path.relative_to(self.root).as_posix()
        git(self.root, "rm", "--", relative)
        git(self.root, "commit", "-q", "-m", "Remove a listed file")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("listed but untracked", output)
        self.assertEqual(self.count_fields(output)["bound"],
                         len(set(recorded) - {relative}))

    def test_listed_path_that_was_never_tracked_is_invalid(self):
        source = self.unowned_probe / "tv_trades.csv"
        path = self.root / "untracked-never-recorded.txt"
        path.write_bytes(source.read_bytes())
        record = self.file_digests()
        record["files"][path.name] = record["files"][
            source.relative_to(self.root).as_posix()]
        self.save_file_digests(record)
        self.assertNotIn(path.name, git(self.root, "ls-files").splitlines())
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("listed but untracked", output)

    def test_tape_digest_disagreement_names_both_records(self):
        name = self.unowned_probe.relative_to(self.root).as_posix()
        replacement = digest(self.unowned_probe / "tv_trades.csv")
        self.assertNotEqual(replacement, self.manifest["tapes"][name])
        self.manifest["tapes"][name] = replacement
        (self.root / "run_manifest.json").write_text(json.dumps(self.manifest))
        self.commit("Change only the tape record")
        self.rewrite_file_digests()
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertTrue(any("run_manifest.json" in line
                            and "file_digests.json" in line
                            and f"{name}/engine_trades.csv" in line
                            for line in output.splitlines()), output)

    def test_owned_digest_disagreement_names_both_records(self):
        original = dict(self.owned["entries"][0]["artifacts"])
        replacement = digest(self.unowned_probe / "tv_trades.csv")
        for key, filename in validator.ARTIFACT_FILES.items():
            with self.subTest(artifact=key):
                source = self.owned_probe / filename
                self.assertNotEqual(replacement, original[key])
                self.owned["entries"][0]["artifacts"] = {**original, key: replacement}
                (self.root / "owned_strategies.json").write_text(json.dumps(self.owned))
                self.commit("Change only an owned artifact record")
                self.rewrite_file_digests()
                output = self.assert_outcome(self.invoke("--records-only"), 1)
                self.assertTrue(any("owned_strategies.json" in line
                                    and "file_digests.json" in line
                                    and source.relative_to(self.root).as_posix() in line
                                    for line in output.splitlines()), output)

    def test_feed_digest_disagreement_names_both_records(self):
        replacement = digest(self.unowned_probe / "tv_trades.csv")
        self.assertNotEqual(replacement, self.manifest["feedSha256"])
        self.manifest["feedSha256"] = replacement
        (self.root / "run_manifest.json").write_text(json.dumps(self.manifest))
        self.commit("Change only the feed record")
        self.rewrite_file_digests()
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertTrue(any("run_manifest.json" in line
                            and "feedSha256" in line
                            and "file_digests.json" in line
                            and validator.FEED in line
                            for line in output.splitlines()), output)

    def test_missing_file_digest_manifest_is_invalid(self):
        path = self.root / "file_digests.json"
        git(self.root, "rm", "--", path.name)
        git(self.root, "commit", "-q", "-m", "Remove the digest manifest")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertIn(str(path), output)
        self.assertIn("missing", output)

    def test_ignored_untracked_manifest_is_invalid(self):
        path = self.root / "file_digests.json"
        git(self.root, "rm", "--cached", "--", path.name)
        ignore = self.root / ".gitignore"
        ignore.write_text(ignore.read_text() + path.name + "\n")
        self.commit("Leave the manifest ignored and untracked")
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(writer.main(["--corpus", str(self.root)]), 0)
        self.assertEqual(git(self.root, "status", "--porcelain"), "")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertTrue(any(str(path) in line and "must be tracked" in line
                            for line in output.splitlines()), output)

    def test_census_difference_lists_are_capped_per_kind(self):
        limit = 50
        old = {f"old-{index}.txt" for index in range(limit + len(self.manifest["tapes"]))}
        new = {name.replace("old-", "new-", 1) for name in old}
        for name in old:
            (self.root / name).write_bytes(b"census input\n")
        self.commit("Record the initial census population")
        self.rewrite_file_digests()
        git(self.root, "rm", "--", *sorted(old))
        for name in new:
            (self.root / name).write_bytes(b"census input\n")
        self.commit("Replace the census population without a manifest rewrite")
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        for kind, paths in (("tracked but unlisted", new), ("listed but untracked", old)):
            with self.subTest(kind=kind):
                lines = [line for line in output.splitlines() if kind in line]
                summary = [line for line in lines if ": and " in line]
                self.assertEqual(
                    summary,
                    [f"FINDING records {self.root / 'file_digests.json'}: "
                     f"{kind}: and {len(paths) - limit} more"])
                listed = [line for line in lines if line not in summary]
                self.assertEqual(len(listed), limit)
                for name, line in zip(sorted(paths)[:limit], listed):
                    self.assertIn(str(self.root / name), line)

    def test_tracked_executable_is_invalid_even_with_unchanged_bytes(self):
        path = self.unowned_probe / "tv_trades.csv"
        path.chmod(0o755)
        self.commit("Make a recorded file executable")
        mode = git(self.root, "ls-files", "--stage", "--",
                   path.relative_to(self.root).as_posix()).split()[0]
        output = self.assert_outcome(self.invoke("--records-only"), 1)
        self.assertTrue(any(str(path) in line and mode in line
                            and "executable" in line
                            for line in output.splitlines()), output)

    def test_file_manifest_schema_and_population_are_required(self):
        original = self.file_digests()
        for field, value in (("schema_version", "unsupported"),
                             ("files", {}), ("files", []), ("files", None)):
            with self.subTest(field=field, value=value):
                record = dict(original)
                record[field] = value
                self.save_file_digests(record)
                output = self.assert_outcome(self.invoke("--records-only"), 1)
                reason = ("unsupported schema_version" if field == "schema_version"
                          else "files must be a nonempty object")
                self.assertIn(f"FINDING records {self.root / 'file_digests.json'}: {reason}",
                              output)

    def test_file_manifest_rejects_duplicate_keys_and_nonobjects(self):
        path = self.root / "file_digests.json"
        record = self.file_digests()
        for content, reason in (
                (json.dumps([]), "JSON object"),
                ('{"files": {}, "files": {}}', "duplicate JSON key")):
            with self.subTest(reason=reason):
                path.write_text(content)
                self.commit("Record malformed file manifest")
                output = self.assert_outcome(self.invoke("--records-only"), 1)
                self.assertTrue(any(str(path) in line and reason in line
                                    for line in output.splitlines()), output)
        self.save_file_digests(record)

    def test_file_manifest_paths_must_be_normalized_and_cannot_name_itself(self):
        original = self.file_digests()
        expected = next(iter(original["files"].values()))
        for name in ("", ".", "..", "/absolute", "./relative", "a//b",
                     "a/../b", "a/", "with space.txt", "file_digests.json"):
            with self.subTest(name=name):
                record = {**original, "files": {**original["files"], name: expected}}
                self.save_file_digests(record)
                output = self.assert_outcome(self.invoke("--records-only"), 1)
                self.assertTrue(any("file_digests.json" in line
                                    and repr(name) in line and "unusable file path" in line
                                    for line in output.splitlines()), output)

    def test_malformed_digest_is_invalid_and_not_counted_as_bound(self):
        original = self.file_digests()
        name = (self.unowned_probe / "tv_trades.csv").relative_to(self.root).as_posix()
        expected = original["files"][name]
        for value in ("A" + expected[1:], expected[:-1], None):
            with self.subTest(value=value):
                record = {**original, "files": {**original["files"], name: value}}
                self.save_file_digests(record)
                output = self.assert_outcome(self.invoke("--records-only"), 1)
                self.assertIn(str(self.root / name), output)
                self.assertIn("malformed", output)
                self.assertEqual(self.count_fields(output)["bound"],
                                 len(set(original["files"]) - {name}))

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
        original = self.directory / "original-readme"
        original.write_bytes(path.read_bytes())
        path.write_bytes(original.read_bytes() + b"dirty\n")
        output = self.assert_outcome(self.invoke_status_fault(path, original), 2)
        self.assertIn("FINDING clean ", output)
        self.assertIn("dirty before validation", output)
        self.assertNotIn("dirty after validation", output)
        self.assertIn(path.relative_to(self.root).as_posix(), output)
        self.assertEqual(path.read_bytes(), original.read_bytes())

    def test_checkout_dirtied_during_validation_is_an_error(self):
        changed = self.root / "changed-during-validation"
        output = self.assert_outcome(self.invoke_status_fault(changed), 2)
        self.assertIn("FINDING clean ", output)
        self.assertNotIn("dirty before validation", output)
        self.assertIn("dirty after validation", output)
        self.assertIn(changed.name, output)
        self.assertTrue(changed.is_file())

    def invoke_status_fault(self, changed, original=None):
        # This controlled fault sits at the tool boundary: one status command
        # is one count. Git may invoke a filesystem-monitor hook several times
        # within a single status command, so a hook cannot control this moment.
        real_git = shutil.which("git")
        self.assertIsNotNone(real_git)
        real_git = str(Path(real_git).resolve())
        directory = self.directory / "git-boundary"
        directory.mkdir()
        shim = directory / "git"
        shim.write_text(
            "#!/bin/sh\n"
            "set -eu\n"
            "for argument do\n"
            '  if [ "$argument" = status ]; then\n'
            "    count=0\n"
            '    if [ -f "$CORPUS_STATUS_COUNTER" ]; then\n'
            '      IFS= read -r count < "$CORPUS_STATUS_COUNTER"\n'
            "    fi\n"
            "    count=$((count + 1))\n"
            '    printf \'%s\\n\' "$count" > "$CORPUS_STATUS_COUNTER"\n'
            '    if [ "$count" -eq 2 ]; then\n'
            '      if [ -n "$CORPUS_STATUS_ORIGINAL" ]; then\n'
            '        cat "$CORPUS_STATUS_ORIGINAL" > "$CORPUS_STATUS_CHANGED"\n'
            "      else\n"
            '        : > "$CORPUS_STATUS_CHANGED"\n'
            "      fi\n"
            "    fi\n"
            "    break\n"
            "  fi\n"
            "done\n"
            'exec "$CORPUS_REAL_GIT" "$@"\n')
        shim.chmod(0o755)
        with mock.patch.dict(os.environ, {
                "PATH": str(directory) + os.pathsep + os.environ.get("PATH", os.defpath),
                "CORPUS_REAL_GIT": real_git,
                "CORPUS_STATUS_COUNTER": str(self.directory / "status-count"),
                "CORPUS_STATUS_CHANGED": str(changed),
                "CORPUS_STATUS_ORIGINAL": str(original) if original is not None else "",
        }):
            return self.invoke("--records-only")

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


class BuildAndProbePathTests(unittest.TestCase):
    """The build-and-probe stage, with a fake cmake and stub engine scripts.

    The engine is a real tiny Git repository whose tracked scripts are the stubs, and
    the corpus is a plain directory. The fake tools only decide which step fails, so
    these tests pin how each failure is reported and which failures are errors; they
    do not claim anything about what a real compiler does.
    """

    PROBE = "validation/sample"
    TAPE_HEADER = "Trade #,Type,Signal"

    FAKE_CMAKE = '''#!{python}
import os
import sys
from pathlib import Path

arguments = sys.argv[1:]
if "--build" in arguments:
    print("fake cmake build")
    if os.environ.get("FAKE_CMAKE_BUILD") == "fail":
        print("fake build failure")
        sys.exit(7)
    libraries = os.environ.get("FAKE_CMAKE_LIBRARIES", "one")
    if libraries != "none":
        for generated in (Path.cwd() / "corpus").rglob("generated.cpp"):
            (generated.parent / "strategy.so").write_bytes(b"")
            if libraries == "two":
                (generated.parent / "strategy.dylib").write_bytes(b"")
    sys.exit(0)
print("fake cmake configure")
if os.environ.get("FAKE_CMAKE_CONFIGURE") == "fail":
    print("fake configure failure")
    sys.exit(5)
'''

    DERIVE = '''import os
import sys
from pathlib import Path

if os.environ.get("FAKE_DERIVE") == "fail":
    print("fake derive failure")
    sys.exit(6)
arguments = sys.argv[1:]
Path(arguments[arguments.index("--derived-workspace") + 1]).mkdir(parents=True, exist_ok=True)
'''

    RUN = '''import os
import sys
from pathlib import Path

if os.environ.get("FAKE_PROBE") == "fail":
    print("fake probe failure")
    sys.exit(9)
arguments = sys.argv[1:]
output = Path(arguments[arguments.index("--output") + 1])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(os.environ["FAKE_HEADER"] + "\\n1,Long,Entry\\n", encoding="utf-8")
'''

    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        self.directory = Path(resources.enter_context(
            tempfile.TemporaryDirectory(prefix="corpus-build-"))).resolve()
        self.engine = self.directory / "engine"
        (self.engine / "scripts").mkdir(parents=True)
        (self.engine / "CMakeLists.txt").write_text("# fake engine\n")
        (self.engine / "scripts" / "derive_corpus_feeds.py").write_text(self.DERIVE)
        (self.engine / "scripts" / "run_strategy.py").write_text(self.RUN)
        git(self.engine, "init", "-q")
        git(self.engine, "add", "--all")
        self.corpus = self.directory / "corpus"
        probe = self.corpus / self.PROBE
        probe.mkdir(parents=True)
        (probe / "generated.cpp").write_text("// fake strategy\n")
        (probe / "engine_trades.csv").write_text(self.TAPE_HEADER + "\n1,Long,Entry\n")
        self.work = self.directory / "work"
        self.work.mkdir()
        bin_directory = self.directory / "bin"
        bin_directory.mkdir()
        cmake = bin_directory / "cmake"
        cmake.write_text(self.FAKE_CMAKE.replace("{python}", sys.executable))
        cmake.chmod(0o755)
        self.environment = {"PATH": f"{bin_directory}{os.pathsep}{os.environ['PATH']}",
                            "FAKE_HEADER": self.TAPE_HEADER}

    def build(self, **fake):
        output = io.StringIO()
        with mock.patch.dict(os.environ, {**self.environment, **fake}):
            with redirect_stdout(output):
                result = validator._build_and_run(
                    self.corpus, self.engine, self.work, {self.PROBE}, [self.PROBE], jobs=1)
        return result, output.getvalue()

    def test_a_clean_run_has_no_findings_and_no_error(self):
        (findings, error), printed = self.build()
        self.assertEqual((findings, error), ([], False))
        self.assertIn("WORK ", printed)

    def test_configure_failure_is_an_error_and_shows_its_log(self):
        (findings, error), printed = self.build(FAKE_CMAKE_CONFIGURE="fail")
        self.assertTrue(error)
        self.assertEqual(len(findings), 1, findings)
        self.assertTrue(findings[0].startswith(
            f"build {self.engine / 'CMakeLists.txt'}: configure failed; see "), findings)
        self.assertIn("fake configure failure", printed)

    def test_build_failure_is_a_finding_and_not_an_error(self):
        (findings, error), printed = self.build(FAKE_CMAKE_BUILD="fail")
        self.assertFalse(error)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("corpus_strategies failed; see ", findings[0])
        self.assertIn("fake build failure", printed)

    def test_a_probe_without_exactly_one_library_is_a_finding(self):
        for libraries in ("none", "two"):
            with self.subTest(libraries=libraries):
                (findings, error), _ = self.build(FAKE_CMAKE_LIBRARIES=libraries)
                self.assertFalse(error)
                self.assertEqual(len(findings), 1, findings)
                self.assertIn("expected one freshly built strategy library", findings[0])

    def test_derivation_failure_is_an_error_and_shows_its_log(self):
        (findings, error), printed = self.build(FAKE_DERIVE="fail")
        self.assertTrue(error)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("derivation failed; see ", findings[0])
        self.assertIn("fake derive failure", printed)

    def test_probe_failure_is_a_finding_and_shows_its_log(self):
        (findings, error), printed = self.build(FAKE_PROBE="fail")
        self.assertFalse(error)
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("probe failed; see ", findings[0])
        self.assertIn("fake probe failure", printed)

    def test_changed_or_undeclared_headers_are_findings(self):
        declared = "Engine range-end"
        cases = (
            ("renamed column", "Trade #,Kind,Signal", "header changed"),
            ("missing column", "Trade #,Type", "header changed"),
            ("undeclared trailing column", f"{self.TAPE_HEADER},Surprise",
             "undeclared or repeated trailing columns"),
            ("repeated declared column", f"{self.TAPE_HEADER},{declared},{declared}",
             "undeclared or repeated trailing columns"),
        )
        for label, header, reason in cases:
            with self.subTest(case=label):
                (findings, error), _ = self.build(FAKE_HEADER=header)
                self.assertFalse(error)
                self.assertEqual(len(findings), 1, findings)
                self.assertTrue(findings[0].startswith("header "), findings)
                self.assertIn(reason, findings[0])

    def test_the_declared_trailing_column_is_accepted(self):
        self.assertEqual(validator.ALLOWED_EXTRA_COLUMNS, ("Engine range-end",))
        (findings, error), _ = self.build(FAKE_HEADER=f"{self.TAPE_HEADER},Engine range-end")
        self.assertEqual((findings, error), ([], False))

    def test_command_returns_the_exit_code_and_shows_the_log_of_a_failure(self):
        log = self.directory / "command.log"
        for code in (0, 3):
            with self.subTest(code=code):
                output = io.StringIO()
                with redirect_stdout(output):
                    result = validator._command(
                        [sys.executable, "-c", f"print('line'); raise SystemExit({code})"],
                        self.directory, log, os.environ.copy())
                self.assertEqual(result, code)
                self.assertEqual(log.read_text(), "line\n")
                self.assertEqual(output.getvalue(), "line\n" if code else "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
