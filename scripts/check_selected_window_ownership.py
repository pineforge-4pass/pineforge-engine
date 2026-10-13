#!/usr/bin/env python3
"""Compare revision-3 ownership with source producers and the D4 reconstruction.

stdlib only, Python 3.10+, safe under -I. This guard reads source; it neither
imports the harness nor loads a strategy. Success is silent. Every finding is
one ownership[check] line. Unknown syntax at a producer fails closed.

The manifest is the contract route. The other route discovers C++ producer
calls (including field-parameter propagation), Python raisers, public input
descriptors, catalog/registry rows, ctypes members, and report dictionaries.
No expected field or schema list is taken from the manifest for discovery.
"""
from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path("tests/ownership/manifest.v1.json")
S = "strategy_value_invalid"
G = "engine_command_value_invalid"
R = "report_result_invalid"
X = "report_post_execution_failed"
PRODUCERS = {S, G, R, X}
# These three later D6 decisions are a closed part of the contract, so a
# manifest cannot erase a KEEP or turn it back into a producer obligation.
KEPT_BRANCH_DECISIONS = {
    "engine.src/native_order.cpp.relative_price": {
        "file": "src/native_order.cpp", "d6_row": 17, "outcome": "native refusal"},
    "engine.src/engine_execution.cpp.settlement": {
        "file": "src/engine_execution.cpp", "d6_row": 21, "outcome": "engine_invariant"},
    "engine.src/engine_orders.cpp.settlement": {
        "file": "src/engine_orders.cpp", "d6_row": 22, "outcome": "engine_invariant"},
}
CPP_TOKEN = re.compile(
    r'//[^\n]*|/\*.*?\*/|(?:u8|u|U|L)?R"(?P<raw>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=raw)"'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    r'|[A-Za-z_]\w*|[0-9]+(?:\.[0-9]*)?(?:[eE][+-]?[0-9]+)?|::|->|\S', re.S)
IDENT = re.compile(r"[A-Za-z_]\w*\Z")


class InputError(Exception):
    pass


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def json_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise InputError(f"duplicate JSON key {key}")
        result[key] = value
    return result


def load_json(path: Path):
    def bad_constant(value):
        raise InputError(f"nonfinite JSON constant {value}")
    return json.loads(read(path), object_pairs_hook=json_pairs, parse_constant=bad_constant)


class Findings:
    def __init__(self):
        self.lines = set()

    def add(self, check, detail):
        self.lines.add(f"ownership[{check}]: {detail}")

    def compare_set(self, check, expected, actual):
        for value in sorted(set(actual) - set(expected)):
            self.add(check, f"missing {value}")
        for value in sorted(set(expected) - set(actual)):
            self.add(check, f"stale {value}")


def unique_rows(rows, key, check, findings):
    result = {}
    for row in rows:
        value = row[key]
        if value in result:
            findings.add(check, value)
        result[value] = row
    return result


class Cpp:
    def __init__(self, text, file=""):
        self.file = file
        self.tokens = [m[0] for m in CPP_TOKEN.finditer(text)
                       if not m[0].startswith(("//", "/*"))]
        self.pairs = {}
        stack = []
        closing = {")": "(", "}": "{", "]": "["}
        for i, token in enumerate(self.tokens):
            if token in ("(", "{", "["):
                stack.append((token, i))
            elif token in closing and stack and stack[-1][0] == closing[token]:
                _, opening = stack.pop()
                self.pairs[opening] = i

    def parts(self, start, end, *, separator=",", angles=False):
        parts, beginning, depth, i = [], start, 0, start
        while i < end:
            token = self.tokens[i]
            if i in self.pairs:
                i = self.pairs[i] + 1
                continue
            if angles and token == "<":
                depth += 1
            elif angles and token == ">" and depth:
                depth -= 1
            elif token == separator and depth == 0:
                parts.append(self.tokens[beginning:i])
                beginning = i + 1
            i += 1
        if beginning < end:
            parts.append(self.tokens[beginning:end])
        return parts

    def calls(self, names=None, start=0, end=None):
        end = len(self.tokens) if end is None else end
        for i in range(start, end - 1):
            name = self.tokens[i]
            if (IDENT.fullmatch(name) and (names is None or name in names)
                    and self.tokens[i + 1] == "(" and i + 1 in self.pairs):
                close = self.pairs[i + 1]
                if close < end:
                    yield name, i, close, self.parts(i + 2, close)

    def definitions(self):
        result = []
        for name, index, close, args in self.calls():
            if name in {"if", "for", "while", "switch", "catch"}:
                continue
            opening = close + 1
            while opening < len(self.tokens) and self.tokens[opening] in {
                    "const", "noexcept", "override", "final"}:
                opening += 1
                if opening in self.pairs and self.tokens[opening] == "(":
                    opening = self.pairs[opening] + 1
            if opening not in self.pairs or self.tokens[opening] != "{":
                continue
            parameters = []
            for arg in args:
                head = arg[:arg.index("=")] if "=" in arg else arg
                parameters.append(head[-1] if head and IDENT.fullmatch(head[-1]) else "")
            result.append({"name": name, "index": index, "close": close,
                           "start": opening + 1, "end": self.pairs[opening],
                           "params": parameters, "args": args})
        return result

    def block(self, category, name):
        for i, token in enumerate(self.tokens):
            if token != name or i == 0:
                continue
            previous = self.tokens[max(0, i - 2):i]
            if category == "struct" and previous[-1:] != ["struct"]:
                continue
            if category == "enum" and previous != ["enum", "class"]:
                continue
            opening = i + 1
            while opening < len(self.tokens) and self.tokens[opening] not in {"{", ";"}:
                opening += 1
            if opening in self.pairs and self.tokens[opening] == "{":
                return opening + 1, self.pairs[opening]
        raise InputError(f"missing {category} descriptor {name}")

    def fields(self, name):
        start, end = self.block("struct", name)
        result = []
        for declaration in self.parts(start, end, separator=";", angles=True):
            # Initializers cannot turn a new public field into an old one.
            boundary = next((i for i, t in enumerate(declaration) if t in {"=", "{"}), len(declaration))
            head = declaration[:boundary]
            if "(" in head:
                continue  # a method, not a data member
            if head and IDENT.fullmatch(head[-1]):
                result.append(head[-1])
            elif head:
                raise InputError(f"unrecognized member of {name}: {' '.join(head)}")
        return result

    def enum(self, name):
        start, end = self.block("enum", name)
        return [part[0] for part in self.parts(start, end) if part]


