#!/usr/bin/env python3
"""Self-tests for the documentation prose lint: it must be able to FAIL.

Each case builds a throwaway tree - a header that declares the live epochs, a
page that offends (or deliberately does not) - and drives the real
``check_doc_lint.main`` over it with ``--root``.

The must-fail half is one case per rule: a roadmap label on a published page, a
``there is no … in this slice`` claim (including the wrapped spelling the real
pages use, which a line-by-line reading would miss), a stale epoch, a stale
hash domain, a link to a missing file and a link to a missing ``#anchor`` - and,
for the three rules lane F2 added, a cited test file that never existed, a
count the tree derives stated wrong (each of the three families), and a
``<!-- verified HEAD -->`` marker on a sentence that states a non-live epoch as
today's.

The must-not-fire half is what keeps the guard usable: a *live* epoch of a
family whose other members moved on, a ``lane L…`` mention in a design
document that is a roadmap on purpose, a link to a directory, a fenced code
block, and any line carrying the ``<!-- verified HEAD -->`` marker for what it
is allowed to exempt: history, a path that is gone on purpose and says so, a
count recorded as it stood.
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

# The count sources of rule 6, each small enough to count by eye: three
# native_c_api.h declarations, two pineforge.h ones, one runtime name, floors
# 202 / 572, and a manifest of three examples, two of them C.
NATIVE_C_API = """#pragma once
PF_API int strategy_native_a_v1(void);
PF_API int strategy_native_b_v1(void);
/* PF_API in a comment is not a declaration */
PF_API void strategy_configure_native_ext_v1(void);
"""
PINEFORGE_H = """#pragma once
PF_API int pf_abi_version(void);
PF_API void report_free(void* r);
"""
C_ABI_RUNTIME = """EXPECTED_RUNTIME = frozenset({
    "pf_abi_version",
})
"""
CI_VERIFY = """KERNEL_MIN_TESTS = 202
RELEASE_MIN_TESTS = 572
"""
INDEPENDENCE = """NATIVE_EXAMPLES = (
    ("hello-kernel", "examples/native/hello_kernel.cpp"),
    ("hello-kernel-c", "examples/native/hello_kernel_c.c"),
    ("price-grid-c", "examples/native/native_price_grid_c.c"),
)
"""
COUNT_SOURCES = {'include__pineforge__native_c_api.h': NATIVE_C_API,
                 'include__pineforge__pineforge.h': PINEFORGE_H,
                 'scripts__check_c_abi_runtime.py': C_ABI_RUNTIME,
                 'scripts__ci_verify.py': CI_VERIFY,
                 'scripts__check_native_include_independence.py': INDEPENDENCE}
LIFECYCLE = 'include__pineforge__exit_leg_lifecycle.hpp'


def counted(page_body: str, **extra: str) -> dict[str, str]:
    """The count sources plus one page."""
    return {**COUNT_SOURCES, PAGE: page_body, **extra}


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


class MustFailF2(unittest.TestCase):
    """Rules 5-7 (lane F2): each can fail on the defect the final audit found."""

    def test_a_cited_test_file_that_never_existed(self) -> None:
        body = '| `strategy.oca.reduce` | `GroupEffect::Reduce` | `tests/test_native_groups.cpp` |\n'
        with tree(**{PAGE: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('dead-path', out)
            self.assertIn('tests/test_native_groups.cpp', out)

    def test_a_cited_example_that_does_not_exist(self) -> None:
        with tree(**{PAGE: 'Built from `examples/native/gone.cpp`.\n'}) as t:
            self.assertEqual(t.run()[0], 1)

    def test_a_placeholder_path_that_matches_nothing(self) -> None:
        with tree(**{PAGE: 'The twin lives in `tests/test_<feature>_twin.cpp`.\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('no file matches the pattern', out)

    def test_a_retired_path_needs_the_marker_too(self) -> None:
        with tree(**{PAGE: 'The oracle `tests/oracle.hpp` was retired.\n'}) as t:
            self.assertEqual(t.run()[0], 1)

    def test_the_marker_cannot_keep_a_dead_witness(self) -> None:
        # A present-tense "Runs in" claim stays false whatever the marker says.
        body = '| `x` | `y` | `tests/test_gone.cpp` | <!-- verified HEAD -->\n'
        with tree(**{PAGE: body}) as t:
            self.assertEqual(t.run()[0], 1)

    def test_the_native_c_api_count_stated_as_a_further_set(self) -> None:
        with tree(**counted('It adds **34 further `PF_API` functions** for hosts.\n')) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('stale-count', out)
            self.assertIn('derives 3', out)

    def test_the_native_c_api_count_by_its_prefix(self) -> None:
        with tree(**counted('The\n32 `strategy_native_*` functions are declared there.\n')) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native-engine.md:2', out)   # the line that carries the number

    def test_the_native_c_api_count_in_a_header_list(self) -> None:
        body = '- `<pineforge/native_c_api.h>` (included by `pineforge.h`) — **32**: the hosts.\n'
        with tree(**counted(body)) as t:
            self.assertEqual(t.run()[0], 1)

    def test_the_additive_count_in_bold(self) -> None:
        with tree(**counted('the **33** additive symbols of the C header.\n')) as t:
            self.assertEqual(t.run()[0], 1)

    def test_the_total_across_two_headers(self) -> None:
        with tree(**counted('The public C surface is **97 `PF_API` declarations** across two headers.\n')) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('derives 5', out)

    def test_native_prefix_is_not_the_whole_header(self) -> None:
        with tree(**counted('The 2 `strategy_native_*` functions are declared there.\n')) as t:
            self.assertEqual(t.run()[0], 0)
        with tree(**counted('The 3 `strategy_native_*` functions are declared there.\n')) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('derives 2', out)

    def test_the_public_and_runtime_counts(self) -> None:
        body = 'It declares exactly 65 public `PF_API` functions: 57 runtime implementations.\n'
        with tree(**counted(body)) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertEqual(out.count('stale-count'), 2 + 1, out)   # 2 offenders + the by-rule line

    def test_the_kernel_floor_stated_in_rows(self) -> None:
        body = 'The row floor: `KERNEL_MIN_TESTS` in `scripts/ci_verify.py` (193 rows) is it.\n'
        with tree(**counted(body)) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('derives 202', out)

    def test_the_release_floor_as_an_assignment(self) -> None:
        with tree(**counted('Since then `RELEASE_MIN_TESTS = 571` holds.\n')) as t:
            self.assertEqual(t.run()[0], 1)

    def test_the_example_manifest_counts(self) -> None:
        body = ('`scripts/check_native_include_independence.py` compiles all fifteen sources\n'
                'against the installed headers, the C one with the C compiler.\n')
        with tree(**counted(body)) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('states 15', out)
            self.assertIn('states 1;', out)                 # "the C one", two are C

    def test_the_example_split(self) -> None:
        body = 'Three hosts ship under `examples/native/` — two C++ and one C.\n'
        with tree(**counted(body)) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertEqual(out.count('[stale-count]'), 2, out)   # C++ 1 and C 2

    def test_a_count_whose_source_cannot_be_read(self) -> None:
        with tree(**{PAGE: 'It adds **3 further `PF_API` functions**.\n'}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('cannot be read', out)

    def test_a_marker_on_an_epoch_stated_as_now(self) -> None:
        body = 'The layout moved. Its inline namespace is now `engine_script_run_v4`; <!-- verified HEAD -->\n'
        with tree(**{PAGE: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('verified-stale-epoch', out)
            self.assertIn('present tense', out)

    def test_a_marker_on_a_domain_stated_as_used(self) -> None:
        body = 'Broker hashes use `pineforge-broker-state/v4`; streams begin. <!-- verified HEAD -->\n'
        with tree(**{PAGE: body}) as t:
            self.assertEqual(t.run()[0], 1)

    def test_a_marker_on_a_sentence_that_wraps(self) -> None:
        body = ('The current baseline uses `native_order_v5`, engine/host <!-- verified HEAD -->\n'
                '`engine_script_run_v17` and nothing else. <!-- verified HEAD -->\n')
        with tree(**{PAGE: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertEqual(out.count('[verified-stale-epoch]'), 2, out)

    def test_a_marker_on_a_stale_literal_cited_in_the_tree(self) -> None:
        body = ('| L12 | hash-domain plan (`"pineforge-broker-state/v17"` engine_state_hash.cpp:23, '
                'pinned) | x | <!-- verified HEAD -->\n')
        with tree(**{DESIGN: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn("file:line of today's tree", out)


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

    def test_an_operator_signature_in_backticks_is_not_a_link(self) -> None:
        # `Series<T>::operator[](k)` reads as [](k) to a naive link scan.
        body = '| `[]` | op | `Series<T>::operator[](k)` |\n'
        with tree(**{PAGE: body}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_a_real_link_whose_label_is_code_is_still_read(self) -> None:
        body = 'See [`docs/gone.md`](../gone.md).\n'
        with tree(**{PAGE: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 1)
            self.assertIn('dead-link', out)

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
            # expectation corrected: "no lane labels, stale negatives, stale epochs or dead
            # links" -> the line below, because lane F2 added three rules to the summary.
            self.assertIn('no lane labels, stale negatives, stale epochs, dead links, dead '
                          'paths, stale counts or markers on a stale present', out)

    def test_cited_paths_that_exist(self) -> None:
        body = ('Pinned by `tests/test_native_order_core.cpp`; the hosts live in '
                '`examples/native/` and `tests/test_<feature>_twin.cpp` has a match; '
                '`./build/examples/native/hello_kernel` is a build output and '
                '`runner/examples/strategy.cpp` is not rooted in `examples/`.\n')
        files = {PAGE: body,
                 'tests__test_native_order_core.cpp': '',
                 'tests__test_margin_twin.cpp': '',
                 'examples__native__hello_kernel.cpp': ''}
        with tree(**files) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_path_gone_on_purpose_and_said_so(self) -> None:
        body = ('The `tests/oracle.hpp` that kept the body was retired too. '
                '<!-- verified HEAD -->\n')
        with tree(**{PAGE: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_paths_in_fenced_code_are_not_citations(self) -> None:
        with tree(**{PAGE: '```\nctest -R tests/nothing_here\n```\n'}) as t:
            self.assertEqual(t.run()[0], 0)

    def test_counts_that_agree(self) -> None:
        body = ('It adds **3 further `PF_API` functions**. The public C surface is **5 `PF_API` '
                'declarations** across two headers: `<pineforge/pineforge.h>` — **2**, and '
                '`<pineforge/native_c_api.h>` (included by `pineforge.h`) — **3**. It declares '
                'exactly two public `PF_API` functions: one runtime implementation. '
                '`KERNEL_MIN_TESTS` (202 rows) and `RELEASE_MIN_TESTS = 572`. '
                'The checker compiles all three sources under `examples/native/`, the two C ones '
                'with the C compiler: one C++ and two C.\n')
        with tree(**counted(body, **{'examples__native__hello_kernel.cpp': ''})) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_subset_of_the_examples_is_not_the_set(self) -> None:
        body = ('P6 shipped with two native examples added. Thirteen more hosts under '
                '`examples/native/` cover the rest.\n')
        with tree(**counted(body, **{'examples__native__x.cpp': ''})) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_count_recorded_as_history(self) -> None:
        body = 'Wave D shipped 33 additive `PF_API` symbols. <!-- verified HEAD -->\n'
        with tree(**counted(body)) as t:
            self.assertEqual(t.run()[0], 0)

    def test_markers_on_epoch_history(self) -> None:
        body = ('PR #246\'s historical baseline used `native_order_v2` for values. <!-- verified HEAD -->\n\n'
                '| `host-ab9714b` immutable provider | `engine_script_run_v16` | <!-- verified HEAD -->\n\n'
                'They stood at `native_order_v5` when this plan was written. <!-- verified HEAD -->\n\n'
                '| the request-epoch sentence | `native_order_v4` | `native_order_v6` engine.hpp:3 | <!-- verified HEAD -->\n\n'
                'R4-D L1 advanced the consumer to `engine_script_run_v17`; R5 L6 advances it. <!-- verified HEAD -->\n')
        with tree(**{PAGE: body}) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_marker_on_a_forward_epoch(self) -> None:
        # lifecycle_v2 is above the live lifecycle_v1: a planned removal, not a stale claim.
        body = ('Both old spellings are DEPRECATED: the fields are removed at the next ABI '
                'version, the enumerators at `lifecycle_v2`. <!-- verified HEAD -->\n')
        files = {PAGE: body, LIFECYCLE: 'inline namespace lifecycle_v1 {\n}\n'}
        with tree(**files) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)


class Derivation(unittest.TestCase):
    """The live epoch set and the stated counts come from the tree, never from a list."""

    def test_counts_read_their_sources(self) -> None:
        with tree(**counted('x\n')) as t:
            counts = guard.derived_counts(t.root)
        self.assertEqual(counts, {'native-c-api': 3, 'native-prefix': 2,
                                  'pineforge-h': 2, 'runtime': 1,
                                  'pf-api-total': 5, 'kernel-floor': 202,
                                  'release-floor': 572, 'examples': 3, 'examples-cpp': 1,
                                  'examples-c': 2})

    def test_the_repository_counts(self) -> None:
        # The live tree: whatever the numbers are, every family must be derivable.
        counts = guard.derived_counts(guard.ROOT)
        self.assertTrue(all(isinstance(v, int) and v > 0 for v in counts.values()), counts)

    def test_live_epochs_read_the_sources(self) -> None:
        with tree(**{PAGE: 'x\n'}) as t:
            namespaces, domains = guard.live_epochs(t.root)
        self.assertEqual(namespaces, {'engine_script_run_v18', 'native_order_v6',
                                      'native_order_v1', 'native_run_spec_v3'})
        self.assertEqual(domains, {'pineforge-broker-state/v18'})


if __name__ == '__main__':
    unittest.main()
