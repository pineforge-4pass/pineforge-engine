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
import sys
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
        source.chmod(0o755)
        (source / "strategy.pine").write_text(STRATEGY, encoding="utf-8")
        (source / "ohlcv.csv").write_text(synthetic_csv(), encoding="utf-8")
        (artifacts / "strategy.pine").write_text(STRATEGY, encoding="utf-8")
        (artifacts / "ohlcv.csv").write_text(synthetic_csv(), encoding="utf-8")

        def run(label, inputs=None, overrides=None, failure_option=None):
            compiled = (artifacts / "compiled" / label).resolve()
            compiled.mkdir(parents=True, exist_ok=True)
            compiled.chmod(0o777)
            command = ["docker", "run", "--rm", "--network", "none",
                       "--mount", f"type=bind,src={source},dst=/in,readonly",
                       "--mount", f"type=bind,src={compiled},dst=/proof"]
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
            fingerprint = document.get("fingerprint") or {}
            provenance = fingerprint.get("provenance", {})
            token = base64.b64decode(fingerprint.get("token", ""))
            check(label + " fingerprint identity", bool(token)
                  and json.loads(token) == provenance
                  and fingerprint.get("digest") == "sha256:" + hashlib.sha256(token).hexdigest(),
                  fingerprint.get("digest"))
            for name, values in (("inputs", inputs), ("overrides", overrides)):
                expected = {key: str(value) for key, value in (values or {}).items()}
                check(label + " applied keys " + name,
                      set(provenance.get("applied", {}).get(name, {})) == set(expected),
                      provenance.get("applied", {}).get(name, {}))
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
            provenance = (report.get("fingerprint") or {}).get("provenance", {})
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
        provenance = (enum_report.get("fingerprint") or {}).get("provenance", {})
        for name, actual in (("strategy", provenance.get("strategy", {})),
                             ("applied overrides", provenance.get("applied", {}).get("overrides", {}))):
            check("all overrides " + name,
                  all(type(actual.get(key)) is type(value) and actual[key] == value
                      for key, value in expected.items()), actual)

        titles = {"armed": 'armed "yes"', "len": "path\\length",
                  "mult": '倍数\\ "quoted"'}
        escaped_strategy = STRATEGY
        for plain, title in titles.items():
            escaped_strategy = escaped_strategy.replace(
                json.dumps(plain), json.dumps(title, ensure_ascii=False))
        escaped_strategy += 'unused = input.string(size.tiny, "unused")\n'
        (source / "strategy.pine").write_text(escaped_strategy, encoding="utf-8")
        (artifacts / "escaped-strategy.pine").write_text(
            escaped_strategy, encoding="utf-8")
        for label, values, expected in (
            ("escaped-defaults", None, (True, 10, 2.0)),
            ("escaped-overrides", {titles["armed"]: "false",
                                   titles["len"]: "14.0",
                                   titles["mult"]: "1.5"}, (False, 14, 1.5)),
            ("escaped-c-string", {titles["armed"] + "\0suffix": "false\0true",
                                  titles["len"]: "14", titles["mult"]: "1.5"},
             (False, 14, 1.5)),
        ):
            report = run(label, values)
            declared = (report.get("fingerprint") or {}).get(
                "provenance", {}).get("inputs", {})
            check(label + " decoded identities",
                  set(declared) == set(titles.values()) | {
                      "scalar", "numeric_text", "unused"}, list(declared))
            for plain, value in zip(titles, expected):
                row = declared.get(titles[plain], {})
                default = {"armed": True, "len": 10, "mult": 2.0}[plain]
                check(label + " typed " + plain,
                      type(row.get("value")) is type(value)
                      and row["value"] == value
                      and type(row.get("default")) is type(default)
                      and row["default"] == default, row)
            check(label + " unused unsupported default",
                  declared.get("unused", {}).get("default") == ""
                  and declared.get("unused", {}).get("value") == "",
                  declared.get("unused"))

    failed = [name for name, passed in results if not passed]
    (artifacts / "checks.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"{len(results) - len(failed)}/{len(results)} checks passed", flush=True)
    if failed:
        print("FAILED: " + "; ".join(failed), flush=True)
    return int(bool(failed))


def review_e2e(image, artifacts, legacy_cpp):
    """Real CLI counterexamples; only checked-export names change for legacy."""
    artifacts.mkdir(parents=True, exist_ok=True)
    checks = []

    def check(name, condition, value):
        checks.append((name, bool(condition)))
        print(f"{'PASS' if condition else 'FAIL'} {name}: {value}", flush=True)

    legacy = legacy_cpp.read_text()
    for name in ("strategy_settings_api_version", "strategy_create_checked",
                 "strategy_set_input_checked", "strategy_set_override_checked"):
        legacy = legacy.replace(name, "fixture_legacy_" + name)
    enum_source = STRATEGY.replace(
        'armed = input.bool', 'enum Side\n    neutral\n    long\n    short\n'
        'side = input.enum(Side.long, "Side", options=[Side.long, Side.short])\n'
        'armed = input.bool').replace(
            "if armed and close > mid", "if armed and side == Side.long and close > mid")
    overrides = {
        "initial_capital": "5000", "initial_capital\0alias": "nan",
        "pyramiding": "2", "pyramiding\0alias": "-1",
        "default_qty_type": "cash", "default_qty_type\0alias": "invalid",
        "commission_type": "cash_per_order", "commission_type\0alias": "invalid",
    }
    cases = [
        ("legacy-key", legacy, {"armed\0suffix": "false\0true"}, {}, None),
        ("legacy-order", legacy, {}, overrides, None),
        ("legacy-unknown-direct-first", legacy,
         {"unused": "true", "unused\0alias": "false"}, {}, None),
        ("legacy-unknown-alias-first", legacy,
         {"unused\0alias": "false", "unused": "true"}, {}, None),
        ("enum-default", enum_source, {}, {}, 1),
        ("enum-label", enum_source, {"Side": "Side.short"}, {}, 2),
        ("enum-index", enum_source, {"Side": "1"}, {}, 1),
    ]
    for label, source_text, inputs, settings, enum_value in cases:
        source = (artifacts / label / "input").resolve()
        compiled = (artifacts / label / "compiled").resolve()
        source.mkdir(parents=True)
        compiled.mkdir()
        compiled.chmod(0o777)
        (source / ("strategy.cpp" if label.startswith("legacy") else
                   "strategy.pine")).write_text(source_text, encoding="utf-8")
        (source / "ohlcv.csv").write_text(synthetic_csv())
        command = ["docker", "run", "--rm", "--network", "none",
                   "--mount", f"type=bind,src={source},dst=/in,readonly",
                   "--mount", f"type=bind,src={compiled},dst=/proof",
                   "-e", "PINEFORGE_INPUTS=" + json.dumps(inputs),
                   "-e", "PINEFORGE_OVERRIDES=" + json.dumps(settings), image]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        case = artifacts / label
        (case / "command.json").write_text(json.dumps(command))
        (case / "stdout.json").write_text(result.stdout)
        (case / "stderr.log").write_text(result.stderr)
        (case / "exit").write_text(str(result.returncode) + "\n")
        report = json.loads(result.stdout)
        check(label + " CLI success", result.returncode == 0, result.returncode)
        fp = report.get("fingerprint") or {}
        provenance = fp.get("provenance", {})
        token = base64.b64decode(fp.get("token", ""))
        check(label + " fingerprint", bool(token) and json.loads(token) == provenance
              and fp.get("digest") == "sha256:" + hashlib.sha256(token).hexdigest(), fp.get("digest"))
        for kind, values in (("inputs", inputs), ("overrides", settings)):
            check(label + " raw " + kind, report.get("applied_" + kind) == values, values)
            check(label + " keys " + kind,
                  set(provenance.get("applied", {}).get(kind, {})) == set(values), values)
        if label == "legacy-key":
            check(label + " native effect", report["summary"]["total_trades"] == 0,
                  report["summary"])
            check(label + " declared bool", provenance.get("inputs", {}).get(
                "armed", {}).get("value") is False, provenance.get("inputs"))
            check(label + " alias bool", provenance.get("applied", {}).get(
                "inputs", {}).get("armed\0suffix") is False, provenance.get("applied"))
        elif label == "legacy-order":
            check(label + " native capital", report["equity_curve"][0]["equity"] == 5000,
                  report["equity_curve"][0])
            check(label + " native pyramiding", report["summary"]["total_trades"] == 2,
                  report["summary"])
            expected = {"initial_capital": 5000.0, "pyramiding": 2,
                        "default_qty_type": "cash", "commission_type": "cash_per_order"}
            for name, value in expected.items():
                actual = provenance.get("strategy", {}).get(name)
                check(label + " ordered " + name,
                      type(actual) is type(value) and actual == value, actual)
                for raw in (name, name + "\0alias"):
                    actual = provenance.get("applied", {}).get("overrides", {}).get(raw)
                    check(label + " applied " + repr(raw),
                          type(actual) is type(value) and actual == value, actual)
        elif label.startswith("legacy-unknown-"):
            declared = provenance.get("inputs", {})
            expected_names = {"armed", "len", "mult", "scalar", "numeric_text"} | set(inputs)
            check(label + " declared and unknown keys",
                  set(declared) == expected_names, list(declared))
            for raw, text in inputs.items():
                row = declared.get(raw, {})
                check(label + " unknown row " + repr(raw),
                      row.get("type") == "unknown"
                      and row.get("default") is None
                      and type(row.get("value")) is str
                      and row["value"] == text, row)
                actual = provenance.get("applied", {}).get("inputs", {}).get(raw)
                check(label + " unknown applied string " + repr(raw),
                      type(actual) is str and actual == text, actual)
            check(label + " native trading unchanged",
                  report["summary"]["total_trades"] == 1
                  and report["trades"][0]["qty"] == 2
                  and report["equity_curve"][0]["equity"] == 10000,
                  report["summary"])
        else:
            row = provenance.get("inputs", {}).get("Side", {})
            check(label + " default int", type(row.get("default")) is int
                  and row["default"] == 1, row)
            check(label + " effective int", type(row.get("value")) is int
                  and row["value"] == enum_value, row)
            if inputs:
                actual = provenance.get("applied", {}).get("inputs", {}).get("Side")
                check(label + " applied int", type(actual) is int and actual == enum_value, actual)
            check(label + " native selection",
                  (report["summary"]["total_trades"] > 0) is (enum_value == 1),
                  report["summary"])
    (artifacts / "checks.json").write_text(json.dumps(checks, indent=2) + "\n")
    print(f"{sum(passed for _, passed in checks)}/{len(checks)} checks passed", flush=True)
    return int(not all(passed for _, passed in checks))


def legacy_declaration_e2e(image, artifacts, escaped_cpp, enum_cpp):
    """Exercise emitted legacy declaration names/defaults through the image CLI."""
    artifacts.mkdir(parents=True, exist_ok=True)
    checks = []

    def check(name, condition, value):
        checks.append((name, bool(condition)))
        print(f"{'PASS' if condition else 'FAIL'} {name}: {value}", flush=True)

    def legacy(path):
        text = path.read_text()
        for name in ("strategy_settings_api_version", "strategy_create_checked",
                     "strategy_set_input_checked", "strategy_set_override_checked"):
            text = text.replace(name, "fixture_legacy_" + name)
        return text

    escaped, enum = legacy(escaped_cpp), legacy(enum_cpp)
    title = "path\\length"
    wrong = "path\\\\length"
    armed = 'armed "yes"'
    multiplier = '倍数\\ "quoted"'
    literal = json.dumps(title)
    assert literal in escaped
    nul_source = escaped.replace(literal, literal[:-1] + r'\000suffix"')
    cases = [
        ("escaped-default", escaped, {}, 10, True, None),
        ("escaped-decoded", escaped,
         {title: "14", armed: "false", multiplier: "1.5"}, 14, False, None),
        ("escaped-alias", escaped,
         {title + "\0alias": "14", armed + "\0alias": "false\0true"}, 14, False, None),
        ("escaped-wrong", escaped, {wrong: "14"}, 10, True, wrong),
        ("escaped-wrong-alias", escaped, {wrong + "\0alias": "14"},
         10, True, wrong + "\0alias"),
        ("escaped-nul-declaration", nul_source, {title + "\0alias": "14"},
         14, True, None),
        ("legacy-enum-default", enum, {}, 1, True, None),
        ("legacy-enum-index", enum, {"Side": "2"}, 2, False, None),
    ]
    for label, text, inputs, expected, trading, unknown in cases:
        case = (artifacts / label).resolve()
        source, compiled = case / "input", case / "compiled"
        source.mkdir(parents=True)
        compiled.mkdir()
        compiled.chmod(0o777)
        (source / "strategy.cpp").write_text(text, encoding="utf-8")
        (source / "ohlcv.csv").write_text(synthetic_csv())
        command = ["docker", "run", "--rm", "--network", "none",
                   "--mount", f"type=bind,src={source},dst=/in,readonly",
                   "--mount", f"type=bind,src={compiled},dst=/proof",
                   "-e", "PINEFORGE_INPUTS=" + json.dumps(inputs), image]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        (case / "command.json").write_text(json.dumps(command))
        (case / "stdout.json").write_text(result.stdout)
        (case / "stderr.log").write_text(result.stderr)
        (case / "exit").write_text(str(result.returncode) + "\n")
        report = json.loads(result.stdout)
        check(label + " CLI success", result.returncode == 0, result.returncode)
        fp = report.get("fingerprint") or {}
        provenance = fp.get("provenance", {})
        token = base64.b64decode(fp.get("token", ""))
        check(label + " fingerprint", bool(token) and json.loads(token) == provenance
              and fp.get("digest") == "sha256:" + hashlib.sha256(token).hexdigest(), fp.get("digest"))
        check(label + " raw map", report.get("applied_inputs") == inputs,
              report.get("applied_inputs"))
        applied = provenance.get("applied", {}).get("inputs", {})
        check(label + " applied keys", set(applied) == set(inputs), applied)
        declared = provenance.get("inputs", {})
        if label.startswith("escaped"):
            names = {armed, title, multiplier, "scalar", "numeric_text", "unused"}
            check(label + " native declaration identities",
                  set(declared) == names | ({unknown} if unknown else set()), list(declared))
            row = declared.get(title, {})
            check(label + " native integer", type(row.get("default")) is int
                  and row["default"] == 10 and type(row.get("value")) is int
                  and row["value"] == expected, row)
            check(label + " native boolean", declared.get(armed, {}).get("value") is trading,
                  declared.get(armed))
            for raw in inputs:
                value = applied.get(raw)
                if raw == unknown:
                    check(label + " unknown string", type(value) is str and value == inputs[raw], value)
                    row = declared.get(raw, {})
                    check(label + " unknown row", row == {"type": "unknown", "default": None,
                                                           "value": inputs[raw]}, row)
                elif raw.split("\0", 1)[0] == title:
                    check(label + " applied native integer", type(value) is int and value == 14, value)
                elif raw.split("\0", 1)[0] == armed:
                    check(label + " applied native boolean", value is False, value)
                else:
                    check(label + " applied native float", type(value) is float and value == 1.5, value)
        else:
            row = declared.get("Side", {})
            check(label + " symbolic default", type(row.get("default")) is int and row["default"] == 1, row)
            check(label + " numeric value", type(row.get("value")) is int and row["value"] == expected, row)
            if inputs:
                check(label + " applied enum integer", type(applied.get("Side")) is int
                      and applied["Side"] == 2, applied)
        check(label + " native trading", (report["summary"]["total_trades"] > 0) is trading,
              report["summary"])
        check(label + " native capital", report["equity_curve"][0]["equity"] == 10000,
              report["equity_curve"][0])
        if trading:
            check(label + " native quantity", report["trades"][0]["qty"] == 2, report["trades"][0])
    (artifacts / "checks.json").write_text(json.dumps(checks, indent=2) + "\n")
    print(f"{sum(passed for _, passed in checks)}/{len(checks)} checks passed", flush=True)
    return int(not all(passed for _, passed in checks))


def producer_classes(unicode_only=False):
    """Oracle from the installed pinned producer, never from the consumer mirror."""
    import importlib.util
    import inspect
    from pineforge_codegen.lexer import Lexer, TokenType
    from pineforge_codegen.codegen.helpers import NamingHelper
    from pineforge_codegen import transpile
    # Any producer change requires re-auditing the class inventory and mirrors.
    pinned = {
        Lexer: "d1285cdfa5883f439d142d9b7855bbb7c1c0ec97709ae43bf7161bc84c15130a",
        NamingHelper: "130a33d7861d97e75644f2444875e1155ad789c13f40382f0d89769b7e56bf1d",
    }
    for owner, digest in pinned.items():
        assert hashlib.sha256(Path(inspect.getfile(owner)).read_bytes()).hexdigest() == digest
    spec = importlib.util.spec_from_file_location("installed_release", "/opt/pineforge/bin/run_json.py")
    consumer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(consumer)
    identifiers = [
        ("ascii-upper", "Ascii", "Member"), ("ascii-lower", "ascii", "member"),
        ("underscore", "_Side", "_member"), ("ascii-digit", "Side2", "member2"),
        ("unicode-Lu", "ΩSide", "Ωvalue"), ("unicode-Ll", "éSide", "évalue"),
        ("unicode-Lt", "ǅSide", "ǅvalue"), ("unicode-Lm", "ʰSide", "ʰvalue"),
        ("unicode-Lo-CJK", "方向", "做多"), ("unicode-Nd", "Side٢", "member٢"),
        ("unicode-Nl", "SideⅫ", "memberⅫ"), ("unicode-No-digit", "Side²", "member²"),
        ("unicode-No-number", "Side½", "member½"),
        ("cpp-reserved", "long", "short"), ("reserved-collision", "_long_", "_short_"),
        ("accessor-name", "gross_profit", "gross_loss"),
        ("emitter-temporary", "_nz_v", "_pf_substring_arg1"),
    ]
    strings = [
        ("double-quote", r'"double-quote: \"yes\""'),
        ("single-quote", r"'single-quote: \'yes\''"),
        ("backslash", r'"backslash: \\tail"'),
        ("newline-escape", r'"newline-escape: \nline"'),
        ("tab-escape", r'"tab-escape: \tstop"'),
        ("unknown-r", r'"unknown-r: \rtail"'),
        ("unknown-zero", r'"unknown-zero: \0tail"'),
        ("unknown-hex", r'"unknown-hex: \x41"'),
        ("unknown-unicode-notation", r'"unknown-unicode-notation: \u4e2d"'),
        ("unknown-letter", r'"unknown-letter: \qtail"'),
        ("unknown-Unicode", r'"unknown-Unicode: \界"'),
        ("escaped-physical-newline", '"escaped-physical-newline: one\\\ntwo"'),
        ("wrapped", '"wrapped: one\n    two"'),
        ("triple-double", '"""triple-double: one\n  two"""'),
        ("triple-single", "'''triple-single: one\n  two'''"),
        ("raw-tab", '"raw-tab: A\tB"'), ("raw-CR", '"raw-CR: A\rB"'),
        ("raw-NUL", '"raw-NUL: before\0after"'),
        ("raw-controls", '"raw-controls: ' + ''.join(chr(n) for n in range(1, 32)
                                                      if n not in (9, 10, 13)) + chr(127) + '"'),
        ("literal-Unicode", '"literal-Unicode: 倍数Ωé🙂"'),
        ("literal-backslash-unknown", r'"literal-backslash-unknown: \\q"'),
    ]
    if unicode_only:
        identifiers = [row for row in identifiers if row[0] == "unicode-Lo-CJK"]
        strings = []
    rows, declarations, quantities, bindings = [], [], [], []
    predicate = getattr(consumer, "_release_codegen_identifier", lambda value: False)
    for i, (label, name, member) in enumerate(identifiers):
        for word in (name, member):
            tokens = [t for t in Lexer(word).tokenize() if t.type not in (TokenType.NEWLINE, TokenType.EOF_TOKEN)]
            assert len(tokens) == 1 and tokens[0].type == TokenType.IDENT and tokens[0].value == word
            bindings.append([label + " identifier " + word, predicate(word)])
        title = "enum:" + label
        declarations.append(f'enum {name}\n    neutral\n    {member}\n    other\nv{i} = input.enum({name}.{member}, "{title}")')
        quantities.append(f'(v{i} == {name}.{member} ? 1 : 2)')
        rows.append({"class": label, "title": title, "default": 1, "override": "2", "value": 2, "kind": "int"})
    for i, (label, literal) in enumerate(strings):
        tokens = [t for t in Lexer(literal).tokenize() if t.type == TokenType.STRING]
        assert len(tokens) == 1
        value = tokens[0].value
        encoded = NamingHelper._cpp_string_escape(value)
        native = value.split("\0", 1)[0]
        bindings.append([label + " emitter inverse", consumer._release_cpp_input_name(encoded) == native])
        declarations.append(f's{i} = input.string({literal}, {literal})')
        override = value + ":override"
        rows.append({"class": label, "title": native, "default": native,
                     "override": override, "value": override.split("\0", 1)[0], "kind": "str",
                     "pine_literal": literal, "pine_decoded": value, "cpp_encoded": encoded})
    for rejected in ("2Side", "٢Side", "a\u0301", "$name", "a-b", "🙂"):
        try:
            tokens = [t for t in Lexer(rejected).tokenize() if t.type not in (TokenType.NEWLINE, TokenType.EOF_TOKEN)]
            accepted = len(tokens) == 1 and tokens[0].type == TokenType.IDENT and tokens[0].value == rejected
        except Exception:
            accepted = False
        bindings.append(["rejected boundary " + repr(rejected), predicate(rejected) == accepted])
    strategy = STRATEGY.replace('armed = input.bool', '\n'.join(declarations) + '\narmed = input.bool')
    strategy = strategy.replace('qty=mult', 'qty=(' + ' + '.join(quantities) + ')')
    return {"producer_sha256": list(pinned.values()), "rows": rows, "bindings": bindings,
            "strategy": strategy, "cpp": transpile(strategy), "enum_count": len(identifiers),
            "scope": "unicode-isolation" if unicode_only else "full-class-matrix"}


def class_matrix_e2e(image, artifacts, unicode_only=False):
    """One producer-bound table for identifier/escape defaults and overrides."""
    artifacts.mkdir(parents=True)
    checks = []
    def check(name, condition, value):
        checks.append((name, bool(condition)))
        print(f"{'PASS' if condition else 'FAIL'} {name}: {value}", flush=True)
    oracle_command = ["docker", "run", "--rm", "--network", "none", "--user", "0",
                      "--mount", f"type=bind,src={Path(__file__).resolve()},dst=/probe.py,readonly",
                      "--entrypoint", "python3", image, "/probe.py",
                      "--producer-unicode" if unicode_only else "--producer-classes"]
    oracle = subprocess.run(oracle_command, capture_output=True, text=True, timeout=120)
    (artifacts / "producer.command.json").write_text(json.dumps(oracle_command))
    (artifacts / "producer.stdout.json").write_text(oracle.stdout)
    (artifacts / "producer.stderr.log").write_text(oracle.stderr)
    (artifacts / "producer.exit").write_text(str(oracle.returncode) + "\n")
    if oracle.returncode:
        raise RuntimeError("Pinned producer binding/setup failed; no matrix run")
    data = json.loads(oracle.stdout)
    for name, passed in data["bindings"]:
        check("binding " + name, passed, passed)
    legacy = None
    for label, is_legacy, overridden in (("checked-default", False, False),
                                         ("legacy-default", True, False),
                                         ("legacy-override", True, True)):
        case = (artifacts / label).resolve()
        source, compiled = case / "input", case / "compiled"
        source.mkdir(parents=True)
        compiled.mkdir()
        compiled.chmod(0o777)
        (source / "strategy.cpp").write_text(
            legacy if is_legacy else data["cpp"], encoding="utf-8")
        (source / "ohlcv.csv").write_text(synthetic_csv())
        inputs = {row["title"]: row["override"] for row in data["rows"]} if overridden else {}
        command = ["docker", "run", "--rm", "--network", "none",
                   "--mount", f"type=bind,src={source},dst=/in,readonly",
                   "--mount", f"type=bind,src={compiled},dst=/proof",
                   "-e", "PINEFORGE_INPUTS=" + json.dumps(inputs), image]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        (case / "command.json").write_text(json.dumps(command))
        (case / "stdout.json").write_text(result.stdout)
        (case / "stderr.log").write_text(result.stderr)
        (case / "exit").write_text(str(result.returncode) + "\n")
        check(label + " CLI success", result.returncode == 0, result.returncode)
        if result.returncode:
            raise RuntimeError("Native matrix run failed; dependent runs not launched")
        report = json.loads(result.stdout)
        if not is_legacy:
            sources = list(compiled.glob("*.cpp"))
            assert len(sources) == 1
            legacy = sources[0].read_text()
            for name in ("strategy_settings_api_version", "strategy_create_checked",
                         "strategy_set_input_checked", "strategy_set_override_checked"):
                legacy = legacy.replace(name, "fixture_legacy_" + name)
        fp = report.get("fingerprint") or {}
        provenance = fp.get("provenance", {})
        token = base64.b64decode(fp.get("token", ""))
        check(label + " fingerprint", bool(token) and json.loads(token) == provenance
              and fp.get("digest") == "sha256:" + hashlib.sha256(token).hexdigest(), fp.get("digest"))
        check(label + " raw inputs", report.get("applied_inputs") == inputs, report.get("applied_inputs"))
        applied = provenance.get("applied", {}).get("inputs", {})
        check(label + " applied keys", set(applied) == set(inputs), list(applied))
        for row in data["rows"]:
            actual = provenance.get("inputs", {}).get(row["title"], {})
            typ = int if row["kind"] == "int" else str
            expected = row["value"] if overridden else row["default"]
            check(label + " default " + row["class"], type(actual.get("default")) is typ
                  and actual["default"] == row["default"], actual)
            check(label + " value " + row["class"], type(actual.get("value")) is typ
                  and actual["value"] == expected, actual)
            if overridden:
                check(label + " applied " + row["class"], type(applied.get(row["title"])) is typ
                      and applied[row["title"]] == expected, applied.get(row["title"]))
        check(label + " native quantity", report["trades"][0]["qty"] == data["enum_count"] * (2 if overridden else 1),
              report["trades"][0])
    (artifacts / "checks.json").write_text(json.dumps(checks, indent=2) + "\n")
    print(f"{sum(passed for _, passed in checks)}/{len(checks)} checks passed", flush=True)
    return int(not all(passed for _, passed in checks))


def shadow_binding_e2e(image, artifacts, enum_cpp):
    artifacts.mkdir(parents=True)
    original = enum_cpp.read_text()
    for name in ("strategy_settings_api_version", "strategy_create_checked",
                 "strategy_set_input_checked", "strategy_set_override_checked"):
        original = original.replace(name, "fixture_legacy_" + name)
    marker = "class GeneratedStrategy : public pineforge::source::PineStrategyHost {\npublic:"
    assert original.count(marker) == 1
    shadow = original.replace(marker, marker + "\n    int Side__long_ = 2;")
    checks = []
    def check(name, condition, value):
        checks.append((name, bool(condition)))
        print(f"{'PASS' if condition else 'FAIL'} {name}: {value}", flush=True)
    for label, cpp, inputs in (("plain", original, {}), ("shadow", shadow, {}),
                                ("shadow-override", shadow, {"Side": "2"})):
        case = (artifacts / label).resolve()
        source, compiled = case / "input", case / "compiled"
        source.mkdir(parents=True)
        compiled.mkdir()
        compiled.chmod(0o777)
        (source / "strategy.cpp").write_text(cpp)
        (source / "ohlcv.csv").write_text(synthetic_csv())
        command = ["docker", "run", "--rm", "--network", "none",
                   "--mount", f"type=bind,src={source},dst=/in,readonly",
                   "--mount", f"type=bind,src={compiled},dst=/proof",
                   "-e", "PINEFORGE_INPUTS=" + json.dumps(inputs), image]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        (case / "command.json").write_text(json.dumps(command))
        (case / "stdout.json").write_text(result.stdout)
        (case / "stderr.log").write_text(result.stderr)
        (case / "exit").write_text(str(result.returncode) + "\n")
        check(label + " CLI success", result.returncode == 0, result.returncode)
        if result.returncode:
            raise RuntimeError("Shadow fixture failed before runtime assertions")
        report = json.loads(result.stdout)
        plugin, = compiled.glob("*.so")
        observe = ["docker", "run", "--rm", "--network", "none",
                   "--mount", f"type=bind,src={Path(__file__).resolve()},dst=/probe.py,readonly",
                   "--mount", f"type=bind,src={compiled},dst=/proof,readonly",
                   "--entrypoint", "python3", image, "/probe.py", "--native-receipt", "/proof/" + plugin.name]
        observed = subprocess.run(observe, capture_output=True, text=True, timeout=60)
        (case / "native.command.json").write_text(json.dumps(observe))
        (case / "native.stdout.json").write_text(observed.stdout)
        (case / "native.stderr.log").write_text(observed.stderr)
        (case / "native.exit").write_text(str(observed.returncode) + "\n")
        if observed.returncode:
            raise RuntimeError("Compiled native receipt observation failed")
        native = next(row for row in json.loads(observed.stdout)["inputs"] if row["name"] == "Side")
        check(label + " native binding", native["default"] == ("1" if label == "plain" else "2"), native)
        fp = report.get("fingerprint") or {}
        token = base64.b64decode(fp.get("token", ""))
        prov = fp.get("provenance", {})
        check(label + " fingerprint", bool(token) and json.loads(token) == prov
              and fp.get("digest") == "sha256:" + hashlib.sha256(token).hexdigest(), fp.get("digest"))
        row = prov.get("inputs", {}).get("Side", {})
        if label == "plain":
            check(label + " typed default", type(row.get("default")) is int and row["default"] == 1, row)
        else:
            check(label + " explicit unresolved", row.get("default") is None and row.get("value") is None
                  and row.get("resolution") == {"status": "unresolved", "reason": "ambiguous_binding",
                                                "raw_default": "Side__long_"}, row)
        check(label + " raw inputs", report.get("applied_inputs") == inputs, report.get("applied_inputs"))
        check(label + " applied keys", set(prov.get("applied", {}).get("inputs", {})) == set(inputs), prov.get("applied"))
        if inputs:
            check(label + " unresolved override remains wire text",
                  prov.get("applied", {}).get("inputs", {}).get("Side") == "2", prov.get("applied"))
    (artifacts / "checks.json").write_text(json.dumps(checks, indent=2) + "\n")
    print(f"{sum(p for _, p in checks)}/{len(checks)} checks passed", flush=True)
    return int(not all(p for _, p in checks))


if __name__ == "__main__":
    if sys.argv[1:] in (["--producer-classes"], ["--producer-unicode"]):
        print(json.dumps(producer_classes(sys.argv[1] == "--producer-unicode"), ensure_ascii=True))
        raise SystemExit(0)
    if len(sys.argv) == 3 and sys.argv[1] == "--native-receipt":
        import importlib.util
        spec = importlib.util.spec_from_file_location("native_observer", "/opt/pineforge/bin/run_json.py")
        observer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(observer)
        library = observer.load_strategy(Path(sys.argv[2]))
        state = observer.create_strategy(library, False)
        try:
            receipt = observer._release_settings_receipt(library, state, True)
            assert receipt is not None
            print(json.dumps(receipt))
        finally:
            library.strategy_free(state)
        raise SystemExit(0)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--harness", type=Path)
    parser.add_argument("--review-legacy-cpp", type=Path)
    parser.add_argument("--legacy-escaped-cpp", type=Path)
    parser.add_argument("--legacy-enum-cpp", type=Path)
    parser.add_argument("--class-matrix", action="store_true")
    parser.add_argument("--unicode-only", action="store_true")
    parser.add_argument("--shadow-cpp", type=Path)
    options = parser.parse_args()
    if options.shadow_cpp:
        raise SystemExit(shadow_binding_e2e(options.image, options.artifacts, options.shadow_cpp))
    if options.class_matrix:
        raise SystemExit(class_matrix_e2e(options.image, options.artifacts, options.unicode_only))
    if options.legacy_escaped_cpp or options.legacy_enum_cpp:
        if not (options.legacy_escaped_cpp and options.legacy_enum_cpp):
            parser.error("both legacy declaration sources are required")
        raise SystemExit(legacy_declaration_e2e(
            options.image, options.artifacts, options.legacy_escaped_cpp, options.legacy_enum_cpp))
    raise SystemExit(
        review_e2e(options.image, options.artifacts, options.review_legacy_cpp)
        if options.review_legacy_cpp else
        image_e2e(options.image, options.artifacts, options.harness))
