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

Lane F2's precision rules are pinned the same way (``Precision``): a symbol
only on the line after the citation fails; a qualified ``A::B`` outside ``A``'s
braces fails, and ``--fix`` moves it inside; a ruling-table anchor with no claim
fails, and a code fragment there is verified verbatim; a continuation list
shares the claim of the anchor that heads it, and a continuation takes the
path of the last *full* anchor before it, never of another continuation.

Lane H-DOCGATES (AUDIT4-opus X13) pins the pin rules (``PinMustFail``, the
audit's probes P09/P10/P11/P16): a pin accompanies the claim and never replaces
it, a single line is not pinned, a blank or doubled pin is rejected, a pinned
ruling row still needs its claim, and a pinned comment window must say it cites
a comment. ``BorrowedClaim``: a citation prose words introduce after a list
comma claims nothing of the list's earlier symbol.
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

struct Reduce {
    int units = 0;
};

enum class GroupEffect { Cancel, Reduce };

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
            # expectation corrected: 'has 24 lines' -> 'has 30 lines', because the
            # fixture header gained `struct Reduce` and `enum class GroupEffect`
            # for lane F2's scope cases.
            self.assertIn('has 30 lines', out)

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

    def test_correct_anchor(self) -> None:
        with tree('See `NativeRunSpec` native_host.hpp:5.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)
            self.assertIn('every documentation anchor resolves', out)

    def test_the_symbol_on_the_cited_line(self) -> None:
        with tree('See `initial_capital` native_host.hpp:6.\n') as t:
            self.assertEqual(t.run()[0], 0)

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


ADR = 'docs/adr/0001-kernel-adapter-boundary.md'
RULING = ('# ADR\n\n## Kernel capabilities the Pine adapter does not declare (rulings)\n\n'
          '| field | ruling |\n|---|---|\n')
PROSE = '# ADR\n\n## How the kernel works\n\n'


class RulingTree(Tree):
    """The same tree with the page written as ADR-0001, where ruling tables live."""

    def __init__(self, page_body: str) -> None:
        super().__init__('placeholder\n')
        self.page.unlink()
        self.page = self.root / ADR
        self.page.parent.mkdir(parents=True, exist_ok=True)
        self.page.write_text(page_body)


@contextlib.contextmanager
def ruling(body: str):
    made = RulingTree(body)
    try:
        yield made
    finally:
        made.close()


class Precision(unittest.TestCase):
    """Lane F2: the anchor names its symbol, in its scope, and a ruling names a claim."""

    def test_a_symbol_only_on_the_next_line_fails(self) -> None:
        # expectation corrected: passed (one line of slack after the window) ->
        # fails, because lane F2 made the window exactly the cited line or range:
        # 70 anchors passed only through that slack, about nine on another field.
        with tree('See `initial_capital` native_host.hpp:5.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('SYMMISS', out)
            self.assertIn('lines 5-5', out)

    def test_a_qualified_symbol_outside_its_scope_fails(self) -> None:
        # `Reduce` is on line 22 as `struct Reduce {`; GroupEffect::Reduce is on 26.
        with tree('The effect `GroupEffect::Reduce` native_host.hpp:22.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('SCOPE', out)
            self.assertIn('outside `GroupEffect`', out)
            self.assertIn('native_host.hpp:26', out)          # the suggestion, in scope

    def test_fix_moves_a_qualified_symbol_into_its_scope(self) -> None:
        with tree('The effect `GroupEffect::Reduce` native_host.hpp:22.\n') as t:
            self.assertEqual(t.run('--fix')[0], 0)
            self.assertEqual(t.page.read_text(),
                             'The effect `GroupEffect::Reduce` native_host.hpp:26.\n')

    def test_a_qualified_symbol_inside_its_scope_passes(self) -> None:
        with tree('The field `NativeRunSpec::initial_capital` native_host.hpp:6.\n') as t:
            self.assertEqual(t.run()[0], 0)

    def test_a_qualifier_the_file_does_not_declare_is_not_checked(self) -> None:
        with tree('The callback `no::on_native_bar` native_host.hpp:18.\n') as t:
            self.assertEqual(t.run()[0], 0)

    def test_a_cpp_namespace_is_not_a_pine_spelling(self) -> None:
        self.assertEqual(guard.symbol_of('ta::ATR'), ('ATR', 'word'))
        self.assertEqual(guard.symbol_of('ta.atr'), (None, 'word'))

    def test_a_ruling_anchor_without_a_claim_fails(self) -> None:
        with ruling(RULING + '| `x` | native only (native_host.hpp:6) |\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('NOCLAIM', out)

    def test_a_ruling_anchor_with_a_symbol_passes(self) -> None:
        with ruling(RULING + '| `x` | native only (`initial_capital` native_host.hpp:6) |\n') as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_ruling_fragment_is_verified_verbatim(self) -> None:
        good = RULING + '| `x` | the default (`int max_open_lots = 0` native_host.hpp:7) |\n'
        bad = RULING + '| `x` | the default (`int max_open_lots = 1` native_host.hpp:7) |\n'
        with ruling(good) as t:
            self.assertEqual(t.run()[0], 0)
        with ruling(bad) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('SYMMISS', out)

    def test_a_prose_anchor_without_a_claim_is_only_line_checked(self) -> None:
        with ruling(PROSE + 'The spec lives at native_host.hpp:6.\n') as t:
            self.assertEqual(t.run()[0], 0)

    def test_a_pine_spelling_is_no_claim_in_a_ruling_row(self) -> None:
        with ruling(RULING + '| `x` | `strategy.risk.max_position_size` (native_host.hpp:6) |\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('NOCLAIM', out)

    def test_a_continuation_list_shares_its_head_claim(self) -> None:
        good = 'Both `NativeStrategyHost` native_host.hpp:16, `:17`, `:18` hold it.\n'
        bad = 'Both `NativeStrategyHost` native_host.hpp:16, `:17`, `:20` hold it.\n'
        with tree(good) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)                  # :17 has no NativeStrategyHost
        body = 'Both `resolve_terms` native_host.hpp:14, `:28` agree.\n'
        with tree(body) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)
        with tree(bad) as t:
            self.assertEqual(t.run()[0], 1)

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
            'ta::ATR': ('ATR', 'word'),
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


class H10MustFail(unittest.TestCase):
    """Each old blind spot accepts the bad citation; the hardened gate rejects it."""

    def test_previous_line_continuation_reproduces_item_12(self) -> None:
        body = ('The declaration `initial_capital` native_host.hpp:6,\n'
                'and `max_open_lots` (`:6`) follow.\n')
        with tree(body) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native_host.hpp:6  [SYMMISS]', out)

    def test_comment_only_window_reproduces_item_8(self) -> None:
        with tree('The callback `on_native_bar` native_host.hpp:2.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('COMMENTONLY', out)

    def test_symbolless_range_reproduces_item_3(self) -> None:
        with tree('The contract is native_host.hpp:5-7.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('NOCLAIM', out)

    def test_source_fence_reproduces_item_33(self) -> None:
        body = '```cpp\n// See `initial_capital` native_host.hpp:7.\n```\n'
        with tree(body) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('SYMMISS', out)



def pin(first: int, last: int, text: str = HEADER) -> str:
    """The content pin of lines first..last of `text`, as a page writes it."""
    lines = text.splitlines()
    import hashlib
    return 'sha256:' + hashlib.sha256('\n'.join(lines[first - 1:last]).encode()).hexdigest()


COMMENT_HEADER = """#pragma once
// A comment block the pages cite as a comment:
// its lines hold no code at all,
// only prose about resolve_terms.
int resolve_terms(int units);
"""


class PinMustFail(unittest.TestCase):
    """R5 lane H-DOCGATES (AUDIT4-opus docs-a N1, probes P09-P11 and P16): a
    content pin accompanies a claim and never replaces it. Before the lane
    each of these citations read OK, because the pin dropped the symbol claim
    and was compared alone."""

    def test_p09_a_pin_over_the_wrong_line_does_not_save_the_claim(self) -> None:
        # The audit's P09: `strategy_create` pinned over the line after it.
        with tree(f'`resolve_terms` `{pin(11, 11)}` native_host.hpp:11 answers.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native_host.hpp:11  [PINLINE]', out)
        # A range pinned exactly but holding another symbol: the claim is still checked.
        with tree(f'`resolve_terms` `{pin(16, 20)}` native_host.hpp:16-20 answers.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native_host.hpp:16-20  [SYMMISS]', out)

    def test_p10_a_pin_over_a_blank_line_pins_nothing(self) -> None:
        empty = 'sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
        self.assertEqual(pin(4, 4), empty)
        with tree(f'`resolve_terms` `{empty}` native_host.hpp:4 answers.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native_host.hpp:4  [PINEMPTY]', out)
        with tree(f'The gap `{pin(27, 27)}` native_host.hpp:27 is blank.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('[PINEMPTY]', out)

    def test_p11_a_doubled_pin_is_rejected(self) -> None:
        garbage = 'sha256:' + '0' * 64
        with tree(f'The struct `{garbage}` `{pin(5, 8)}` native_host.hpp:5-8 holds it.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native_host.hpp:5-8  [PINDOUBLE]', out)

    def test_p16_a_pin_is_no_claim_in_a_ruling_row(self) -> None:
        body = ('## Residual TradingView-named surface\n\n'
                '| name | where | ruling |\n|---|---|---|\n'
                f'| `probe_ruling_h` | a row pinned over code `{pin(16, 20)}` '
                'native_host.hpp:16-20 | probe |\n')
        with ruling(body) as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native_host.hpp:16-20  [NOCLAIM]', out)
        # With the symbol the window holds, the same pinned row passes.
        body = body.replace('a row pinned over code', '`NativeStrategyHost`')
        with ruling(body) as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_pinned_comment_window_must_say_it_cites_a_comment(self) -> None:
        with tree(f'`resolve_terms` `{pin(2, 4, COMMENT_HEADER)}` comments.hpp:2-4 decides.\n') as t:
            (t.root / 'include/pineforge/comments.hpp').write_text(COMMENT_HEADER)
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('comments.hpp:2-4  [COMMENTONLY]', out)
        with tree(f'The comment `{pin(2, 4, COMMENT_HEADER)}` comments.hpp:2-4 says so.\n') as t:
            (t.root / 'include/pineforge/comments.hpp').write_text(COMMENT_HEADER)
            code, out = t.run()
            self.assertEqual(code, 0, out)

    def test_a_pinned_range_with_its_claim_passes(self) -> None:
        with tree(f'The class `NativeStrategyHost` `{pin(16, 20)}` native_host.hpp:16-20.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)
        with tree(f'The class `{pin(16, 20)}` native_host.hpp:16-20 opens here.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)   # prose outside ruling rows: the pin alone
        with tree(f'The class `{pin(16, 19)}` native_host.hpp:16-20 moved.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('[HASHMISS]', out)


class BorrowedClaim(unittest.TestCase):
    """AUDIT4-opus docs-a N2 / GAP-5: a citation that prose words introduce after
    an earlier citation's comma claims nothing; it does not borrow the list's
    symbol (design:177's "FX curve" and "probe" rode `strategy_configure_native_v1`)."""

    def test_a_prose_citation_after_a_list_comma_is_noclaim(self) -> None:
        with tree('`resolve_terms` native_host.hpp:14, other native_host.hpp:14.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('[NOCLAIM]', out)
            listed = t.run('--list')[1]
            self.assertRegex(listed, r'OK +native_host\.hpp:14 +`resolve_terms`')
            self.assertRegex(listed, r'NOCLAIM +native_host\.hpp:14 +\(no symbol\)')
        with tree('`resolve_terms` `native_host.hpp:14`, and its body `native_host.hpp:28`.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 1, out)
            self.assertIn('native_host.hpp:28  [NOCLAIM]', out)

    def test_a_list_continuation_and_a_sentence_comma_still_share_the_claim(self) -> None:
        for body in ('`resolve_terms` native_host.hpp:14, :28.\n',
                     '`resolve_terms` native_host.hpp:14, and native_host.hpp:28.\n',
                     '`resolve_terms`, the one function, native_host.hpp:28.\n',
                     '`resolve_terms` (native_host.hpp:14, native_host.hpp:28).\n'):
            with self.subTest(body=body):
                with tree(body) as t:
                    code, out = t.run()
                    self.assertEqual(code, 0, out)
        with tree('`resolve_terms` native_host.hpp:14, other `resolve_terms` native_host.hpp:28.\n') as t:
            code, out = t.run()
            self.assertEqual(code, 0, out)       # its own claim


if __name__ == '__main__':
    unittest.main()
