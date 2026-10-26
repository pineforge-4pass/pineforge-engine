"""S4E E-03: the replay converter retains report interpretation metadata."""
import json

import pytest

import pineforge_sink_to_json_test as replay


@pytest.mark.parametrize("digest_ms", [None, 0.125], ids=["no-digest", "digest"])
@pytest.mark.parametrize("profile,admitted", [("unjailed", False), ("jailed", True)])
def test_annualization_and_phase_timing_survive_conversion(
        tmp_path, digest_ms, profile, admitted):
    sink, metadata, expected = replay.synthetic(tmp_path)
    curve = expected["equity_curve"]
    assert 0 < curve[-1]["time_ms"] - curve[0]["time_ms"] < 86_400_000
    # The original synthetic fixture leaves cagr null. Supply finite metrics
    # and their contract-defined interpretation on the same sub-day curve.
    expected["metrics"]["equity"]["cagr"] = 12.5
    expected["metrics"]["equity"]["calmar"] = 2.5
    expected["diagnostics"]["annualization"] = {
        "cagr": "simple_proportional_subday",
        "calmar": "simple_proportional_subday",
    }
    expected["diagnostics"]["run_profile"] = profile
    expected["diagnostics"]["scratch_arena_jailed"] = admitted
    expected["diagnostics"]["phase_timing"] = {
        "version": 1, "execution_ms": 12.5,
        "result_assembly_ms": 0.25, "results_digest_ms": digest_ms,
    }
    metadata.write_bytes(replay.report_bytes(replay.projection(expected)))
    result = replay.invoke(sink, metadata)
    assert result.returncode == 0, result.stderr.decode()
    replay.fixtures.assert_run_profile(json.loads(result.stdout), profile, admitted=admitted)
    assert result.stdout == replay.report_bytes(expected)
    assert json.loads(result.stdout)["diagnostics"] == expected["diagnostics"]
    assert replay.diagnostics(result)[-1]["metadata_drops"] == 0


@pytest.mark.parametrize("key,value", [
    pytest.param("run_profile", "unknown", id="unknown-profile"),
    pytest.param("run_profile", "JAILED", id="uppercase-profile"),
    pytest.param("run_profile", True, id="boolean-profile"),
    pytest.param("run_profile", 1, id="integer-profile"),
    pytest.param("run_profile", None, id="null-profile"),
    pytest.param("scratch_arena_jailed", 0, id="zero-marker"),
    pytest.param("scratch_arena_jailed", 1, id="one-marker"),
    pytest.param("scratch_arena_jailed", "true", id="string-marker"),
    pytest.param("scratch_arena_jailed", None, id="null-marker"),
])
def test_invalid_run_profile_metadata_is_dropped(tmp_path, key, value):
    sink, metadata, expected = replay.synthetic(tmp_path)
    expected["diagnostics"]["run_profile"] = "unjailed"
    expected["diagnostics"]["scratch_arena_jailed"] = False
    source = replay.projection(expected)
    source["diagnostics"][key] = value
    metadata.write_bytes(replay.report_bytes(source))
    del expected["diagnostics"][key]
    result = replay.invoke(sink, metadata)
    assert result.returncode == 0, result.stderr.decode()
    document = json.loads(result.stdout)
    assert key not in document["diagnostics"], document["diagnostics"]
    drops = [line for line in replay.diagnostics(result) if line["event"] == "metadata_drop"]
    assert [(line["path"], line["reason"]) for line in drops] == [
        ("$.diagnostics." + key, "invalid_value")], drops
    assert document["diagnostics"] == expected["diagnostics"], document["diagnostics"]
    assert result.stdout == replay.report_bytes(expected)
    assert replay.diagnostics(result)[-1]["metadata_drops"] == 1


def test_old_report_without_run_profile_metadata_stays_readable(tmp_path):
    sink, metadata, expected = replay.synthetic(tmp_path)
    replay.fixtures.assert_no_run_profile(json.loads(metadata.read_bytes()))
    result = replay.invoke(sink, metadata)
    assert result.returncode == 0, result.stderr.decode()
    document = json.loads(result.stdout)
    replay.fixtures.assert_no_run_profile(document)
    assert result.stdout == replay.report_bytes(expected)
    assert replay.diagnostics(result)[-1]["metadata_drops"] == 0
