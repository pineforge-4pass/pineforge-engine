#!/usr/bin/env python3
"""Real ELF/archive tests for the internal staging checker.

Run only on an admitted Linux test host:
  python3 scripts/test_check_prebuilt_glibc.py --scratch-root /explicit/scratch

cc, GNU ar, and GNU readelf must already be installed. Missing tools fail.
Fixtures and mutation candidates stay in the explicit scratch directory for
review; nothing is installed, extracted, loaded, executed, or deleted. Fixture
generation uses no checker code. Mocked tests cover errors only; symbol policy
tests and mutation controls use real ELF metadata and real child processes.
"""
from __future__ import annotations

import argparse
import ast
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


CHECKER = Path(__file__).absolute().with_name("check_prebuilt_glibc.py")
SCRATCH: Path | None = None
ENV = {"LC_ALL": "C", "LANG": "C", "PATH": os.defpath}
PROTECTED = {
    ".git", ".ssh", ".aws", ".codex", ".agents", ".config",
    "config", "configs", "credentials", "keys", "secrets",
}
sys.dont_write_bytecode = True


def declare(*paths: Path) -> None:
    print(json.dumps({"fixture_paths": [str(path) for path in paths]}), flush=True)


def safe_path(path: Path) -> Path:
    """Validate before reading/writing; generated paths must be under SCRATCH."""
    if ".." in path.parts:
        raise AssertionError(f"parent traversal refused: {path}")
    absolute = path.absolute()
    for part in absolute.parts:
        lower = part.lower()
        if (lower in PROTECTED or lower.startswith(".env")
                or lower.endswith((".key", ".pem")) or lower.startswith("id_rsa")):
            raise AssertionError(f"protected fixture path refused: {absolute}")
    current = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode):
            raise AssertionError(f"symlink fixture access refused: {current}")
    return absolute


class RealArtifacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if SCRATCH is None:
            raise AssertionError("--scratch-root is required; no implicit /tmp writes")
        declare(SCRATCH, CHECKER)
        cls.scratch = safe_path(SCRATCH)
        if not cls.scratch.is_dir():
            raise AssertionError(f"scratch root must already exist: {cls.scratch}")
        safe_path(CHECKER)
        if not CHECKER.is_file():
            raise AssertionError(f"production checker missing: {CHECKER}")
        cls.tools = {}
        for name in ("cc", "ar", "readelf"):
            tool = shutil.which(name, path=os.defpath)
            if tool is None:
                raise AssertionError(f"installed fixture tool required: {name}")
            cls.tools[name] = tool

    def setUp(self) -> None:
        declare(self.scratch)
        self.area = Path(tempfile.mkdtemp(prefix="prebuilt-glibc-", dir=self.scratch))
        declare(self.area)
        self.root = self.area / "staging root"
        declare(self.root)
        self.root.mkdir()
        self.counter = 0

    def generated(self, path: Path) -> Path:
        path = safe_path(path)
        path.relative_to(self.area)
        return path

    def source(self, path: Path, contents: str | bytes) -> Path:
        declare(path)
        self.generated(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(contents, bytes):
            path.write_bytes(contents)
        else:
            path.write_text(contents, encoding="utf-8")
        return path

    def copy(self, source: Path, target: Path) -> Path:
        declare(source, target)
        self.generated(source)
        self.generated(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        return target

    def command(self, argv: list[str]) -> subprocess.CompletedProcess:
        # Callers declare all fixture inputs/outputs before using this helper.
        result = subprocess.run(argv, capture_output=True,
                                env={**ENV, "TMPDIR": str(self.area)})
        print(json.dumps({
            "fixture_command": argv, "returncode": result.returncode,
            "stdout": result.stdout.decode("utf-8", "surrogateescape"),
            "stderr": result.stderr.decode("utf-8", "surrogateescape"),
        }), flush=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def work(self) -> Path:
        self.counter += 1
        path = self.area / f"fixture {self.counter}"
        declare(path)
        path.mkdir()
        return path

    def object(self, name: str = "member.o", reference: str | None = None,
               version: str | None = None, source_name: str = "unit.c") -> Path:
        work = self.work()
        if reference:
            text = (f"extern long {reference}(const char *, char **, int);\n"
                    f"long fixture_call(const char *s) {{ return {reference}(s, 0, 10); }}\n")
        elif version:
            text = (f'__asm__(".symver fixture_import,fixture_import@{version}");\n'
                    "extern int fixture_import(void);\n"
                    "int fixture_call(void) { return fixture_import(); }\n")
        else:
            text = ("const char fixture_text[] = \"GLIBC_99.0 __isoc23_strtol\";\n"
                    "int fixture_value(void) { return 7; }\n")
        source = self.source(work / source_name, text)
        output = work / name
        declare(source, output)
        self.command([self.tools["cc"], "-c", "-fPIC", "-O0", "-fno-lto",
                      str(source), "-o", str(output)])
        if reference or version:
            declare(output)
            probe = self.command([self.tools["readelf"], "--wide", "--symbols",
                                  str(output)]).stdout.decode()
            expected = reference or f"fixture_import@{version}"
            self.assertRegex(probe, r"UND\s+" + expected.replace(".", r"\."))
        return output

    def shared(self, relative: str, versions: tuple[str, ...] = (),
               definitions: bool = False) -> Path:
        work = self.work()
        output = self.root / relative
        declare(output)
        self.generated(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        if versions:
            provider = self.source(work / "provider.c", "".join(
                f"int fixture_api_{i}(void) {{ return {i + 1}; }}\n"
                for i in range(len(versions))))
            script = self.source(work / "versions.map", "".join(
                f"{version} {{ global: fixture_api_{i}; }};\n"
                for i, version in enumerate(versions)))
            dependency = work / "fixture-provider.so"
            declare(provider, script, dependency)
            self.command([self.tools["cc"], "-shared", "-fPIC", "-nostdlib",
                          str(provider), "-Xlinker", "--version-script",
                          "-Xlinker", str(script), "-Wl,-soname,fixture-provider.so",
                          "-o", str(dependency)])
            if definitions:
                self.copy(dependency, output)
            else:
                consumer = self.source(work / "consumer.c", "".join(
                    f"extern int fixture_api_{i}(void);\n" for i in range(len(versions)))
                    + "int fixture_use(void) { return "
                    + " + ".join(f"fixture_api_{i}()" for i in range(len(versions)))
                    + "; }\n")
                declare(consumer, dependency, output)
                self.command([self.tools["cc"], "-shared", "-fPIC", "-nostdlib",
                              str(consumer), str(dependency), "-o", str(output)])
            declare(output)
            probe = self.command([self.tools["readelf"], "--wide", "--symbols",
                                  "--version-info", str(output)]).stdout.decode()
            for version in versions:
                self.assertIn("Name: " + version, probe)
                self.assertIn("@" + version, probe)
        else:
            source = self.source(work / "clean.c",
                                 "int fixture_export(void) { return 17; }\n")
            declare(source, output)
            self.command([self.tools["cc"], "-shared", "-fPIC", "-nostdlib",
                          str(source), "-o", str(output)])
        return output

    def archive(self, relative: str, members: list[Path], thin: bool = False,
                duplicate: bool = False) -> Path:
        output = self.root / relative
        declare(*members, output)
        for member in members:
            self.generated(member)
        self.generated(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        flags = "rcT" if thin else ("qc" if duplicate else "rcs")
        self.command([self.tools["ar"], flags, str(output),
                      *(str(member) for member in members)])
        if duplicate:
            self.command([self.tools["ar"], "s", str(output)])
        return output

    def run_checker(self, root: Path | None = None, checker: Path = CHECKER):
        root = self.root if root is None else root
        declare(checker, root)
        safe_path(checker)
        if checker != CHECKER:
            self.generated(checker)
        argv = [sys.executable, "-B", str(checker), str(root)]
        result = subprocess.run(argv, capture_output=True, env=ENV)
        print(json.dumps({
            "checker_command": argv, "returncode": result.returncode,
            "stdout": result.stdout.decode("utf-8", "surrogateescape"),
            "stderr": result.stderr.decode("utf-8", "surrogateescape"),
        }), flush=True)
        self.assertEqual(result.stderr, b"", result.stderr)
        records = [json.loads(line) for line in result.stdout.decode().splitlines()]
        self.assertTrue(records, "checker must emit inspection evidence")
        self.assertEqual(records[-1]["event"], "summary")
        return result.returncode, records

    def assert_verdict(self, outcome, success: bool) -> list[dict]:
        rc, records = outcome
        if success:
            self.assertEqual(rc, 0, records)
        else:
            self.assertNotEqual(rc, 0, records)
        self.assertEqual(records[-1]["verdict"], "PASS" if success else "FAIL")
        return records

    def targets(self, records: list[dict]) -> dict[str, dict]:
        rows = [row for row in records if row["event"] == "target"]
        self.assertEqual(len({row["path"] for row in rows}), len(rows))
        return {row["path"]: row for row in rows}

    def assert_issue(self, records: list[dict], path: Path, text: str) -> None:
        issues = [row for row in records if row["event"] == "issue"]
        self.assertTrue(any(row["path"] == str(path) and text in row["cause"]
                            for row in issues), issues)

    def load_checker(self):
        declare(CHECKER)
        safe_path(CHECKER)
        spec = importlib.util.spec_from_file_location("_glibc_checker_under_test", CHECKER)
        self.assertIsNotNone(spec)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def in_process(self, module):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            rc = module.main([str(self.root)])
        print(json.dumps({"checker_main_returncode": rc, "stdout": output.getvalue()}),
              flush=True)
        return rc, [json.loads(line) for line in output.getvalue().splitlines()]

    def test_complete_inventory_members_spaces_and_content_identity(self):
        paths = [
            self.shared("lib/first library.so"),
            self.shared("lib/deep/provider.so.1.2", ("GLIBC_2.34",)),
            self.archive("lib/a.a", [self.object("first.o"), self.object("member space.o")]),
            self.archive("other/nested/b.a", [self.object("last.o")]),
        ]
        records = self.assert_verdict(self.run_checker(), True)
        inventory = next(row for row in records if row["event"] == "inventory")
        self.assertEqual(set(inventory["paths"]), {str(path) for path in paths})
        targets = self.targets(records)
        self.assertEqual(set(targets), {str(path) for path in paths})
        inventory_index = records.index(inventory)
        for path in paths:
            declare(path)
            self.generated(path)
            row = targets[str(path)]
            self.assertTrue(row["inspected"])
            self.assertEqual(row["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertGreater(row["size"], 0)
            self.assertGreater(row["symbols"], 0)
            self.assertLess(inventory_index, records.index(row))
        self.assertEqual([row["name"] for row in targets[str(paths[2])]["members"]],
                         ["first.o", "member space.o"])
        self.assertEqual(records[-1]["discovered"], 4)
        self.assertEqual(records[-1]["inspected"], 4)
        self.assertEqual(records[-1]["archive_members"], 3)
        self.assertEqual(targets[str(paths[1])]["max_glibc"], "2.34")
        for index, record in enumerate(records):
            if record["event"] == "command" and record["path"] in targets:
                self.assertGreater(index, inventory_index)

    def test_older_numeric_versions_are_accepted(self):
        for version in ("2.2.5", "2.9", "2.34", "2.36", "2.36.0"):
            with self.subTest(version=version):
                path = self.shared(f"older/{version}/provider.so", ("GLIBC_" + version,))
                records = self.assert_verdict(self.run_checker(), True)
                self.assertEqual(self.targets(records)[str(path)]["max_glibc"], version)

    def test_future_minor_patch_and_major_versions_are_rejected(self):
        for version in ("2.37", "2.38", "2.39", "2.40", "2.100", "3.0", "10.0", "2.36.1"):
            with self.subTest(version=version):
                path = self.shared(f"future/{version}/library.so.9", ("GLIBC_" + version,))
                records = self.assert_verdict(self.run_checker(), False)
                self.assert_issue(records, path, "GLIBC_" + version)
                self.assertEqual(self.targets(records)[str(path)]["max_glibc"], version)

    def test_maximum_is_numeric_and_reports_all_violations(self):
        path = self.shared("lib/all.so", ("GLIBC_2.9", "GLIBC_2.40", "GLIBC_2.100"))
        records = self.assert_verdict(self.run_checker(), False)
        target = self.targets(records)[str(path)]
        self.assertEqual(target["max_glibc"], "2.100")
        self.assertEqual(set(target["versions"]), {"GLIBC_2.9", "GLIBC_2.40", "GLIBC_2.100"})
        for version in ("GLIBC_2.40", "GLIBC_2.100"):
            self.assert_issue(records, path, version)

    def test_version_definitions_are_inspected_as_well_as_requirements(self):
        clean = self.shared("lib/clean definition.so", ("GLIBC_2.36",), definitions=True)
        records = self.assert_verdict(self.run_checker(), True)
        self.assertEqual(self.targets(records)[str(clean)]["max_glibc"], "2.36")
        bad = self.shared("lib/high definition.so", ("GLIBC_3.0",), definitions=True)
        records = self.assert_verdict(self.run_checker(), False)
        self.assert_issue(records, bad, "GLIBC_3.0")

    def test_private_and_unknown_glibc_grammar_are_rejected(self):
        for version in ("GLIBC_PRIVATE", "GLIBC_FUTURE", "GLIBC_2.bad", "GLIBC_2"):
            with self.subTest(version=version):
                path = self.shared(f"unknown/{version}/library.so", (version,))
                records = self.assert_verdict(self.run_checker(), False)
                self.assert_issue(records, path, version)

    def test_other_namespace_and_data_strings_are_not_numeric_glibc(self):
        path = self.shared("lib/GLIBC_99.0.so", ("GLIBCXX_3.4",))
        archive = self.archive("lib/text.a", [
            self.object("first.o", source_name="__isoc23_strtol.c"),
            self.object("second.o", source_name="unit@GLIBC_99.0.c"),
        ])
        records = self.assert_verdict(self.run_checker(), True)
        targets = self.targets(records)
        self.assertIsNone(targets[str(path)]["max_glibc"])
        self.assertIsNone(targets[str(archive)]["max_glibc"])
        self.assertIn("GLIBCXX_3.4", targets[str(path)]["other_versions"])
        self.assertEqual(targets[str(archive)]["isoc23_references"], [])

    def test_unversioned_isoc23_references_in_static_objects_are_rejected(self):
        for symbol in ("__isoc23_strtol", "__isoc23_strtoll", "__isoc23_future"):
            with self.subTest(symbol=symbol):
                path = self.archive(f"lib/{symbol}.a", [self.object(reference=symbol)])
                records = self.assert_verdict(self.run_checker(), False)
                self.assert_issue(records, path, symbol)
                row = self.targets(records)[str(path)]
                self.assertIsNone(row["max_glibc"])
                self.assertIn(symbol, [ref["symbol"] for ref in row["isoc23_references"]])

    def test_versioned_static_undefined_reference_is_rejected(self):
        path = self.archive("lib/versioned.a", [self.object(version="GLIBC_3.0")])
        records = self.assert_verdict(self.run_checker(), False)
        self.assert_issue(records, path, "GLIBC_3.0")
        self.assertEqual(self.targets(records)[str(path)]["max_glibc"], "3.0")

    def test_unversioned_shared_isoc23_import_is_rejected(self):
        member = self.object(reference="__isoc23_strtol")
        path = self.root / "lib" / "unversioned import.so"
        declare(member, path)
        self.generated(path)
        path.parent.mkdir()
        self.command([self.tools["cc"], "-shared", "-nostdlib", str(member), "-o", str(path)])
        records = self.assert_verdict(self.run_checker(), False)
        self.assert_issue(records, path, "__isoc23_strtol")
        row = self.targets(records)[str(path)]
        self.assertTrue(row["inspected"])
        self.assertIsNone(row["max_glibc"])

    def test_second_archive_late_member_nested_shared_and_all_targets_are_scanned(self):
        first = self.archive("lib/a.a", [self.object("first.o")])
        second = self.archive("lib/z.a", [
            self.object("early.o"), self.object("middle.o"),
            self.object("late object.o", reference="__isoc23_strtoll"),
        ])
        shared = self.shared("elsewhere/lib/deep/provider.so.3.4", ("GLIBC_2.38",))
        final = self.archive("zz/final.a", [self.object("final.o", reference="__isoc23_strtol")])
        records = self.assert_verdict(self.run_checker(), False)
        targets = self.targets(records)
        self.assertEqual(set(targets), {str(path) for path in (first, second, shared, final)})
        self.assertTrue(all(row["inspected"] for row in targets.values()))
        self.assertEqual(records[-1]["archive_members"], 5)
        self.assertEqual([row["name"] for row in targets[str(second)]["members"]],
                         ["early.o", "middle.o", "late object.o"])
        self.assert_issue(records, second, "__isoc23_strtoll")
        self.assert_issue(records, shared, "GLIBC_2.38")
        self.assert_issue(records, final, "__isoc23_strtol")

    def test_duplicate_member_names_have_separate_ordinals(self):
        path = self.archive("lib/duplicate.a", [
            self.object("same.o"), self.object("same.o", reference="__isoc23_strtol"),
        ], duplicate=True)
        records = self.assert_verdict(self.run_checker(), False)
        members = self.targets(records)[str(path)]["members"]
        self.assertEqual([row["name"] for row in members], ["same.o", "same.o"])
        self.assertEqual([row["ordinal"] for row in members], [1, 2])
        self.assert_issue(records, path, "__isoc23_strtol")

    def test_no_libraries_and_missing_root_fail(self):
        records = self.assert_verdict(self.run_checker(), False)
        self.assertEqual(records[-1]["discovered"], 0)
        missing = self.area / "absent"
        records = self.assert_verdict(self.run_checker(missing), False)
        self.assert_issue(records, missing, "root")

    def test_malformed_empty_and_wrong_format_targets_fail_with_paths(self):
        invalid = [
            self.source(self.root / "not ELF.so", b"text GLIBC_2.34"),
            self.source(self.root / "not archive.a", b"text"),
            self.source(self.root / "zero.a", b""),
            self.archive("empty.a", []),
            self.copy(self.object(), self.root / "relocatable.so"),
        ]
        records = self.assert_verdict(self.run_checker(), False)
        targets = self.targets(records)
        for path in invalid:
            self.assertFalse(targets[str(path)]["inspected"], path)
            self.assertTrue(any(row["event"] == "issue" and row["path"] == str(path)
                                for row in records), path)

    def test_truncated_elf_preserves_real_tool_error_and_nonzero_cli_exit(self):
        path = self.source(self.root / "truncated.so", b"\x7fELF" + b"\x00" * 8)
        records = self.assert_verdict(self.run_checker(), False)
        commands = [row for row in records if row["event"] == "command"
                    and row["path"] == str(path) and "--version" not in row["argv"]]
        self.assertTrue(commands)
        self.assertTrue(any(row["returncode"] != 0 and row["stderr"] for row in commands))
        self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_nonelf_archive_member_and_shared_member_fail(self):
        text = self.source(self.work() / "not object.txt", b"not ELF")
        invalid = self.archive("lib/invalid.a", [self.object("good.o"), text])
        shared = self.shared("lib/good.so")
        wrong = self.archive("lib/wrong type.a", [shared])
        records = self.assert_verdict(self.run_checker(), False)
        for path in (invalid, wrong):
            self.assertFalse(self.targets(records)[str(path)]["inspected"])
        failed = self.targets(records)[str(invalid)]
        self.assertEqual(failed["expected_members"], ["good.o", "not object.txt"])
        self.assertEqual([member["name"] for member in failed["members"]],
                         failed["expected_members"])
        self.assertFalse(any(member["inspected"] for member in failed["members"]))

    def test_thin_archive_is_rejected_without_following_members(self):
        path = self.archive("lib/thin.a", [self.object(reference="__isoc23_strtol")], thin=True)
        records = self.assert_verdict(self.run_checker(), False)
        self.assert_issue(records, path, "thin")
        self.assertFalse(any(row["event"] == "command" and row["path"] == str(path)
                             for row in records))

    def test_all_library_symlinks_are_rejected_including_internal_links(self):
        actual = self.shared("lib/real.so")
        outside = self.copy(actual, self.area / "outside.so")
        links = [
            (self.root / "internal.so.1", actual),
            (self.root / "outside.so", outside),
            (self.root / "broken.a", self.area / "missing.a"),
        ]
        for link, destination in links:
            declare(link, destination)
            link.symlink_to(destination)
        records = self.assert_verdict(self.run_checker(), False)
        for link, _ in links:
            self.assert_issue(records, link, "symlink")
            self.assertFalse(self.targets(records)[str(link)]["inspected"])
            self.assertFalse(any(row["event"] == "command" and row["path"] == str(link)
                                 for row in records))

    def test_root_and_directory_symlinks_are_rejected(self):
        self.shared("lib/real.so")
        alias = self.area / "root alias"
        declare(alias, self.root)
        alias.symlink_to(self.root, target_is_directory=True)
        records = self.assert_verdict(self.run_checker(alias), False)
        self.assert_issue(records, alias, "symlink")
        records = self.assert_verdict(self.run_checker(alias / "lib"), False)
        self.assert_issue(records, alias / "lib", "symlink")
        external = self.area / "outside directory"
        declare(external)
        external.mkdir()
        link = self.root / "linked directory"
        declare(link, external)
        link.symlink_to(external, target_is_directory=True)
        records = self.assert_verdict(self.run_checker(), False)
        self.assert_issue(records, link, "symlink")

    def test_protected_entry_and_library_named_directory_fail_without_content_reads(self):
        self.shared("lib/good.so")
        protected = self.root / ".env"
        # Deliberately create, never read, this forbidden fixture entry.
        declare(protected)
        protected.mkdir()
        misleading = self.root / "directory.so"
        declare(misleading)
        misleading.mkdir()
        records = self.assert_verdict(self.run_checker(), False)
        self.assert_issue(records, protected, "protected")
        self.assert_issue(records, misleading, "regular")
        self.assertFalse(self.targets(records)[str(misleading)]["inspected"])

    def test_unreadable_library_and_directory_fail_even_for_privileged_testrunner(self):
        path = self.shared("lib/unreadable.so")
        directory = self.root / "unreadable directory"
        declare(directory)
        directory.mkdir()
        path.chmod(0)
        directory.chmod(0)
        try:
            records = self.assert_verdict(self.run_checker(), False)
            self.assert_issue(records, path, "read")
            self.assert_issue(records, directory, "read")
        finally:
            path.chmod(0o644)
            directory.chmod(0o755)

    def test_tool_missing_fails_instead_of_empty_success(self):
        path = self.shared("lib/good.so")
        module = self.load_checker()
        with mock.patch.object(module.shutil, "which", return_value=None):
            records = self.assert_verdict(self.in_process(module), False)
        self.assertTrue(any(row["event"] == "issue" and "tool" in row["cause"]
                            for row in records))
        self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_child_returncode_and_exact_streams_are_preserved(self):
        path = self.shared("lib/good.so")
        module = self.load_checker()
        original = subprocess.run

        def broken(argv, **kwargs):
            if "--symbols" in argv:
                return subprocess.CompletedProcess(argv, 23, b"partial stdout\n", b"reader failed\n")
            return original(argv, **kwargs)

        with mock.patch.object(module.subprocess, "run", side_effect=broken):
            records = self.assert_verdict(self.in_process(module), False)
        row = next(row for row in records if row["event"] == "command"
                   and row["path"] == str(path) and row["returncode"] == 23)
        self.assertEqual(row["stdout"], "partial stdout\n")
        self.assertEqual(row["stderr"], "reader failed\n")
        self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_empty_malformed_or_warning_tool_output_never_passes(self):
        path = self.shared("lib/good.so")
        for stdout, stderr in ((b"", b""), (b"unparseable\n", b""),
                               (b"ELF Header:\n", b"warning: incomplete\n")):
            with self.subTest(stdout=stdout, stderr=stderr):
                module = self.load_checker()
                original = subprocess.run

                def broken(argv, **kwargs):
                    if "--symbols" in argv:
                        return subprocess.CompletedProcess(argv, 0, stdout, stderr)
                    return original(argv, **kwargs)

                with mock.patch.object(module.subprocess, "run", side_effect=broken):
                    records = self.assert_verdict(self.in_process(module), False)
                self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_read_failure_never_becomes_clean(self):
        path = self.shared("lib/good.so")
        module = self.load_checker()
        original = os.read

        def unreadable(descriptor, count):
            if stat.S_ISREG(os.fstat(descriptor).st_mode):
                raise OSError("injected read failure")
            return original(descriptor, count)

        with mock.patch.object(module.os, "read", side_effect=unreadable):
            records = self.assert_verdict(self.in_process(module), False)
        self.assert_issue(records, path, "injected read failure")
        self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_tool_start_error_is_preserved_and_fails(self):
        path = self.shared("lib/good.so")
        module = self.load_checker()
        original = subprocess.run

        def unavailable(argv, **kwargs):
            if "--symbols" in argv:
                raise OSError("inspection tool start denied")
            return original(argv, **kwargs)

        with mock.patch.object(module.subprocess, "run", side_effect=unavailable):
            records = self.assert_verdict(self.in_process(module), False)
        self.assert_issue(records, path, "inspection tool start denied")
        command = next(row for row in records if row["event"] == "command"
                       and row["path"] == str(path))
        self.assertIsNone(command["returncode"])
        self.assertEqual(command["error"], "inspection tool start denied")

    def test_incomplete_real_symbol_and_version_output_is_refused(self):
        path = self.shared("lib/good.so", ("GLIBC_2.34",))
        self.assert_verdict(self.run_checker(), True)
        for mode in ("symbol", "version"):
            with self.subTest(mode=mode):
                module = self.load_checker()
                original = subprocess.run

                def incomplete(argv, **kwargs):
                    result = original(argv, **kwargs)
                    if "--symbols" not in argv:
                        return result
                    lines = result.stdout.decode().splitlines()
                    pattern = (r"\s*1:\s+[0-9a-fA-F]+\s+" if mode == "symbol"
                               else r"\s*(?:0x)?[0-9a-fA-F]+:\s+Name:\s+GLIBC_")
                    matches = [index for index, line in enumerate(lines)
                               if re.match(pattern, line)]
                    self.assertTrue(matches, "must actually remove a real metadata row")
                    del lines[matches[0]]
                    return subprocess.CompletedProcess(
                        argv, result.returncode, ("\n".join(lines) + "\n").encode(), result.stderr)

                with mock.patch.object(module.subprocess, "run", side_effect=incomplete):
                    records = self.assert_verdict(self.in_process(module), False)
                self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_null_only_dynamic_table_cannot_borrow_static_table_coverage(self):
        path = self.shared("lib/good.so")
        self.assert_verdict(self.run_checker(), True)
        module = self.load_checker()
        original = subprocess.run

        def empty_dynamic(argv, **kwargs):
            result = original(argv, **kwargs)
            if "--symbols" not in argv:
                return result
            lines = result.stdout.decode().splitlines()
            start = next(index for index, line in enumerate(lines)
                         if line.startswith("Symbol table '.dynsym' contains "))
            end = start + 3
            while end < len(lines) and lines[end].strip():
                end += 1
            self.assertGreater(end, start + 3, "must remove actual non-null dynamic symbols")
            changed = (lines[:start] + ["Symbol table '.dynsym' contains 1 entry:"]
                       + lines[start + 1:start + 3] + lines[end:])
            return subprocess.CompletedProcess(
                argv, result.returncode, ("\n".join(changed) + "\n").encode(), result.stderr)

        with mock.patch.object(module.subprocess, "run", side_effect=empty_dynamic):
            records = self.assert_verdict(self.in_process(module), False)
        self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_archive_member_inventory_must_match_readelf(self):
        path = self.archive("lib/good.a", [self.object("first.o"), self.object("second.o")])
        module = self.load_checker()
        original = subprocess.run

        def incomplete(argv, **kwargs):
            if len(argv) > 1 and argv[1] == "t":
                return subprocess.CompletedProcess(argv, 0, b"first.o\n", b"")
            return original(argv, **kwargs)

        with mock.patch.object(module.subprocess, "run", side_effect=incomplete):
            records = self.assert_verdict(self.in_process(module), False)
        self.assert_issue(records, path, "member")
        self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_empty_archive_tool_listing_cannot_hide_real_members(self):
        path = self.archive("lib/good.a", [self.object()])
        module = self.load_checker()
        original = subprocess.run

        def empty(argv, **kwargs):
            if len(argv) > 1 and argv[1] == "t":
                return subprocess.CompletedProcess(argv, 0, b"", b"")
            return original(argv, **kwargs)

        with mock.patch.object(module.subprocess, "run", side_effect=empty):
            records = self.assert_verdict(self.in_process(module), False)
        self.assert_issue(records, path, "empty archive")
        self.assertFalse(self.targets(records)[str(path)]["inspected"])

    def test_no_policy_loosening_option_exists(self):
        self.shared("lib/good.so")
        declare(CHECKER, self.root)
        result = subprocess.run([sys.executable, "-B", str(CHECKER), str(self.root),
                                 "--max-glibc", "99.0"], capture_output=True, env=ENV)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"unrecognized arguments", result.stderr)

    def mutant(self, predicate: str) -> Path:
        declare(CHECKER)
        safe_path(CHECKER)
        tree = ast.parse(CHECKER.read_text(encoding="utf-8"))
        count = 0
        for node in tree.body:
            if isinstance(node, ast.FunctionDef) and node.name == predicate:
                node.body = [ast.Return(value=ast.Constant(value=False))]
                count += 1
        self.assertEqual(count, 1, f"must mutate exactly one policy predicate: {predicate}")
        ast.fix_missing_locations(tree)
        return self.source(self.area / (predicate + "_mutant.py"), ast.unparse(tree) + "\n")

    def test_version_policy_mutation_breaks_real_rejection_assertion(self):
        self.shared("lib/arbitrary name.so.7", ("GLIBC_2.100",))
        self.assert_verdict(self.run_checker(), False)
        mutant = self.mutant("version_exceeds_ceiling")
        changed = self.run_checker(checker=mutant)
        self.assert_verdict(changed, True)
        with self.assertRaises(AssertionError):
            self.assert_verdict(changed, False)

    def test_isoc23_policy_mutation_breaks_real_rejection_assertion(self):
        self.archive("deep/arbitrary name.a", [
            self.object("early.o"), self.object("late.o", reference="__isoc23_strtoll"),
        ])
        self.assert_verdict(self.run_checker(), False)
        mutant = self.mutant("is_isoc23_reference")
        changed = self.run_checker(checker=mutant)
        self.assert_verdict(changed, True)
        with self.assertRaises(AssertionError):
            self.assert_verdict(changed, False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scratch-root", type=Path, required=True)
    options, remaining = parser.parse_known_args()
    SCRATCH = options.scratch_root
    unittest.main(argv=[sys.argv[0], *remaining])
