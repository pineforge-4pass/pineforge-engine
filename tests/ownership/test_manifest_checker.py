#!/usr/bin/env python3
"""EN-132 negative controls and an independent D4 reconstruction; no engine.

Run with Python 3.10+ (including -I). Mutations touch temporary copies only.
The positive tree check deliberately requires the integrated producers, rather
than accepting a recorded count or treating existing integration findings as a
successful check. The supervisor runs this suite on the completed tree.
"""
from __future__ import annotations

import ast
import copy
import ctypes
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "scripts/check_selected_window_ownership.py"
MANIFEST = ROOT / "tests/ownership/manifest.v1.json"


def emitted_schema(root: Path) -> dict:
    """D4 route two: evaluate the actual Python builders, not checker helpers.

    Only ctypes classes and pure report functions are compiled from the AST.
    No imports, CLI, CDLL, native library or main block is executed. One trade
    and one mark make both array element schemas observable even on an empty
    normal report. A second evaluation with defined absences derives nullable
    columns from _num / _stats_dict instead of borrowing the manifest's flags.
    """
    tree = ast.parse((root / "docker/run_json.py").read_text(encoding="utf-8"))
    classes = {"TradeC", "TradeStatsC", "EquityStatsC", "MetricsC", "EquityPointC"}
    functions = {"_num", "_stats_dict", "fmt_utc", "build_report_dict"}
    selected = [ast.ImportFrom(module="__future__", names=[ast.alias(name="annotations")], level=0)]
    found = set()
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name in classes:
            selected.append(node)
            found.add(node.name)
        elif isinstance(node, ast.FunctionDef) and node.name in functions:
            selected.append(node)
            found.add(node.name)
        elif isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "EQUITY_REPORT_KEYS" for t in node.targets):
            selected.append(node)
            found.add("EQUITY_REPORT_KEYS")
    if found != classes | functions | {"EQUITY_REPORT_KEYS"}:
        raise AssertionError(f"independent D4 builder extraction is incomplete: {sorted(found)}")
    namespace = {"ctypes": ctypes, "math": math, "Path": Path,
                 "datetime": datetime, "timezone": timezone}
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(root / "docker/run_json.py"), "exec"), namespace)

    # Independently compare the live ctypes members with the C POD closure.
    header = (root / "include/pineforge/pineforge.h").read_text(encoding="utf-8")
    header = re.sub(r"/\*.*?\*/|//[^\n]*", "", header, flags=re.DOTALL)
    for cls, pod in (("TradeC", "pf_trade"), ("TradeStatsC", "pf_trade_stats"),
                     ("EquityStatsC", "pf_equity_stats"), ("EquityPointC", "pf_equity_point")):
        body = re.search(r"typedef\s+struct\s+" + pod + r"_s\s*\{(.*?)\}\s*" + pod + r"_t", header, re.S)
        if body is None:
            raise AssertionError(f"missing C descriptor {pod}")
        fields = re.findall(r"\b(double|int|int32_t|int64_t)\s+(\w+)\s*;", body[1])
        types = {"double": ctypes.c_double, "int": ctypes.c_int,
                 "int32_t": ctypes.c_int32, "int64_t": ctypes.c_int64}
        actual = namespace[cls]._fields_
        if [(name, types[kind]) for kind, name in fields] != actual:
            raise AssertionError(f"independent ctypes/C descriptor divergence: {cls}")

    class Report:
        trades_len = 1
        net_profit = 2.0
        input_bars_processed = 1
        script_bars_processed = 1
        magnifier_sub_bars_total = 0
        magnifier_sample_ticks_total = 0
        bar_magnifier_enabled = 0
        equity_curve_len = 1

    report = Report()
    report.trades = (namespace["TradeC"] * 1)()
    report.equity_curve = (namespace["EquityPointC"] * 1)()
    report.metrics = namespace["MetricsC"]()
    for name, kind in namespace["TradeC"]._fields_:
        setattr(report.trades[0], name, 1.0 if kind is ctypes.c_double else 1)
    for name, kind in namespace["EquityPointC"]._fields_:
        setattr(report.equity_curve[0], name, 1.0 if kind is ctypes.c_double else 1)
    for block, kind in namespace["MetricsC"]._fields_:
        for name, ctype in kind._fields_:
            setattr(getattr(report.metrics, block), name, 1.0 if ctype is ctypes.c_double else 1)

    def build():
        out = namespace["build_report_dict"](
            report, Path("feed.csv"), 1, 1, 1, 0.0, {}, {}, {}, [1])
        # D4's version leaf comes from the real selected-window wire version;
        # this is a reconstruction, not a claim that a digest is implemented.
        window = ast.parse((root / "docker/selected_window_report.py").read_text(encoding="utf-8"))
        versions = [ast.literal_eval(n.value) for n in window.body if isinstance(n, ast.Assign)
                    and any(isinstance(t, ast.Name) and t.id == "WIRE_VERSION" for t in n.targets)]
        if len(versions) != 1 or type(versions[0]) is not int:
            raise AssertionError("independent D4 version source is ambiguous")
        return {"version": versions[0], **{key: out[key] for key in
                ("summary", "metrics", "trades", "equity_curve")}}

    finite = build()
    report.trades[0].is_long = 0
    opposite = build()
    sides = sorted({finite["trades"][0]["side"], opposite["trades"][0]["side"]})
    for block, kind in namespace["MetricsC"]._fields_:
        for name, ctype in kind._fields_:
            if ctype is ctypes.c_double:
                setattr(getattr(report.metrics, block), name, float("nan"))
    report.equity_curve[0].equity = float("nan")
    report.equity_curve[0].open_profit = float("nan")
    absent = build()
    leaves, containers = {}, {}

    def walk(value, undefined, path):
        if isinstance(value, dict):
            containers[path] = "object"
            for key, item in value.items():
                walk(item, undefined[key], key if path == "$" else f"{path}.{key}")
        elif isinstance(value, list):
            containers[path] = "array"
            if len(value) != 1:
                raise AssertionError(f"independent D4 needs one observable element at {path}")
            walk(value[0], undefined[0], path + "[*]")
        else:
            kind = {int: "integer", float: "number", bool: "boolean", str: "string"}.get(type(value))
            if kind is None:
                raise AssertionError(f"independent D4 unknown scalar {path}: {type(value)}")
            leaves[path] = {"kind": kind, "null_allowed": undefined is None}

    walk(finite, absent, "$")
    leaves["trades[*].side"]["values"] = sides
    return {"leaves": leaves, "containers": containers, "version": finite["version"]}


