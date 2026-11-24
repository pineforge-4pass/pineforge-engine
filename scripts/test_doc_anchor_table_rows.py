"""A continuation may inherit a citation within its own Markdown table row."""
import unittest

from test_check_doc_anchors import tree


class TableRows(unittest.TestCase):
    def test_a_continuation_needs_a_file_in_its_own_row(self):
        rows = ('Declaration | Member', '--- | ---',
                '`NativeRunSpec` native_host.hpp:5 | `initial_capital` :6',
                'Limits | `max_open_lots` :7')
        for outer_pipe in ('|', ''):
            body = ''.join(f'{outer_pipe} {row} {outer_pipe}\n' for row in rows)
            with self.subTest(outer_pipe=outer_pipe), tree(body) as t:
                code, out = t.run('--list')
                self.assertEqual(code, 1, out)
                self.assertIn('docs/pages/native-engine.md:4  :7  [ORPHAN]', out)
                self.assertRegex(out, r':3  OK +native_host\.hpp:6 +`initial_capital`')
                t.page.write_text(body.replace(':7', 'native_host.hpp:7'))
                code, out = t.run()
                self.assertEqual(code, 0, out)


if __name__ == '__main__':
    unittest.main()
