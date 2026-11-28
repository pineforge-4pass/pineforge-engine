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

# The two lines most invented modules start with, and the guard's two
# statements: the pinned literal, then exit code 77.
START = 'from pathlib import Path\nbars = Path("<ABSENT>")\n'
SKIP = ('    print("SKIP private fixtures not present: " + str(bars))\n'
        '    raise SystemExit(77)\n')


class ModuleSelfGate(unittest.TestCase):
    """Every module text below is invented and lives in a temporary tree. The
    real files read are the controls script, in the test that names its exempt
    entry, and the four settlement-family scripts, in the test that names the
    family."""

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

    def sweep_row(self, text, name='reader', runner='', beside=None):
        """Register one module as a Python row of a temporary tree and run the
        sweep on it: its exit code, its output and the module's path. `runner`
        stands before the module in the command (`-m pytest `), and `beside`
        maps the file names of modules written next to it to their texts."""
        module = self.tests / 'reader.py'
        module.write_text(text)
        for other, source in (beside or {}).items():
            (self.tests / other).write_text(source)
        self.cmake.write_text(
            f'add_test(NAME {name} COMMAND ${{Python3_EXECUTABLE}} {runner}tests/reader.py)\n')
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

    def test_a_guard_after_or_beside_the_read_of_the_bound_path_gates_nothing(self):
        forms = {
            'the guard after the read': (2, (
                'from pathlib import Path\n'
                'bars = Path("<ABSENT>")\n'
                'text = bars.read_text()\n'
                'if not bars.exists():\n'
                '    print("SKIP private fixtures not present: " + str(bars))\n'
                '    raise SystemExit(77)\n'
                'print(text)\n')),
            'the guard beside the read, in the other branch': (3, (
                'import sys\n'
                'from pathlib import Path\n'
                'bars = Path("<ABSENT>")\n'
                'if len(sys.argv) > 1:\n'
                '    print(bars.read_text())\n'
                'else:\n'
                '    if not bars.exists():\n'
                '        print("SKIP private fixtures not present: " + str(bars))\n'
                '        raise SystemExit(77)\n')),
            'the guard in main(), the read at module top level': (2, (
                'from pathlib import Path\n'
                'bars = Path("<ABSENT>")\n'
                '\n'
                '\n'
                'def main() -> int:\n'
                '    if not bars.exists():\n'
                '        print("SKIP private fixtures not present: " + str(bars))\n'
                '        return 77\n'
                '    return 0\n'
                '\n'
                '\n'
                'text = bars.read_text()\n'
                'raise SystemExit(main())\n')),
        }
        for form, (bound, text) in forms.items():
            with self.subTest(form=form):
                code, out, module = self.sweep_row(self.named(text))
                self.assertEqual(code, 1, out)
                self.assertIn(f'reader: reads absent {self.absent} ({module}:{bound})', out)
                self.assertIn('but it does not stand before the use of bars', out)

    def test_a_guard_that_tests_another_path_gates_nothing(self):
        text = self.named(
            'from pathlib import Path\n'
            'bars = Path("<ABSENT>")\n'
            'marker = Path("tests/marker.txt")\n'
            'if not marker.exists():\n'
            '    print("SKIP private fixtures not present: " + str(marker))\n'
            '    raise SystemExit(77)\n'
            'print(bars.read_text())\n')
        code, out, module = self.sweep_row(text)
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:2)', out)
        self.assertIn('but its test does not name bars', out)

        # The same statements with the test on the path that is read.
        probe = 'if not marker.exists():\n'
        self.assertEqual(text.count(probe), 1)
        code, out, _ = self.sweep_row(text.replace(probe, 'if not bars.exists():\n'))
        self.assertEqual(code, 0, out)
        self.assertIn(COUNT + '0', out.splitlines())

    def test_the_settlement_family_is_gated_by_the_probe_of_its_own_path(self):
        family = ('check_settlement_cpp_abi.py', 'prepare_settlement_cpp_abi_base.py',
                  'check_native_cpp_abi.py', 'check_pending_order_prefix.py')
        for script in family:
            with self.subTest(script=script):
                text = (REPO / 'scripts' / script).read_text()
                code, out, module = self.sweep_row(text)
                self.assertEqual(code, 0, out)
                self.assertIn(COUNT + '0', out.splitlines())
                with patch.object(sweep, 'ROOT', self.root):
                    rule, unguarded = sweep.admitted_by('reader', {}, {module: text})
                self.assertEqual((rule, unguarded), ('self-gating', []))

        # One of the four with its probe turned to another path: the guard
        # still stands where it stood, and it no longer answers for the path
        # the module reads.
        text = (REPO / 'scripts' / 'check_pending_order_prefix.py').read_text()
        probe = 'for path in (FIXTURE,) if'
        self.assertEqual(text.count(probe), 1)
        code, out, _ = self.sweep_row(text.replace(probe, 'for path in (ROOT,) if'))
        self.assertEqual(code, 1, out)
        self.assertIn('but its test does not name FIXTURE', out)

    def test_a_read_that_feeds_the_guards_test_is_still_a_read(self):
        forms = {
            'through the loop variable the test needs': ('path', 4, (
                START +
                'for path in (bars,):\n'
                '    data = path.read_text()\n'
                '    if not path.exists():\n'
                '        print("SKIP private fixtures not present: " + str(path))\n'
                '        raise SystemExit(77)\n')),
            'in an assignment the test uses': ('bars', 3, (
                START +
                'size = len(bars.read_text())\n'
                'if not bars.exists() or not size:\n'
                + SKIP)),
            'in a call on a name the test mentions': ('bars', 4, (
                START +
                'seen = []\n'
                'seen.append(bars.read_text())\n'
                'if not bars.exists() or not seen:\n'
                + SKIP)),
        }
        for form, (name, line, text) in forms.items():
            with self.subTest(form=form):
                code, out, module = self.sweep_row(self.named(text))
                self.assertEqual(code, 1, out)
                self.assertIn(f'reader: reads absent {self.absent} ({module}:2)', out)
                self.assertIn('but it does not stand before the use of '
                              f'{name} at line {line}', out)

    def test_the_guards_test_has_to_be_an_existence_test_of_the_bound_path(self):
        read = 'print(bars.read_text())\n'
        refused = {
            'the guard stands where the path exists': 'if bars.exists():\n',
            'the test only mentions the name': 'if len(str(bars)) > 200:\n',
            'the test asks about the directory around the path':
                'if not bars.parent.exists():\n',
        }
        for form, test in refused.items():
            with self.subTest(form=form):
                code, out, module = self.sweep_row(self.named(START + test + SKIP + read))
                self.assertEqual(code, 1, out)
                self.assertIn(f'reader: reads absent {self.absent} ({module}:2)', out)
                self.assertIn('but its test is not an existence test that sends an '
                              'absent bars to the guard', out)

        admitted = {
            'not P.exists()': START + 'if not bars.exists():\n' + SKIP + read,
            'not os.path.exists(P)':
                'import os\n' + START + 'if not os.path.exists(bars):\n' + SKIP + read,
            'P.is_file(), with the guard in the else-branch':
                START + 'if bars.is_file():\n    pass\nelse:\n' + SKIP + read,
        }
        for form, text in admitted.items():
            with self.subTest(form=form):
                code, out, _ = self.sweep_row(self.named(text))
                self.assertEqual(code, 0, out)
                self.assertIn(COUNT + '0', out.splitlines())

    def test_a_guard_above_the_binding_that_tests_another_marker_gates_nothing(self):
        code, out, module = self.sweep_row(self.named(
            'from pathlib import Path\n'
            'marker = Path("tests/marker.txt")\n'
            'if not marker.exists():\n'
            '    print("SKIP private fixtures not present: " + str(marker))\n'
            '    raise SystemExit(77)\n'
            'bars = Path("<ABSENT>")\n'
            'print(bars.read_text())\n'))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:6)', out)
        self.assertIn('raise SystemExit(77) at line 5 in the scope that binds the path '
                      'to a name, but its test does not name bars', out)

    def test_a_guards_exit_has_to_be_a_real_exit(self):
        probe = ('if not bars.exists():\n'
                 '    print("SKIP private fixtures not present: " + str(bars))\n')
        read = 'print(bars.read_text())\n'
        refused = {
            'exit is a function of the module': (
                'def exit(code):\n    pass\n\n\n' + START + probe + '    exit(77)\n' + read),
            'exit is a method of another object': (
                'class Runner:\n    def exit(self, code):\n        self.code = code\n\n\n'
                + START + 'runner = Runner()\n' + probe + '    runner.exit(77)\n' + read),
            'the name sys is another module': (
                'import os as sys\n' + START + probe + '    sys.exit(77)\n' + read),
        }
        for form, text in refused.items():
            with self.subTest(form=form):
                bound = text[:text.index('bars = ')].count('\n') + 1
                code, out, module = self.sweep_row(self.named(text))
                self.assertEqual(code, 1, out)
                self.assertIn(f'reader: reads absent {self.absent} ({module}:{bound})', out)
                self.assertIn('holds a reachable guard under an `if`', out)

        admitted = {
            'exit(77), a name the module does not bind': ('', 'exit(77)'),
            'quit(77), a name the module does not bind': ('', 'quit(77)'),
            'sys.exit(77)': ('import sys\n', 'sys.exit(77)'),
            'os._exit(77)': ('import os\n', 'os._exit(77)'),
            'sys imported under another name': ('import sys as system\n', 'system.exit(77)'),
        }
        for form, (imports, call) in admitted.items():
            with self.subTest(form=form):
                code, out, _ = self.sweep_row(self.named(
                    imports + START + probe + f'    {call}\n' + read))
                self.assertEqual(code, 0, out)
                self.assertIn(COUNT + '0', out.splitlines())

    def test_a_function_nothing_calls_and_a_main_block_gate_no_test(self):
        # The guard is in main(), which only the main block runs; the test
        # function reads the path, and nothing in the module calls it.
        text = self.named(
            START +
            '\n'
            '\n'
            'def test_bars():\n'
            '    assert bars.read_text()\n'
            '\n'
            '\n'
            'def main() -> int:\n'
            '    if not bars.exists():\n'
            '        print("SKIP private fixtures not present: " + str(bars))\n'
            '        return 77\n'
            '    return 0\n'
            '\n'
            '\n'
            'if __name__ == "__main__":\n'
            '    raise SystemExit(main())\n')
        for runner in ('-m pytest ', ''):
            with self.subTest(runner=runner):
                code, out, module = self.sweep_row(text, runner=runner)
                self.assertEqual(code, 1, out)
                self.assertIn(f'reader: reads absent {self.absent} ({module}:2)', out)
                self.assertIn('return 77 at line 12 in main(), which the module top level '
                              'runs, but it does not stand before the use of bars at line 6',
                              out)

        # The same module with the function called behind the guard: as a
        # program its read is guarded.
        ending = '    return 0\n'
        self.assertEqual(text.count(ending), 1)
        code, out, _ = self.sweep_row(text.replace(ending, '    test_bars()\n' + ending))
        self.assertEqual(code, 0, out)
        self.assertIn(COUNT + '0', out.splitlines())

        # A guard in the main block, above a test that reads the path
        # directly: under pytest the block never runs.
        block = 'if __name__ == "__main__":\n'
        text = self.named(
            'import os\n'
            'marker = os.environ.get("MARKER", "")\n'
            + block +
            '    if not os.path.exists(marker):\n'
            '        print("SKIP private fixtures not present: " + marker)\n'
            '        raise SystemExit(77)\n'
            '\n'
            '\n'
            'def test_bars():\n'
            '    assert open("<ABSENT>").read()\n')
        code, out, module = self.sweep_row(text, runner='-m pytest ')
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: reads absent {self.absent} ({module}:10)', out)

        # The same guard under a test that always holds gates the def below.
        self.assertEqual(text.count(block), 1)
        code, out, _ = self.sweep_row(text.replace(block, 'if True:\n'), runner='-m pytest ')
        self.assertEqual(code, 0, out)
        self.assertIn(COUNT + '0', out.splitlines())

    def test_a_skip_that_cannot_run_or_lives_in_another_module_admits_no_row(self):
        start = 'import sys\nimport unittest\nfrom pathlib import Path\n'
        skip = 'raise unittest.SkipTest("SKIP private fixtures not present: bars")\n'
        read = 'print(Path("<ABSENT>").read_text())\n'
        skips = ('import unittest\n'
                 '\n'
                 '\n'
                 'def absent(name):\n'
                 '    raise unittest.SkipTest("SKIP private fixtures not present: " + name)\n')
        # The form that writes a second module stands last: the file stays in
        # the temporary tree.
        forms = {
            'under a test that never holds': (6, start + 'if False:\n    ' + skip + read, None),
            'behind an exit that always runs': (4, start + read + 'sys.exit(0)\n' + skip, None),
            'in another module of the closure': (
                5, start + 'import skips\n' + read, {'skips.py': skips}),
        }
        for form, (line, text, beside) in forms.items():
            with self.subTest(form=form):
                code, out, module = self.sweep_row(self.named(text), beside=beside)
                self.assertEqual(code, 1, out)
                self.assertIn(f'reader: reads absent {self.absent} ({module}:{line})', out)
                self.assertIn(COUNT + '1', out.splitlines())

    def test_each_clause_of_what_feeds_the_test_names_the_path(self):
        read = 'print(bars.read_text())\n'
        forms = {
            'an assignment to a name the test needs': (
                START +
                'absent = [path for path in (bars,) if not path.exists()]\n'
                'if absent:\n'
                + SKIP + read),
            'the sequence of a for whose target the test needs': (
                START +
                'for path in (bars,):\n'
                '    if not path.exists():\n'
                '        print("SKIP private fixtures not present: " + str(path))\n'
                '        raise SystemExit(77)\n'
                '    print(path.read_text())\n'),
            'a call statement on a name the test needs': (
                'import argparse\n'
                + START +
                'parser = argparse.ArgumentParser()\n'
                'parser.add_argument("--bars", type=Path, default=bars)\n'
                'args = parser.parse_args()\n'
                'if not args.bars.exists():\n'
                '    print("SKIP private fixtures not present: " + str(args.bars))\n'
                '    raise SystemExit(77)\n'
                'print(args.bars.read_text())\n'),
        }
        for form, text in forms.items():
            with self.subTest(form=form):
                text = self.named(text)
                code, out, module = self.sweep_row(text)
                self.assertEqual(code, 0, out)
                self.assertIn(COUNT + '0', out.splitlines())
                with patch.object(sweep, 'ROOT', self.root):
                    rule, unguarded = sweep.admitted_by('reader', {}, {module: text})
                self.assertEqual((rule, unguarded), ('self-gating', []))

        # A call statement on the bound name itself feeds nothing: it is a
        # use of the path, and it stands before the guard.
        code, out, _ = self.sweep_row(self.named(
            START + 'bars.read_text()\nif not bars.exists():\n' + SKIP))
        self.assertEqual(code, 1, out)
        self.assertIn('but it does not stand before the use of bars at line 3', out)

    def test_a_use_in_a_default_a_method_or_a_class_body(self):
        probe = 'if not bars.exists():\n' + SKIP
        load = '\n\ndef load(path=bars):\n    return path.read_text()\n\n\n'
        tests = ('\n\nclass Bars(unittest.TestCase):\n'
                 '    def test_bars(self):\n'
                 '        self.assertTrue(bars.read_text())\n\n\n')
        admitted = {
            'a default that passes the name on, the function called behind the guard':
                START + load + probe + 'print(load())\n',
            'a method of a class defined behind the guard':
                'import unittest\n' + START + probe + tests,
        }
        for form, text in admitted.items():
            with self.subTest(form=form):
                code, out, _ = self.sweep_row(self.named(text))
                self.assertEqual(code, 0, out)
                self.assertIn(COUNT + '0', out.splitlines())

        refused = {
            'a default that passes the name on, the function called before the guard':
                (5, START + load + 'print(load())\n' + probe),
            'a default that opens the path where the def stands':
                (5, START + '\n\ndef load(text=bars.read_text()):\n    return text\n\n\n'
                 + probe + 'print(load())\n'),
            'a method of a class defined before the guard':
                (8, 'import unittest\n' + START + tests + probe),
            'a class body before the guard':
                (6, START + '\n\nclass Bars:\n    text = bars.read_text()\n\n\n' + probe),
        }
        for form, (line, text) in refused.items():
            with self.subTest(form=form):
                code, out, _ = self.sweep_row(self.named(text))
                self.assertEqual(code, 1, out)
                self.assertIn(f'but it does not stand before the use of bars at line {line}',
                              out)

        # A path bound in a class body: its uses are attributes of the class
        # and are not followed, whatever the class body tests.
        code, out, module = self.sweep_row(self.named(
            'from pathlib import Path\n'
            '\n'
            '\n'
            'class Bars:\n'
            '    path = Path("<ABSENT>")\n'
            '    if not path.exists():\n'
            '        print("SKIP private fixtures not present: " + str(path))\n'
            '        raise SystemExit(77)\n'
            '    text = path.read_text()\n'))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: {module}:5 is not gated by its own module: the path is '
                      'bound in a class body', out)

    def test_a_guard_that_exits_77_needs_the_pinned_literal(self):
        read = 'print(bars.read_text())\n'
        code, out, module = self.sweep_row(self.named(
            START + 'if not bars.exists():\n    raise SystemExit(77)\n' + read))
        self.assertEqual(code, 1, out)
        self.assertIn(f'reader: {module}:2 is not gated by its own module: '
                      'raise SystemExit(77) at line 4 in the scope that binds the path to '
                      'a name, but no string constant of the module holds '
                      f'{sweep.SKIP_LITERAL!r}', out)

        # unittest's own skip carries its reason and needs no literal.
        text = self.named(
            'import unittest\n' + START +
            'if not bars.exists():\n'
            '    raise unittest.SkipTest("bars are absent")\n' + read)
        code, out, module = self.sweep_row(text)
        self.assertEqual(code, 0, out)
        with patch.object(sweep, 'ROOT', self.root):
            rule, unguarded = sweep.admitted_by('reader', {}, {module: text})
        self.assertEqual((rule, unguarded), ('self-gating', []))


if __name__ == '__main__':
    unittest.main(verbosity=2)