def string_value(tokens):
    if len(tokens) == 1 and tokens[0].startswith('"'):
        try:
            value = ast.literal_eval(tokens[0])
        except (SyntaxError, ValueError):
            return None
        return value if isinstance(value, str) else None
    return None


def field_value(tokens, constants, parameters=()):
    literal = string_value(tokens)
    if literal is not None:
        return ("literal", literal)
    if len(tokens) == 1 and tokens[0] in parameters:
        return ("parameter", parameters.index(tokens[0]))
    if len(tokens) >= 3 and tokens[-3:-1] == ["value_field", "::"]:
        return ("literal", constants.get(tokens[-1], f"<unknown value_field::{tokens[-1]}>"))
    if tokens in (["nullptr"], ["0"]) or not tokens:
        return ("absent", "")
    return ("unknown", " ".join(tokens))


def enum_values(tokens, enum):
    values = {tokens[i + 2] for i in range(len(tokens) - 2)
              if tokens[i:i + 2] == [enum, "::"]}
    literal = string_value(tokens)
    if literal is not None:
        values.add(literal)
    return values


def sink_row(identifier, file, anchor, producer, fields=(), *, stage=None, phases=(), reasons=()):
    grouped = defaultdict(set)
    for field, reason in fields:
        grouped[field].add(reason)
    return {"id": identifier, "file": file, "anchor": anchor, "producer": producer,
            "stage": stage, "phases": sorted(set(phases)), "reasons": sorted(set(reasons)),
            "fields": [{"field": field, "reasons": sorted(values)} for field, values in sorted(grouped.items())]}


def source_producers(cpp_files, constants, findings):
    functions = []
    for path, cpp in cpp_files.items():
        if path.startswith("src/source/"):
            for function in cpp.definitions():
                if ("field" in function["params"] or function["name"].startswith(
                        ("admit_strategy_", "admit_pine_risk_"))):
                    function.update(file=path, cpp=cpp)
                    functions.append(function)
    helpers = defaultdict(set)
    # Derive field flow from throw sites, then substitute caller arguments
    # until the helper summaries reach a fixed point. Parameters remain
    # abstract inside helpers; an effective door must resolve to a literal.
    for _ in range(len(functions) + 1):
        changed = False
        for function in functions:
            name, cpp = function["name"], function["cpp"]
            summary = set()
            for callee, _, _, args in cpp.calls(set(helpers) | {"throw_strategy_value_invalid"},
                                                function["start"], function["end"]):
                if callee == "throw_strategy_value_invalid":
                    if len(args) != 2:
                        findings.add("producer-call", f"{function['file']}: malformed {callee}")
                        continue
                    reasons = enum_values(args[1], "StrategyValueReason")
                    if not reasons:
                        findings.add("producer-call", f"{function['file']}: unresolved source reason")
                    for reason in reasons:
                        summary.add((field_value(args[0], constants, function["params"]), reason))
                else:
                    for field, reason in helpers[callee]:
                        if field[0] == "parameter":
                            field = (field_value(args[field[1]], constants, function["params"])
                                     if field[1] < len(args) else ("absent", ""))
                        if field[0] != "absent":
                            summary.add((field, reason))
            old = len(helpers[name])
            helpers[name].update(summary)
            changed |= len(helpers[name]) != old
        if not changed:
            break
    rows = []
    for function in functions:
        if not function["name"].startswith(("admit_strategy_", "admit_pine_risk_")):
            continue
        fields = []
        for field, reason in helpers[function["name"]]:
            if field[0] != "literal":
                findings.add("field-literal", f"{function['name']}: source field is not an engine literal")
            else:
                fields.append((field[1], reason))
        rows.append(sink_row("source." + function["name"], function["file"], function["name"], S, fields))
    # Raw arithmetic is admitted at literal-bearing callsites, not every call
    # of a generic helper (which also serves already engine-owned arithmetic).
    for path, cpp in cpp_files.items():
        if path != "src/source/pine_adapter.cpp":
            continue
        fields = []
        definitions = cpp.definitions()
        definition_indices = {function["index"] for function in definitions}
        arithmetic_helpers = {name for name, summary in helpers.items()
                              if any(field[0] == "parameter" and reason == "numeric_overflow"
                                     for field, reason in summary)}
        for callee, index, _, args in cpp.calls(arithmetic_helpers):
            if index in definition_indices:
                continue
            containing = [function for function in definitions if function["start"] <= index < function["end"]]
            parameters = min(containing, key=lambda f: f["end"] - f["start"])["params"] if containing else ()
            for field, reason in helpers[callee]:
                if field[0] == "parameter":
                    field = field_value(args[field[1]], constants, parameters) if field[1] < len(args) else ("absent", "")
                if field[0] == "literal":
                    fields.append((field[1], reason))
                elif field[0] == "unknown":
                    findings.add("field-literal", f"{path}: unresolved field passed to {callee}: {field[1]}")
        if fields:
            rows.append(sink_row("source.arithmetic", path, "source_arith_value", S, fields))
    # A newly added direct raiser cannot hide outside the known admission
    # family. Discover it without consulting the manifest's source-site list.
    known = {(f["file"], f["name"]) for f in functions}
    for path, cpp in cpp_files.items():
        if "throw_strategy_value_invalid" not in cpp.tokens:
            continue
        definitions = cpp.definitions()
        definition_indices = {f["index"] for f in definitions}
        extras = defaultdict(list)
        for _, index, _, args in cpp.calls({"throw_strategy_value_invalid"}):
            if index in definition_indices:
                continue
            enclosing = [f for f in definitions if f["start"] <= index < f["end"]]
            caller = min(enclosing, key=lambda f: f["end"] - f["start"]) if enclosing else None
            if caller is not None and (path, caller["name"]) in known:
                continue
            if len(args) != 2:
                findings.add("producer-call", f"{path}: malformed throw_strategy_value_invalid")
                continue
            field = field_value(args[0], constants)
            reasons = enum_values(args[1], "StrategyValueReason")
            if field[0] != "literal" or not reasons:
                findings.add("field-literal", f"{path}: untracked source field/reason forwarding")
                continue
            name = caller["name"] if caller else "global"
            extras[name].extend((field[1], reason) for reason in reasons)
        for name, fields in extras.items():
            rows.append(sink_row(f"source.{path}.{name}", path, name, S, fields))
    return rows


