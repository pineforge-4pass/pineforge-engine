#!/usr/bin/env python3
"""Unit tests for scripts/check_duplicate_test_names.py."""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

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


class CheckDuplicateTestNamesTests(unittest.TestCase):
    def invoke(self, root: Path) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = guard.main(['--root', str(root)])
        return code, out.getvalue()

    def test_duplicate_test_names_exits_one_and_reports_file_and_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scripts_dir = root / 'scripts'
            docker_dir = root / 'docker'
            scripts_dir.mkdir()
            docker_dir.mkdir()

            dup_script = scripts_dir / 'test_sample.py'
            dup_script.write_text(DUP_SCRIPT_SOURCE, encoding='utf-8')

            dup_docker = docker_dir / 'sample_test.py'
            dup_docker.write_text(DUP_DOCKER_SOURCE, encoding='utf-8')

            code, output = self.invoke(root)
            self.assertEqual(code, 1, output)
            self.assertIn(f'{dup_script}:7', output)
            self.assertIn(f'{dup_docker}:4', output)

    def test_clean_test_files_exit_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scripts_dir = root / 'scripts'
            docker_dir = root / 'docker'
            scripts_dir.mkdir()
            docker_dir.mkdir()

            (scripts_dir / 'test_clean.py').write_text(CLEAN_SCRIPT_SOURCE, encoding='utf-8')
            (docker_dir / 'clean_test.py').write_text(CLEAN_DOCKER_SOURCE, encoding='utf-8')

            code, output = self.invoke(root)
            self.assertEqual(code, 0, output)
            self.assertEqual(output, '')


if __name__ == '__main__':
    unittest.main()
