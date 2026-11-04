"""Real CLI boundary proof. Frozen base fails the first unlimited 120k run."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True)
    parser.add_argument("--runner", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cr559-map-cli-") as temporary:
        tape = Path(temporary) / "bars.csv"
        tape.write_text(
            "timestamp,open,high,low,close,volume\n"
            "1704067200000,100,102,99,101,10\n"
            "1704067500000,101,103,100,102,11\n", encoding="ascii")
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        environment.pop("PINEFORGE_RUN_JSON_PROFILE", None)

        def run(pairs, strings, *options):
            command = [
                sys.executable, "-B", args.runner, "--so", args.library,
                "--ohlcv", str(tape), "--input-tf", "5", "--outputs",
                "--inputs", json.dumps({
                    "pairs": str(pairs), "string_keys": "true" if strings else "false"}), *options]
            result = subprocess.run(command, capture_output=True, text=True, env=environment)
            lines = result.stdout.splitlines()
            assert len(lines) == 1, (command, result.stdout, result.stderr)
            return result.returncode, json.loads(lines[0])

        def succeeds(pairs, strings, *options):
            status, document = run(pairs, strings, *options)
            assert status == 0, document
            assert "error" not in document, document
            series = document["outputs"]["series"]
            assert len(series) == 1 and series[0]["slot"] == 0, document
            assert series[0]["output"] == "size" and series[0]["values"] == [pairs, pairs], document

        for strings in (False, True):
            succeeds(120000, strings)
            succeeds(50001, strings, "--limit-map-pairs", "0")
            succeeds(50000, strings, "--limit-map-pairs", "50000")
            for maximum in (3, 50000):
                status, document = run(maximum + 1, strings, "--limit-map-pairs", str(maximum))
                assert status == 1, document
                assert document["code"] == "pine_map_capacity_exceeded", document
                assert document["args"] == {
                    "code": "RE10070", "maxSize": maximum,
                    "currSize": maximum + 1, "bar_index": 0}, document
                assert document["error"] == (
                    f"Error on bar 0: A map cannot have more than {maximum} "
                    f"key-value pairs. The current size is {maximum + 1}."), document
                assert set(document) == {"engine", "error", "code", "args"}, document
        status, document = run(1, False, "--limit-map-pairs", "-1")
        assert status == 2 and document["code"] == "run_request_invalid", document
    print("CR559 CLI: default/zero, int/string edges, typed refusal and no report checked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
