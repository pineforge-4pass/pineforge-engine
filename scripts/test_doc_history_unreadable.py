"""An unchanged page must still have a readable Git blob."""
import stat
import unittest

from test_check_doc_reverts import BASE_PAGE, repo


class UnreadablePage(unittest.TestCase):
    def test_code_only_walk_reads_the_unchanged_pages(self):
        with repo() as r:
            (r.root / 'README.md').write_text('# Readable index\n')
            base = r.commit(BASE_PAGE, 'docs: publish the guide')
            path = r.page.relative_to(r.root).as_posix()
            r.page = r.root / 'source.txt'
            head = r.commit('new source\n', 'edit source')
            self.assertEqual(r.git('diff', '--name-only', base, head), 'source.txt')
            blob = r.git('rev-parse', f'{base}:{path}')
            self.assertEqual(r.git('rev-parse', f'{head}:{path}'), blob)
            code, out = r.run('--base', base)
            self.assertEqual(code, 0, out)

            r.page = r.root / 'README.md'
            later = r.commit('# Index\n', 'docs: update the index')
            stored = r.root / '.git' / 'objects' / blob[:2] / blob[2:]
            stored.chmod(stored.stat().st_mode | stat.S_IWRITE)
            stored.unlink()
            self.assertEqual(r.git('ls-tree', '--name-only', head, '--', path), path)
            for tip in (head, later):
                with self.subTest(tip=tip):
                    r.git('update-ref', 'HEAD', tip)
                    code, out = r.run('--base', base)
                    self.assertEqual(code, 2, out)
                    self.assertIn(f'NOT COMPARED: cannot read {head[:7]}:{path}', out)
                    self.assertNotIn('... OK', out)


if __name__ == '__main__':
    unittest.main()
