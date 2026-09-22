#!/usr/bin/env python3
"""Self-tests for the documentation anchor gate: it must be able to FAIL.

A gate that cannot fail is decoration. Each case builds a throwaway tree — a
header, a source file and a page that cites them — and drives the real
``check_doc_anchors.main`` over it with ``--root``, so the grammar, the path
resolution, the symbol claim and ``--fix`` are all exercised through the same
entry point ``ci_preflight`` uses.

The must-fail half: a wrong line, a wrong file, a symbol that is nowhere in the
cited file, and a range that runs past EOF each exit 1 and name themselves.
The must-not-fire half is just as load-bearing, because a checker that flags
the pages' own conventions would be turned off: fenced code blocks, Pine
spellings in the migration table's left column, the ``closed_trade_*`` and
``/ _exit_id`` elisions, and a table cell's bar as a scope boundary.

``--fix`` is pinned on both sides: it re-anchors a uniquely-locatable symbol
and it refuses an ambiguous one, leaving the page byte-identical.
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

import check_doc_anchors as guard

HEADER = """#pragma once
// on_native_bar is named here in a comment only.
namespace pineforge {

struct NativeRunSpec {
    double initial_capital = 0.0;
    int max_open_lots = 0;
};

struct AcceptedEvent {
    int id = 0;
};

int resolve_terms(int units);

class NativeStrategyHost {
  public:
    virtual void on_native_bar(const Bar&) {}
    virtual void on_native_applied(const Event&) {}
};

inline int resolve_terms(int units) { return units; }

}  // namespace pineforge
"""

SOURCE = """#include <pineforge/native_host.hpp>
namespace pineforge {
void drive() {
    NativeStrategyHost host;
    host.on_native_bar({});
    host.on_native_applied({});
}
}  // namespace pineforge
"""


class Tree:
    """A throwaway repository with one header, one source and one page."""

    def __init__(self, page_body: str, *, name: str = 'native-engine.md') -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        (self.root / 'include/pineforge').mkdir(parents=True)
        (self.root / 'src').mkdir(parents=True)
        (self.root / 'docs/pages').mkdir(parents=True)
        (self.root / 'include/pineforge/native_host.hpp').write_text(HEADER)
        (self.root / 'src/native_host_driver.cpp').write_text(SOURCE)
        self.page = self.root / 'docs/pages' / name
        self.page.write_text(page_body)

    def run(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = guard.main(['--root', str(self.root), *args])
        return code, out.getvalue()

    def close(self) -> None:
        self.dir.cleanup()


@contextlib.contextmanager
def tree(body: str, **kwargs):
    made = Tree(body, **kwargs)
    try:
        yield made
    finally:
        made.close()


class MustFail(unittest.TestCase):
    """Every way a citation can be wrong is a non-zero exit that names itself."""

    def test_wrong_line(self) -> None:
        with tree('The host callback `on_native_bar` native_host.hpp:5.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('SYMMISS', out)
            self.assertIn('native_host.hpp:5', out)
            self.assertIn('native_host.hpp:18', out)   # the suggestion

    def test_wrong_file(self) -> None:
        with tree('See `NativeRunSpec` native_runspec.hpp:5.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('NOFILE', out)
            self.assertIn('native_runspec.hpp:5', out)

    def test_symbol_absent_from_file(self) -> None:
        with tree('See `NoSuchSymbol` native_host.hpp:5.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('SYMMISS', out)
            self.assertIn('NONE', out)                  # nowhere to point it at

    def test_range_past_eof(self) -> None:
        with tree('See `NativeRunSpec` native_host.hpp:900-910.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('NOLINE', out)
            self.assertIn('has 24 lines', out)

    def test_inverted_range(self) -> None:
        with tree('See `NativeRunSpec` native_host.hpp:9-5.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('NOLINE', out)

    def test_range_misses_the_symbol(self) -> None:
        with tree('See `AcceptedEvent` native_host.hpp:5-7.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('SYMMISS', out)
            self.assertIn('native_host.hpp:10-12', out)  # span length preserved

    def test_continuation_range_is_checked(self) -> None:
        body = 'Both `NativeRunSpec` native_host.hpp:5 and `AcceptedEvent` :5.\n'
        with tree(body) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native_host.hpp:5  [SYMMISS]', out)


class MustPass(unittest.TestCase):
    """The pages' own conventions are not defects."""

    def test_a_continuation_takes_the_last_full_anchor_s_path(self) -> None:
        # Before lane F2 the second `:3` took native_host.hpp's path from the
        # earlier continuation `:18` and was judged against the wrong file.
        body = ('`on_native_bar` native_host.hpp:18 (`:18`), `drive` '
                'src/native_host_driver.cpp:3 (`:3`).\n')
        with tree(body) as t:
            code, out = t.run('--list')
            self.assertEqual(code, 0, out)
            self.assertIn('src/native_host_driver.cpp:3', out)
            self.assertNotIn('native_host.hpp:3 ', out)

    def test_correct_anchor(self) -> None:
        with tree('See `NativeRunSpec` native_host.hpp:5.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)
            self.assertIn('every documentation anchor resolves', out)

    def test_one_line_of_slack_for_a_wrapped_declaration(self) -> None:
        with tree('See `initial_capital` native_host.hpp:5.\n') as t:
            self.assertEqual(t.run()[0], 0)             # declared on line 6

    def test_fenced_code_is_not_a_citation(self) -> None:
        body = 'Text.\n\n```\nnative_host.hpp:9999: error: nope\n```\n'
        with tree(body) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)
            self.assertNotIn('9999', out)

    def test_pine_spelling_lends_no_symbol(self) -> None:
        body = '| `strategy.risk.max_position_size` x native_host.hpp:1 |\n'
        with tree(body) as t:
            self.assertEqual(t.run()[0], 0)

    def test_table_cell_bar_bounds_the_scope(self) -> None:
        # The Pine column must not lend `AcceptedEvent` to the Native column.
        body = '| `AcceptedEvent` | `NativeRunSpec` native_host.hpp:5 |\n'
        with tree(body) as t:
            self.assertEqual(t.run()[0], 0)

    def test_prefix_and_suffix_elisions(self) -> None:
        body = ('`on_native_*` native_host.hpp:18\n\n'
                'and `_native_applied` native_host.hpp:19\n')
        with tree(body) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_bare_name_resolves_through_the_search_order(self) -> None:
        with tree('See `drive` native_host_driver.cpp:3.\n') as t:
            self.assertEqual(t.run()[0], 0)

    def test_explicit_path_is_taken_as_is(self) -> None:
        with tree('See `drive` src/native_host_driver.cpp:3.\n') as t:
            self.assertEqual(t.run()[0], 0)

    def test_explicit_path_that_does_not_exist_is_not_searched_for(self) -> None:
        with tree('See `drive` other/native_host_driver.cpp:3.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('NOFILE', out)


class Fixing(unittest.TestCase):
    """--fix re-anchors what it can prove and refuses what it cannot."""

    def test_fix_rewrites_a_unique_symbol(self) -> None:
        with tree('See `initial_capital` native_host.hpp:99.\n') as t:
            code, out = t.run('--fix')
            self.assertEqual(code, 0, out)
            self.assertIn('re-anchored 1', out)
            self.assertEqual(t.page.read_text(),
                             'See `initial_capital` native_host.hpp:6.\n')

    def test_fix_preserves_the_span_length(self) -> None:
        with tree('See `AcceptedEvent` native_host.hpp:200-202.\n') as t:
            self.assertEqual(t.run('--fix')[0], 0)
            self.assertEqual(t.page.read_text(),
                             'See `AcceptedEvent` native_host.hpp:10-12.\n')

    def test_fix_changes_line_numbers_and_nothing_else(self) -> None:
        body = 'A `initial_capital` native_host.hpp:99 and `max_open_lots` native_host.hpp:99.\n'
        with tree(body) as t:
            t.run('--fix')
            after = t.page.read_text()
            self.assertEqual(after.replace(':6', ':N').replace(':7', ':N'),
                             body.replace(':99', ':N'))

    def test_fix_refuses_an_ambiguous_symbol(self) -> None:
        # `resolve_terms` is on two code lines: forward declaration and body.
        with tree('See `resolve_terms` native_host.hpp:3.\n') as t:
            before = t.page.read_text()
            code, out = t.run('--fix')
            self.assertEqual(code, 1, out)
            self.assertIn('ambiguous', out)
            self.assertEqual(t.page.read_text(), before)

    def test_fix_refuses_an_absent_symbol(self) -> None:
        with tree('See `NoSuchSymbol` native_host.hpp:5.\n') as t:
            before = t.page.read_text()
            self.assertEqual(t.run('--fix')[0], 1)
            self.assertEqual(t.page.read_text(), before)

    def test_fix_refuses_an_unresolvable_file(self) -> None:
        with tree('See `NativeRunSpec` native_runspec.hpp:5.\n') as t:
            before = t.page.read_text()
            self.assertEqual(t.run('--fix')[0], 1)
            self.assertEqual(t.page.read_text(), before)

    def test_fix_is_idempotent(self) -> None:
        with tree('See `initial_capital` native_host.hpp:99.\n') as t:
            self.assertEqual(t.run('--fix')[0], 0)
            first = t.page.read_text()
            self.assertEqual(t.run('--fix')[0], 0)
            self.assertEqual(t.page.read_text(), first)

    def test_a_type_declaration_disambiguates(self) -> None:
        # `NativeStrategyHost` is on the class line and on the driver's use of
        # it; within the header exactly one line declares the type.
        with tree('See `NativeStrategyHost` native_host.hpp:1.\n') as t:
            self.assertEqual(t.run('--fix')[0], 0)
            self.assertEqual(t.page.read_text(),
                             'See `NativeStrategyHost` native_host.hpp:16.\n')


class Listing(unittest.TestCase):
    """--list is the work list: every anchor, verdict and suggestion."""

    def test_list_dumps_good_and_bad(self) -> None:
        body = 'See `NativeRunSpec` native_host.hpp:5 and `AcceptedEvent` native_host.hpp:5.\n'
        with tree(body) as t:
            code, out = t.run('--list')
            self.assertEqual(code, 1, out)
            self.assertEqual(out.count('docs/pages/native-engine.md:1  OK'), 1)
            self.assertEqual(out.count('docs/pages/native-engine.md:1  SYMMISS'), 1)


class Grammar(unittest.TestCase):
    """The symbol extraction rules, checked directly."""

    def test_symbol_of(self) -> None:
        cases = {
            'NativeRunSpec::calculation': ('calculation', 'word'),
            'cancel_where(text, NativeRequestField::Label)': ('cancel_where', 'word'),
            'native_toolkit::OrderBook<Key>': ('OrderBook', 'word'),
            'Reduce{OwnerOpenedUnits{}}': ('Reduce', 'word'),
            'closed_trade_*': ('closed_trade_', 'prefix'),
            '_exit_comment': ('_exit_comment', 'suffix'),
            'ticks * price_tick': (None, 'word'),
            'src/engine_run.cpp': (None, 'word'),
            'strategy.risk.max_drawdown': (None, 'word'),
            'std::optional<double>': (None, 'word'),
            '-DPINEFORGE_BUILD_TESTS=ON': (None, 'word'),
        }
        for span, expected in cases.items():
            with self.subTest(span=span):
                self.assertEqual(guard.symbol_of(span), expected)

    def test_strip_comments_keeps_line_numbering(self) -> None:
        lines = ['int a;  // name', '/* name', 'name */ int b;', 'int name;']
        stripped = guard.strip_comments(lines)
        self.assertEqual(len(stripped), 4)
        self.assertEqual(guard.occurrences(stripped, 'name'), [4])

    def test_runs_groups_contiguous_lines(self) -> None:
        self.assertEqual(guard.runs([3, 4, 5, 9, 11, 12]), [(3, 5), (9, 9), (11, 12)])


if __name__ == '__main__':
    unittest.main()
