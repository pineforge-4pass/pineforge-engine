#!/usr/bin/env python3
"""Logic fixtures only: these tests do not certify a recorded native run."""
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

import check_corpus_headline_evidence as guard


HEADLINE = ("Verified 2 strategies — excellent=2, strong=0, moderate=0, weak=0, "
            "minimal=0, anomaly=0, engine_only=0, missing=0").encode()


class HeadlineTextTests(unittest.TestCase):
    def test_literal_default_is_read_without_environment_or_shell(self):
        script = b'EXPECTED_VERIFY="${EXPECTED_VERIFY:-' + HEADLINE + b'}"\n'
        with mock.patch.dict("os.environ", {"EXPECTED_VERIFY": "unrelated"}):
            self.assertEqual(guard.assignment(script)[0], HEADLINE)

    def test_ambiguous_and_dynamic_assignments_refuse(self):
        for script in (
            b'EXPECTED_VERIFY="$(touch forbidden)"\n',
            b'EXPECTED_VERIFY="${EXPECTED_VERIFY:-$OTHER}"\n',
            b'EXPECTED_VERIFY=x\nEXPECTED_VERIFY=y\n',
            b'export EXPECTED_VERIFY="x"\n',
            b'read EXPECTED_VERIFY\n',
            b'# EXPECTED_VERIFY="x"\n',
        ):
            with self.subTest(script=script), self.assertRaises(ValueError):
                guard.assignment(script)

    def test_native_output_has_exactly_one_nonzero_consistent_headline(self):
        self.assertEqual(guard.headline(b"\n" + HEADLINE + b"\n"), (HEADLINE, 2))
        for output in (b"", HEADLINE + b"\r\n", HEADLINE + b"\n" + HEADLINE,
                       HEADLINE.replace(b"Verified 2", b"Verified 0"),
                       HEADLINE.replace(b"excellent=2", b"excellent=1")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                guard.headline(output)

    def test_duplicate_json_keys_cannot_override_identity(self):
        with self.assertRaises(ValueError):
            guard.json_object(b'{"run_id":1,"run_id":2}')


class GitComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.git("init", "-q")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        (self.repo / "scripts").mkdir()
        self.script = self.repo / guard.PARITY
        self.script.write_bytes(b'EXPECTED_VERIFY="${EXPECTED_VERIFY:-old}"\n')
        self.base = self.commit("base")

    def git(self, *args, data=None):
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                              input=data, capture_output=True).stdout.decode().strip()

    def commit(self, message):
        # Private Git objects, without staging or invoking git commit.
        def write_tree(directory):
            rows = []
            for path in sorted(directory.iterdir()):
                if path.name == ".git":
                    continue
                if path.is_dir():
                    oid = write_tree(path)
                    rows.append(f"040000 tree {oid}\t{path.name}\n")
                else:
                    oid = self.git("hash-object", "-w", "--stdin", data=path.read_bytes())
                    rows.append(f"100644 blob {oid}\t{path.name}\n")
            return self.git("mktree", data="".join(rows).encode())
        parents = ["-p", self.parent] if hasattr(self, "parent") else []
        self.parent = self.git("commit-tree", write_tree(self.repo), *parents, "-m", message)
        self.git("update-ref", "HEAD", self.parent)
        return self.parent

    def test_change_in_earlier_pr_commit_is_not_hidden_by_last_commit(self):
        self.script.write_bytes(b'EXPECTED_VERIFY="${EXPECTED_VERIFY:-new}"\n')
        self.commit("change")
        (self.repo / "README.md").write_text("later unrelated commit\n")
        head = self.commit("docs")
        self.assertEqual(guard.compared(self.repo, self.base, head)[1:], (b"old", b"new"))

    def test_unchanged_control_needs_no_service_or_attachment(self):
        (self.repo / "README.md").write_text("unrelated\n")
        head = self.commit("docs")
        with mock.patch.object(guard, "Service", side_effect=AssertionError("network")):
            self.assertEqual(guard.main(["--repo", str(self.repo),
                                         "--base", self.base, "--head", head]), 0)

    def test_changed_without_attachment_fails_closed(self):
        self.script.write_bytes(b'EXPECTED_VERIFY="${EXPECTED_VERIFY:-new}"\n')
        head = self.commit("change")
        self.assertEqual(guard.main(["--repo", str(self.repo),
                                     "--base", self.base, "--head", head]), 1)

    def test_empty_or_wrong_ref_comparison_refuses(self):
        for base, head in ((self.base, self.base), ("HEAD", self.base), ("0" * 40, self.base)):
            with self.subTest(base=base), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                guard.compared(self.repo, base, head)

    def test_scientific_identity_ignores_docs_and_pin_but_binds_grader(self):
        (self.repo / "scripts/verify_corpus.py").write_text("grader = 1\n")
        first = self.commit("grader")
        self.script.write_bytes(b'EXPECTED_VERIFY="${EXPECTED_VERIFY:-new}"\n')
        (self.repo / "README.md").write_text("docs\n")
        second = self.commit("pin and docs")
        self.assertEqual(guard.engine_identity(self.repo, first),
                         guard.engine_identity(self.repo, second))
        (self.repo / "scripts/verify_corpus.py").write_text("grader = 2\n")
        third = self.commit("different grader")
        self.assertNotEqual(guard.engine_identity(self.repo, second),
                            guard.engine_identity(self.repo, third))


