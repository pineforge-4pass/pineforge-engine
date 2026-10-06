"""External regression against a real release-recipe image, not a mock library.

Run with python3 docker/run_json_provenance_test.py --image IMAGE --artifacts DIR.
An optional --harness bind mount must match the image's recorded engine/codegen
sources; record its digest and the image inspection alongside the artifacts.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


STRATEGY = '''//@version=6
strategy("Typed Inputs", initial_capital=10000, process_orders_on_close=false)
armed = input.bool(true, "armed")
len = input.int(10, "len")
mult = input.float(2.0, "mult")
scalar = input.string("base", "scalar")
numeric_text = input.string("base", "numeric_text")
mid = ta.sma(close, len)
if armed and close > mid
    strategy.entry("L", strategy.long, qty=mult)
if close < mid
    strategy.close("L")
'''


def synthetic_csv():
    rows = ["timestamp,open,high,low,close,volume"]
    price = 100.0
    for index in range(400):
        price += 0.05 if index < 200 else -0.05
        rows.append(f"{index * 60000},{price:.2f},{price + .07:.2f},"
                    f"{price - .05:.2f},{price + .02:.2f},10")
    return "\n".join(rows) + "\n"


def image_e2e(image, artifacts, harness=None):
    artifacts.mkdir(parents=True, exist_ok=True)
    results = []

    def check(name, condition, detail):
        results.append((name, bool(condition)))
        print(f"{'PASS' if condition else 'FAIL'} {name}: {detail}", flush=True)

    with tempfile.TemporaryDirectory(prefix="typed-provenance-") as temporary:
        source = Path(temporary)
        (source / "strategy.pine").write_text(STRATEGY, encoding="utf-8")
        (source / "ohlcv.csv").write_text(synthetic_csv(), encoding="utf-8")
        (artifacts / "strategy.pine").write_text(STRATEGY, encoding="utf-8")
        (artifacts / "ohlcv.csv").write_text(synthetic_csv(), encoding="utf-8")

        def run(label, inputs=None, overrides=None, failure_option=None):
            command = ["docker", "run", "--rm", "--network", "none",
                       "--mount", f"type=bind,src={source},dst=/in,readonly"]
            if harness:
                command += ["--mount", f"type=bind,src={harness.resolve()},"
                            "dst=/opt/pineforge/bin/run_json.py,readonly"]
            for name, values in (("INPUTS", inputs), ("OVERRIDES", overrides)):
                if values is not None:
                    command += ["-e", f"PINEFORGE_{name}={json.dumps(values)}"]
            command.append(image)
            process = subprocess.run(command, capture_output=True, text=True,
                                     timeout=180)
            (artifacts / f"{label}.command.json").write_text(json.dumps(command))
            (artifacts / f"{label}.stdout.json").write_text(process.stdout)
            (artifacts / f"{label}.stderr.log").write_text(process.stderr)
            (artifacts / f"{label}.exit").write_text(str(process.returncode) + "\n")
            document = json.loads(process.stdout)
            if failure_option:
                check(label + " refusal", process.returncode != 0
                      and document.get("code") == "run_request_invalid"
                      and document.get("args", {}).get("option") == failure_option,
                      document)
                return document
            check(label + " CLI success", process.returncode == 0
                  and "fingerprint" in document, process.returncode)
            fingerprint = document.get("fingerprint", {})
            provenance = fingerprint.get("provenance", {})
            token = base64.b64decode(fingerprint.get("token", ""))
            check(label + " fingerprint identity", bool(token)
                  and json.loads(token) == provenance
                  and fingerprint.get("digest") == "sha256:" + hashlib.sha256(token).hexdigest(),
                  fingerprint.get("digest"))
            for name, values in (("inputs", inputs), ("overrides", overrides)):
                expected = {key: str(value) for key, value in (values or {}).items()}
                check(label + " setter wire " + name,
                      document.get("applied_" + name) == expected,
                      document.get("applied_" + name))
            return document

        run("native-input-bool", {"armed": False}, failure_option="inputs")
        run("native-override-bool", overrides={"process_orders_on_close": True},
            failure_option="overrides")
        control = run("defaults")
        check("control trades", control.get("summary", {}).get("total_trades", 0) > 0,
              control.get("summary"))
        check("default quantity", control.get("trades", [{}])[0].get("qty") == 2,
              control.get("trades", [{}])[0].get("qty"))
        cases = [
            ("false", {"armed": "false", "len": 14, "mult": 1.5,
                       "scalar": "false", "numeric_text": "14"},
             {"initial_capital": 5000, "process_orders_on_close": "true"}, False, 14, 1.5, True),
            ("true", {"armed": "true", "len": "14", "mult": "1.5",
                      "scalar": "true", "numeric_text": "1.5"},
             {"initial_capital": "5e3", "process_orders_on_close": "false"}, True, 14, 1.5, False),
            ("zero", {"armed": "0", "len": "1.4e1", "mult": "1.50e0"},
             {"initial_capital": "5000.0", "process_orders_on_close": "1"}, False, 14, 1.5, True),
            ("one", {"armed": "1", "len": "14.0", "mult": "2"},
             {"initial_capital": "+5000", "process_orders_on_close": "0"}, True, 14, 2.0, False),
        ]
        reports = {}
        for label, inputs, overrides, armed, length, multiplier, close in cases:
            report = run(label, inputs, overrides)
            reports[label] = report
            provenance = report.get("fingerprint", {}).get("provenance", {})
            declared = provenance.get("inputs", {})
            for key, expected, value_type in (("armed", armed, bool), ("len", length, int),
                                              ("mult", multiplier, float)):
                actual = declared.get(key, {}).get("value")
                check(label + " typed input " + key,
                      type(actual) is value_type and actual == expected, actual)
                check(label + " typed applied input " + key,
                      type(provenance.get("applied", {}).get("inputs", {}).get(key)) is value_type,
                      provenance.get("applied", {}).get("inputs", {}).get(key))
            for key in ("scalar", "numeric_text"):
                if key in inputs:
                    check(label + " string control " + key,
                          declared.get(key, {}).get("value") == inputs[key]
                          and type(declared.get(key, {}).get("value")) is str,
                          declared.get(key))
            for section in (provenance.get("strategy", {}),
                            provenance.get("applied", {}).get("overrides", {})):
                check(label + " typed capital", type(section.get("initial_capital")) is float
                      and section["initial_capital"] == 5000.0, section)
                check(label + " typed order-close", type(section.get("process_orders_on_close")) is bool
                      and section["process_orders_on_close"] is close, section)
            check(label + " strategy armed effect",
                  (report.get("summary", {}).get("total_trades", 0) > 0) is armed,
                  report.get("summary"))
            check(label + " capital effect", report.get("equity_curve", [{}])[0].get("equity") == 5000,
                  report.get("equity_curve", [{}])[0])
            if armed:
                check(label + " quantity effect", report.get("trades", [{}])[0].get("qty") == multiplier,
                      report.get("trades", [{}])[0].get("qty"))
        close_report = run("close-effect", {"armed": "true", "len": 14, "mult": 1.5},
                           {"initial_capital": 5000, "process_orders_on_close": "true"})
        check("order-close fill effect",
              close_report["trades"][0]["entry_bar_index"] + 1
              == reports["true"]["trades"][0]["entry_bar_index"],
              [close_report["trades"][0], reports["true"]["trades"][0]])
        enum_report = run("all-overrides", {"armed": "false"}, {
            "initial_capital": "5e3", "commission_value": "0.04", "default_qty_value": "3.5",
            "pyramiding": "2.0", "slippage": "1e0", "process_orders_on_close": "1",
            "calc_on_order_fills": "0", "close_entries_rule": "any",
            "default_qty_type": "strategy.cash", "commission_type": "strategy.commission.cash_per_order"})
        expected = {"initial_capital": 5000.0, "commission_value": 0.04, "default_qty_value": 3.5,
                    "pyramiding": 2, "slippage": 1, "process_orders_on_close": True,
                    "calc_on_order_fills": False, "close_entries_rule": "ANY",
                    "default_qty_type": "cash", "commission_type": "cash_per_order"}
        provenance = enum_report.get("fingerprint", {}).get("provenance", {})
        for name, actual in (("strategy", provenance.get("strategy", {})),
                             ("applied overrides", provenance.get("applied", {}).get("overrides", {}))):
            check("all overrides " + name,
                  all(type(actual.get(key)) is type(value) and actual[key] == value
                      for key, value in expected.items()), actual)

    failed = [name for name, passed in results if not passed]
    (artifacts / "checks.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"{len(results) - len(failed)}/{len(results)} checks passed", flush=True)
    if failed:
        print("FAILED: " + "; ".join(failed), flush=True)
    return int(bool(failed))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--harness", type=Path)
    options = parser.parse_args()
    raise SystemExit(image_e2e(options.image, options.artifacts, options.harness))