def engine_producers(cpp_files, constants, findings):
    rows = []
    names = {"engine_command_value_invalid_or_count", "throw_engine_command_value_invalid"}
    for path, cpp in cpp_files.items():
        definitions = cpp.definitions() if any(n in cpp.tokens for n in names) else []
        definitions_by_index = {function["index"] for function in definitions}
        grouped = defaultdict(list)
        for callee, index, _, args in cpp.calls(names):
            if index in definitions_by_index:
                continue
            if (path == "src/engine_value_invalid.hpp" and callee == "throw_engine_command_value_invalid"
                    and args == [["field"], ["stage"], ["reason"]]):
                continue  # the authenticated tripwire wrapper's forwarding call
            if len(args) != 3:
                findings.add("producer-call", f"{path}: malformed {callee}")
                continue
            field = field_value(args[0], constants)
            stages = enum_values(args[1], "EngineValueStage")
            reasons = enum_values(args[2], "EngineValueReason")
            if field[0] != "literal" or not stages or not reasons:
                findings.add("field-literal", f"{path}: unresolved field/stage/reason passed to {callee}")
                continue
            for stage in stages:
                for reason in reasons:
                    grouped[stage].append((field[1], reason))
        for stage, fields in grouped.items():
            rows.append(sink_row(f"engine.{path}.{stage}", path, "engine_command_value_invalid_or_count", G,
                                 fields, stage=stage))
    return rows


def py_assignments(tree):
    result = {}
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    result[target.id] = node.value
    return result


def py_dict(node):
    if not isinstance(node, ast.Dict):
        raise InputError("unrecognized Python object producer")
    result = {}
    for key, value in zip(node.keys, node.values):
        if not isinstance(key, ast.Constant) or not isinstance(key.value, str) or key.value in result:
            raise InputError("nonliteral or duplicate Python producer key")
        result[key.value] = value
    return result


def py_domains(function):
    result = defaultdict(set)
    for node in ast.walk(function):
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant) and isinstance(node.value.value, str):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    result[target.id].add(node.value.value)
        elif isinstance(node, ast.For) and isinstance(node.target, ast.Tuple) and isinstance(node.iter, (ast.Tuple, ast.List)):
            for row in node.iter.elts:
                if isinstance(row, (ast.Tuple, ast.List)) and len(row.elts) == len(node.target.elts):
                    for target, value in zip(node.target.elts, row.elts):
                        if isinstance(target, ast.Name) and isinstance(value, ast.Constant) and isinstance(value.value, str):
                            result[target.id].add(value.value)
    return result


def py_values(node, domains):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return {node.value}
    if isinstance(node, ast.Name):
        return domains[node.id]
    raise InputError("unresolved Python producer argument")


def python_producers(root, findings):
    rows = []
    for path in sorted((root / "docker").glob("*.py")):
        text = read(path)
        if not any(code in text for code in (S, G, R, X)):
            continue
        tree = ast.parse(text, filename=str(path))
        assignments = py_assignments(tree)
        phases = ast.literal_eval(assignments.get("_POST_BOUNDARY_PHASES", ast.Tuple(elts=[], ctx=ast.Load())))
        for function in [n for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]:
            domains = py_domains(function)
            domains["phase"].update(phases)
            grouped, post_reasons, post_phases = defaultdict(list), set(), set()
            for node in ast.walk(function):
                args = None
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "RunFailure":
                    args = node.args
                elif isinstance(node, ast.Return) and isinstance(node.value, ast.Tuple):
                    args = node.value.elts
                if args is None or len(args) < 3 or not isinstance(args[1], ast.Constant) or args[1].value not in PRODUCERS:
                    continue
                code = args[1].value
                payload = py_dict(args[2])
                reasons = py_values(payload["reason"], domains)
                if not reasons:
                    findings.add("producer-call", f"{path.name}:{function.name}: unresolved reason")
                if code == X:
                    values = py_values(payload["phase"], domains)
                    if not values:
                        findings.add("producer-call", f"{path.name}:{function.name}: unresolved phase")
                    post_reasons.update(reasons)
                    post_phases.update(values)
                else:
                    fields = py_values(payload["field"], domains)
                    if not fields:
                        findings.add("field-literal", f"{path.name}:{function.name}: unresolved field")
                    for field in fields:
                        for reason in reasons:
                            grouped[code].append((field, reason))
            file = path.relative_to(root).as_posix()
            name = function.name if file == "docker/run_json.py" else f"{file}.{function.name}"
            for code, fields in grouped.items():
                rows.append(sink_row("report." + name, file, function.name, code, fields))
            if post_reasons:
                rows.append(sink_row("post." + name, file, function.name, X,
                                     phases=post_phases, reasons=post_reasons))
    return rows


