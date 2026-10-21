"""Real legacy provider: SUMMARY_ONLY must be a typed pre-execution refusal.

Fail-before expectation on 583c2217d2: the populated legacy control succeeds,
then run_json reports run_request_invalid for the unknown --report-mode option
instead of report_mode_unsupported. This is an expected RED until the transport
lane implements negotiation. No fabricated report, optional library or skip.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def document(stdout: str) -> dict:
    lines = stdout.splitlines()
    require(len(lines) == 1, f"expected exactly one JSON result, received {stdout!r}")

    def nonfinite(token: str):
        raise AssertionError(f"nonfinite JSON token: {token}")

    result = json.loads(lines[0], parse_constant=nonfinite)
    require(isinstance(result, dict), "report must be a JSON object")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    options = parser.parse_args()
    request = json.loads(options.fixture.read_text(encoding="utf-8"))
    feed = options.fixture.with_name("bars.csv")
    with feed.open(encoding="utf-8", newline="") as source:
        population = list(csv.DictReader(source))
    expected = request["population"]
    require(len(population) == expected["input_bars"], "fixture bar population changed")
    require(options.module.is_file(), f"required real legacy provider missing: {options.module}")
    command = [
        sys.executable, "-B", str(options.runner), "--so", str(options.module),
        "--ohlcv", str(feed), "--input-tf", "1", "--script-tf", "1",
    ]
    with tempfile.TemporaryDirectory(prefix="o1-contract-") as temporary:
        marker = Path(temporary) / "execution"
        environment = dict(os.environ, PINEFORGE_O1_EXECUTION_MARKER=str(marker))
        full = subprocess.run(command, capture_output=True, text=True, env=environment)
        require(full.returncode == 0, f"legacy control failed: {full.stdout}\n{full.stderr}")
        report = document(full.stdout)
        require(report["summary"]["total_trades"] == expected["trades"],
                "control must actually produce three logical trades")
        require(len(report["trades"]) == expected["trades"], "control must return every trade row")
        require(len(report["equity_curve"]) == expected["equity_points"],
                "control must return eight equity points")
        require(report["summary"]["bars_processed"] == expected["script_bars"],
                "control must evaluate all script bars")
        visits = marker.read_text(encoding="utf-8").splitlines()
        require(visits == [row["timestamp"] for row in population],
                "control callback witness must consume the actual ordered population")
        marker.unlink()

        refused = subprocess.run(command + ["--report-mode", request["mode"]],
                                 capture_output=True, text=True, env=environment)
        failure = document(refused.stdout)
        unsupported = request["unsupported"]
        require(failure.get("code") == unsupported["code"],
                f"expected {unsupported['code']}, received {failure.get('code')!r}: {refused.stdout}")
        require(refused.returncode == unsupported["exit_status"],
                f"SUMMARY_ONLY must fail with the unsupported-mode status: {refused.stdout}")
        require(failure.get("args") == unsupported["args"],
                "unsupported mode must name the requested mode and envelope contract")
        require(unsupported["diagnostic"] in failure.get("error", ""),
                "unsupported mode needs an explicit diagnostic")
        require(set(failure) == {"engine", "error", "code", "args"},
                "an unsupported request must not masquerade as a full or empty report")
        require(not marker.exists(), "an unsupported SUMMARY_ONLY request must not execute callbacks")
    print("PASS O1 unsupported mode: populated legacy control; SUMMARY_ONLY refused before execution")


if __name__ == "__main__":
    main()
