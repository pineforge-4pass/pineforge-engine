#!/usr/bin/env python3
"""Unit tests for scripts/check_duplicate_test_names.py."""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import textwrap
import unittest
import warnings

import check_duplicate_test_names as guard

DUP_SCRIPT_SOURCE = """import unittest

class SampleTests(unittest.TestCase):
    def test_alpha(self):
        self.assertTrue(True)

    def test_alpha(self):
        self.assertTrue(False)
"""

DUP_DOCKER_SOURCE = """def test_top_level():
    assert True

def test_top_level():
    assert False
"""

CLEAN_SCRIPT_SOURCE = """import unittest

class FirstSuite(unittest.TestCase):
    def test_one(self):
        self.assertTrue(True)

    def test_two(self):
        self.assertTrue(True)

class SecondSuite(unittest.TestCase):
    def test_one(self):
        self.assertTrue(True)
"""

CLEAN_DOCKER_SOURCE = """def test_first():
    assert True

def test_second():
    assert True
"""

# Keep the inventory control independent of the checker's glob table.
INVENTORY_PATHS = (
    'scripts/test_sample.py',
    'scripts/derive_corpus_feeds_self_test.py',
    'scripts/live_flags_lane.py',
    'scripts/check_market_admission_schema.py',
    'docker/sample_test.py',
    'tests/test_sample.py',
    'tests/ownership/test_sample.py',
    'benchmarks/tests/test_sample.py',
    'benchmarks/check_provenance.py',
    'tests/native_live_service_e2e.py',
    'tests/confirmed_receipt_versions.py',
    'tests/lv_d8_tick_stop_equivalence.py',
)
EXPECTED_DIRECTORIES = (
    'scripts', 'docker', 'tests', 'tests/ownership', 'benchmarks', 'benchmarks/tests',
)

# Each body shares its enclosing module or class namespace, including suites
# reached through exception handlers and match cases.
BLOCKS = (
    'if True:\n{body}',
    'if False:\n    pass\nelse:\n{body}',
    'try:\n{body}\nexcept Exception:\n    pass',
    'try:\n    raise ValueError\nexcept ValueError:\n{body}',
    'try:\n    pass\nexcept Exception:\n    pass\nelse:\n{body}',
    'try:\n    pass\nfinally:\n{body}',
    'with contextlib.nullcontext():\n{body}',
    'for item in (1,):\n{body}',
    'for item in ():\n    pass\nelse:\n{body}',
    'while True:\n{body}\n    break',
    'while False:\n    pass\nelse:\n{body}',
    'match 1:\n    case 1:\n{nested_body}',
)