def check_vocabulary(doc, rows, cpp_files, constants, catalog, findings):
    vocab = doc["vocabulary"]
    if set(vocab) != PRODUCERS:
        findings.add("vocabulary", "producer code set differs")
    for code in PRODUCERS:
        entry = vocab[code]
        for key, values in entry.items():
            if len(values) != len(set(values)):
                findings.add("vocabulary", f"{code}: duplicate {key}")
        for key, singular in (("reasons", "reason"), ("stages", "stage"), ("phases", "phase")):
            if key in entry and set(entry[key]) != set(catalog[code]["args"][singular]["values"]):
                findings.add("vocabulary", f"{code}: {key} differ from catalog")
    for code in (G, R):
        obligated = {field["field"] for row in doc["sinks"] if row["producer"] == code for field in row["fields"]}
        if set(vocab[code]["fields"]) != obligated:
            findings.add("vocabulary", f"{code}: field set differs from sink obligations")
    if set(constants.values()) != set(vocab[S]["fields"]) or len(constants) != len(set(constants.values())):
        findings.add("vocabulary", "strategy_value_invalid: constant field set differs from manifest")
    for row in rows:
        code = row["producer"]
        for field in row["fields"]:
            if field["field"] not in vocab[code]["fields"]:
                findings.add("vocabulary", f"{code}: unknown field {field['field']}")
            for reason in field["reasons"]:
                if reason not in vocab[code]["reasons"]:
                    findings.add("vocabulary", f"{code}: unknown reason {reason}")
        for reason in row["reasons"]:
            if reason not in vocab[code]["reasons"]:
                findings.add("vocabulary", f"{code}: unknown reason {reason}")
        if row["stage"] is not None and row["stage"] not in vocab[code]["stages"]:
            findings.add("vocabulary", f"{code}: unknown stage {row['stage']}")
        for phase in row["phases"]:
            if phase not in vocab[code]["phases"]:
                findings.add("vocabulary", f"{code}: unknown phase {phase}")
    for file, enum, code, key in (("src/source/pine_value_admission.hpp", "StrategyValueReason", S, "reasons"),
                                  ("src/engine_value_invalid.hpp", "EngineValueStage", G, "stages"),
                                  ("src/engine_value_invalid.hpp", "EngineValueReason", G, "reasons")):
        if set(cpp_files[file].enum(enum)) != set(vocab[code][key]):
            findings.add("vocabulary", f"{enum}: enum differs from manifest")
        # Verify each enum's spelling reaches the wire unchanged. Comments
        # and unrelated string occurrences are not producer evidence.
        tokens = cpp_files[file].tokens
        mappings = {tokens[i + 2]: string_value([tokens[i + 5]])
                    for i in range(len(tokens) - 5)
                    if tokens[i:i + 2] == [enum, "::"] and tokens[i + 3:i + 5] == [":", "return"]}
        if mappings != {value: value for value in vocab[code][key]}:
            findings.add("vocabulary", f"{enum}: wire mapping differs from manifest")


def check_producer_helpers(cpp_files, findings):
    for file, helper, code, payload in (
            ("src/source/pine_value_admission.hpp", "throw_strategy_value_invalid", S,
             ('{"field",field}', '{"reason",strategy_value_reason_name(reason)}')),
            ("src/engine_value_invalid.hpp", "throw_engine_command_value_invalid", G,
             ('{"field",field}', '{"stage",engine_value_stage_name(stage)}',
              '{"reason",engine_value_reason_name(reason)}'))):
        cpp = cpp_files[file]
        definitions = [f for f in cpp.definitions() if f["name"] == helper]
        if len(definitions) != 1:
            findings.add("producer-helper", f"{helper}: missing unique definition")
            continue
        definition = definitions[0]
        body = "".join(cpp.tokens[definition["start"]:definition["end"]])
        if ("throwcoded<" not in body or "RunFailureCode::" + code not in body
                or "RunFailureArgs" not in body or not all(part in body for part in payload)):
            findings.add("producer-helper", f"{helper}: coded producer/payload differs from contract")


def check_producer_bindings(cpp_files, constants, root, findings):
    cpp = cpp_files["src/source/pine_value_admission.cpp"]
    for definition in cpp.definitions():
        name = definition["name"]
        if not name.startswith("admit_strategy_"):
            continue
        family = name.removeprefix("admit_strategy_")
        if family == "close_token":
            family = "close"
        for _, _, _, args in cpp.calls({"admit_text", "admit_level"}, definition["start"], definition["end"]):
            if len(args) != 2 or len(args[0]) != 1 or not IDENT.fullmatch(args[0][0]):
                findings.add("producer-binding", f"{name}: unresolved source operand")
                continue
            field = field_value(args[1], constants)
            expected = family + "." + args[0][0]
            if field != ("literal", expected):
                findings.add("producer-binding", f"{name}: {args[0][0]} is labelled {field[1]}")
    # In the Python report guard the key and its wire field must travel
    # together. Swapping two tuple fields preserves the entire field set.
    tree = ast.parse(read(root / "docker/run_json.py"))
    validator = function(tree, "_validate_report_result")
    for node in ast.walk(validator):
        if isinstance(node, ast.For) and isinstance(node.target, ast.Tuple) and isinstance(node.iter, (ast.Tuple, ast.List)):
            for row in node.iter.elts:
                if isinstance(row, ast.Tuple) and len(row.elts) == 2:
                    key, field = (ast.literal_eval(value) for value in row.elts)
                    if field.rsplit(".", 1)[-1] != key:
                        findings.add("producer-binding", f"_validate_report_result: {key} is labelled {field}")


def check_admission_routes(doc, cpp_files, findings):
    actual = {}
    for file in ("src/source/pine_strategy_commands.cpp", "src/source/pine_strategy_host.cpp",
                 "src/source/pine_value_admit_risk.hpp"):
        cpp = cpp_files[file]
        for definition in cpp.definitions():
            name = definition["name"]
            if not (name.startswith(("strategy_", "set_pine_risk_")) or name in {
                    "configure_pine_strategy", "set_strategy_override", "replay_intent",
                    "selected_replay_at_open", "assert_admitted_pine_replay_risk_config"}):
                continue
            anchor = f"{name}/{len(definition['args'])}"
            if name == "replay_intent":
                types = [t for arg in definition["args"] for t in arg if t.startswith("Intent")]
                if len(types) != 1:
                    findings.add("admission-route", "replay_intent: unresolved retained row type")
                    continue
                anchor = "replay_intent/" + types[0]
            calls = [callee for callee, _, _, _ in cpp.calls(start=definition["start"], end=definition["end"])
                     if callee.startswith(("admit_strategy_", "admit_pine_risk_")) or callee in {
                         "assert_admitted_pine_replay_risk_config", "replay_intent"}]
            actual[f"{file}:{anchor}"] = calls
    expected = unique_rows(doc["admission_routes"], "id", "duplicate-admission-route", findings)
    findings.compare_set("admission-route-set", expected, actual)
    for name, row in expected.items():
        if name in actual and row["calls"] != actual[name]:
            findings.add("admission-route", f"{name}: helper calls differ from manifest")


