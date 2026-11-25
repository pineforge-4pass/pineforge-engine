"""A CTest listing can establish input readiness, not execution."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import check_abi_receipt_skips as checker
from test_abi_receipt_skips import receipt_command


class ReceiptRegistry(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name)
        (self.build / 'CMakeCache.txt').write_text('CMAKE_PROJECT_NAME:STATIC=example\n')
        self.receipt = self.build / 'receipt.json'
        self.receipt.write_text('{}')

    def inspect(self, row):
        listing = json.dumps({'tests': [{'name': 'plain', 'command': [sys.executable]}, row]})
        out, err = io.StringIO(), io.StringIO()
        with patch.object(subprocess, 'run', return_value=subprocess.CompletedProcess(
                ['ctest'], 0, listing, '')), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = checker.main(['--build-dir', str(self.build)])
        return code, out.getvalue() + err.getvalue()

    def test_an_unavailable_command_is_not_a_plain_row(self):
        for fields in ({}, {'command': []}, {'command': 'python'}, {'command': ['', 'check']},
                       {'command': ['NOT_AVAILABLE']}, {'command': [str(self.build / 'unbuilt')]}):
            with self.subTest(fields=fields):
                code, out = self.inspect({'name': 'broken', **fields})
                self.assertEqual(code, 2, out)
                self.assertIn('NOT COMPARED', out)
                self.assertIn('broken', out)
                self.assertIn('command', out)

    def test_receipt_mode_requires_an_input_and_complete_path_arguments(self):
        command = receipt_command(self.build, 'check_script_cpp_abi.py', [], '--require-receipts')
        for flags, reason in (
                ([], '--v18-frozen-receipt'),
                (['--base-receipt'], '--base-receipt'),
                (['--base-receipt', '--receipt', str(self.receipt)], '--base-receipt'),
                (['--base-receipt', ''], '--base-receipt'),
                (['--base-receipt', str(self.receipt), '--compile-commands'], '--compile-commands'),
                (['--base-receipt', str(self.receipt), '--skip-if-receipt-missing'], 'mode')):
            with self.subTest(flags=flags):
                code, out = self.inspect({'name': 'gated', 'command': command + flags})
                self.assertEqual(code, 2, out)
                self.assertIn('NOT COMPARED', out)
                self.assertIn('gated', out)
                self.assertIn(reason, out)

    def test_a_directory_cannot_supply_an_input_file(self):
        command = receipt_command(self.build, 'check_script_cpp_abi.py',
                                  ['--v18-frozen-receipt', str(self.build)])
        code, out = self.inspect({'name': 'budget', 'command': command})
        self.assertEqual(code, 1, out)
        self.assertIn('budget', out)
        self.assertIn('receipt missing', out)

    def test_relative_inputs_belong_to_the_registered_working_directory(self):
        command = receipt_command(self.build, 'check_script_cpp_abi.py',
                                  ['--v18-frozen-receipt', self.receipt.name])
        code, out = self.inspect({
            'name': 'relative', 'command': command,
            'properties': [{'name': 'WORKING_DIRECTORY', 'value': str(self.build)}]})
        self.assertEqual(code, 0, out)
        self.assertRegex(out, r'inputs present +relative')

    def test_present_inputs_do_not_prove_the_command_executed(self):
        # The empty include directory has no source-host headers for this program.
        command = receipt_command(self.build, 'check_script_cpp_abi.py',
                                  ['--v18-frozen-receipt', str(self.receipt)])
        code, out = self.inspect({'name': 'would_fail', 'command': command})
        self.assertEqual(code, 0, out)
        self.assertRegex(out, r'inputs present +would_fail')
        self.assertIn('execution: NOT COMPARED', out)
        self.assertNotIn('all 1 run', out)


if __name__ == '__main__':
    unittest.main(verbosity=2)