class CheckDuplicateTestNamesTests(unittest.TestCase):
    def invoke(self, root: Path, *paths: Path) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = guard.main(['--root', str(root), *(str(path) for path in paths)])
        return code, out.getvalue()

    def prepare_root(self, root: Path) -> None:
        for directory in EXPECTED_DIRECTORIES:
            (root / directory).mkdir(parents=True, exist_ok=True)

    def write_source(self, root: Path, source: str, relative: str = 'scripts/test_sample.py') -> Path:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding='utf-8')
        return path

    def test_duplicate_test_names_exits_one_and_reports_file_and_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.prepare_root(root)
            scripts_dir = root / 'scripts'
            docker_dir = root / 'docker'

            dup_script = scripts_dir / 'test_sample.py'
            dup_script.write_text(DUP_SCRIPT_SOURCE, encoding='utf-8')

            dup_docker = docker_dir / 'sample_test.py'
            dup_docker.write_text(DUP_DOCKER_SOURCE, encoding='utf-8')

            code, output = self.invoke(root)
            self.assertEqual(code, 1, output)
            self.assertIn(
                f'{dup_script}:7: duplicate method SampleTests.test_alpha '
                '(first defined at line 4)\n', output,
            )
            self.assertIn(
                f'{dup_docker}:4: duplicate top-level test function test_top_level '
                '(first defined at line 1)\n', output,
            )

    def test_clean_test_files_exit_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.prepare_root(root)
            scripts_dir = root / 'scripts'
            docker_dir = root / 'docker'

            (scripts_dir / 'test_clean.py').write_text(CLEAN_SCRIPT_SOURCE, encoding='utf-8')
            (docker_dir / 'clean_test.py').write_text(CLEAN_DOCKER_SOURCE, encoding='utf-8')

            code, output = self.invoke(root)
            self.assertEqual(code, 0, output)
            self.assertEqual(output, '')

    def test_invalid_escape_docstring_without_duplicates_is_silent(self) -> None:
        source = r'''#!/usr/bin/env python3
"""A pattern ends in \w*."""
def test_gate():
    pass
'''
        for action in ('always', 'error'):
            with self.subTest(action=action), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                self.prepare_root(root)
                self.write_source(root, source, 'scripts/check_native_c_api_surface.py')
                with warnings.catch_warnings():
                    warnings.simplefilter(action)
                    code, output = self.invoke(root)
                self.assertEqual(code, 0, output)
                self.assertEqual(output, '')

    def test_invalid_escape_docstring_reports_only_the_real_duplicate(self) -> None:
        source = r'''#!/usr/bin/env python3
"""A pattern ends in \w*."""
def test_gate():
    pass
def test_gate():
    pass
'''
        for action in ('always', 'error'):
            with self.subTest(action=action), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                self.prepare_root(root)
                path = self.write_source(root, source, 'scripts/check_native_c_api_surface.py')
                with warnings.catch_warnings():
                    warnings.simplefilter(action)
                    code, output = self.invoke(root)
                self.assertEqual(code, 1, output)
                self.assertEqual(
                    output,
                    f'{path}:5: duplicate top-level test function test_gate '
                    '(first defined at line 3)\n',
                )

    def test_conditional_method_replaces_failing_test(self) -> None:
        source = """import unittest
class Suite(unittest.TestCase):
    def test_gate(self):
        self.fail("lost")
    if True:
        def test_gate(self):
            pass
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.prepare_root(root)
            path = self.write_source(root, source)
            code, output = self.invoke(root)
            self.assertEqual(code, 1, output)
            self.assertIn(
                f'{path}:6: duplicate method Suite.test_gate (first defined at line 3)\n',
                output,
            )

    def test_all_scope_preserving_blocks_reject_duplicate_methods_and_functions(self) -> None:
        for block in BLOCKS:
            for class_scope in (False, True):
                for asynchronous in (False, True):
                    with self.subTest(block=block, class_scope=class_scope, asynchronous=asynchronous):
                        definition = ('async ' if asynchronous else '') + 'def test_gate(self=None):\n    pass\n'
                        replacement = block.format(
                            body=textwrap.indent(definition.rstrip(), '    '),
                            nested_body=textwrap.indent(definition.rstrip(), '        '),
                        )
                        source = definition + replacement + '\n'
                        if class_scope:
                            source = 'class Suite:\n' + textwrap.indent(source, '    ')
                        with tempfile.TemporaryDirectory() as tmp:
                            root = Path(tmp)
                            self.prepare_root(root)
                            path = self.write_source(root, source)
                            code, output = self.invoke(root)
                            self.assertEqual(code, 1, output)
                            kind = 'method Suite.test_gate' if class_scope else 'top-level test function test_gate'
                            self.assertIn(f'duplicate {kind} ', output)
                            self.assertIn(str(path), output)

    def test_repeated_class_binding_cannot_erase_a_suite(self) -> None:
        first = 'class Suite:\n    def test_gate(self):\n        self.fail("lost")\n'
        replacements = (
            'class Suite:\n    def test_other(self):\n        pass\n',
            'class Suite:\n    pass\n',
            'if True:\n    class Suite:\n        pass\n',
        )
        for replacement in replacements:
            with self.subTest(replacement=replacement), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                self.prepare_root(root)
                path = self.write_source(root, first + replacement)
                code, output = self.invoke(root)
                self.assertEqual(code, 1, output)
                line = 5 if replacement.startswith('if') else 4
                self.assertIn(
                    f'{path}:{line}: duplicate test class Suite (first defined at line 1)\n',
                    output,
                )

    def test_repeated_class_bindings_in_every_scope_preserving_block_are_rejected(self) -> None:
        first = 'class Suite:\n    def test_gate(self):\n        pass\n'
        for block in BLOCKS:
            with self.subTest(block=block), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                self.prepare_root(root)
                replacement = 'class Suite:\n    pass'
                self.write_source(root, first + block.format(
                    body=textwrap.indent(replacement, '    '),
                    nested_body=textwrap.indent(replacement, '        '),
                ) + '\n')
                code, output = self.invoke(root)
                self.assertEqual(code, 1, output)
                self.assertIn('duplicate test class Suite (first defined at line 1)', output)

    def test_repeated_class_binding_cannot_erase_inherited_tests(self) -> None:
        source = """class Base:
    def test_gate(self):
        self.fail("lost")