def check_kept_branches(doc, cpp_files, findings):
    rows = doc.get("kept_branches", [])
    if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
        raise InputError("kept_branches must be a closed list of decision objects")
    expected = unique_rows(rows, "id", "duplicate-kept-branch", findings)
    findings.compare_set("kept-branch-set", expected, KEPT_BRANCH_DECISIONS)
    obligated = {row["id"] for row in doc["sinks"]}
    for name, row in expected.items():
        if set(row) != {"id", "file", "anchor", "outcome", "d6_row", "reason"}:
            findings.add("kept-branch-decision", f"{name}: decision keys differ from closed schema")
        for key, value in KEPT_BRANCH_DECISIONS.get(name, {}).items():
            if row.get(key) != value:
                findings.add("kept-branch-decision", f"{name}: {key} differs from D6 KEEP decision")
        reason = row.get("reason")
        if (not isinstance(reason, str) or "\n" in reason or "\r" in reason
                or not reason.startswith("decided KEEP by L6, 10-13;")
                or not reason.endswith("user effect unchanged")):
            findings.add("kept-branch-decision", f"{name}: missing one-line L6 KEEP reason")
        if name in obligated:
            findings.add("kept-branch-decision", f"{name}: KEEP is still a producer obligation")
        anchor = row.get("anchor")
        if (not isinstance(anchor, dict) or set(anchor) != {"function", "literal"}
                or any(not isinstance(value, str) or not IDENT.fullmatch(value)
                       for value in anchor.values())):
            findings.add("kept-branch-anchor", f"{name}: expected function and literal code tokens")
            continue
        cpp = cpp_files.get(row.get("file"))
        if cpp is None:
            findings.add("kept-branch-anchor", f"{name}: file {row.get('file')} absent")
            continue
        definitions = [f for f in cpp.definitions() if f["name"] == anchor["function"]]
        if len(definitions) != 1:
            findings.add("kept-branch-anchor", f"{name}: anchor function {anchor['function']} lacks a unique definition")
        elif anchor["literal"] not in cpp.tokens[definitions[0]["start"]:definitions[0]["end"]]:
            findings.add("kept-branch-anchor", f"{name}: anchor {anchor['literal']} absent in {anchor['function']}")


def check_kept_branch_producers(cpp_files, constants, findings):
    # Scan the actual calls regardless of which rows the manifest contains.
    # Settlement stage OR settlement.row is forbidden in both settlement
    # files; native arming's field is forbidden irrespective of its stage.
    for file in ("src/engine_execution.cpp", "src/engine_orders.cpp", "src/native_order.cpp"):
        cpp = cpp_files.get(file)
        if cpp is None:
            findings.add("kept-branch-producer", f"{file}: source file absent")
            continue
        definitions = {f["index"] for f in cpp.definitions()}
        for callee, index, _, args in cpp.calls({"engine_command_value_invalid_or_count"}):
            if index in definitions or len(args) != 3:
                continue  # malformed calls are already producer-call findings
            field = field_value(args[0], constants)
            stages = enum_values(args[1], "EngineValueStage")
            forbidden = (field == ("literal", "relative_price.level") if file == "src/native_order.cpp"
                         else "settlement" in stages or field == ("literal", "settlement.row"))
            if forbidden:
                stage = ",".join(sorted(stages)) or "<unresolved>"
                findings.add("kept-branch-producer",
                             f"{file}: {callee} stage {stage} field {field[1]} contradicts KEEP")


def check_sinks(root, doc, rows, cpp_files, findings):
    expected = unique_rows(doc["sinks"], "id", "duplicate-sink", findings)
    actual = unique_rows(rows, "id", "duplicate-code-sink", findings)
    findings.compare_set("sink-set", expected, actual)
    for name, row in expected.items():
        path = root / row["file"]
        if not path.is_file():
            findings.add("producer-anchor", f"{name}: file {row['file']} absent")
            continue
        if row["file"] in cpp_files:
            present = row["anchor"] in cpp_files[row["file"]].tokens
        else:
            tree = ast.parse(read(path))
            present = any(isinstance(n, ast.FunctionDef) and n.name == row["anchor"] for n in ast.walk(tree))
        if not present:
            findings.add("producer-anchor", f"{name}: anchor {row['anchor']} absent")
        if name not in actual:
            if row["fields"]:
                findings.add("producer-missing", f"{name}: no {row['producer']} field producer at {row['file']}:{row['anchor']}")
            continue
        observed = actual[name]
        for key in ("file", "anchor", "producer", "stage", "phases", "reasons"):
            wanted = sorted(row.get(key, [])) if key in {"phases", "reasons"} else row.get(key)
            if wanted != observed[key]:
                findings.add("sink-mapping", f"{name}: {key} differs from code")
        wanted_fields = {field["field"]: set(field["reasons"]) for field in row["fields"]}
        observed_fields = {field["field"]: set(field["reasons"]) for field in observed["fields"]}
        if len(wanted_fields) != len(row["fields"]):
            findings.add("duplicate-field", name)
        if wanted_fields != observed_fields:
            findings.add("sink-mapping", f"{name}: fields/reasons differ from code")


