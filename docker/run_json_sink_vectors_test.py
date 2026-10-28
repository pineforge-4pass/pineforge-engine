"""Python byte drift and reader/converter checks for committed v1 vectors.

Run on a build host: python3 -m pytest docker/run_json_sink_vectors_test.py
The committed files must exist; missing vectors are failures.
Captured native files are verified from the manifest and reader, never regenerated.
"""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys

import pytest

DOCKER = Path(__file__).resolve().parent
ROOT = DOCKER.parent
VECTORS = ROOT / "tests" / "data" / "sink_vectors_v1"
BAD_STEMS = (
    "bad-trailer-counts",
    "bad-trailer-sha256",
    "truncated-mid-record",
    "truncated-at-line",
)
EXPECTED_FILES = {
    "valid.pfsink", "valid.expected.parse.json", "valid.expected.to-json.json",
    "MANIFEST.json",
    *(stem + suffix for stem in BAD_STEMS
      for suffix in (".pfsink", ".expected.refusal.json")),
}
NATIVE_FILES = {"native.pfsink", "native.expected.parse.json", "native.provenance.json"}

sys.path.insert(0, str(DOCKER))

import pf_sink


def native_files(manifest):
    names = set(manifest["files"]) & NATIVE_FILES
    assert names == NATIVE_FILES, (
        f"manifest must list all three captured native files, missing: {sorted(NATIVE_FILES - names)}")
    for name in names:
        assert manifest["files"][name].get("captured_native") is True, (
            f"manifest must mark {name} as captured_native: true")
    return names


@pytest.fixture(scope="module")
def vectors():
    assert VECTORS.is_dir(), f"missing committed sink vector directory: {VECTORS}"
    names = {path.name for path in VECTORS.iterdir()}
    missing = EXPECTED_FILES - names
    assert not missing, f"missing committed sink vector files: {', '.join(sorted(missing))}"
    manifest = json.loads((VECTORS / "MANIFEST.json").read_bytes())
    expected = EXPECTED_FILES | native_files(manifest)
    missing = expected - names
    assert not missing, f"missing committed sink vector files: {', '.join(sorted(missing))}"
    assert names == expected, f"unexpected sink vector files: {sorted(names - expected)}"
    for name in expected:
        assert (VECTORS / name).is_file(), f"committed vector is not a file: {name}"
    return VECTORS


