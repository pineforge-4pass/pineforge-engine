#!/usr/bin/env python3
"""Component controls only; never SDK, hosted or native-loader qualification.

Run later on an admitted test worker:
  python3 -B docker/sdk-glibc235/test_diagnostic.py --scratch-root /explicit/root

GNU cc/ar/readelf and an existing scratch root are required. Real fixture ELFs
are compiled and inspected, never executed. All artifacts remain in scratch.
Only the case-collection tests use doubles, explicitly scoped to unit
orchestration; those doubles cannot produce successful qualification records.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


DRIVER = Path(__file__).absolute().with_name("diagnostic.py")
CHECKER = DRIVER.parents[2] / "scripts/check_prebuilt_glibc.py"
SCRATCH: Path | None = None
sys.dont_write_bytecode = True


def load_driver():
    spec = importlib.util.spec_from_file_location("_sdk_diagnostic_component", DRIVER)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot import component: {DRIVER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DiagnosticComponents(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = load_driver()
        if SCRATCH is None:
            raise AssertionError("explicit --scratch-root required")
        cls.scratch = cls.d.checked(cls.d.absolute(SCRATCH))
        if not cls.scratch.is_dir():
            raise AssertionError("scratch root must already exist")
        if sys.platform != "linux":
            raise AssertionError("real ELF component controls require Linux")
        cls.tools = {}
        for name in ("cc", "ar", "readelf"):
            tool = shutil.which(name, path=os.defpath)
            if tool is None:
                raise AssertionError(f"installed GNU fixture tool required: {name}")
            cls.tools[name] = tool

    def setUp(self):
        self.area = Path(tempfile.mkdtemp(prefix="sdk-component-", dir=self.scratch))
        self.report = self.area / "reports"
        self.report.mkdir()
        self.root = self.area / "generated"
        self.root.mkdir()
        self.session = self.d.Session(
            self.report, "component", [self.area, DRIVER, CHECKER])
        self.counter = 0
        self.floor = self.d.load_floor_checker(self.session, CHECKER)

    def source(self, path, text):
        self.d.checked(path.parent)
        self.d.put(path, text)
        return path

    def command(self, argv):
        result = subprocess.run(
            [str(arg) for arg in argv], capture_output=True, check=False,
            env={**self.d.ENV, "TMPDIR": str(self.area)})
        print(json.dumps({
            "scope": "component-fixture", "argv": [str(arg) for arg in argv],
            "returncode": result.returncode,
            "stdout": result.stdout.decode("utf-8", "surrogateescape"),
            "stderr": result.stderr.decode("utf-8", "surrogateescape"),
        }), flush=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b"", result.stderr)
        return result.stdout.decode("utf-8", "strict")

    def fixture(self, output, *, pie=False, version=None, forbidden=False):
        """Use genuine symbol imports/version needs, not embedded data strings."""
        self.counter += 1
        work = self.area / f"fixture-{self.counter}"
        work.mkdir()
        symbol = "__isoc23_strtol" if forbidden else "fixture_api"
        inputs = []
        if version is not None or forbidden:
            provider = self.source(
                work / "provider.c",
                f"long {symbol}(const char *s) {{ return s ? s[0] : 0; }}\n")
            dependency = work / "provider.so"
            argv = [self.tools["cc"], "-shared", "-fPIC", "-nostdlib", provider,
                    "-Wl,-soname,fixture-provider.so", "-o", dependency]
            if version is not None:
                script = self.source(
                    work / "versions.map", f"{version} {{ global: {symbol}; }};\n")
                argv += ["-Xlinker", "--version-script", "-Xlinker", script]
            self.command(argv)
            inputs.append(dependency)
            text = (f"extern long {symbol}(const char *);\n"
                    f"int main(int argc, char **argv) {{ return argc > 1 ? "
                    f"(int){symbol}(argv[1]) : 2; }}\n" if pie else
                    f"extern long {symbol}(const char *);\n"
                    f"long fixture_use(const char *s) {{ return {symbol}(s); }}\n")
        else:
            text = (
                "int main(int argc, char **argv) {\n"
                "    return argc == 2 ? (unsigned char)argv[1][0] : 2;\n"
                "}\n" if pie else
                "int fixture_use(const char *s) { return s ? s[0] : 0; }\n")
        source = self.source(work / "consumer.c", text)
        output.parent.mkdir(parents=True, exist_ok=True)
        flags = (["-fPIE", "-pie", "-Wl,-e,main", "-Wl,--export-dynamic"]
                 if pie else ["-shared", "-fPIC"])
        self.command([self.tools["cc"], "-std=c11", "-O2", "-Wall", "-Wextra",
                      "-Werror", "-nostdlib", *flags, source, *inputs, "-o", output])
        return output

    def receipts(self):
        return {kind: self.session.digest(self.root / f"{kind}-loader")
                for kind in ("old", "new")}

    def generated(self):
        for kind in ("old", "new"):
            self.fixture(self.root / f"{kind}-loader", pie=True)
        self.fixture(self.root / "new-strategy.so", version="GLIBC_2.35")
        return self.receipts()

    def baseline(self, version="GLIBC_2.35"):
        root = self.area / f"published-{self.counter}"
        root.mkdir()
        self.fixture(root / "released.so", version=version)
        return root

    def elf_facts(self, path):
        text = self.command([self.tools["readelf"], "--wide", "--file-header",
                             "--symbols", "--version-info", "--", path])
        layout = self.command([self.tools["readelf"], "--wide", "--program-headers",
                               "--dynamic", "--", path])
        return text, layout, self.d.parse_executable_elf(self.floor, text, layout)

    def test_real_pie_tools_and_shared_inventory_have_distinct_kinds(self):
        receipts = self.generated()
        baseline = self.baseline()
        self.source(self.root / "ordinary.txt", "a non-ELF input\n")
        summary = self.d.inspect_symbols(
            self.session, CHECKER, self.root, baseline, "component-symbols",
            tool_receipts=receipts)
        self.assertEqual(summary["discovered"], 1)
        result = json.loads(self.session.read(
            self.report / "component-symbols-result.json"))
        self.assertEqual([row["path"] for row in result["targets"]],
                         [str(self.root / "new-strategy.so")])
        inspected = self.d.inspect_tools(self.session, CHECKER, self.root, receipts)
        self.assertEqual(inspected["status"], "INSPECTED")
        self.assertEqual({row["file"]["path"] for row in inspected["tools"]},
                         {row["path"] for row in receipts.values()})
        for row in inspected["tools"]:
            self.assertEqual(row["elf"]["elf_type"], "DYN")
            self.assertEqual(row["elf"]["kind"], "pie-executable")
            self.assertTrue(row["elf"]["machine"])
            self.assertTrue(row["elf"]["symbol_tables"][".dynsym"])
            self.assertTrue(row["inspected"])
        text, _, _ = self.elf_facts(self.root / "old-loader")
        with self.assertRaisesRegex(self.floor.InspectionError,
                                    "executable ELF is not a shared library"):
            self.floor.parse_elf(text.splitlines(), "DYN")

    def test_unrecognized_elf_or_archive_is_never_a_tool_exemption(self):
        receipts = self.generated()
        extra = self.fixture(self.root / "third-loader", pie=True)
        with self.assertRaisesRegex(self.d.Refusal, "outside.*inventory"):
            self.d.elf_inventory(self.session, self.root, tool_receipts=receipts)
        # Separate root: the archive control cannot be masked by the third ELF.
        other = self.area / "archive-inventory"
        other.mkdir()
        copied = {}
        for kind in ("old", "new"):
            destination = other / f"{kind}-loader"
            self.session.copy(self.root / destination.name, destination)
            copied[kind] = self.session.digest(destination)
        self.session.copy(self.root / "new-strategy.so", other / "new-strategy.so")
        self.command([self.tools["ar"], "rcs", other / "hidden-archive", extra])
        with self.assertRaisesRegex(self.d.Refusal, "outside.*inventory"):
            self.d.elf_inventory(self.session, other, tool_receipts=copied)

    def test_changed_missing_and_symlinked_tools_are_refused(self):
        for mutation in ("changed", "missing", "symlink", "wrong-receipt"):
            with self.subTest(mutation=mutation):
                root = self.area / mutation
                root.mkdir()
                for kind in ("old", "new"):
                    self.fixture(root / f"{kind}-loader", pie=True)
                self.fixture(root / "new-strategy.so")
                receipts = {kind: self.session.digest(root / f"{kind}-loader")
                            for kind in ("old", "new")}
                path = root / "old-loader"
                if mutation == "changed":
                    # A new valid executable replaces the compiled bytes.
                    self.fixture(path, pie=True, version="GLIBC_2.35")
                elif mutation == "missing":
                    path.rename(root / "retained-old-loader")
                elif mutation == "symlink":
                    path.rename(root / "retained-old-loader")
                    path.symlink_to(root / "retained-old-loader")
                else:
                    receipts["old"] = receipts["new"]
                with self.assertRaises(self.d.Refusal):
                    self.d.elf_inventory(
                        self.session, root, tool_receipts=receipts)

    def test_complete_consumer_tree_refuses_an_extra_named_archive(self):
        root = self.area / "consumers"
        root.mkdir()
        records = {}
        for kind in ("old", "new"):
            relative_loader = Path("generated") / f"{kind}-loader"
            relative_strategy = (Path("old/strategy.so") if kind == "old"
                                 else Path("generated/new-strategy.so"))
            self.fixture(root / relative_loader, pie=True)
            self.fixture(root / relative_strategy)
            records[kind] = {
                role: {**self.session.digest(root / path),
                       "path": str(Path("/output") / path)}
                for role, path in (("loader", relative_loader), ("strategy", relative_strategy))
            }
        inventory = self.d.consumer_inventory(self.session, root, records)
        self.assertEqual(set(inventory), {
            "generated/old-loader", "generated/new-loader",
            "old/strategy.so", "generated/new-strategy.so"})
        self.command([self.tools["ar"], "rcs", root / "extra.a", root / "old/strategy.so"])
        with self.assertRaisesRegex(self.d.Refusal, "unexpected/malformed consumer"):
            self.d.consumer_inventory(self.session, root, records)

    def test_real_tool_symbols_and_floor_decide_policy(self):
        for label, version, forbidden, accepted in (
                ("boundary", "GLIBC_2.35", False, True),
                ("above", "GLIBC_2.36", False, False),
                ("private", "GLIBC_PRIVATE", False, False),
                ("isoc23", "GLIBC_2.35", True, False)):
            with self.subTest(label=label):
                root, report = self.area / label, self.area / (label + "-reports")
                root.mkdir()
                report.mkdir()
                self.fixture(root / "old-loader", pie=True,
                             version=version, forbidden=forbidden)
                self.fixture(root / "new-loader", pie=True)
                self.fixture(root / "new-strategy.so")
                session = self.d.Session(report, "component", [self.area, CHECKER])
                receipts = {kind: session.digest(root / f"{kind}-loader")
                            for kind in ("old", "new")}
                if accepted:
                    self.d.inspect_tools(session, CHECKER, root, receipts)
                else:
                    with self.assertRaises(self.d.Refusal):
                        self.d.inspect_tools(session, CHECKER, root, receipts)
                evidence = json.loads(session.read(report / "consumer-tools-result.json"))
                self.assertEqual(evidence["status"], "INSPECTED" if accepted else "INCOMPLETE")
                facts = evidence["tools"][0]["elf"]
                self.assertEqual(facts["versions"], [version])
                self.assertTrue(facts["glibc_references"])
                violations = evidence["tools"][0]["violations"]
                self.assertEqual(not violations, accepted, violations)
                if forbidden:
                    self.assertTrue(any(ref["symbol"].startswith("__isoc23_strtol")
                                        for ref in facts["isoc23_references"]))
                if version == "GLIBC_2.36":
                    self.assertTrue(any("2.36" in message for message in violations))

    def test_truncated_real_tool_metadata_cannot_be_inspected(self):
        path = self.fixture(self.area / "tool", pie=True, version="GLIBC_2.35")
        text, layout, _ = self.elf_facts(path)
        lines = text.splitlines()
        row = next(index for index, line in enumerate(lines)
                   if self.floor.SYMBOL_ROW.fullmatch(line)
                   and self.floor.SYMBOL_ROW.fullmatch(line)[1] == "1")
        changed = "\n".join(lines[:row] + lines[row + 1:]) + "\n"
        with self.assertRaises(self.d.Refusal):
            self.d.parse_executable_elf(self.floor, changed, layout)
        with self.assertRaises(self.d.Refusal):
            self.d.parse_executable_elf(self.floor, text, "")

    def test_old_reference_requires_actual_policy_failure_not_a_setup_error(self):
        for label, forbidden, malformed, version, accepted in (
                ("incompatible", True, False, None, True),
                ("compatible", False, False, None, False),
                ("bad-elf", False, True, None, False),
                ("unrelated-violation", True, False, "GLIBC_PRIVATE", False)):
            with self.subTest(label=label):
                root = self.area / label
                root.mkdir()
                if malformed:
                    self.source(root / "strategy.so", b"\x7fELF" + b"\0" * 8)
                else:
                    self.fixture(root / "strategy.so", forbidden=forbidden, version=version)
                if accepted:
                    summary = self.d.inspect_symbols(
                        self.session, CHECKER, root, None, "old-" + label,
                        expected_negative=True)
                    self.assertEqual(summary["verdict"], "FAIL")
                    evidence = json.loads(self.session.read(
                        self.report / f"old-{label}-result.json"))
                    self.assertEqual(evidence["classification"], "EXPECTED_ISOC23_REFUSAL")
                    self.assertEqual(evidence["command"]["returncode"], 1)
                    self.assertTrue(evidence["targets"][0]["isoc23_references"])
                else:
                    with self.assertRaises(self.d.Refusal):
                        self.d.inspect_symbols(
                            self.session, CHECKER, root, None, "old-" + label,
                            expected_negative=True)
                    evidence = json.loads(self.session.read(
                        self.report / f"old-{label}-result.json"))
                    self.assertEqual(evidence["classification"], "REFUSED")

    def test_published_baseline_tagsets_still_control_positive_admission(self):
        self.fixture(self.root / "new-strategy.so", version="GLIBC_2.35")
        baseline = self.baseline("GLIBC_2.34")
        with self.assertRaises(self.d.Refusal):
            self.d.inspect_symbols(
                self.session, CHECKER, self.root, baseline, "low-baseline")
        result = json.loads(self.session.read(self.report / "low-baseline-result.json"))
        self.assertEqual(result["classification"], "REFUSED")
        self.assertEqual(result["targets"][0]["versions"], ["GLIBC_2.35"])
        self.assertEqual(result["baseline_targets"][0]["versions"], ["GLIBC_2.34"])
        self.assertTrue(result["baseline_comparisons"][0]["exceeds"])

    def test_real_spawn_error_has_no_invented_child_exit_status(self):
        row = self.session.run(
            "missing-command-control", [self.area / "absent-executable"],
            allow_nonzero=True, allow_unavailable=True)
        self.assertIsNone(row["returncode"])
        self.assertIn("FileNotFoundError", row["spawn_or_wait_error"])
        self.assertFalse(Path(row["stdout"]).with_suffix(".exit").exists())
        for stream in ("stdout", "stderr"):
            path = Path(row[stream])
            self.assertTrue(path.is_file())
            self.assertEqual(row[stream + "_hash"], self.session.digest(path))
            self.assertEqual(self.session.read(path), b"")
        with self.assertRaises(OSError):
            self.session.run("missing-command-fatal", [self.area / "absent-executable"])

    def test_unit_orchestration_keeps_later_cases_after_unexpected_first_case(self):
        """Doubles only exercise collection; all verdicts are unit-only failures."""
        calls = []
        consumers, dependencies, packed = (self.area / name for name in
                                          ("consumers", "dependencies", "packed"))
        compiled = {"inputs": {kind: {"abi": 7} for kind in ("old", "new")}}

        def unit_case(s, os_name, image, image_id, kind, *args):
            calls.append((os_name, kind))
            result = {
                "scope": "unit-orchestration-only", "os": os_name, "sdk": kind,
                "verdict": ("UNIT_INJECTED_UNEXPECTED" if len(calls) == 1
                            else "UNIT_CASE_REACHED"),
                "errors": ["unit control: no runtime qualification"],
            }
            self.d.put_json(s.report / f"case-{os_name}-{kind}.json", result)
            return result

        with mock.patch.object(self.session, "run", side_effect=lambda *a, **k: {
                "label": a[0], "returncode": 0, "scope": "unit-orchestration-only"}), \
                mock.patch.object(self.d, "image_identity",
                                  side_effect=lambda *a: {"Id": "unit-image"}), \
                mock.patch.object(self.d, "actual_case", side_effect=unit_case):
            cases = self.d.collect_cases(
                self.session, consumers, dependencies, packed, compiled, "unit",
                {"old": {}, "new": {}})
        self.assertEqual(calls, [
            ("ubuntu2204", "old"), ("ubuntu2204", "new"),
            ("debian12", "old"), ("debian12", "new")])
        with self.assertRaises(self.d.Refusal):
            self.d.require_complete_cases(self.session, cases)
        aggregate = json.loads(self.session.read(self.report / "cases.result.json"))
        self.assertEqual(aggregate["status"], "INCOMPLETE")
        self.assertEqual(len(aggregate["cases"]), 4)
        self.assertFalse((self.report / "RUN.COMPLETE").exists())

    def test_unit_unavailable_runtime_keeps_other_runtime_cases(self):
        """Input-sensitive unit doubles; no successful loader verdict is emitted."""
        calls, pulls = [], []
        compiled = {"inputs": {kind: {"abi": 7} for kind in ("old", "new")}}

        def unit_pull(label, argv, **kwargs):
            self.assertEqual(argv[:4], ["docker", "pull", "--platform", "linux/amd64"])
            self.assertEqual(argv[4], self.d.RUNTIMES[label.removeprefix("pull-")])
            pulls.append(label)
            return {"label": label, "returncode": 125 if label == "pull-ubuntu2204" else 0,
                    "scope": "unit-orchestration-only"}

        def unit_case(s, os_name, image, image_id, kind, *args):
            calls.append((os_name, kind))
            result = {
                "scope": "unit-orchestration-only", "os": os_name, "sdk": kind,
                "verdict": "UNIT_CASE_REACHED", "docker_run_attempted": False,
                "errors": ["unit control: no runtime qualification"],
            }
            self.d.put_json(s.report / f"case-{os_name}-{kind}.json", result)
            return result

        with mock.patch.object(self.session, "run", side_effect=unit_pull), \
                mock.patch.object(self.d, "image_identity",
                                  side_effect=lambda *a: {"Id": "unit-image"}), \
                mock.patch.object(self.d, "actual_case", side_effect=unit_case):
            cases = self.d.collect_cases(
                self.session, self.area / "consumers", self.area / "dependencies",
                self.area / "packed", compiled, "unit", {"old": {}, "new": {}})
        self.assertEqual(pulls, ["pull-ubuntu2204", "pull-debian12"])
        self.assertEqual(calls, [("debian12", "old"), ("debian12", "new")])
        self.assertEqual(len(cases), 4)
        for case in cases[:2]:
            self.assertEqual(case["verdict"], "RUNTIME_UNAVAILABLE")
            self.assertIsNone(case["raw"])
            self.assertIsNone(case["container"])
            self.assertFalse(case["docker_run_attempted"])
            self.assertEqual(case["runtime_preparation"]["pull"]["returncode"], 125)
        with self.assertRaises(self.d.Refusal):
            self.d.require_complete_cases(self.session, cases)
        self.assertFalse((self.report / "RUN.COMPLETE").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scratch-root", type=Path, required=True)
    options, remaining = parser.parse_known_args()
    SCRATCH = options.scratch_root
    unittest.main(argv=[sys.argv[0], *remaining])
