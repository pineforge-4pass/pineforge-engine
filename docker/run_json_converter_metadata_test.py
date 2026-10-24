"""S4E E-03: the replay converter retains report interpretation metadata."""
import json

import pytest

import pineforge_sink_to_json_test as replay


@pytest.mark.parametrize("digest_ms", [None, 0.125], ids=["no-digest", "digest"])
def test_annualization_and_phase_timing_survive_conversion(tmp_path, digest_ms):
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
    expected["diagnostics"]["phase_timing"] = {
        "version": 1, "execution_ms": 12.5,
        "result_assembly_ms": 0.25, "results_digest_ms": digest_ms,
    }
    metadata.write_bytes(replay.report_bytes(replay.projection(expected)))
    result = replay.invoke(sink, metadata)
    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == replay.report_bytes(expected)
    assert json.loads(result.stdout)["diagnostics"] == expected["diagnostics"]
    assert replay.diagnostics(result)[-1]["metadata_drops"] == 0
