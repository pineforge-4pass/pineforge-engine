"""Synthetic normalized-tick witness for the actual genuine_case caller.

This exercises a real runner and local HTTP receiver, not new market-data parity.
Build the runner and an observer-linked example library before invoking it.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import sys

from native_live_equivalence_e2e import Strategy, action_key, chart_bar_array, write_json
from native_live_tick_tape import direct_tape, genuine_case, message_groups, message_hashes


def synthetic_fixture():
    warmup = [{"timestamp": str(index * 900000), "open": "100", "high": "102",
        "low": "99", "close": "101", "volume": "4"} for index in range(2)]
    packets = []
    bars = []
    sequence = 1
    for bar_index in range(64):
        opening = (bar_index + 2) * 900000
        for tick_index in range(2048):
            packets.append({"type": "tick", "ts": opening + 1 + tick_index * 100,
                "seq": sequence, "price": (100, 102, 99, 101)[tick_index % 4], "qty": 0.001})
            sequence += 1
        packets.append({"type": "time", "ts": opening + 900000})
        bars.append({"timestamp": str(opening), "open": "100", "high": "102",
            "low": "99", "close": "101", "volume": "2.048"})
    return warmup, packets, bars


def check_case(result, directory, reference, total_inputs):
    assert result["status"] == "PASS", result
    assert result["expected_actions"] == len(reference["actions"]) > 0, result
    assert result["actions"] == result["expected_actions"], result
    assert result["action_difference"] is None and result["hash_difference"] is None, result
    assert result["pending_events"] == 0 and result["committed_inputs"] == total_inputs, result
    effects = json.loads((directory / "effects.json").read_text())
    assert [action_key(effect) for effect in effects] == [action_key(action) for action in reference["actions"]]
    invocations = json.loads((directory / "invocations.json").read_text())
    expected_commands = ["run"] * (2 if result["restart"] else 1)
    if result["fail_first"]:
        expected_commands.append("redeliver")
    assert [entry["command"][1] for entry in invocations] == expected_commands, invocations
    assert all(entry["command"][0] != "/usr/bin/time" for entry in invocations), invocations
    costs = json.loads((directory / "commands.json").read_text())
    assert len(costs) == len(invocations), (costs, invocations)
    assert all(command[:2] == ["/usr/bin/time", "-v"] for command in costs), costs
    if result["restart"]:
        assert result["sigkill"]["signal"] == 9 and result["sigkill"]["time_returncode"] == 137, result
        assert 0 < result["sigkill"]["committed_before_kill"] < total_inputs, result
        assert 0 < result["resume_from_input"] < total_inputs, result
    if result["fail_first"]:
        attempts = json.loads((directory / "attempts.json").read_text())
        rejected = Counter(attempt["event_id"] for attempt in attempts if attempt["status"] == 503)
        assert rejected == Counter(effect["event_id"] for effect in effects), rejected
        redelivery = json.loads((directory / "redelivery.json").read_text())
        assert len(redelivery["invocations"]) == 1, redelivery
        assert redelivery["invocations"][0] == invocations[-1], (redelivery, invocations)
        assert result["redelivery_returncode"] == 0, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenarios", nargs="+", default=[
        "file-batch", "restart-batch", "retry-batch", "combined-batch"])
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / "scripts"))
    from run_strategy import BarC, ReportC

    output = arguments.output.resolve()
    output.mkdir(parents=True)
    strategy = Strategy(arguments.library, BarC, ReportC)
    warmup, packets, bars = synthetic_fixture()
    write_json(output / "fixture.json", {"kind": "synthetic normalized ticks; no genuine-market-data claim",
        "chart_input_tf": 15, "script_tf": 15, "warmup_bars": len(warmup),
        "live_bars": len(bars), "ticks_per_bar": 2048, "packets": len(packets),
        "runner": str(arguments.runner.resolve()), "library": str(strategy.path),
        "library_sha256": strategy.sha256})
    reference = direct_tape(strategy, chart_bar_array(strategy, warmup), packets, 1024,
        output, hashes=True, retain=True, timeframe=15, message_size=1024)
    assert reference["actions"], "synthetic fixture must produce actions"
    assert [action_key(action) for action in reference["observed_actions"]] == [
        action_key(action) for action in reference["actions"]], "independent observer mismatch"
    write_json(output / "reference.json", reference)
    expected_hashes = message_hashes(reference["hashes"], len(packets), 1024)
    write_json(output / "expected-message-hashes.json", expected_hashes)
    total_inputs = len(list(message_groups(packets, 1024)))
    assert total_inputs == len(expected_hashes) > 100, "restart window must be non-vacuous"
    results = []
    failures = []
    for scenario in arguments.scenarios:
        if scenario not in ("file-batch", "restart-batch", "retry-batch", "combined-batch"):
            raise ValueError("unsupported witness scenario " + scenario)
        directory = output / scenario
        result = genuine_case(strategy, reference, warmup, packets, directory,
            arguments.runner.resolve(), scenario)
        results.append(result)
        write_json(output / "results.json", results)
        try:
            check_case(result, directory, reference, total_inputs)
        except AssertionError as error:
            failures.append({"scenario": scenario, "error": str(error)})
    write_json(output / "witness-failures.json", failures)
    if failures:
        print(json.dumps(failures), file=sys.stderr)
        return 1
    print(f"PASS {len(results)} actual genuine_case synthetic scenarios; "
        f"actions={len(reference['actions'])}, message_hashes={total_inputs}, pending=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
