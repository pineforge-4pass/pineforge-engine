"""A module gates its own private reads by a guard that runs, not by text."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts'))
import check_kernel_python_private_reads as sweep

COUNT = 'kernel python rows reading an absent private file: '

# The settlement-ABI gate form: the path is bound to a module constant, a
# helper lists what the tree lacks, main() prints the pinned literal and
# returns 77 before the work, and the module exits with main()'s code.
SETTLEMENT_FORM = (
    'from pathlib import Path\n'
    '\n'
    'ROOT = Path(__file__).resolve().parents[1]\n'
    'MANIFEST = ROOT / "<ABSENT>"\n'
    'PRIVATE_FIXTURE_SKIP_LITERAL = "SKIP private fixtures not present: "\n'
    '\n'
    '\n'
    'def missing_private_fixtures() -> list:\n'
    '    return [path for path in (MANIFEST,) if not path.exists()]\n'
    '\n'
    '\n'
    'def verify() -> str:\n'
    '    return MANIFEST.read_text()\n'
    '\n'
    '\n'
    'def main() -> int:\n'
    '    try:\n'
    '        absent = missing_private_fixtures()\n'
    '        if absent:\n'
    '            print(PRIVATE_FIXTURE_SKIP_LITERAL + str(absent[0].relative_to(ROOT)))\n'
    '            return 77\n'
    '        verify()\n'
    '    except OSError as error:\n'
    '        raise SystemExit(str(error))\n'
    '    return 0\n'
    '\n'
    '\n'
    'if __name__ == "__main__":\n'
    '    raise SystemExit(main())\n')


class ModuleSelfGate(unittest.TestCase):
    """Every module text below is invented and lives in a temporary tree; the
    one real file read is the script whose admission the fourth test names."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tests = self.root / 'tests'
        self.tests.mkdir()
        self.cmake = self.tests / 'CMakeLists.txt'
        self.absent = 'tests/' + 'fixtures/absent/manifest.json'

    def named(self, text):
        """An invented module text with the absent private path filled in."""
        return text.replace('<ABSENT>', self.absent)

    def sweep_row(self, text, name='reader'):
        """Register one module as a Python row of a temporary tree and run the
        sweep on it: its exit code, its output and the module's path."""
        module = self.tests / 'reader.py'
        module.write_text(text)
        self.cmake.write_text(
            f'add_test(NAME {name} COMMAND ${{Python3_EXECUTABLE}} tests/reader.py)\n')
        out, err = io.StringIO(), io.StringIO()
        with patch.object(sweep, 'ROOT', self.root), \
                patch.object(sys, 'argv', ['check', '--cmake', str(self.cmake)]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = sweep.main()
        return code, out.getvalue() + err.getvalue(), module

    def test_a_guard_spelled_only_in_a_string_constant_gates_nothing(self):
        code, out, module = self.sweep_row(self.named(
            'from pathlib import Path\n'
            'GATE_AS_DATA = """\n'
            'if not bars.exists():\n'
            '    print("SKIP private fixtures not present: " + str(bars))\n'
            '    raise SystemExit(77)\n'
            '"""\n'
            'bars = Path("<ABSENT>")\n'
            'print(bars.read_text())\n'))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:7)', out)
        self.assertIn(COUNT + '1', out.splitlines())

    def test_a_guard_spelled_only_in_a_comment_gates_nothing(self):
        code, out, module = self.sweep_row(self.named(
            'from pathlib import Path\n'
            '# The gate this module once carried:\n'
            '#     print("SKIP private fixtures not present: " + str(bars))\n'
            '#     raise SystemExit(77)\n'
            'bars = Path("<ABSENT>")\n'
            'print(bars.read_text())\n'))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:5)', out)
        self.assertIn(COUNT + '1', out.splitlines())

    def test_a_unittest_skip_spelled_only_in_a_string_constant_admits_no_row(self):
        code, out, module = self.sweep_row(self.named(
            'from pathlib import Path\n'
            'SKIP_AS_DATA = """\n'
            'if not bars.exists():\n'
            '    raise unittest.SkipTest("SKIP private fixtures not present: " + str(bars))\n'
            '"""\n'
            'print(Path("<ABSENT>").read_text())\n'))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:6)', out)
        self.assertIn(COUNT + '1', out.splitlines())

        # The same skip as a statement that runs, in setUp. It is no gate of
        # the module's own (another function reads the path), and the row's
        # skip path is what admits it.
        skip = '            raise unittest.SkipTest("SKIP private fixtures not present: bars")\n'
        text = self.named(
            'import unittest\n'
            'from pathlib import Path\n'
            '\n'
            '\n'
            'class Bars(unittest.TestCase):\n'
            '    def setUp(self):\n'
            '        if not Path("tests").is_dir():\n'
            + skip +
            '\n'
            '    def test_bars(self):\n'
            '        self.assertTrue(Path("<ABSENT>").read_text())\n')
        code, out, module = self.sweep_row(text)
        self.assertEqual(code, 0, out)
        self.assertIn(COUNT + '0', out.splitlines())
        with patch.object(sweep, 'ROOT', self.root):
            rule, _ = sweep.admitted_by('reader', {}, {module: text})
        self.assertEqual(rule, 'skip path')

        # The statement still raises, and the pinned literal is now only in a
        # comment: no string constant holds it, so the row has no skip path.
        self.assertEqual(text.count(skip), 1)
        code, out, module = self.sweep_row(text.replace(
            skip, '            # SKIP private fixtures not present: bars\n'
                  '            raise unittest.SkipTest("bars are absent")\n'))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:12)', out)

    def test_the_settlement_gate_form_is_gated_by_its_return_statement(self):
        text = self.named(SETTLEMENT_FORM)
        code, out, _ = self.sweep_row(text)
        self.assertEqual(code, 0, out)
        self.assertIn(COUNT + '0', out.splitlines())

        # The same module with that one statement turned into a comment: all
        # the text of the gate is still in the file, and the gate is gone.
        gate = '            return 77\n'
        self.assertEqual(text.count(gate), 1)
        code, out, module = self.sweep_row(
            text.replace(gate, '            pass  # return 77\n'))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:4)', out)

        verdict = sweep.read_gate(text, 4)
        self.assertTrue(verdict.guarded, verdict.reason)
        returned = text.splitlines().index(gate.rstrip('\n')) + 1
        self.assertIn(f'return 77 at line {returned} in main()', verdict.reason)

    def test_the_reads_controls_script_is_admitted_by_its_exempt_entry(self):
        row = 'test_private_fixture_reads_controls'
        self.assertIn(row, sweep.EXEMPT)
        text = (REPO / 'scripts' / 'test_private_fixture_reads.py').read_text()

        # Under any other row name the same script is reported: no module
        # gate and no skip path admits it.
        other = 'reads_controls_under_another_name'
        code, out, _ = self.sweep_row(text, name=other)
        self.assertEqual(code, 1, out)
        self.assertIn(f'{other}: reads absent ', out)

        code, out, module = self.sweep_row(text, name=row)
        self.assertEqual(code, 0, out)
        self.assertIn(COUNT + '0', out.splitlines())
        with patch.object(sweep, 'ROOT', self.root):
            rule, unguarded = sweep.admitted_by(row, {}, {module: text})
        self.assertEqual(rule, 'exempt')
        self.assertTrue(unguarded, 'the script names no private path any more')

    def test_a_module_that_does_not_parse_is_reported_with_the_reason(self):
        code, out, module = self.sweep_row(self.named(
            'from pathlib import Path\n'
            'bars = Path("<ABSENT>")\n'
            'if not bars.exists():\n'
            '    print("SKIP private fixtures not present: " + str(bars))\n'
            '    raise SystemExit(77)\n'
            'print(bars.read_text()\n'))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:2)', out)
        self.assertIn(f'reader: {module}:2 is not gated by its own module: '
                      'the module does not parse', out)

    def test_a_guard_that_cannot_run_gates_nothing(self):
        forms = {
            'the body of a constant-false if': (
                'from pathlib import Path\n'
                'bars = Path("<ABSENT>")\n'
                'if False:\n'
                '    print("SKIP private fixtures not present: " + str(bars))\n'
                '    raise SystemExit(77)\n'
                'print(bars.read_text())\n'),
            'a function the module never calls': (
                'from pathlib import Path\n'
                'bars = Path("<ABSENT>")\n'
                '\n'
                '\n'
                'def unused() -> int:\n'
                '    print("SKIP private fixtures not present: " + str(bars))\n'
                '    return 77\n'
                '\n'
                '\n'
                'print(bars.read_text())\n'),
        }
        for form, text in forms.items():
            with self.subTest(form=form):
                code, out, module = self.sweep_row(self.named(text))
                self.assertEqual(code, 1, out)
                self.assertIn(f'reader: reads absent {self.absent} ({module}:2)', out)

    def test_statement_order_decides_for_a_direct_read(self):
        start = 'import os\nmarker = os.environ["MARKER"]\n'
        gate = ('if not os.path.exists(marker):\n'
                '    print("SKIP private fixtures not present: " + marker)\n'
                '    raise SystemExit(77)\n')
        read = 'print(open("<ABSENT>").read())\n'
        code, out, _ = self.sweep_row(self.named(start + gate + read))
        self.assertEqual(code, 0, out)
        self.assertIn(COUNT + '0', out.splitlines())
        code, out, module = self.sweep_row(self.named(start + read + gate))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:3)', out)


if __name__ == '__main__':
    unittest.main(verbosity=2)
