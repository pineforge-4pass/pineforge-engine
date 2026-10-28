#!/usr/bin/env python3
"""Subprocess tests for the release ABI inventory, including must-fail inputs."""

from __future__ import annotations

import ast
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COLUMNS = "symbol\tdeclared_in\timplemented_in_c_abi_cpp"
NATIVE_SECTION = (
    "# native C API: independent; implementations in src/native_c_host.cpp"
)


def source_symbols(path: Path) -> list[str]:
    """Read declaration/definition names with token splitting, not the checker."""
    names = []
    for line in path.read_text(encoding="utf-8").splitlines():
        prefix, opening, _ = line.partition("(")
        tokens = prefix.split()
        if opening and tokens and tokens[0] == "PF_API":
            names.append(tokens[-1].lstrip("*"))
    return names


class ExportListTests(unittest.TestCase):
    def run_emitter(
        self, *args: str, root: Path = ROOT,
    ) -> subprocess.CompletedProcess[bytes]:
        # Deliberately omit -B here: the emitter must suppress checker bytecode.
        return subprocess.run(
            [sys.executable, str(root / "scripts" / "emit_abi_export_list.py"), *args],
            cwd=root, capture_output=True, check=False, timeout=30,
        )

    def successful_output(self, *, root: Path = ROOT) -> bytes:
        result = self.run_emitter(root=root)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stderr, b"")
        return result.stdout

    def rows(self, output: bytes) -> tuple[list[list[str]], list[list[str]]]:
        lines = output.decode("utf-8").splitlines()
        self.assertEqual(lines[0], COLUMNS)
        boundary = lines.index(NATIVE_SECTION)
        self.assertEqual(lines[boundary + 1], COLUMNS)
        runtime = [line.split("\t") for line in lines[1:boundary]]
        native = [
            line.split("\t") for line in lines[boundary + 2:]
            if not line.startswith("#")
        ]
        for row in runtime + native:
            self.assertEqual(len(row), 3, row)
        return runtime, native

    def copy_inputs(self) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        shutil.copytree(ROOT / "include", root / "include")
        (root / "src").mkdir()
        for name in ("c_abi.cpp", "native_c_host.cpp"):
            shutil.copyfile(ROOT / "src" / name, root / "src" / name)
        (root / "scripts").mkdir()
        for name in ("emit_abi_export_list.py", "check_c_abi_runtime.py"):
            shutil.copyfile(ROOT / "scripts" / name, root / "scripts" / name)
        # Prove the copied tree is complete before a mutation can earn a failure.
        self.assertEqual(self.successful_output(root=root), self.successful_output())
        self.assertFalse((root / "scripts" / "__pycache__").exists())
        return root

    def assert_declaration_rejected(self, root: Path) -> None:
        result = self.run_emitter(root=root)
        self.assertEqual(result.returncode, 1, result.stderr.decode())
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"check_c_abi_runtime:", result.stderr)
        self.assertIn(b"PF_API declaration count", result.stderr)

        # Validation must precede even a forced replacement of an existing file.
        output = root / "inventory.tsv"
        sentinel = b"keep this artifact\x00\r\n"
        output.write_bytes(sentinel)
        result = self.run_emitter("--output", str(output), "--force", root=root)
        self.assertEqual(result.returncode, 1, result.stderr.decode())
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"check_c_abi_runtime:", result.stderr)
        self.assertEqual(output.read_bytes(), sentinel)

    def test_rows_and_measured_counts_match_sources(self) -> None:
        output = self.successful_output()
        runtime, native = self.rows(output)
        include_dir = ROOT / "include" / "pineforge"
        implemented_names = source_symbols(ROOT / "src" / "c_abi.cpp")
        implemented = set(implemented_names)
        # This is the release contract; no checker/emitter constants are imported.
        expected_runtime = []
        for header in (
            "pineforge.h", "execution_observer.h", "selected_window.h", "report_sink.h",
        ):
            expected_runtime.extend(
                [symbol, header, "yes" if symbol in implemented else "no"]
                for symbol in source_symbols(include_dir / header)
            )
        self.assertEqual(runtime, sorted(expected_runtime))
        self.assertEqual(len(runtime), 115)
        self.assertEqual(len(implemented_names), 95)
        yes_count = sum(row[2] == "yes" for row in runtime)
        self.assertEqual(yes_count, 95)
        self.assertEqual(yes_count, len(implemented_names))
        for symbol, header, _ in runtime + native:
            with self.subTest(symbol=symbol, header=header):
                self.assertIn(symbol, (include_dir / header).read_text(encoding="utf-8"))

        native_names = source_symbols(include_dir / "native_c_api.h")
        native_implementations = source_symbols(ROOT / "src" / "native_c_host.cpp")
        self.assertEqual(len(native_names), 43)
        self.assertEqual(len(native_implementations), 43)
        self.assertEqual(set(native_names), set(native_implementations))
        self.assertTrue(set(native_names).isdisjoint(implemented))
        self.assertEqual(native, sorted(
            [symbol, "native_c_api.h", "yes" if symbol in implemented else "no"]
            for symbol in native_names
        ))
        self.assertEqual(len(native), len(native_names))
        lines = output.decode("utf-8").splitlines()
        self.assertIn(
            f"# native_c_api_declarations={len(native)} "
            f"native_c_api_implementations={len(native_implementations)}",
            lines,
        )
        self.assertEqual(
            lines[-2],
            f"# runtime_declarations={len(runtime)} runtime_implementations={yes_count} "
            f"native_c_api_declarations={len(native)}",
        )

    def test_two_runs_are_byte_identical(self) -> None:
        self.assertEqual(self.successful_output(), self.successful_output())

    def test_sha256_covers_every_preceding_byte(self) -> None:
        output = self.successful_output()
        body, marker, digest = output.rpartition(b"# sha256=")
        self.assertEqual(marker, b"# sha256=")
        self.assertEqual(output.count(marker), 1)
        self.assertEqual(digest, hashlib.sha256(body).hexdigest().encode("ascii") + b"\n")

    def test_report_modes_comment_matches_cli_refusal(self) -> None:
        source = (ROOT / "docker" / "run_json.py").read_text(encoding="utf-8")
        tree = ast.parse(source)
        branches = [
            node for node in ast.walk(tree)
            if isinstance(node, ast.If)
            and isinstance(node.test, ast.Compare)
            and isinstance(node.test.left, ast.Attribute)
            and node.test.left.attr == "report_mode"
            and len(node.test.ops) == 1
            and isinstance(node.test.ops[0], ast.In)
        ]
        self.assertEqual(len(branches), 1)
        branch = branches[0]
        self.assertEqual(
            set(ast.literal_eval(branch.test.comparators[0])),
            {"SUMMARY_ONLY", "STREAMED"},
        )
        refusal_codes = [
            node.exc.args[1].value for node in ast.walk(branch)
            if isinstance(node, ast.Raise)
            and isinstance(node.exc, ast.Call)
            and isinstance(node.exc.func, ast.Name)
            and node.exc.func.id == "RunFailure"
            and len(node.exc.args) > 1
            and isinstance(node.exc.args[1], ast.Constant)
        ]
        self.assertIn("report_mode_unsupported", refusal_codes)
        self.assertIn(
            "# report modes: FULL only through the C ABI/CLI; SUMMARY_ONLY and "
            "STREAMED refuse typed (report_mode_unsupported)",
            self.successful_output().decode("utf-8").splitlines(),
        )

    def test_report_mode_is_not_an_emitter_option(self) -> None:
        result = self.run_emitter("--report-mode", "FULL")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"unrecognized arguments", result.stderr)

    def test_output_file_and_forced_replacement_match_stdout(self) -> None:
        expected = self.successful_output()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "inventory.tsv"
            result = self.run_emitter("--output", str(output))
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(result.stdout, b"")
            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(set(Path(directory).iterdir()), {output})

            output.write_bytes(b"old inventory")
            result = self.run_emitter("--output", str(output), "--force")
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(result.stdout, b"")
            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(set(Path(directory).iterdir()), {output})

    def test_existing_output_is_unchanged_without_force(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "inventory.tsv"
            sentinel = b"preexisting artifact\x00\r\n"
            output.write_bytes(sentinel)
            result = self.run_emitter("--output", str(output))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, b"")
            self.assertIn(b"--force", result.stderr)
            self.assertEqual(output.read_bytes(), sentinel)
            self.assertEqual(set(Path(directory).iterdir()), {output})

    def test_removed_declaration_emits_nothing(self) -> None:
        root = self.copy_inputs()
        header = root / "include" / "pineforge" / "pineforge.h"
        before = source_symbols(header)
        text = header.read_text(encoding="utf-8")
        declaration = "PF_API int pf_abi_version(void);\n"
        self.assertEqual(text.count(declaration), 1)
        header.write_text(text.replace(declaration, "", 1), encoding="utf-8")
        self.assertEqual(len(source_symbols(header)), len(before) - 1)
        self.assert_declaration_rejected(root)

    def test_extra_declaration_emits_nothing(self) -> None:
        root = self.copy_inputs()
        header = root / "include" / "pineforge" / "pineforge.h"
        before = source_symbols(header)
        header.write_text(
            header.read_text(encoding="utf-8")
            + "\nPF_API int export_list_unexpected_declaration(void);\n",
            encoding="utf-8",
        )
        self.assertEqual(len(source_symbols(header)), len(before) + 1)
        self.assert_declaration_rejected(root)

    def test_missing_input_preserves_checker_exit_code(self) -> None:
        root = self.copy_inputs()
        (root / "src" / "c_abi.cpp").unlink()
        result = self.run_emitter(root=root)
        self.assertEqual(result.returncode, 2, result.stderr.decode())
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"check_c_abi_runtime: missing", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
