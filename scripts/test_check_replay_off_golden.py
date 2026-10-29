#!/usr/bin/env python3
"""Real-byte regression tests for the default-off build identity mask."""
from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from check_replay_off_golden import mask_build_identity, measure


def compact(value, *, canonical=False):
    # These fixtures use ASCII keys and integers, for which this is the
    # runner's canonical provenance encoding.
    return json.dumps(value, sort_keys=canonical, separators=(",", ":")).encode("utf-8")


def report(version=(1, 7, 0), *, price=128, applied="fifo", provenance_input=None,
           missing=None, raw_edit=None, token_edit=None):
    engine = {
        "version_string": ".".join(map(str, version)),
        "major": version[0], "minor": version[1], "patch": version[2],
        "commit_sha": "0123456",
    }
    if missing is not None:
        del engine[missing]
    provenance = {
        "engine": engine,
        "applied": {"inputs": {"shape": applied if provenance_input is None else provenance_input},
                    "overrides": {}},
        "runtime": {"major": 1, "minor": 7, "patch": 0},
    }
    token_bytes = compact(provenance, canonical=True)
    if token_edit is not None:
        token_bytes = token_edit(token_bytes)
    document = {
        "trades": [{"entry_price": price, "major": 1, "minor": 7, "patch": 0}],
        "applied_inputs": {"shape": applied},
        "fingerprint": {
            "token": base64.b64encode(token_bytes).decode("ascii"),
            "digest": "sha256:" + hashlib.sha256(token_bytes).hexdigest(),
            "provenance": provenance,
        },
    }
    text = compact(document) + b"\n"
    if raw_edit is not None:
        text = raw_edit(text)
    return text, json.loads(text)


def primary(version="1.7.0", *, price=128, run_id="replay-off-golden", edit=None):
    header = compact({
        "schema": "pf-report-sink", "version": 1, "run_id": run_id,
        "engine": version, "records": [],
    })
    if edit is not None:
        header = edit(header)
    # The identical version-looking body string must remain compared.
    return (header + b"\n" + compact(["T", price, "1.7.0"]) + b"\n"
            + compact({"trailer": "pf-report-sink", "version": 1, "counts": {"T": 1}})
            + b"\n")