def canonical_json(value):
    # Deliberately independent of the generator's serializer and capture code.
    return json.dumps(value, sort_keys=True, ensure_ascii=False,
                      separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n"


def current_parse(path):
    payload = path.read_bytes()
    records, exception = [], None
    with pf_sink.SinkReader(stream=io.BytesIO(payload)) as reader:
        try:
            for record in reader:
                records.append(record)
        except pf_sink.SinkFormatError as error:
            exception = {"class": type(error).__name__, "message": str(error)}
        result = {
            "header": reader.header,
            "records": [{"tag": record.tag, "fields": list(record.fields),
                         "values": list(record.values)} for record in records],
            "counts": reader.counts, "trailer": reader.trailer,
            "sha256": reader.sha256, "complete": reader.complete,
        }
    if exception is not None:
        result["exception"] = exception
    else:
        # Only use parse_verified after the SAME bytes passed full receipt
        # verification. This reader entry point accepts E rows exclusively.
        assert reader.complete
        parser = pf_sink.RecordParser(reader.header)
        lines = payload.splitlines(keepends=True)[1:-1]
        assert len(lines) == len(records)
        for line, record in zip(lines, records):
            if record.tag == "E":
                assert parser.parse_verified(line) == record
    return result


def convert(path, tmp_path):
    metadata = tmp_path / "stdout.json"
    metadata.write_bytes(canonical_json({}))
    return subprocess.run(
        [sys.executable, "-I", "-B", "-S", str(DOCKER / "pineforge-sink-to-json"),
         "--in", str(path), "--stdout-json", str(metadata), "--out", "-",
         "--max-bytes", str(path.stat().st_size)],
        stdin=subprocess.DEVNULL, capture_output=True)


def test_regeneration_and_manifest_are_byte_exact(vectors, tmp_path):
    manifest_bytes = (vectors / "MANIFEST.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    assert manifest_bytes == canonical_json(manifest)
    assert set(manifest) == {"wire_version", "engine_sha", "generator_command", "files"}
    assert manifest["wire_version"] == "pf-report-sink/v1"
    assert re.fullmatch(r"[0-9a-f]{40}", manifest["engine_sha"])
    native = native_files(manifest)
    python_command = [
        "python3", "-I", "docker/gen_sink_vectors.py",
        "--out", "tests/data/sink_vectors_v1", "--engine-sha", manifest["engine_sha"],
    ]
    assert manifest["generator_command"] == (
        python_command + (["--native-dir", "NATIVE_DIR"] if native else []))
    assert set(manifest["files"]) == (EXPECTED_FILES - {"MANIFEST.json"}) | native
    for name, entry in manifest["files"].items():
        data = (vectors / name).read_bytes()
        expected = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        if name in native:
            expected["captured_native"] = True
        assert entry == expected, name

    # Only the Python fixtures are regenerated. Native capture needs a built
    # strategy module, which this drift test neither loads nor executes.
    regenerated = tmp_path / "regenerated"
    result = subprocess.run(
        [sys.executable, "-I", str(DOCKER / "gen_sink_vectors.py"),
         "--out", str(regenerated), "--engine-sha", manifest["engine_sha"]],
        stdin=subprocess.DEVNULL, capture_output=True, cwd=tmp_path)
    assert result.returncode == 0, result.stderr.decode("utf-8", errors="replace")
    assert {path.name for path in regenerated.iterdir()} == EXPECTED_FILES
    for name in sorted(EXPECTED_FILES - {"MANIFEST.json"}):
        assert (regenerated / name).read_bytes() == (vectors / name).read_bytes(), name
    python_manifest = {
        **manifest,
        "generator_command": python_command,
        "files": {name: entry for name, entry in manifest["files"].items()
                  if name not in native},
    }
    assert (regenerated / "MANIFEST.json").read_bytes() == canonical_json(python_manifest)


def test_committed_valid_parses_match_current_reader(vectors):
    actual = current_parse(vectors / "valid.pfsink")
    assert "exception" not in actual
    assert actual["complete"] is True
    assert set(actual["counts"]) == set("ETOVKXH")
    assert all(count > 0 for count in actual["counts"].values())
    observed = {tag: sum(record["tag"] == tag for record in actual["records"])
                for tag in actual["counts"]}
    assert observed == actual["counts"] == actual["trailer"]["counts"]
    assert actual["sha256"] == actual["trailer"]["sha256"]
    assert (vectors / "valid.expected.parse.json").read_bytes() == canonical_json(actual)

    manifest = json.loads((vectors / "MANIFEST.json").read_bytes())
    if native_files(manifest):
        actual = current_parse(vectors / "native.pfsink")
        assert "exception" not in actual, actual.get("exception")
        assert actual["complete"] is True
        assert all(actual["counts"].get(tag, 0) > 0 for tag in "ET")
        assert actual["header"]["run_id"], "native header run_id must be non-empty"
        assert actual["header"]["engine"] and actual["header"]["engine"] != "test", (
            "native header engine must carry a real engine version string")
        assert (vectors / "native.expected.parse.json").read_bytes() == canonical_json(actual)
        provenance = json.loads((vectors / "native.provenance.json").read_bytes())
        assert provenance["captured_native"] is True
        assert provenance["header"] == {
            "run_id": actual["header"]["run_id"], "engine": actual["header"]["engine"]}
        assert provenance["engine_version_string"] == actual["header"]["engine"]
        assert provenance["record_kinds"] == [
            tag for tag in "ETOVKXH" if actual["counts"].get(tag, 0) > 0]
        assert provenance["missing_record_kinds"] == [
            tag for tag in "ETOVKXH" if actual["counts"].get(tag, 0) == 0]


def test_committed_report_matches_current_converter(vectors, tmp_path):
    result = convert(vectors / "valid.pfsink", tmp_path)
    assert result.returncode == 0, result.stderr.decode("utf-8", errors="replace")
    assert result.stdout == (vectors / "valid.expected.to-json.json").read_bytes()


@pytest.mark.parametrize("stem", BAD_STEMS)
def test_committed_refusal_matches_current_reader_and_converter(vectors, tmp_path, stem):
    actual = current_parse(vectors / (stem + ".pfsink"))
    assert "exception" in actual, f"reader accepted refusal vector: {stem}"
    assert actual["complete"] is False
    assert actual["records"], f"no provisional records were returned: {stem}"
    expected_bytes = (vectors / (stem + ".expected.refusal.json")).read_bytes()
    assert expected_bytes == canonical_json(actual)

    result = convert(vectors / (stem + ".pfsink"), tmp_path)
    assert result.returncode != 0, f"converter accepted refusal vector: {stem}"
    assert result.stdout == b""
    diagnostics = [json.loads(line) for line in result.stderr.splitlines()]
    assert diagnostics
    assert diagnostics[-1]["event"] == "sink_refused"
    assert diagnostics[-1]["sink_verified"] is False
    assert diagnostics[-1]["exit_code"] == result.returncode
    assert diagnostics[-1]["detail"] == actual["exception"]["message"]
