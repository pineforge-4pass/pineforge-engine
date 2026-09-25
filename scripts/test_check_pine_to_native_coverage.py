#!/usr/bin/env python3
"""Must-fail self-test for the migration page's Pine and design inventories."""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

import check_pine_to_native_coverage as guard


class CoverageGuard(unittest.TestCase):
    def make_root(self) -> Path:
        raw = Path(tempfile.mkdtemp())
        (raw / 'docs').mkdir()
        (raw / 'docs/pages').mkdir()
        (raw / 'docs/design').mkdir()
        (raw / 'docs/pine_v6_coverage_detail.md').write_text('# inventory\n')
        (raw / 'docs/design/native-feature-parity.md').write_text(
            '## 1. Inventory\n\n| ID | Feature | Native today |\n|---|---|---|\n'
            '| OL1 | one | yes |\n| OL2 | two | no |\n\n'
            '## 2. Structural work\n')
        return raw

    def page(self, root: Path, ids: str) -> None:
        (root / 'docs/pages/pine-to-native.md').write_text(
            '| group | offered | covered | of which native | of which ruled *none* |\n'
            '| --- | ---: | ---: | ---: | ---: |\n'
            '| **total** | **0** | **0** | **0** | **0** |\n\n'
            '## Design inventory crosswalk\n\n'
            '| ID | disposition |\n|---|---|\n' + ids)

    def invoke(self, root: Path) -> tuple[int, str]:
        out = io.StringIO()
        old = guard.DECLARATION_PARAMETERS
        guard.DECLARATION_PARAMETERS = ()
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                code = guard.main(['--root', str(root)])
        finally:
            guard.DECLARATION_PARAMETERS = old
        return code, out.getvalue()

    def test_missing_design_id_fails_then_passes(self) -> None:
        root = self.make_root()
        self.page(root, '| OL1 | mapped above |\n')
        code, out = self.invoke(root)
        self.assertEqual(code, 1, out)
        self.assertIn('OL2', out)
        self.page(root, '| OL1 | mapped above |\n| OL2 | no native counterpart |\n')
        code, out = self.invoke(root)
        self.assertEqual(code, 0, out)
        self.assertIn('inventory_ids=2 inventory_rows=2', out)


if __name__ == '__main__':
    unittest.main()