class ReplayOffGoldenTests(unittest.TestCase):
    def masked_primary(self, data):
        # Only the external producer is substituted. measure(), the default
        # masker, and the sink masker all execute on the real fixture bytes.
        default_text, _ = report()
        with tempfile.TemporaryDirectory(prefix="replay-off-mask-test-") as scratch:
            directory = Path(scratch)

            def produce(command, **kwargs):
                if "--primary" in command:
                    (directory / "primary.ndjson").write_bytes(data)
                return subprocess.CompletedProcess(command, 0, default_text, b"")

            with mock.patch("check_replay_off_golden.subprocess.run", side_effect=produce):
                return measure(Path("runner.py"), Path("strategy.so"), "fifo",
                               directory)["primary_sink"]

    def test_default_versions_are_equal_and_non_identity_bytes_stay_visible(self):
        baseline = mask_build_identity(*report())
        # Mutation control: reverting numeric masking leaves minor:7 versus
        # minor:8 in the raw report, decoded provenance and token spelling.
        for version in ((1, 8, 0), (2, 7, 0), (1, 7, 1), (12, 34, 56)):
            with self.subTest(version=version):
                self.assertEqual(baseline, mask_build_identity(*report(version)))
        for changes in ({"price": 129}, {"applied": "lifo"}, {"provenance_input": "lifo"}):
            with self.subTest(non_identity=changes):
                self.assertNotEqual(baseline, mask_build_identity(*report(**changes)))
        for before, after in (
                (b'"entry_price":128,"major":1', b'"entry_price":128,"major":2'),
                (b'"runtime":{"major":1', b'"runtime":{"major":2')):
            with self.subTest(non_engine_numbers=before):
                edit = lambda data: data.replace(before, after)
                self.assertNotEqual(baseline, mask_build_identity(*report(raw_edit=edit)))
        input_edit = lambda data: data.replace(b'"shape":"fifo"', b'"shape":"lifo"')
        self.assertNotEqual(baseline, mask_build_identity(*report(token_edit=input_edit)))
        runtime_edit = lambda data: data.replace(b'"runtime":{"major":1',
                                                 b'"runtime":{"major":2')
        self.assertNotEqual(baseline, mask_build_identity(*report(token_edit=runtime_edit)))
        # Token bytes are pinned even when their decoded value is unchanged.
        spaced = lambda data: data.replace(b'"applied":{', b'"applied": {')
        self.assertNotEqual(baseline, mask_build_identity(*report(token_edit=spaced)))

    def test_default_missing_or_duplicate_engine_numbers_are_refused(self):
        for key, value in (("major", 1), ("minor", 7), ("patch", 0)):
            member = f'"{key}":{value}'.encode("ascii")
            with self.subTest(key=key, location="decoded provenance", defect="missing"):
                with self.assertRaisesRegex(RuntimeError, "engine.*identity"):
                    mask_build_identity(*report(missing=key))
            for location in ("raw_edit", "token_edit"):
                for defect in ("missing", "duplicate same", "duplicate different", "duplicate spaced"):
                    # Restrict the mutation to the engine object; the fixture
                    # deliberately has identically named non-engine numbers.
                    def edit(data, *, member=member, key=key, defect=defect):
                        start = data.index(b'"engine":{')
                        end = data.index(b"}", start)
                        block = data[start:end]
                        if defect == "missing":
                            replacement = b""
                        elif defect == "duplicate same":
                            replacement = member + b"," + member + b","
                        elif defect == "duplicate spaced":
                            replacement = member + b"," + member.replace(b":", b" :") + b","
                        else:
                            replacement = member + b',"' + key.encode("ascii") + b'":99,'
                        block = block.replace(member + b",", replacement)
                        return data[:start] + block + data[end:]

                    with self.subTest(key=key, location=location, defect=defect):
                        error = ("engine.*identity" if location == "token_edit" and defect == "missing"
                                 else f"engine {key}.*exactly once")
                        with self.assertRaisesRegex(RuntimeError, error):
                            mask_build_identity(*report(**{location: edit}))

    def test_sink_versions_are_equal_and_non_identity_bytes_stay_visible(self):
        original = primary()
        baseline = self.masked_primary(original)
        for version in ("1.8.0", "2.7.0", "1.7.1", "12.34.56"):
            with self.subTest(version=version):
                self.assertEqual(baseline, self.masked_primary(primary(version)))
        header, newline, body = original.partition(b"\n")
        self.assertEqual(baseline, header.replace(b'"engine":"1.7.0"',
                                                 b'"engine":"<build-identity>"') + newline + body)
        for changes in ({"price": 129}, {"run_id": "different-run"}):
            with self.subTest(non_identity=changes):
                self.assertNotEqual(baseline, self.masked_primary(primary(**changes)))
        changed_body = original.replace(b'["T",128,"1.7.0"]', b'["T",128,"1.8.0"]')
        self.assertNotEqual(baseline, self.masked_primary(changed_body))
        changed_schema = lambda data: data.replace(b'"version":1', b'"version":2')
        self.assertNotEqual(baseline, self.masked_primary(primary(edit=changed_schema)))

    def test_sink_missing_or_duplicate_header_engine_is_refused(self):
        for replacement in (b"", b'"engine":"1.7.0","engine":"1.8.0",',
                            b'"engine":"1.7.0","engine" :"1.7.0",'):
            with self.subTest(replacement=replacement):
                edit = lambda data: data.replace(b'"engine":"1.7.0",', replacement)
                with self.assertRaisesRegex(RuntimeError, "sink header engine.*exactly once"):
                    self.masked_primary(primary(edit=edit))


if __name__ == "__main__":
    unittest.main()