def check_catalog(doc, root, catalog, findings):
    rows = unique_rows(doc["catalog"]["codes"], "code", "duplicate-catalog-code", findings)
    findings.compare_set("catalog-set", rows, catalog)
    registry = read(root / "src/run_failure_registry.inc")
    block = re.search(r"kRegistryCodes\[\]\s*=\s*\{(.*?)\n\};", registry, re.S)
    if block is None:
        raise InputError("registry descriptor kRegistryCodes absent")
    matches = re.findall(r'\{"([a-z0-9_]*)",\s*RunFailureClass::(\w+),\s*(true|false),\s*\d+,\s*\d+\}', block[1])
    if len(re.findall(r"\{[^{}]*\}", block[1])) != len(matches):
        findings.add("registry-row", "unrecognized registry entry")
    names = [name for name, _, _ in matches]
    if names.count("") != 1:
        findings.add("registry-sentinel", "expected one unnamed no-failure row")
    actual = {}
    for name, category, retryable in matches:
        if not name:
            continue
        if name in actual:
            findings.add("duplicate-registry-code", name)
        actual[name] = {"class": category, "retryable": retryable == "true"}
    findings.compare_set("registry-set", rows, actual)
    findings.compare_set("catalog-registry-set", catalog, actual)
    for name, row in rows.items():
        if row.get("reachability") not in {"direct", "indirect", "no"} or not row.get("route"):
            findings.add("catalog-reachability", f"{name}: missing reachability/route")
        if row.get("appendix") not in {"C-existing", "C-window", "catalog-extension"}:
            findings.add("catalog-reachability", f"{name}: missing contract reconciliation")
        if name in catalog:
            for key in ("class", "retryable", "since"):
                if row[key] != catalog[name][key]:
                    findings.add("catalog-mapping", f"{name}: {key} differs from catalog")
        if name in actual:
            for key in ("class", "retryable"):
                if row[key] != actual[name][key]:
                    findings.add("registry-mapping", f"{name}: {key} differs from manifest")


def discover_descriptors(cpp_files):
    adapter = cpp_files["include/pineforge/source/pine_adapter.hpp"]
    run_spec = cpp_files["include/pineforge/native_run_spec.hpp"]
    order = cpp_files["include/pineforge/native_order.hpp"]
    doors = cpp_files["src/source/pine_value_admit_doors.hpp"]
    host = cpp_files["include/pineforge/source/pine_strategy_host.hpp"]
    result = {}

    def put(identifier, provider, anchor, fields):
        result[identifier] = {"file": provider.file, "anchor": anchor, "fields": fields}

    for name in ("PineStrategyConfig", "StrategyOverrides", "PineReplayRiskConfig"):
        put(name, adapter, name, adapter.fields(name))
    put("NativeRunSpecField", run_spec, "NativeRunSpecField", run_spec.enum("NativeRunSpecField"))
    _, limit = order.block("struct", "Request")
    for i in range(limit):
        if order.tokens[i] == "struct" and i + 1 < limit and IDENT.fullmatch(order.tokens[i + 1]):
            name = order.tokens[i + 1]
            put("native_order." + name, order, name, order.fields(name))
    for name, _, _, args in doors.calls():
        if name.startswith("admit_strategy_"):
            put(name, doors, name, [arg[-1] for arg in args])
    put("source_command_doors", host, "strategy_entry",
        [f"{name}/{len(args)}" for name, _, _, args in host.calls() if name.startswith("strategy_")])
    risk = {name for name, _, _, _ in host.calls() if name.startswith("set_pine_risk_")}
    put("risk_setters", host, "set_pine_risk_direction", sorted(risk))
    settings = cpp_files["include/pineforge/checked_settings.hpp"]
    put("checked_settings.Setting", settings, "Setting", settings.fields("Setting"))
    engine = cpp_files["include/pineforge/engine.hpp"]
    put("bound_input_getters", engine, "get_input_double",
        sorted({name for name, _, _, _ in engine.calls() if name.startswith("get_input_")} |
               {name for name, _, _, _ in host.calls() if name.startswith("get_input_")}))
    request_names = {"register_security_eval", "register_security_lower_tf_eval",
                     "security_column_value", "recorded_series_value"}
    seen = defaultdict(int)
    requests = []
    for name, _, _, args in host.calls(request_names):
        seen[(name, len(args))] += 1
        requests.append(f"{name}/{len(args)}/{seen[(name, len(args))]}")
    put("request_doors", host, "register_security_eval", requests)
    actions = cpp_files["include/pineforge/order_action.hpp"]
    execution = cpp_files["include/pineforge/execution.hpp"]
    put("native_order.Transact", actions, "Transact", actions.fields("Transact"))
    put("native_order.Flatten", execution, "Flatten", execution.fields("Flatten"))
    return result


def check_descriptors(doc, cpp_files, findings):
    expected = unique_rows(doc["descriptors"], "id", "duplicate-descriptor", findings)
    actual = discover_descriptors(cpp_files)
    findings.compare_set("descriptor-set", expected, actual)
    for name, observed in actual.items():
        if name not in expected:
            continue
        row = expected[name]
        fields = observed["fields"]
        if row["fields"] != fields:
            findings.add("descriptor-fields", f"{name}: declaration fields differ from manifest")
        for key in ("file", "anchor"):
            if row[key] != observed[key]:
                findings.add("descriptor-anchor", f"{name}: {key} differs from code")
        if row.get("owner") not in {"source", "caller", "engine", "source-or-caller"} or not row.get("policy"):
            findings.add("descriptor-owner", f"{name}: missing owner/policy")
        if len(fields) != len(set(fields)):
            findings.add("descriptor-fields", f"{name}: duplicate fields")


def ctypes_fields(tree, name):
    classes = [node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == name]
    if len(classes) != 1:
        raise InputError(f"ambiguous ctypes class {name}")
    value = py_assignments(classes[0])["_fields_"]
    if not isinstance(value, ast.List):
        raise InputError(f"unrecognized ctypes descriptor {name}")
    result = []
    for row in value.elts:
        if not isinstance(row, ast.Tuple) or len(row.elts) != 2:
            raise InputError(f"unrecognized ctypes member {name}")
        result.append((ast.literal_eval(row.elts[0]), ast.unparse(row.elts[1])))
    if len(result) != len({field for field, _ in result}):
        raise InputError(f"duplicate ctypes member {name}")
    return result


