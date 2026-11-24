"""A Python registration owns its programs and its skip properties."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import check_kernel_python_private_reads as sweep


class PythonPopulation(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tests = self.root / 'tests'
        self.tests.mkdir()
        self.cmake = self.tests / 'CMakeLists.txt'
        self.absent = 'tests/' + 'fixtures/absent.csv'

    def check(self, registrations):
        self.cmake.write_text(registrations)
        out, err = io.StringIO(), io.StringIO()
        with patch.object(sweep, 'ROOT', self.root), \
                patch.object(sys, 'argv', ['check', '--cmake', str(self.cmake)]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = sweep.main()
        return code, out.getvalue() + err.getvalue()

    def test_every_collected_module_has_its_own_read_check(self):
        first = self.tests / 'test_a.py'
        first.write_text('import unittest\n'
                         'raise unittest.SkipTest("SKIP private fixtures not present: a")\n')
        second = self.tests / 'test_b.py'
        second.write_text(f'open("{self.absent}").read()\n')
        for command in ('-m unittest discover -s tests',
                        '-m pytest -q tests/test_a.py tests/test_b.py'):
            with self.subTest(command=command):
                code, out = self.check(
                    f'add_test(NAME readers COMMAND ${{Python3_EXECUTABLE}} {command})\n')
                self.assertEqual(code, 1, out)
                self.assertIn(f'readers: reads absent {self.absent} ({second}:1)', out)

    def test_unittest_discovery_enters_packages_with_the_default_pattern(self):
        (self.tests / 'test_a.py').write_text('pass\n')
        package = self.tests / 'nested'
        package.mkdir()
        (package / '__init__.py').write_text('')
        reader = package / 'testBars.py'
        reader.write_text(f'open("{self.absent}").read()\n')
        code, out = self.check(
            'add_test(NAME nested COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests)\n')
        self.assertEqual(code, 1, out)
        self.assertIn(f'nested: reads absent {self.absent} ({reader}:1)', out)

    def test_dependency_does_not_inherit_the_owners_skip(self):
        (self.tests / 'bars.py').write_text(
            'from pathlib import Path\n'
            f'bars = Path("{self.absent}")\n'
            'if not bars.is_file():\n'
            '    print("SKIP private fixtures not present: " + str(bars))\n'
            '    missing = 77\n'
            '    raise SystemExit(missing)\n'
            'print(bars.read_text())\n')
        rows = ('add_test(NAME reader COMMAND ${Python3_EXECUTABLE} tests/bars.py)\n'
                'add_test(NAME other COMMAND ${CMAKE_COMMAND} -E echo done)\n')
        code, out = self.check(rows +
            'set_tests_properties(other PROPERTIES DEPENDS reader SKIP_RETURN_CODE 77)\n')
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent}', out)
        code, out = self.check(rows +
            'set_tests_properties(other "reader" PROPERTIES SKIP_RETURN_CODE "77")\n')
        self.assertEqual(code, 0, out)

    def test_registry_without_python_is_not_compared(self):
        code, out = self.check('add_test(NAME native COMMAND native)\n')
        self.assertEqual(code, 2, out)
        self.assertIn('NOT COMPARED: no registered Python programs', out)
        self.assertNotIn('reading an absent private file: 0', out)


if __name__ == '__main__':
    unittest.main(verbosity=2)