class RecordLogicTests(unittest.TestCase):
    def setUp(self):
        self.population = ["validation/a", "validation/b"]
        self.scientific = {"engine_sha256": "a" * 64, "grader_git_blob": "b" * 40,
                           "corpus_commit": "c" * 40}
        self.raw = b"\n" + HEADLINE + b"\n"
        self.summary = b"ran 2 strategies in 1s -- ok=2 fail=0\n"
        self.record = {
            "schema": guard.SCHEMA, "source_commit": "d" * 40,
            "scientific": self.scientific, "population": self.population,
            "inputs_sha256": "e" * 64,
            "environment": {"LD_LIBRARY_PATH": "absent"},
            "derived_inputs": {"ohlcv_ETH-USDT-USDT_15m.csv": "3" * 64,
                               "ohlcv_ETH-USDT-USDT_15m_window.csv": "4" * 64},
            "native": {"exit": 0, "total": 2, "ok": 2, "fail": 0,
                       "probes": self.population,
                       "outputs": {p: "f" * 64 for p in self.population},
                       "libraries": {p: "1" * 64 for p in self.population},
                       "runtime_sha256": "2" * 64},
            "verifier": {"exit": 0, "sha256": guard.sha256(self.raw)},
            "native_summary_sha256": guard.sha256(self.summary),
            "build": {"type": "Release", "runner": "ctypes",
                      "compiler": "13.3.0", "cmake": "3.28.3", "python": "3.12.3",
                      "cache_sha256": "5" * 64},
        }

    def validate(self, record=None, raw=None, expected=HEADLINE):
        return guard.validate_record(self.record if record is None else record,
                                     self.raw if raw is None else raw,
                                     self.summary, expected, self.scientific, self.population,
                                     "d" * 40, guard.sha256(self.raw))

    def test_structural_fixture_acceptance_is_only_parser_evidence(self):
        self.assertEqual(self.validate(), 2)

    def test_recorded_loader_environment_requires_the_exact_binding(self):
        location = "/opt/hostedtoolcache/Python/3.12.3/x64"
        allowed = {
            "LD_LIBRARY_PATH": "pythonLocation/lib",
            "pythonLocation": "Python/3.12.3/x64",
            "pythonLocation_sha256": hashlib.sha256(location.encode()).hexdigest(),
            "LD_LIBRARY_PATH_sha256": hashlib.sha256((location + "/lib").encode()).hexdigest(),
        }
        for environment in ({"LD_LIBRARY_PATH": "absent"},
                            {"LD_LIBRARY_PATH": "empty"}, allowed):
            record = copy.deepcopy(self.record)
            record["environment"] = environment
            with self.subTest(environment=environment):
                self.assertEqual(self.validate(record), 2)
        invalid = [
            None, {}, {"LD_LIBRARY_PATH": "unknown"},
            {"LD_LIBRARY_PATH": "/other"},
            {"LD_LIBRARY_PATH": "absent", "pythonLocation": "unbound"},
            {**allowed, "pythonLocation": "Python/3.12.3/x64/.."},
            {**allowed, "pythonLocation": location},
            {**allowed, "pythonLocation_sha256": "0" * 64},
            {**allowed, "LD_LIBRARY_PATH_sha256": "0" * 64},
            {key: value for key, value in allowed.items() if key != "pythonLocation"},
        ]
        for environment in invalid:
            record = copy.deepcopy(self.record)
            record["environment"] = environment
            with self.subTest(environment=environment), self.assertRaisesRegex(
                    ValueError, "loader environment"):
                self.validate(record)
        record = copy.deepcopy(self.record)
        del record["environment"]
        with self.assertRaisesRegex(ValueError, "loader environment"):
            self.validate(record)
        record = copy.deepcopy(self.record)
        record["environment"] = allowed
        record["build"]["python"] = "3.12.4"
        with self.assertRaisesRegex(ValueError, "loader environment"):
            self.validate(record)

    def test_wrong_output_digest_and_wrong_headline_are_independent(self):
        bad = copy.deepcopy(self.record)
        bad["verifier"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "digest"):
            self.validate(bad)
        with self.assertRaisesRegex(ValueError, "headline"):
            self.validate(expected=HEADLINE.replace(b"excellent=2", b"excellent=1"))

    def test_wrong_scientific_inputs_and_stale_source_refuse(self):
        for field in self.scientific:
            bad = copy.deepcopy(self.record)
            bad["scientific"][field] = "0" * len(bad["scientific"][field])
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "scientific"):
                self.validate(bad)
        bad = copy.deepcopy(self.record)
        bad["source_commit"] = "0" * 40
        with self.assertRaisesRegex(ValueError, "source"):
            self.validate(bad)

    def test_empty_partial_and_failed_measurements_refuse(self):
        with self.assertRaises(ValueError):
            self.validate(raw=b"")
        for key, value in (("probes", self.population[:1]), ("total", 1),
                           ("ok", 0), ("fail", 1), ("exit", 1), ("outputs", {})):
            bad = copy.deepcopy(self.record)
            bad["native"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(bad)
        bad = copy.deepcopy(self.record)
        bad["verifier"]["exit"] = 1
        with self.assertRaises(ValueError):
            self.validate(bad)

    def test_archive_digest_checked_before_inner_bytes(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("record.json", json.dumps(self.record))
            archive.writestr("verify_corpus.log", self.raw)
            archive.writestr("native-summary.log", self.summary)
        actual = data.getvalue()
        with self.assertRaisesRegex(ValueError, "artifact digest"):
            guard.unpack(actual, "0" * 64)
        record, raw, summary = guard.unpack(actual, guard.sha256(actual))
        self.assertEqual((record, raw, summary), (self.record, self.raw, self.summary))

    def test_service_path_requires_actual_download_and_accepted_producer_bytes(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("record.json", json.dumps(self.record))
            archive.writestr("verify_corpus.log", self.raw)
            archive.writestr("native-summary.log", self.summary)
        archive_bytes = data.getvalue()
        attached = {"run_id": 7, "artifact_id": 8, "artifact_sha256": guard.sha256(archive_bytes),
                    "output_sha256": guard.sha256(self.raw)}
        run = {"id": 7, "run_attempt": 1, "workflow_id": 9, "path": guard.WORKFLOW,
               "event": "workflow_dispatch", "status": "completed", "conclusion": "failure",
               "head_sha": "d" * 40, "repository": {"id": 11, "full_name": guard.REPOSITORY},
               "head_repository": {"id": 11, "full_name": guard.REPOSITORY}}
        artifact = {"id": 8, "name": "corpus-headline-evidence-1", "expired": False,
                    "digest": "sha256:" + attached["artifact_sha256"],
                    "workflow_run": {"id": 7, "repository_id": 11, "head_repository_id": 11,
                                     "head_sha": "d" * 40}}
        jobs = {"total_count": 1, "jobs": [{"name": "corpus-parity", "status": "completed",
                 "conclusion": "failure", "steps": [
                     {"name": "Check TradingView parity against the committed oracle",
                      "status": "completed", "conclusion": "failure"},
                     {"name": "Retain recorded headline evidence", "status": "completed",
                      "conclusion": "success"}]}]}
        corpus = {"sha": "c" * 40, "truncated": False,
                  "tree": [{"path": p + "/generated.cpp", "type": "blob"} for p in self.population]}
        def read(path):
            if "/actions/workflows/" in path:
                return {"path": guard.WORKFLOW, "id": 9}
            if "/jobs?" in path:
                return jobs
            if "/actions/runs/" in path:
                return run
            if "/actions/artifacts/" in path:
                return artifact
            return corpus
        service = mock.Mock()
        service.read.side_effect = read
        service.download.return_value = archive_bytes
        accepted = lambda path: (b'EXPECTED_VERIFY="${EXPECTED_VERIFY:-old}"\n'
                                 if path == guard.PARITY else b"accepted producer\n")
        service.source.side_effect = lambda path, revision: accepted(path)
        with mock.patch.object(guard, "blob", side_effect=lambda repo, rev, path: accepted(path)), \
                mock.patch.object(guard, "scientific", return_value=self.scientific):
            self.assertEqual(guard.check_service(service, attached, ".", "a" * 40,
                                                  "b" * 40, HEADLINE), 2)
            service.download.assert_called_with(8)
            service.source.side_effect = lambda path, revision: b"hand-written replacement\n"
            with self.assertRaisesRegex(ValueError, "producer"):
                guard.check_service(service, attached, ".", "a" * 40, "b" * 40, HEADLINE)
            service.source.side_effect = lambda path, revision: accepted(path)
            service.download.return_value = b"unrecorded bytes"
            with self.assertRaisesRegex(ValueError, "artifact digest"):
                guard.check_service(service, attached, ".", "a" * 40, "b" * 40, HEADLINE)


class ServiceTrustTests(unittest.TestCase):
    def test_service_identity_rejects_wrong_repository_ref_attempt_and_producer(self):
        run = {"id": 7, "run_attempt": 1, "workflow_id": 9,
               "path": guard.WORKFLOW, "event": "workflow_dispatch",
               "status": "completed", "conclusion": "failure", "head_sha": "a" * 40,
               "repository": {"id": 11, "full_name": guard.REPOSITORY},
               "head_repository": {"id": 11, "full_name": guard.REPOSITORY}}
        artifact = {"id": 8, "name": "corpus-headline-evidence-1",
                    "expired": False, "digest": "sha256:" + "b" * 64,
                    "workflow_run": {"id": 7, "repository_id": 11,
                                     "head_repository_id": 11, "head_sha": "a" * 40}}
        attachment = {"run_id": 7, "artifact_id": 8,
                      "artifact_sha256": "b" * 64, "output_sha256": "c" * 64}
        self.assertEqual(guard.service_identity(run, artifact, attachment, 9), "a" * 40)
        for field, value in (("head_sha", "wrong"), ("event", "pull_request"),
                             ("workflow_id", 10), ("run_attempt", 2),
                             ("conclusion", "cancelled"), ("head_repository", {})):
            bad = copy.deepcopy(run)
            bad[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                guard.service_identity(bad, artifact, attachment, 9)


if __name__ == "__main__":
    unittest.main()