def c_pod_fields(header, pod):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", header, flags=re.S)
    block = re.search(r"typedef\s+struct\s+" + pod + r"_s\s*\{(.*?)\}\s*" + pod + r"_t", text, re.S)
    if block is None:
        raise InputError(f"missing C POD {pod}")
    types = {"double": "ctypes.c_double", "int": "ctypes.c_int", "int32_t": "ctypes.c_int32",
             "int64_t": "ctypes.c_int64", "pf_trade_stats_t": "TradeStatsC", "pf_equity_stats_t": "EquityStatsC"}
    fields = []
    for statement in block[1].split(";"):
        if not statement.strip():
            continue
        declaration = re.fullmatch(r"\s*(\w+)\s+([\w\s,]+)\s*", statement)
        if declaration is None or declaration[1] not in types:
            raise InputError(f"unrecognized C POD member {pod}")
        fields.extend((name.strip(), types[declaration[1]]) for name in declaration[2].split(","))
    return fields


def function(tree, name):
    matches = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == name]
    if len(matches) != 1:
        raise InputError(f"ambiguous Python producer {name}")
    return matches[0]


def scalar_type(node, environment, resolving=frozenset()):
    if isinstance(node, ast.Constant):
        kind = {int: "integer", float: "number", bool: "boolean", str: "string"}.get(type(node.value))
        if kind is not None:
            result = {"kind": kind, "null_allowed": False}
            if kind == "string":
                result["values"] = [node.value]
            return result
    if isinstance(node, ast.Name) and node.id in environment and node.id not in resolving:
        return scalar_type(environment[node.id], environment, resolving | {node.id})
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
        name = node.func.id
        if name in {"int", "len", "sum"}:
            return {"kind": "integer", "null_allowed": False}
        if name == "bool":
            return {"kind": "boolean", "null_allowed": False}
        if name in {"float", "round", "_num", "max", "min"}:
            return {"kind": "number", "null_allowed": name == "_num"}
    if isinstance(node, ast.IfExp):
        left = scalar_type(node.body, environment, resolving)
        right = scalar_type(node.orelse, environment, resolving)
        if left["kind"] == right["kind"] == "string":
            return {"kind": "string", "null_allowed": False,
                    "values": sorted(set(left["values"]) | set(right["values"]))}
        if left == right:
            return left
        if left["kind"] in {"integer", "number"} and right["kind"] in {"integer", "number"}:
            return {"kind": "number", "null_allowed": left["null_allowed"] or right["null_allowed"]}
    if isinstance(node, ast.BinOp):
        left, right = scalar_type(node.left, environment, resolving), scalar_type(node.right, environment, resolving)
        if left["kind"] in {"integer", "number"} and right["kind"] in {"integer", "number"}:
            return {"kind": "number" if isinstance(node.op, ast.Div) or "number" in {left["kind"], right["kind"]} else "integer",
                    "null_allowed": False}
    raise InputError(f"unrecognized scalar producer expression: {ast.unparse(node)}")


def reconstruct_schema(root, findings):
    # Static route: dictionary syntax and ctypes/C PODs, entirely independent
    # of manifest paths and of the test's evaluated-builder route.
    tree = ast.parse(read(root / "docker/run_json.py"))
    fields = {name: ctypes_fields(tree, name) for name in
              ("TradeC", "TradeStatsC", "EquityStatsC", "MetricsC", "EquityPointC")}
    header = read(root / "include/pineforge/pineforge.h")
    for name, pod in (("TradeC", "pf_trade"), ("TradeStatsC", "pf_trade_stats"),
                      ("EquityStatsC", "pf_equity_stats"), ("MetricsC", "pf_metrics"),
                      ("EquityPointC", "pf_equity_point")):
        if fields[name] != c_pod_fields(header, pod):
            findings.add("schema-ctypes", f"{name}: C descriptor differs from ctypes")
    aliases = ast.literal_eval(py_assignments(tree)["EQUITY_REPORT_KEYS"])
    stats = function(tree, "_stats_dict")
    # The recognized serializer must consume _fields_, apply the alias map,
    # and preserve the double/null versus integer distinction. A changed
    # serializer is a new reconstruction obligation, not an automatic pass.
    stats_text = ast.unparse(stats)
    if not all(fragment in stats_text for fragment in
               ("s._fields_", "keys.get(name, name)", "_num(v) if ctype is ctypes.c_double else int(v)",
                "EQUITY_REPORT_KEYS if isinstance(s, EquityStatsC) else {}")):
        raise InputError("unrecognized _stats_dict serializer; update the independent reconstruction")
    builder = function(tree, "build_report_dict")
    environment = {"i": ast.Constant(value=0)}
    for node in ast.walk(builder):
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    environment[target.id] = node.value
                elif isinstance(target, ast.Tuple) and isinstance(node.value, ast.Tuple):
                    for name, value in zip(target.elts, node.value.elts):
                        if isinstance(name, ast.Name):
                            environment[name.id] = value
    outputs = [node.value for node in builder.body if isinstance(node, ast.Return)]
    if len(outputs) != 1:
        raise InputError("ambiguous build_report_dict return")
    out = py_dict(outputs[0])
    leaves = {}
    containers = {"$": "object"}
    for key in ("summary", "metrics", "trades", "equity_curve"):
        if key not in out:
            raise InputError(f"canonical report block {key} absent")
    containers["summary"] = "object"
    for key, value in py_dict(out["summary"]).items():
        leaves["summary." + key] = scalar_type(value, environment)
    metrics = out["metrics"]
    if isinstance(metrics, ast.Name):
        metrics = environment[metrics.id]
    containers["metrics"] = "object"
    metric_types = dict(fields["MetricsC"])
    for block, call in py_dict(metrics).items():
        if (not isinstance(call, ast.Call) or not isinstance(call.func, ast.Name)
                or call.func.id != "_stats_dict" or len(call.args) != 1
                or not isinstance(call.args[0], ast.Attribute) or call.args[0].attr not in metric_types):
            raise InputError(f"unrecognized metrics producer {block}")
        cls = metric_types[call.args[0].attr]
        containers["metrics." + block] = "object"
        for name, ctype in fields[cls]:
            key = aliases.get(name, name) if cls == "EquityStatsC" else name
            path = f"metrics.{block}.{key}"
            if path in leaves:
                raise InputError(f"duplicate emitted metric leaf {path}")
            leaves[path] = {"kind": "number" if ctype == "ctypes.c_double" else "integer",
                            "null_allowed": ctype == "ctypes.c_double"}
    for block, cls in (("trades", "TradeC"), ("equity_curve", "EquityPointC")):
        value = out[block]
        if not isinstance(value, ast.Name):
            raise InputError(f"unrecognized array producer {block}")
        appends = [node.args[0] for node in ast.walk(builder) if isinstance(node, ast.Call)
                   and isinstance(node.func, ast.Attribute) and node.func.attr == "append"
                   and isinstance(node.func.value, ast.Name) and node.func.value.id == value.id
                   and len(node.args) == 1]
        if len(appends) != 1:
            raise InputError(f"ambiguous array element producer {block}")
        containers[block] = "array"
        containers[block + "[*]"] = "object"
        for key, expression in py_dict(appends[0]).items():
            # Every native attribute read must belong to the checked POD.
            for attribute in ast.walk(expression):
                if (isinstance(attribute, ast.Attribute) and isinstance(attribute.value, ast.Name)
                        and attribute.value.id in {"t", "p"}
                        and attribute.attr not in dict(fields[cls])):
                    raise InputError(f"unknown native member {cls}.{attribute.attr}")
            leaves[f"{block}[*].{key}"] = scalar_type(expression, environment)
    window = ast.parse(read(root / "docker/selected_window_report.py"))
    version = ast.literal_eval(py_assignments(window)["WIRE_VERSION"])
    if type(version) is not int:
        raise InputError("D4 WIRE_VERSION is not an integer")
    leaves["version"] = {"kind": "integer", "null_allowed": False}
    return {"leaves": leaves, "containers": containers, "version": version}


