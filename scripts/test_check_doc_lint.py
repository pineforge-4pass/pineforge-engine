#!/usr/bin/env python3
"""Self-tests for the documentation prose lint: it must be able to FAIL.

Each case builds a throwaway tree - a header that declares the live epochs, a
page that offends (or deliberately does not) - and drives the real
``check_doc_lint.main`` over it with ``--root``.

The must-fail half is one case per rule: a roadmap label on a published page, a
``there is no … in this slice`` claim (including the wrapped spelling the real
pages use, which a line-by-line reading would miss), a stale epoch, a stale
hash domain, a link to a missing file and a link to a missing ``#anchor``.

The must-not-fire half is what keeps the guard usable: a *live* epoch of a
family whose other members moved on, a ``lane L…`` mention in a design
document that is a roadmap on purpose, a link to a directory, a fenced code
block, and any line carrying the ``<!-- verified HEAD -->`` marker.
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

import check_doc_lint as guard

HEADER = """#pragma once
namespace pineforge {
inline namespace engine_script_run_v18 {
}
inline namespace native_order_v6 {
}
inline namespace native_order_v1 {
}
inline namespace native_run_spec_v3 {
}
}
"""

SOURCE = """#include <pineforge/engine.hpp>
const char* domain() { return "pineforge-broker-state/v18"; }
"""


class Tree:
    """A throwaway repository: one header, one source, and pages under docs/."""

    def __init__(self, **files: str) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        (self.root / 'include/pineforge').mkdir(parents=True)
        (self.root / 'src').mkdir(parents=True)
        (self.root / 'docs/pages').mkdir(parents=True)
        (self.root / 'docs/design').mkdir(parents=True)
        (self.root / 'include/pineforge/engine.hpp').write_text(HEADER)
        (self.root / 'src/engine_state_hash.cpp').write_text(SOURCE)
        for name, body in files.items():
            path = self.root / name.replace('__', '/')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body)

    def run(self) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = guard.main(['--root', str(self.root)])
        return code, out.getvalue()

    def close(self) -> None:
        self.dir.cleanup()


@contextlib.contextmanager
def tree(**files: str):
    made = Tree(**files)
    try:
        yield made
    finally:
        made.close()


PAGE = 'docs__pages__native-engine.md'
DESIGN = 'docs__design__native-feature-parity.md'


class MustFail(unittest.TestCase):
    def test_lane_label_on_a_published_page(self) -> None:
        with tree(**{PAGE: 'Percent-of-equity sizing is **lane L3**.\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('lane-label', out)

    def test_lane_label_in_the_readme(self) -> None:
        with tree(**{'README.md': 'Margin is lane L4.\n'}) as t:
            self.assertEqual(t.run()[0], 1)

    def test_stale_negative_claim(self) -> None:
        with tree(**{PAGE: 'There is no C request API in this slice.\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('stale-negative', out)

    def test_stale_negative_claim_wrapped_over_a_line(self) -> None:
        body = 'Serialized calls may command between inputs. There is no C\nrequest API in this\nslice.\n'
        with tree(**{PAGE: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('stale-negative', out)

    def test_stale_negative_claim_with_a_dotted_spelling(self) -> None:
        body = 'There is no native subscription API for `request.security` series in this slice.\n'
        with tree(**{PAGE: body}) as t:
            self.assertEqual(t.run()[0], 1)

    def test_stale_epoch(self) -> None:
        with tree(**{PAGE: 'At host epoch `engine_script_run_v17`.\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('stale-epoch', out)
            self.assertIn('engine_script_run_v18', out)   # what the tree declares

    def test_stale_hash_domain(self) -> None:
        with tree(**{PAGE: 'Folded under `pineforge-broker-state/v17`.\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('pineforge-broker-state/v18', out)

    def test_stale_epoch_in_a_design_document(self) -> None:
        # Rule 3 covers everything under docs/, not only the published pages.
        with tree(**{DESIGN: 'Budget: `native_run_spec_v2`.\n'}) as t:
            self.assertEqual(t.run()[0], 1)

    def test_link_to_a_missing_file(self) -> None:
        with tree(**{PAGE: 'See [the guide](missing-page.md).\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('dead-link', out)

    def test_link_to_a_missing_anchor(self) -> None:
        pages = {PAGE: '# Native engine\n\nSee [risk](pine-to-native.md#no_such_id).\n',
                 'docs__pages__pine-to-native.md': '## Risk limits {#pine_to_native_risk}\n'}
        with tree(**pages) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('no `#no_such_id`', out)


class MustNotFire(unittest.TestCase):
    def test_a_live_epoch_of_a_moved_family(self) -> None:
        # native_order.hpp is at v6, but native_order_v1 is the live identity
        # epoch. A hardcoded `native_order_v[1-5]` rule would flag a true line.
        with tree(**{PAGE: 'Identity types stay `native_order_v1`.\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_live_epoch(self) -> None:
        with tree(**{PAGE: 'Host epoch `engine_script_run_v18`.\n'}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_an_unrelated_versioned_word(self) -> None:
        with tree(**{PAGE: 'The fixture `probe_v2` is frozen.\n'}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_lane_label_in_a_design_document(self) -> None:
        # The design doc IS the roadmap; rule 1 is about the published pages.
        with tree(**{DESIGN: 'Deferred to lane L9.\n'}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_verified_marker_exempts_the_line(self) -> None:
        body = ('There is no C request API in this slice. <!-- verified HEAD -->\n\n'
                'Epoch `engine_script_run_v17` was retired. <!-- verified HEAD -->\n')
        with tree(**{PAGE: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_fenced_code_is_not_prose(self) -> None:
        body = '```\ninline namespace engine_script_run_v17 {}\nlane L3\n```\n'
        with tree(**{PAGE: body}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_link_to_a_directory(self) -> None:
        with tree(**{PAGE: 'See [the sources](../../src/).\n'}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_link_to_a_source_file_has_no_anchor_to_check(self) -> None:
        body = 'See [the header](../../include/pineforge/engine.hpp#L3).\n'
        with tree(**{PAGE: body}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_absolute_url_is_not_ours(self) -> None:
        with tree(**{PAGE: 'See [site](https://pineforge.dev/nope).\n'}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_explicit_id_and_heading_slug_both_resolve(self) -> None:
        pages = {PAGE: ('# Native engine\n\n'
                        '[a](pine-to-native.md#pine_to_native_risk) '
                        '[b](pine-to-native.md#order-lifecycle) '
                        '[c](#native-engine)\n'),
                 'docs__pages__pine-to-native.md': ('## Risk limits {#pine_to_native_risk}\n\n'
                                                   '## Order lifecycle\n')}
        with tree(**pages) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_clean_tree_says_so(self) -> None:
        with tree(**{PAGE: '# Native engine\n\nThe kernel runs.\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)
            self.assertIn('no lane labels, stale negatives, stale epochs or dead links', out)


class Derivation(unittest.TestCase):
    """The live epoch set comes from the tree, never from a hardcoded list."""

    def test_live_epochs_read_the_sources(self) -> None:
        with tree(**{PAGE: 'x\n'}) as t:
            namespaces, domains = guard.live_epochs(t.root)
        self.assertEqual(namespaces, {'engine_script_run_v18', 'native_order_v6',
                                      'native_order_v1', 'native_run_spec_v3'})
        self.assertEqual(domains, {'pineforge-broker-state/v18'})


if __name__ == '__main__':
    unittest.main()