class Suite(Base):
    pass
class Suite:
    pass
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.prepare_root(root)
            path = self.write_source(root, source)
            code, output = self.invoke(root)
            self.assertEqual(code, 1, output)
            self.assertIn(
                f'{path}:6: duplicate test class Suite (first defined at line 4)\n', output,
            )

    def test_nested_class_names_use_their_own_namespace(self) -> None:
        source = """class First:
    class Suite:
        def test_gate(self):
            pass
class Second:
    class Suite:
        def test_gate(self):
            pass
def helper():
    def test_gate():
        pass
    def test_gate():
        pass
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.prepare_root(root)
            self.write_source(root, source)
            code, output = self.invoke(root)
            self.assertEqual(code, 0, output)
            self.assertEqual(output, '')

    def test_every_registered_inventory_family_is_scanned(self) -> None:
        for relative in INVENTORY_PATHS:
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                self.prepare_root(root)
                self.write_source(root, CLEAN_DOCKER_SOURCE, 'scripts/test_control.py')
                path = self.write_source(root, DUP_SCRIPT_SOURCE, relative)
                code, output = self.invoke(root)
                self.assertEqual(code, 1, output)
                self.assertIn(f'{path}:7: duplicate method SampleTests.test_alpha ', output)

    def test_missing_repository_root_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'absent'
            code, output = self.invoke(root)
            self.assertEqual(code, 1, output)
            self.assertIn(f'missing repository root: {root}', output)

    def test_empty_repository_directory_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            code, output = self.invoke(Path(tmp))
            self.assertEqual(code, 1, output)
            self.assertIn('missing expected test directory:', output)

    def test_each_expected_directory_is_mandatory(self) -> None:
        for missing in EXPECTED_DIRECTORIES:
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                for directory in EXPECTED_DIRECTORIES:
                    if directory != missing and not directory.startswith(missing + '/'):
                        (root / directory).mkdir(parents=True, exist_ok=True)
                # Preserve a nonempty population while the required directory is absent.
                control = 'docker/control_test.py' if missing == 'scripts' else 'scripts/test_control.py'
                self.write_source(root, CLEAN_DOCKER_SOURCE, control)
                code, output = self.invoke(root)
                self.assertEqual(code, 1, output)
                self.assertIn(f'missing expected test directory: {root / missing}', output)

    def test_complete_directory_layout_without_files_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.prepare_root(root)
            code, output = self.invoke(root)
            self.assertEqual(code, 1, output)
            self.assertIn('no test files inspected', output)

    def test_files_without_test_names_are_refused(self) -> None:
        for source in ('', 'class Helper:\n    def helper(self):\n        pass\n'):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                self.prepare_root(root)
                path = self.write_source(root, source)
                code, output = self.invoke(root)
                self.assertEqual(code, 1, output)
                self.assertIn(f'{path}:1: no test names inspected', output)

    def test_explicit_empty_files_and_directories_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            leaf = root / 'leaf'
            leaf.mkdir()
            code, output = self.invoke(root, leaf)
            self.assertEqual(code, 1, output)
            self.assertIn('no test files inspected', output)
            path = self.write_source(root, '', 'leaf/test_empty.py')
            for explicit in (path, leaf):
                with self.subTest(explicit=explicit):
                    code, output = self.invoke(root, explicit)
                    self.assertEqual(code, 1, output)
                    self.assertIn(f'{path}:1: no test names inspected', output)

    def test_explicit_nonempty_files_and_leaf_directories_are_supported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = self.write_source(root, CLEAN_SCRIPT_SOURCE, 'leaf/test_clean.py')
            for explicit in (path, path.parent):
                with self.subTest(explicit=explicit):
                    code, output = self.invoke(root, explicit)
                    self.assertEqual(code, 0, output)
                    self.assertEqual(output, '')

    def test_population_count_is_across_files_not_per_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.prepare_root(root)
            self.write_source(root, '', 'scripts/test_empty.py')
            self.write_source(root, CLEAN_DOCKER_SOURCE, 'docker/control_test.py')
            code, output = self.invoke(root)
            self.assertEqual(code, 0, output)
            self.assertEqual(output, '')

    def test_repository_inventory_has_no_duplicate_bindings(self) -> None:
        root = Path(__file__).resolve().parent.parent
        code, output = self.invoke(root)
        self.assertEqual(code, 0, output)
        self.assertEqual(output, '')


if __name__ == '__main__':
    unittest.main()