def check_schema(doc, reconstructed, findings):
    schema = doc["canonical_schema"]
    leaves = unique_rows(schema["leaves"], "path", "duplicate-schema-leaf", findings)
    containers = unique_rows(schema["containers"], "path", "duplicate-schema-container", findings)
    findings.compare_set("schema-leaves", leaves, reconstructed["leaves"])
    findings.compare_set("schema-containers", containers, reconstructed["containers"])
    if type(schema["version"]) is not int or schema["version"] != reconstructed["version"]:
        findings.add("schema-version", "version differs from selected_window_report.WIRE_VERSION")
    for path, row in leaves.items():
        if row.get("owner") != "producer" or not row.get("detection") or not row.get("reasons"):
            findings.add("schema-owner", f"{path}: missing ownership/detection/reasons")
        if type(row["null_allowed"]) is not bool:
            findings.add("schema-leaf-type", f"{path}: null policy must be boolean")
        reasons = {"integer": {"type_schema"}, "boolean": {"type_schema"},
                   "number": {"nonfinite"}, "string": {"invalid_unicode", "type_schema"}}
        if row["kind"] not in reasons or set(row["reasons"]) != reasons[row["kind"]]:
            findings.add("schema-owner", f"{path}: result reasons differ from Appendix A")
        prefix = "M" if path.startswith("metrics.") else "T" if path.startswith("trades[*].") else "E" if path.startswith("equity_curve[*].") else None
        if set(row["detection"]) != ({prefix, "C"} if prefix else {"C"}):
            findings.add("schema-owner", f"{path}: detection differs from Appendix A")
        if path in reconstructed["leaves"]:
            expected = {"kind": row["kind"], "null_allowed": row["null_allowed"]}
            if "values" in row:
                expected["values"] = row["values"]
            if expected != reconstructed["leaves"][path]:
                findings.add("schema-leaf-type", f"{path}: kind/null/enum policy differs from code")
    for path, row in containers.items():
        if row.get("owner") != "producer":
            findings.add("schema-owner", f"{path}: missing container ownership")
        if path in reconstructed["containers"] and row["kind"] != reconstructed["containers"][path]:
            findings.add("schema-container-type", f"{path}: kind differs from code")


def check(root, manifest):
    findings = Findings()
    doc = load_json(manifest)
    if doc.get("format") != "pineforge-selected-window-ownership/v1":
        raise InputError("manifest format must be pineforge-selected-window-ownership/v1")
    cpp_files = {path.relative_to(root).as_posix(): Cpp(read(path), path.relative_to(root).as_posix())
                 for directory in ("src", "include") for path in sorted((root / directory).rglob("*"))
                 if path.is_file() and path.suffix in {".h", ".hpp", ".cpp", ".inc"}}
    tokens = cpp_files["src/source/pine_value_admission.hpp"].tokens
    constants = {}
    for i in range(len(tokens) - 3):
        if (IDENT.fullmatch(tokens[i]) and tokens[i + 1] == "="
                and tokens[max(0, i - 2):i] == ["char", "*"]):
            literal = string_value([tokens[i + 2]])
            if literal is not None:
                constants[tokens[i]] = literal
    catalog = load_json(root / "docker/run_failure_codes.json")["codes"]
    rows = source_producers(cpp_files, constants, findings)
    rows.extend(engine_producers(cpp_files, constants, findings))
    rows.extend(python_producers(root, findings))
    check_producer_helpers(cpp_files, findings)
    check_producer_bindings(cpp_files, constants, root, findings)
    check_admission_routes(doc, cpp_files, findings)
    check_vocabulary(doc, rows, cpp_files, constants, catalog, findings)
    check_sinks(root, doc, rows, cpp_files, findings)
    check_kept_branches(doc, cpp_files, findings)
    check_kept_branch_producers(cpp_files, constants, findings)
    check_catalog(doc, root, catalog, findings)
    check_descriptors(doc, cpp_files, findings)
    reconstructed = reconstruct_schema(root, findings)
    check_schema(doc, reconstructed, findings)
    return sorted(findings.lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    manifest = args.manifest if args.manifest is not None else root / MANIFEST
    try:
        lines = check(root, manifest)
    except (OSError, UnicodeError, ValueError, SyntaxError, KeyError, TypeError, InputError) as error:
        print(f"ownership[input]: {error}")
        return 2
    for line in lines:
        print(line)
    return 1 if lines else 0


if __name__ == "__main__":
    sys.exit(main())
