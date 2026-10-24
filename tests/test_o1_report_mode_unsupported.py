"""Real legacy provider: FULL is legacy; the other modes refuse before execution.

Fail-before expectation on 583c2217d2: the populated legacy control succeeds,
then run_json reports run_request_invalid for the unknown --report-mode option
instead of report_mode_unsupported. On 03d3d10a3d40, explicit FULL is incorrectly
refused too. No fabricated report, optional library or skip.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from unittest import mock


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
        sys.executable, "-B", str(Path(__file__).resolve()), "--run-json", str(options.runner),
        "--so", str(options.module),
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

        explicit_full = subprocess.run(command + ["--report-mode", "FULL"],
                                       capture_output=True, text=True, env=environment)
        require(explicit_full.returncode == 0,
                f"explicit FULL failed: {explicit_full.stdout}\n{explicit_full.stderr}")
        require(document(explicit_full.stdout) == report,
                "explicit FULL must return the populated legacy report unchanged")
        require(explicit_full.stdout == full.stdout,
                "explicit FULL must be byte-for-byte identical to omitting --report-mode")
        require(marker.read_text(encoding="utf-8").splitlines() == visits,
                "explicit FULL must execute the same ordered callback population")
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
        # Sink support cannot stand in for STREAMED envelope support.
        refused = subprocess.run(command + ["--report-mode", "STREAMED"],
                                 capture_output=True, text=True, env=environment)
        failure = document(refused.stdout)
        require(refused.returncode == unsupported["exit_status"]
                and failure.get("code") == unsupported["code"],
                f"legacy provider accepted an unsupported STREAMED envelope: {refused.stdout}")
        require(failure.get("args") == {"mode": "STREAMED", "contract": request["contract"]},
                "envelope refusal did not preserve the STREAMED request")
        require("unsupported report mode: STREAMED" in failure.get("error", ""),
                "unsupported STREAMED mode needs an explicit diagnostic")
        require(set(failure) == {"engine", "error", "code", "args"},
                "an unsupported STREAMED request must not masquerade as a report")
        require(not marker.exists(), "unsupported STREAMED envelope executed callbacks")
        # These inputs cannot be opened. A mode refused after feed access or
        # library loading would produce a different typed failure.
        missing_feed = Path(temporary) / "missing.csv"
        missing_module = Path(temporary) / "missing.so"
        require(not missing_feed.exists() and not missing_module.exists(),
                "early-refusal controls must name missing inputs")
        for mode in (request["mode"], "STREAMED"):
            early = subprocess.run(command + [
                "--so", str(missing_module), "--ohlcv", str(missing_feed), "--report-mode", mode,
            ], capture_output=True, text=True, env=environment)
            failure = document(early.stdout)
            require(early.returncode == unsupported["exit_status"]
                    and failure.get("code") == unsupported["code"],
                    f"{mode} refusal happened after opening an input: {early.stdout}")
            require(failure.get("args") == {"mode": mode, "contract": request["contract"]},
                    f"early refusal lost the {mode} request")
            require(not marker.exists(), f"early {mode} refusal executed callbacks")
    print("PASS O1 modes: FULL equals legacy; SUMMARY_ONLY and STREAMED refused before execution")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--run-json":
        runner = Path(sys.argv[2]).resolve()
        sys.argv = [str(runner), *sys.argv[3:]]
        sys.path.insert(0, str(runner.parent))
        # Preserve the complete JSON, including elapsed_seconds, when
        # comparing two executions of the real populated legacy provider.
        with mock.patch("time.time", return_value=10.0):
            runpy.run_path(str(runner), run_name="__main__")
    else:
        main()
