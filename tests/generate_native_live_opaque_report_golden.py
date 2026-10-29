"""Capture the opaque-module report oracle using the pinned main runner.

On the lab, build main separately, then run this file with --main-source,
--main-runner and --stub (the current live_contract_native_stub module).
The test deliberately fails until the resulting golden is committed.
"""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import tempfile


MAIN_REVISION = "a34a0d0d4fc6e3d26fd3a37f756a26fa659b52e6"
GOLDEN = Path(__file__).parent / "data" / "native_live_opaque_report_main.json"
INPUT_COUNT = 5


def write_fixture(root):
    (root / "warmup.csv").write_text(
        "timestamp,open,high,low,close,volume\n"
        "0,100,102,99,101,4\n60000,101,103,100,102,4\n120000,102,104,101,103,4\n")
    config = {
        "run": {"session_key": "opaque-report", "run_number": 1},
        "clock": {"input_tf": "1", "script_tf": "1", "timezone": "UTC",
                  "session": "24x7", "chart_timezone": ""},
        "instrument": {"ticker": "MOCK", "tickerid": "TEST:MOCK", "type": "crypto",
                       "currency": "USDT", "basecurrency": "ETH", "description": "",
                       "volumetype": ""},
        "execution": {"initial_capital": 10000, "point_value": 1, "account_fx": 1,
                      "price_tick": 0.01, "slippage_ticks": 0, "fee_kind": "CashPerExecution",
                      "fee_value": 6, "quantity_grid": None, "close_execution": "NextEligiblePoint",
                      "max_abs_units": None, "max_open_lots": None,
                      "allowed_open_directions": 3, "initial_margin_fraction": None},
    }
    (root / "native.json").write_text(json.dumps(config) + "\n")
    (root / "feed.jsonl").write_text("".join(
        json.dumps({"type": "bar", "bar": {
            "ts_open": 180000 + i * 60000, "o": 103 + i, "h": 105 + i,
            "l": 102 + i, "c": 104 + i, "v": 4 + i}}) + "\n"
        for i in range(INPUT_COUNT)))


def fixture_digest(root):
    return hashlib.sha256(b"".join((root / name).read_bytes()
                          for name in ("warmup.csv", "native.json", "feed.jsonl"))).hexdigest()


def command(runner, module, root, interval=1):
    args = [str(runner), "run", "--strategy", str(module),
            "--warmup", str(root / "warmup.csv"), "--native-config", str(root / "native.json"),
            "--mode", "bars", "--feed", "-", "--ledger", str(root / "ledger.sqlite3"),
            "--report-jsonl=delta"]
    # Main predates the interval option. Its default is the K=1 oracle.
    if interval != 1:
        args += ["--ratio-interval", str(interval)]
    return args


def ledger_bytes(path):
    with sqlite3.connect(path) as db:
        deployment = db.execute("SELECT identity FROM metadata WHERE singleton=1").fetchone()[0]
        rows = {
            "metadata": db.execute("SELECT singleton,schema_version,identity FROM metadata "
                                   "ORDER BY singleton").fetchall(),
            "inputs": db.execute("SELECT input_index,canonical_json,state_hash FROM inputs "
                                 "ORDER BY input_index").fetchall(),
            "events": db.execute("SELECT ordinal,input_index,input_position,event_id,payload,"
                                 "attempts,acknowledged,last_error FROM events ORDER BY ordinal").fetchall(),
            "report_snapshots": db.execute("SELECT input_cursor,payload FROM report_snapshots "
                                           "ORDER BY input_cursor").fetchall(),
        }
    # Keep the payload strings opaque: no report JSON parse/serialize round trip.
    return deployment, json.dumps(rows, separators=(",", ":")).encode("utf-8")


def capture(runner, module, root, interval=1):
    result = subprocess.run(command(runner, module, root, interval),
                            input=(root / "feed.jsonl").read_bytes(),
                            capture_output=True, timeout=60)
    assert result.returncode == 0, (
        "live report runner must finish successfully", result.returncode,
        result.stdout, result.stderr)
    deployment, rows = ledger_bytes(root / "ledger.sqlite3")
    return deployment, rows, result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--main-source", type=Path, required=True)
    parser.add_argument("--main-runner", type=Path, required=True)
    parser.add_argument("--stub", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=GOLDEN)
    args = parser.parse_args()
    source = args.main_source.resolve()
    runner = args.main_runner.resolve()
    revision = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    assert revision == MAIN_REVISION, "golden requires the pinned main checkout"
    dirty = subprocess.check_output(
        ["git", "-C", str(source), "diff", "HEAD", "--", "runner", "src", "include",
         "CMakeLists.txt"])
    assert not dirty, "reference runner sources must match main"
    caches = [parent / "CMakeCache.txt" for parent in runner.parents
              if (parent / "CMakeCache.txt").is_file()]
    assert caches, "reference runner must belong to the main CMake build"
    assert f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n" in caches[0].read_text(), (
        "reference runner was built from another source tree")
    runner_data = runner.read_bytes()
    assert b"strategy_stream_engine_layout_version" not in runner_data, (
        "refusing a golden from the layout-stamp fix runner")
    assert runner.stat().st_mtime >= (source / "runner" / "main.cpp").stat().st_mtime, (
        "reference runner is older than main.cpp; rebuild it on the lab")
    with tempfile.TemporaryDirectory(prefix="pineforge-opaque-main-") as temporary:
        root = Path(temporary)
        write_fixture(root)
        deployment, rows, output = capture(runner, args.stub.resolve(), root)
        golden = {
            "source_revision": revision,
            "runner_sha256": hashlib.sha256(runner_data).hexdigest(),
            "module_sha256": hashlib.sha256(args.stub.read_bytes()).hexdigest(),
            "stub_source_sha256": hashlib.sha256(
                (Path(__file__).parent / "test_live_contract_plugins.cpp").read_bytes()).hexdigest(),
            "fixture_sha256": fixture_digest(root),
            "deployment": deployment,
            "ledger_base64": base64.b64encode(rows).decode("ascii"),
            "jsonl_base64": base64.b64encode(output).decode("ascii"),
        }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(golden, indent=2) + "\n")
    print(f"wrote main-runner golden: {args.output}")


if __name__ == "__main__":
    main()
