"""A preflight skip must identify the guard's missing input on one output line."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

from ci_preflight import run_checks


class PrivateSkip(unittest.TestCase):
    def test_only_the_guard_can_report_one_of_its_missing_inputs(self):
        guard = 'source-guard-native-versions'
        prefix = 'SKIP private fixtures not present: '
        reason = 'SKIP private fixtures not present: tests/fixtures/native_cpp_abi/host-f736676/manifest.json'
        cases = (
            ('doc-anchors', reason, '', 'failed'),
            ('source-guard-broker-hash', reason, '', 'failed'),
            (guard, 'diagnostic: ' + reason, '', 'failed'),
            (guard, reason + ' extra', '', 'failed'),
            (guard, prefix, '', 'failed'),
            (guard, 'SKIP private fixtures not present: tests/fixtures/unrelated.txt', '', 'failed'),
            (guard, reason[:10], reason[10:], 'failed'),
            (guard, 'checking\n', reason + '\n', 'skipped'),
        )
        for name, stdout, stderr, expected in cases:
            with self.subTest(name=name, stdout=stdout, stderr=stderr), \
                    tempfile.TemporaryDirectory() as temp, \
                    mock.patch.dict(os.environ, {'PINEFORGE_PRIVATE_FIXTURES': ''}):
                root = Path(temp)
                argv = [sys.executable, '-c', 'import sys; sys.stdout.write(sys.argv[1]); '
                        'sys.stderr.write(sys.argv[2]); sys.exit(77)', stdout, stderr]
                console = io.StringIO()
                with contextlib.redirect_stdout(console):
                    code = run_checks([(name, argv)], root / 'logs', source=root)
                summary = json.loads((root / 'logs/preflight-summary.json').read_text())
                self.assertEqual(code, 1 if expected == 'failed' else 0, console.getvalue())
                self.assertEqual([(s['name'], s['status'], s['exitCode']) for s in summary['stages']],
                                 [(name, expected, 77)])
                if expected == 'failed':
                    self.assertIn(f'ci_preflight: FAIL {name}: exit 77;', console.getvalue())
                else:
                    self.assertIn(f'ci_preflight: {name}: skipped (exit 77', console.getvalue())


if __name__ == '__main__':
    unittest.main()
