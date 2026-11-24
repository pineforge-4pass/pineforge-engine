"""A renamed guide must account for the sentences it loses."""
import unittest

from test_check_doc_reverts import BASE_PAGE, KEEP_HANDLE, repo, with_keep_handle


class RenamedPage(unittest.TestCase):
    def test_rename_compares_the_source_with_the_destination(self):
        with repo() as r:
            r.git('config', 'diff.renames', 'true')
            base = r.commit(with_keep_handle(), 'docs: publish the guide')
            old = r.page.relative_to(r.root).as_posix()
            r.page = r.page.rename(r.page.with_name('renamed guide.md'))
            new = r.page.relative_to(r.root).as_posix()
            head = r.commit(BASE_PAGE, 'docs: move the guide')
            status = r.git('diff', '--name-status', '-z', base, head).split('\0')
            self.assertRegex(status[0], r'^R\d+$')
            self.assertEqual(status[1:], [old, new, ''])

            for args in ((), ('--head', head), ('--head', head, '--files', old),
                         ('--head', head, '--files', new),
                         ('--head', head, '--files', old, new)):
                with self.subTest(args=args):
                    code, out = r.run('--base', base, *args)
                    self.assertEqual(code, 1, out)
                    self.assertIn('unnamed deletion of ' + repr(KEEP_HANDLE), out)
                    self.assertIn('introduced by ' + base[:7], out)
                    self.assertIn('1 unnamed sentence deletions or reverts ... FAIL', out)

            r.git('commit', '--amend', '-q', '-m', 'docs: remove "keeps its handle keeps its lineage"')
            for args in ((), ('--head', 'HEAD')):
                with self.subTest(named=True, args=args):
                    code, out = r.run('--base', base, *args)
                    self.assertEqual(code, 0, out)
                    self.assertIn('0 unnamed sentence deletions or reverts ... OK', out)


if __name__ == '__main__':
    unittest.main()