def manifest_schema(doc: dict) -> dict:
    # Route one is an explicit contract list, not a call to either derivation.
    schema = doc["canonical_schema"]
    leaves = {row["path"]: {"kind": row["kind"], "null_allowed": row["null_allowed"]}
              for row in schema["leaves"]}
    for row in schema["leaves"]:
        if "values" in row:
            leaves[row["path"]]["values"] = row["values"]
    containers = {row["path"]: row["kind"] for row in schema["containers"]}
    if len(leaves) != len(schema["leaves"]) or len(containers) != len(schema["containers"]):
        raise AssertionError("D4 manifest contains duplicate paths")
    return {"leaves": leaves, "containers": containers, "version": schema["version"]}


class ManifestCheckerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pineforge-ownership-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.doc = json.loads(MANIFEST.read_text(encoding="utf-8"))

    def run_checker(self, *, copied_tree=False):
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps(self.doc, indent=2) + "\n", encoding="utf-8")
        return subprocess.run(
            [sys.executable, "-I", "-B", str(CHECKER), "--root",
             str(self.root if copied_tree else ROOT), "--manifest", str(manifest)],
            cwd=self.root, capture_output=True, text=True, timeout=60)

    def copy_tree(self):
        for directory in ("src", "include", "docker"):
            shutil.copytree(ROOT / directory, self.root / directory,
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    def reject(self, check, detail, *, copied_tree=False):
        result = self.run_checker(copied_tree=copied_tree)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "", result.stderr)
        self.assertIn(f"ownership[{check}]: {detail}", result.stdout.splitlines(), result.stdout)

    def sink(self, name):
        return next(row for row in self.doc["sinks"] if row["id"] == name)

    def test_integrated_tree_is_green(self):
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "")

    def test_kept_branch_anchor_removed_is_rejected(self):
        self.copy_tree()
        cases = (
            ("engine.src/engine_execution.cpp.settlement", "src/engine_execution.cpp",
             "execution::Status BacktestEngine::prepare_native_settlement_commit(",
             "prepare_native_settlement_commit", "InvalidAccounting", "NoEffect"),
            ("engine.src/engine_orders.cpp.settlement", "src/engine_orders.cpp",
             "bool internal::closed_row_all_finite(",
             "closed_row_all_finite", "isfinite", "isnan"),
            ("engine.src/native_order.cpp.relative_price", "src/native_order.cpp",
             "Preparation<PreparedMutation> WorkingRequestCore::prepare_owner_applied(",
             "prepare_owner_applied", "NonrepresentableQuantity", "InvalidCause"),
        )
        for name, file, signature, function, literal, replacement in cases:
            with self.subTest(branch=name):
                path = self.root / file
                text = path.read_text(encoding="utf-8")
                start = text.index(signature)
                end = text.index("\n}", start)
                # Keep the literal in other functions and in a comment at
                # this site: neither may satisfy the function's code anchor.
                self.assertIn(literal, text[:start] + text[end:])
                body, changed = re.subn(r"\b" + re.escape(literal) + r"\b",
                                        replacement, text[start:end])
                self.assertGreater(changed, 0)
                body += f"\n    // Previous KEEP anchor: {literal}\n"
                path.write_text(text[:start] + body + text[end:], encoding="utf-8")
                try:
                    self.reject("kept-branch-anchor",
                                f"{name}: anchor {literal} absent in {function}", copied_tree=True)
                finally:
                    path.write_text(text, encoding="utf-8")

    def test_kept_branch_gains_a_producer_is_rejected(self):
        self.copy_tree()
        cases = (
            ("src/engine_execution.cpp", "execution::Status BacktestEngine::prepare_native_settlement_commit(",
             "settlement", "settlement.row"),
            ("src/engine_execution.cpp", "execution::Status BacktestEngine::prepare_native_settlement_commit(",
             "settlement", "fee.commission"),
            ("src/engine_execution.cpp", "execution::Status BacktestEngine::prepare_native_settlement_commit(",
             "fee", "settlement.row"),
            ("src/engine_orders.cpp", "Trade BacktestEngine::build_close_trade_with_costs(",
             "settlement", "settlement.row"),
            ("src/engine_orders.cpp", "Trade BacktestEngine::build_close_trade_with_costs(",
             "settlement", "fee.commission"),
            ("src/engine_orders.cpp", "Trade BacktestEngine::build_close_trade_with_costs(",
             "fee", "settlement.row"),
            ("src/native_order.cpp", "Preparation<PreparedMutation> WorkingRequestCore::prepare_owner_applied(",
             "relative_price", "relative_price.level"),
            ("src/native_order.cpp", "Preparation<PreparedMutation> WorkingRequestCore::prepare_owner_applied(",
             "sizing", "relative_price.level"),
        )
        for file, signature, stage, field in cases:
            with self.subTest(file=file, stage=stage, field=field):
                path = self.root / file
                text = path.read_text(encoding="utf-8")
                opening = text.index("{", text.index(signature)) + 1
                call = (f'\n    pineforge::engine_command_value_invalid_or_count("{field}",\n'
                        f"        pineforge::EngineValueStage::{stage},\n"
                        "        pineforge::EngineValueReason::nonfinite);\n")
                path.write_text('#include "engine_value_invalid.hpp"\n'
                                + text[:opening] + call + text[opening:], encoding="utf-8")
                try:
                    self.reject("kept-branch-producer",
                                f"{file}: engine_command_value_invalid_or_count stage {stage} "
                                f"field {field} contradicts KEEP", copied_tree=True)
                finally:
                    path.write_text(text, encoding="utf-8")

    def test_omitted_kept_branch_is_rejected(self):
        rows = self.doc["kept_branches"]
        rows.remove(next(row for row in rows if row["id"] == "engine.src/native_order.cpp.relative_price"))
        self.reject("kept-branch-set", "missing engine.src/native_order.cpp.relative_price")

    def test_duplicate_kept_branch_is_rejected(self):
        row = next(row for row in self.doc["kept_branches"]
                   if row["id"] == "engine.src/native_order.cpp.relative_price")
        self.doc["kept_branches"].append(copy.deepcopy(row))
        self.reject("duplicate-kept-branch", "engine.src/native_order.cpp.relative_price")

    def test_kept_branch_outcome_changed_is_rejected(self):
        row = next(row for row in self.doc["kept_branches"]
                   if row["id"] == "engine.src/engine_execution.cpp.settlement")
        row["outcome"] = "engine_command_value_invalid"
        self.reject("kept-branch-decision",
                    "engine.src/engine_execution.cpp.settlement: outcome differs from D6 KEEP decision")

    def test_live_fee_producer_mapping_matches_contract(self):
        # This check isolates the code-literal mutant from other integration
        # gaps: it must pass before the mutant and fail for the fee mapping.
        result = self.run_checker()
        self.assertNotEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertFalse(any(line.startswith("ownership[vocabulary]: engine_command_value_invalid: unknown field fee.")
                             or line.startswith("ownership[sink-mapping]: engine.src/engine_execution.cpp.fee:")
                             for line in result.stdout.splitlines()), result.stdout)

    def test_live_selected_sink_inventory_matches_contract(self):
        result = self.run_checker()
        self.assertNotEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertFalse(any(line.startswith("ownership[sink-set]: missing source.")
                             for line in result.stdout.splitlines()), result.stdout)

    def test_renamed_manifest_field_is_rejected(self):
        self.sink("source.admit_strategy_entry")["fields"][0]["field"] = "entry.renamed_id"
        self.reject("sink-mapping", "source.admit_strategy_entry: fields/reasons differ from code")

    def test_omitted_sink_is_rejected(self):
        self.doc["sinks"].remove(self.sink("source.admit_strategy_entry"))
        self.reject("sink-set", "missing source.admit_strategy_entry")

    def test_duplicate_sink_is_rejected(self):
        self.doc["sinks"].append(copy.deepcopy(self.sink("source.admit_strategy_entry")))
        self.reject("duplicate-sink", "source.admit_strategy_entry")

    def test_stale_producer_anchor_is_rejected(self):
        self.sink("source.admit_strategy_entry")["anchor"] = "admit_strategy_entry_removed"
        self.reject("producer-anchor", "source.admit_strategy_entry: anchor admit_strategy_entry_removed absent")

    def test_same_count_mapping_swap_is_rejected(self):
        entry, order = self.sink("source.admit_strategy_entry"), self.sink("source.admit_strategy_order")
        before = len(self.doc["sinks"])
        entry["fields"][0], order["fields"][0] = order["fields"][0], entry["fields"][0]
        self.assertEqual(before, len(self.doc["sinks"]))
        self.reject("sink-mapping", "source.admit_strategy_entry: fields/reasons differ from code")

    def test_catalog_code_missing_from_manifest_is_rejected(self):
        self.doc["catalog"]["codes"] = [row for row in self.doc["catalog"]["codes"]
                                         if row["code"] != "outputs_limit"]
        self.reject("catalog-set", "missing outputs_limit")

    def test_catalog_registry_missing_code_is_rejected(self):
        self.copy_tree()
        path = self.root / "src/run_failure_registry.inc"
        text = path.read_text(encoding="utf-8")
        text, changed = re.subn(r'^\s*\{"outputs_limit",[^\n]*\n', "", text, flags=re.M)
        self.assertEqual(changed, 1)
        path.write_text(text, encoding="utf-8")
        self.reject("registry-set", "stale outputs_limit", copied_tree=True)

    def test_unknown_producer_field_is_rejected_by_vocabulary(self):
        # Killer for vocabulary-check-disabled, even if another check also fails.
        self.copy_tree()
        path = self.root / "src/engine_execution.cpp"
        text = path.read_text(encoding="utf-8")
        self.assertIn('engine_command_value_invalid_or_count("fee.commission"', text)
        path.write_text(text.replace('engine_command_value_invalid_or_count("fee.commission"',
                                    'engine_command_value_invalid_or_count("fee.unknown"', 1), encoding="utf-8")
        self.reject("vocabulary", "engine_command_value_invalid: unknown field fee.unknown", copied_tree=True)

    def test_unknown_stage_is_rejected_by_vocabulary(self):
        self.copy_tree()
        path = self.root / "src/engine_execution.cpp"
        text = path.read_text(encoding="utf-8")
        self.assertIn("EngineValueStage::fee", text)
        path.write_text(text.replace("EngineValueStage::fee", "EngineValueStage::unknown", 1), encoding="utf-8")
        self.reject("vocabulary", "engine_command_value_invalid: unknown stage unknown", copied_tree=True)

    def test_unknown_reason_is_rejected_by_vocabulary(self):
        self.copy_tree()
        path = self.root / "src/source/pine_value_admit_decl.hpp"
        text = path.read_text(encoding="utf-8")
        self.assertIn("StrategyValueReason::nonfinite", text)
        path.write_text(text.replace("StrategyValueReason::nonfinite", "StrategyValueReason::unknown", 1), encoding="utf-8")
        self.reject("vocabulary", "strategy_value_invalid: unknown reason unknown", copied_tree=True)

    def test_propagated_constant_literal_is_rejected_by_vocabulary(self):
        self.copy_tree()
        path = self.root / "src/source/pine_value_admission.hpp"
        text = path.read_text(encoding="utf-8")
        self.assertIn('entry_id = "entry.id"', text)
        path.write_text(text.replace('entry_id = "entry.id"', 'entry_id = "entry.unknown"', 1), encoding="utf-8")
        self.reject("vocabulary", "strategy_value_invalid: unknown field entry.unknown", copied_tree=True)

    def test_stale_code_producer_is_rejected(self):
        self.copy_tree()
        path = self.root / "src/source/pine_value_admission.cpp"
        text = path.read_text(encoding="utf-8")
        self.assertIn("admit_text(id, value_field::entry_id);", text)
        path.write_text(text.replace("admit_text(id, value_field::entry_id);", "", 1), encoding="utf-8")
        self.reject("sink-mapping", "source.admit_strategy_entry: fields/reasons differ from code", copied_tree=True)

    def test_same_count_code_operand_swap_is_rejected(self):
        self.copy_tree()
        path = self.root / "src/source/pine_value_admission.cpp"
        text = path.read_text(encoding="utf-8")
        first = "admit_text(id, value_field::entry_id);"
        second = "admit_text(comment, value_field::entry_comment);"
        self.assertEqual(text.count(first), 1)
        self.assertEqual(text.count(second), 1)
        changed = text.replace(first, "admit_text(id, value_field::entry_comment);", 1)
        changed = changed.replace(second, "admit_text(comment, value_field::entry_id);", 1)
        path.write_text(changed, encoding="utf-8")
        self.reject("producer-binding", "admit_strategy_entry: id is labelled entry.comment", copied_tree=True)

    def test_disconnected_selected_door_is_rejected(self):
        self.copy_tree()
        path = self.root / "src/source/pine_strategy_commands.cpp"
        text = path.read_text(encoding="utf-8")
        call = ("    admit_strategy_entry(id, is_long, limit_price, stop_price, qty, comment, oca_name,\n"
                "                         oca_type, qty_type);\n")
        self.assertEqual(text.count(call), 1)
        path.write_text(text.replace(call, "", 1), encoding="utf-8")
        self.reject("admission-route", "src/source/pine_strategy_commands.cpp:strategy_entry/9: helper calls differ from manifest", copied_tree=True)

    def test_source_helper_code_removal_is_rejected(self):
        self.copy_tree()
        path = self.root / "src/source/pine_value_admission.hpp"
        text = path.read_text(encoding="utf-8")
        self.assertIn("RunFailureCode::strategy_value_invalid,", text)
        path.write_text(text.replace("RunFailureCode::strategy_value_invalid,", "RunFailureCode::engine_invariant,", 1), encoding="utf-8")
        self.reject("producer-helper", "throw_strategy_value_invalid: coded producer/payload differs from contract", copied_tree=True)

    def test_new_public_field_has_no_implicit_owner(self):
        self.copy_tree()
        path = self.root / "include/pineforge/source/pine_adapter.hpp"
        text = path.read_text(encoding="utf-8")
        self.assertIn("struct PineStrategyConfig {", text)
        path.write_text(text.replace("struct PineStrategyConfig {", "struct PineStrategyConfig {\n    double new_value = 0.0;", 1), encoding="utf-8")
        self.reject("descriptor-fields", "PineStrategyConfig: declaration fields differ from manifest", copied_tree=True)

    def test_d4_independent_reconstruction_matches_manifest(self):
        self.assertEqual(manifest_schema(self.doc), emitted_schema(ROOT))

    def test_d4_manifest_leaf_removed_is_rejected(self):
        # Killer for D4-comparison-disabled; checks this specific diagnostic.
        rows = self.doc["canonical_schema"]["leaves"]
        rows.remove(next(row for row in rows if row["path"] == "summary.net_pnl"))
        self.assertNotEqual(manifest_schema(self.doc), emitted_schema(ROOT))
        self.reject("schema-leaves", "missing summary.net_pnl")

    def test_d4_same_count_code_alias_change_is_rejected(self):
        self.copy_tree()
        path = self.root / "docker/run_json.py"
        text = path.read_text(encoding="utf-8")
        self.assertIn('"sharpe_monthly": "sharpe_tv"', text)
        path.write_text(text.replace('"sharpe_monthly": "sharpe_tv"', '"sharpe_monthly": "sharpe_changed"', 1), encoding="utf-8")
        self.assertNotEqual(manifest_schema(self.doc), emitted_schema(self.root))
        self.reject("schema-leaves", "missing metrics.equity.sharpe_changed", copied_tree=True)

    def test_d4_code_leaf_removed_is_rejected(self):
        self.copy_tree()
        path = self.root / "docker/run_json.py"
        text = path.read_text(encoding="utf-8")
        line = '            "net_pnl":        float(report.net_profit),\n'
        self.assertEqual(text.count(line), 1)
        path.write_text(text.replace(line, "", 1), encoding="utf-8")
        self.assertNotEqual(manifest_schema(self.doc), emitted_schema(self.root))
        self.reject("schema-leaves", "stale summary.net_pnl", copied_tree=True)

    def test_d4_container_removed_is_rejected(self):
        rows = self.doc["canonical_schema"]["containers"]
        rows.remove(next(row for row in rows if row["path"] == "trades[*]"))
        self.reject("schema-containers", "missing trades[*]")

    def test_d4_code_enum_changed_is_rejected(self):
        self.copy_tree()
        path = self.root / "docker/run_json.py"
        text = path.read_text(encoding="utf-8")
        line = '"side":         "long" if t.is_long else "short"'
        self.assertEqual(text.count(line), 1)
        path.write_text(text.replace(line, '"side":         "long_changed" if t.is_long else "short"', 1), encoding="utf-8")
        self.assertNotEqual(manifest_schema(self.doc), emitted_schema(self.root))
        self.reject("schema-leaf-type", "trades[*].side: kind/null/enum policy differs from code", copied_tree=True)

    def test_duplicate_json_key_is_rejected(self):
        path = self.root / "bad.json"
        path.write_text('{"format":"one","format":"two"}', encoding="utf-8")
        result = subprocess.run([sys.executable, "-I", "-B", str(CHECKER), "--manifest", str(path)],
                                capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ownership[input]: duplicate JSON key format", result.stdout.splitlines())


if __name__ == "__main__":
    unittest.main()
