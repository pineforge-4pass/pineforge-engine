#!/usr/bin/env python3
"""Self-tests for the kernel seam-row gate: it must be able to FAIL.

Each case writes a throwaway tree -- a kernel `engine.hpp`, the four source
headers the member reader skips, one source-layer file that writes kernel
state, and an ADR -- and drives the real ``check_kernel_seam_rows.main`` over
it. A kernel `source_*` seam, a member the source layer writes (unqualified,
through a host reference, or declared right after an inline function body)
and a row field written through the kernel's rows each fail without a row
naming them in its first cell, and pass with one. A read is not a write, a
member a source class declares again is that class's own, a row field spelled
bare or a name quoted in a later cell is not a row, and a field of an object
that is not one of the kernel's rows is not a row write.

The excursion writers have cases of their own. A tree whose rows declare
`max_runup` and `max_drawdown` passes with the two functions
``gate.EXCURSION_WRITERS`` names and fails on a third writer of a lot's
excursion, one planted write per case, in the object spellings and write
forms the gate's docstring says it reads: a lot of the book taken with `[i]`,
`.back()`, `.front()` and `.at(i)`, a range-for over the book, an iterator, a
reference of the row type, a local reference to the book, a pointer, a const
and a volatile pointer (the qualifier after the `*`, or after the type word)
and a const iterator, an element of a row-vector parameter (its `&` set against
the name as well) and a range-for over it, a source-class member of row type;
an assignment, a compound assignment, a suffix `++` and a suffix `--`. It fails
as well on a `Trade`'s excursion written outside the re-cut (taken by a lambda
or from the ledger), on a lot's written in the `Trade` writer, on an object the
gate cannot type (a qualified pointer of a type the pattern cannot spell among
them) and on a named writer that writes nothing; the source layer's own field
of the same name is not a row. The forms the gate's docstring lists as not read
have no case here, except a qualified pointer of a type the pattern cannot
spell, which must fail as an object the gate cannot type. Two cases read this
repository's own source layer: its only writers are the two named ones, and a
third writer planted in a temporary copy of the adapter fails.

R5 lane H-DOCGATES (AUDIT4-opus X10).
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import shutil
import tempfile
import unittest

import check_kernel_seam_rows as gate

ENGINE = """
namespace pineforge {
struct Trade {
    long time = 0;
    bool exit_from_bracket = false;
};
struct PyramidEntry {
    double qty = 0.0;
};
class BacktestEngine {
public:
    virtual bool source_feed_enabled() const;
    int shown() const { return 1; }
protected:
    Bar current_bar_;
    int bar_index_ = 0;
    int kernel_only_ = 0;
    int shadowed_ = 0;
    std::vector<Trade> trades_;
};
}
"""

SOURCE_HEADERS = {
    "include/pineforge/source/pine_adapter.hpp": "class PineExecutionAdapter {\n    int a_ = 0;\n};\n",
    "include/pineforge/source/pine_scheduler.hpp": "class PineScheduler {\n    int b_ = 0;\n};\n",
    "include/pineforge/source/pine_strategy_host.hpp":
        "class PineStrategyHost : public BacktestEngine {\n    int shadowed_ = 0;\n};\n",
    "include/pineforge/source/pine_language_state.hpp": "struct PineLanguageState {\n    int c_ = 0;\n};\n",
}

SOURCE = """
void PineStrategyHost::publish(const Bar& bar) {
    current_bar_ = bar;            // declared right after an inline function body
    const int seen = kernel_only_; // a read, not a write
    shadowed_ = seen;              // the source class's own member
}
void scope(PineStrategyHost& host, int index) {
    host.bar_index_ = index;       // through a host reference
}
void label(std::vector<Trade>& rows) {
    for (auto& trade : rows) trade.exit_from_bracket = true;
    Sized sized;
    sized.time = 3;                // not one of the kernel's rows
}
"""

ADR_HEAD = """# ADR

| kernel state | who writes it | ruling |
|---|---|---|
"""

ALL_ROWS = (
    "| `source_feed_enabled` | the Pine host | retained |\n"
    "| the presented bar -- `current_bar_`, `bar_index_` | the Pine host | retained |\n"
    "| `Trade::exit_from_bracket` | the Pine host | retained |\n"
)


class Tree:
    def __init__(self, adr_rows: str, source: str = SOURCE,
                 files: dict[str, str] | None = None) -> None:
        self.dir = tempfile.TemporaryDirectory(prefix="pineforge-seam-rows-")
        self.root = Path(self.dir.name)
        self.write("include/pineforge/engine.hpp", ENGINE)
        for relative, text in SOURCE_HEADERS.items():
            self.write(relative, text)
        self.write("src/source/pine_strategy_host.cpp", source)
        self.write(gate.ADR, ADR_HEAD + adr_rows)
        for relative, text in (files or {}).items():   # over the smallest tree's files
            self.write(relative, text)

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def run(self, *args: str) -> tuple[int, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gate.main(["--root", str(self.root), *args])
        return code, out.getvalue() + err.getvalue()

    def close(self) -> None:
        self.dir.cleanup()


@contextlib.contextmanager
def tree(adr_rows: str, source: str = SOURCE, files: dict[str, str] | None = None):
    made = Tree(adr_rows, source, files)
    try:
        yield made
    finally:
        made.close()


# A tree whose rows carry the two excursion fields, with the two writers
# gate.EXCURSION_WRITERS names and the adapter's own risk limit of the same name.
EXCURSION_ENGINE = """
namespace pineforge {
struct Trade {
    long time = 0;
    double max_runup = 0.0;
    double max_drawdown = 0.0;
};
struct PyramidEntry {
    double qty = 0.0;
    double max_runup = 0.0;
    double max_drawdown = 0.0;
};
class BacktestEngine {
public:
    int shown() const { return 1; }
protected:
    std::vector<Trade> trades_;
    std::vector<PyramidEntry> pyramid_entries_;
};
}
"""

EXCURSION_ADAPTER_HEADER = (
    "struct PineRiskState {\n    double max_drawdown = 0.0;\n};\n"
    "struct PineReplayRiskConfig {\n    double max_drawdown = 0.0;\n};\n"
    "class PineExecutionAdapter {\n    int a_ = 0;\n    PineRiskState risk_{};\n};\n")

EXCURSION_ADAPTER = """
namespace pineforge::source {
void PineExecutionAdapter::set_risk_max_drawdown(double value, bool percent) noexcept {
    risk_.max_drawdown = value;                  // the adapter's own limit, not a row
}
PineReplayRiskConfig PineExecutionAdapter::capture_replay_risk_config() const noexcept {
    PineReplayRiskConfig snapshot;
    snapshot.max_drawdown = risk_.max_drawdown;  // nor is its copy
    return snapshot;
}
bool PineExecutionAdapter::submit_long_tick_margin_units(
        const Bar& bar, const double* path, int point, double mark,
        const NativeDecisionContext& context, double units) {
    auto* pine = pine_view_of(host_);
    PyramidEntry& booked = pine->pyramid_entries_.front();
    const double bar_favorable = booked.max_runup;   // a read
    booked.max_runup = std::min(path[point], bar_favorable);
    booked.max_drawdown = 0.0;
    const auto restore = [&]() noexcept {
        if (pine->pyramid_entries_.size() != 1) {
            return;
        }
        PyramidEntry& survivor = pine->pyramid_entries_.front();
        survivor.max_runup = bar_favorable;
        survivor.max_drawdown = 0.0;
    };
    restore();
    return submit_margin_call_units(mark, context, units, true, mark);
}
} // namespace pineforge::source
"""

EXCURSION_HOST = """
void source::PineStrategyHost::order_global_exit_children(
        const native_order::ExecutionAppliedEvent& event) {
    visit_slices([&](std::size_t lot, double share) {
        const Trade& whole = trades_[lot];
        Trade row{};
        row.max_runup = whole.max_runup * share;
        row.max_drawdown = whole.max_drawdown * share;
        emit_row(row);
    });
}
"""

EXCURSION_ROWS = (
    "| a row's excursion -- `PyramidEntry::max_runup`, `PyramidEntry::max_drawdown`, "
    "`Trade::max_runup`, `Trade::max_drawdown` | its two named writers | retained |\n")

ADAPTER_END = "} // namespace pineforge::source\n"


def excursion_files(adapter: str = EXCURSION_ADAPTER,
                    host: str = EXCURSION_HOST) -> dict[str, str]:
    return {
        gate.ENGINE: EXCURSION_ENGINE,
        "include/pineforge/source/pine_adapter.hpp": EXCURSION_ADAPTER_HEADER,
        "src/source/pine_adapter.cpp": adapter,
        "src/source/pine_strategy_host.cpp": host,
    }


def adapter_with(body: str, parameters: str = "PineStrategyHost& host") -> str:
    """The adapter with one more function, whose body is `body`."""
    return EXCURSION_ADAPTER.replace(
        ADAPTER_END,
        f"void PineExecutionAdapter::third_writer({parameters}) {{\n"
        "    auto* pine = pine_view_of(host_);\n" + body + "}\n" + ADAPTER_END)


class Inventory(unittest.TestCase):
    def test_what_the_gate_reads(self) -> None:
        with tree(ALL_ROWS) as t:
            found = gate.inventory(t.root)
        self.assertEqual(set(found.seams), {"source_feed_enabled"})
        self.assertEqual(set(found.written), {"current_bar_", "bar_index_", "exit_from_bracket"})
        self.assertEqual(found.written["current_bar_"], ["src/source/pine_strategy_host.cpp:3"])
        self.assertEqual(found.written["bar_index_"], ["src/source/pine_strategy_host.cpp:8"])

    def test_a_block_comment_keeps_the_line_numbers(self) -> None:
        source = "/* two\n lines */\n" + SOURCE
        with tree(ALL_ROWS, source) as t:
            found = gate.inventory(t.root)
        self.assertEqual(found.written["current_bar_"], ["src/source/pine_strategy_host.cpp:5"])


class MustFail(unittest.TestCase):
    def assert_fails_naming(self, rows: str, name: str) -> None:
        with tree(rows) as t:
            code, out = t.run()
        self.assertEqual(code, 1, out)
        self.assertIn(f"`{name}`", out)
        self.assertIn("FAIL", out)

    def test_a_seam_without_a_row(self) -> None:
        self.assert_fails_naming(ALL_ROWS.replace("| `source_feed_enabled` | the Pine host | retained |\n", ""),
                                 "source_feed_enabled")

    def test_a_member_declared_after_an_inline_function(self) -> None:
        self.assert_fails_naming(ALL_ROWS.replace("`current_bar_`, ", ""), "current_bar_")

    def test_a_member_written_through_a_host_reference(self) -> None:
        self.assert_fails_naming(ALL_ROWS.replace(", `bar_index_`", ""), "bar_index_")

    def test_a_row_field_written_through_the_kernels_rows(self) -> None:
        self.assert_fails_naming(ALL_ROWS.replace("| `Trade::exit_from_bracket` | the Pine host | retained |\n", ""),
                                 "exit_from_bracket")

    def test_a_row_field_spelled_bare_is_not_its_row(self) -> None:
        self.assert_fails_naming(ALL_ROWS.replace("`Trade::exit_from_bracket`", "`exit_from_bracket`"),
                                 "exit_from_bracket")

    def test_a_name_quoted_in_a_later_cell_is_not_its_row(self) -> None:
        rows = ALL_ROWS.replace("| the presented bar -- `current_bar_`, `bar_index_` | the Pine host | retained |\n",
                                "| the presented bar -- `bar_index_` | the kernel writes `current_bar_` too | retained |\n")
        self.assert_fails_naming(rows, "current_bar_")

    def test_prose_is_not_a_row(self) -> None:
        rows = ALL_ROWS.replace("| `source_feed_enabled` | the Pine host | retained |\n", "")
        rows += "\nThe seam `source_feed_enabled` is the Pine host's.\n"
        self.assert_fails_naming(rows, "source_feed_enabled")

    def test_a_missing_file_is_exit_2(self) -> None:
        with tree(ALL_ROWS) as t:
            (t.root / gate.ADR).unlink()
            code, out = t.run()
        self.assertEqual(code, 2, out)


class MustPass(unittest.TestCase):
    def test_every_entry_has_its_row(self) -> None:
        with tree(ALL_ROWS) as t:
            code, out = t.run("--list")
        self.assertEqual(code, 0, out)
        self.assertIn("1 kernel source_* seams and 3 kernel members the source layer writes, "
                      "0 without a row ... OK", out)
        self.assertNotIn("kernel_only_", out)   # read, never written
        self.assertNotIn("shadowed_", out)      # the source class's own member
        self.assertNotIn(" time ", out)         # `sized.time` is not a kernel row
        self.assertNotIn("row excursions", out)  # these rows carry no excursion field


class ExcursionWriters(unittest.TestCase):
    """A row's `max_runup` and `max_drawdown` are held to their named writers."""

    def run_tree(self, files: dict[str, str]) -> tuple[int, str]:
        with tree(EXCURSION_ROWS, files=files) as t:
            return t.run("--list")

    def assert_fails_naming(self, files: dict[str, str], *named: str) -> str:
        code, out = self.run_tree(files)
        self.assertEqual(code, 1, out)
        for name in named:
            self.assertIn(name, out)
        self.assertIn("0 without a row ... OK", out)   # every field still has its row
        self.assertRegex(out, r"row excursions: .* ... FAIL")
        return out

    def test_the_two_named_writers_pass(self) -> None:
        code, out = self.run_tree(excursion_files())
        self.assertEqual(code, 0, out)
        self.assertIn("row excursions: 6 source-layer writes of a row's run-up or drawdown, "
                      "0 findings against the writers ADR-0001 names ... OK", out)
        self.assertIn("excursion PyramidEntry::max_runup (written at "
                      "src/source/pine_adapter.cpp:17, in "
                      "PineExecutionAdapter::submit_long_tick_margin_units)", out)
        # Inside the lambda: the function the lambda is written in.
        self.assertIn("excursion PyramidEntry::max_drawdown (written at "
                      "src/source/pine_adapter.cpp:25, in "
                      "PineExecutionAdapter::submit_long_tick_margin_units)", out)
        self.assertIn("excursion Trade::max_runup (written at "
                      "src/source/pine_strategy_host.cpp:7, in "
                      "source::PineStrategyHost::order_global_exit_children)", out)

    def test_what_the_excursion_check_reads(self) -> None:
        with tree(EXCURSION_ROWS, files=excursion_files()) as t:
            found = gate.excursion_writes(t.root)
        self.assertEqual(found.guarded, ["Trade", "PyramidEntry"])
        self.assertEqual(
            [(write.struct, write.name, write.site) for write in found.writes],
            [("PyramidEntry", "max_runup", "src/source/pine_adapter.cpp:17"),
             ("PyramidEntry", "max_drawdown", "src/source/pine_adapter.cpp:18"),
             ("PyramidEntry", "max_runup", "src/source/pine_adapter.cpp:24"),
             ("PyramidEntry", "max_drawdown", "src/source/pine_adapter.cpp:25"),
             ("Trade", "max_runup", "src/source/pine_strategy_host.cpp:7"),
             ("Trade", "max_drawdown", "src/source/pine_strategy_host.cpp:8")])
        self.assertEqual(gate.excursion_findings(found), [])

    def test_a_third_writer_of_a_lots_excursion(self) -> None:
        spellings = {
            "a range-for over the book reached through a pointer":
                ("    for (auto& lot : pine->pyramid_entries_) lot.max_runup = 0.0;\n",
                 "max_runup"),
            "a braced range-for":
                ("    for (auto& lot : pine->pyramid_entries_) {\n"
                 "        lot.max_drawdown = 0.0;\n    }\n", "max_drawdown"),
            "the front lot, directly":
                ("    pine->pyramid_entries_.front().max_drawdown = 0.0;\n", "max_drawdown"),
            "an iterator":
                ("    auto it = pine->pyramid_entries_.begin();\n    it->max_runup = 0.0;\n",
                 "max_runup"),
            "a pointer and a compound assignment":
                ("    PyramidEntry* lot = &pine->pyramid_entries_[0];\n"
                 "    lot->max_runup += 1.0;\n", "max_runup"),
            "the host's book":
                ("    host.pyramid_entries_.back().max_drawdown = 0.0;\n", "max_drawdown"),
            "an indexed lot":
                ("    pine->pyramid_entries_[0].max_runup = 0.0;\n", "max_runup"),
            "a lot taken with at()":
                ("    pine->pyramid_entries_.at(0).max_drawdown = 0.0;\n", "max_drawdown"),
            "a suffix increment":
                ("    for (auto& lot : pine->pyramid_entries_) lot.max_runup++;\n", "max_runup"),
            "a suffix decrement":
                ("    for (auto& lot : pine->pyramid_entries_) lot.max_drawdown--;\n",
                 "max_drawdown"),
            "a reference of the row type":
                ("    PyramidEntry& lot = pine->pyramid_entries_.front();\n"
                 "    lot.max_runup = 0.0;\n", "max_runup"),
            "a local reference to the book":
                ("    std::vector<PyramidEntry>& book = pine->pyramid_entries_;\n"
                 "    book[0].max_drawdown = 0.0;\n", "max_drawdown"),
        }
        for label, (body, name) in spellings.items():
            with self.subTest(label):
                self.assert_fails_naming(excursion_files(adapter=adapter_with(body)),
                                         f"`PyramidEntry::{name}`",
                                         "in `PineExecutionAdapter::third_writer`",
                                         "`PineExecutionAdapter::submit_long_tick_margin_units`")

    def test_a_third_writer_through_a_qualified_pointer(self) -> None:
        # A qualifier between the type and the name, before or after the `*`,
        # is part of the declaration, never its type: read as the type, it made
        # the lot the source layer's own and the write passed.
        spellings = {
            "a const pointer, its type deduced":
                ("    auto* const lot = &pine->pyramid_entries_.front();\n"
                 "    lot->max_runup = 0.0;\n", "max_runup"),
            "a const pointer of the row type":
                ("    PyramidEntry* const lot = &pine->pyramid_entries_.front();\n"
                 "    lot->max_drawdown = 0.0;\n", "max_drawdown"),
            "a volatile pointer":
                ("    auto* volatile lot = &pine->pyramid_entries_.front();\n"
                 "    lot->max_runup = 0.0;\n", "max_runup"),
            "a const pointer, the qualifier written after the type":
                ("    auto const lot = &pine->pyramid_entries_.front();\n"
                 "    lot->max_runup = 0.0;\n", "max_runup"),
            "a volatile pointer, the qualifier written after the type":
                ("    auto volatile lot = &pine->pyramid_entries_.front();\n"
                 "    lot->max_drawdown = 0.0;\n", "max_drawdown"),
            "a const iterator":
                ("    auto const it = pine->pyramid_entries_.begin();\n"
                 "    it->max_drawdown = 0.0;\n", "max_drawdown"),
        }
        for label, (body, name) in spellings.items():
            with self.subTest(label):
                self.assert_fails_naming(excursion_files(adapter=adapter_with(body)),
                                         f"`PyramidEntry::{name}`",
                                         "in `PineExecutionAdapter::third_writer`")

    def test_a_qualified_pointer_of_a_type_the_gate_cannot_spell(self) -> None:
        # The qualifier stands right before the name but the type before it is
        # no one word the pattern reads: the qualifier is not the type, so the
        # name has no declaration here and the gate cannot type the object. Read
        # as the type, `const` made the lot the source layer's own and the
        # write passed.
        body = ("    std::remove_reference_t<decltype(pine->pyramid_entries_.front())>* const lot =\n"
                "        &pine->pyramid_entries_.front();\n"
                "    lot->max_runup = 0.0;\n")
        out = self.assert_fails_naming(excursion_files(adapter=adapter_with(body)),
                                       "`max_runup` is written at", "cannot type",
                                       "in `PineExecutionAdapter::third_writer`")
        self.assertIn("excursion ?::max_runup", out)

    def test_a_third_writer_through_a_row_vector_parameter(self) -> None:
        spaced = "std::vector<PyramidEntry>& lots"
        # With its `&` set against the name the parameter is no declaration the
        # pattern reads, so only the reader of row-vector references finds it.
        against = "std::vector<PyramidEntry> &lots"
        spellings = {
            "an element of the parameter":
                ("    lots[0].max_runup = 0.0;\n", "max_runup", spaced),
            "an element of the parameter, its `&` set against the name":
                ("    lots[0].max_runup = 0.0;\n", "max_runup", against),
            "a range-for over the parameter":
                ("    for (auto& lot : lots) lot.max_drawdown = 0.0;\n", "max_drawdown", spaced),
        }
        for label, (body, name, parameters) in spellings.items():
            with self.subTest(label):
                adapter = adapter_with(body, parameters)
                self.assert_fails_naming(excursion_files(adapter=adapter),
                                         f"`PyramidEntry::{name}`",
                                         "in `PineExecutionAdapter::third_writer`")

    def test_a_third_writer_through_a_source_class_member_of_row_type(self) -> None:
        files = excursion_files(adapter=adapter_with("    held_lot_.max_runup = 0.0;\n"))
        member = "    PineRiskState risk_{};\n"
        self.assertIn(member, EXCURSION_ADAPTER_HEADER)
        files["include/pineforge/source/pine_adapter.hpp"] = EXCURSION_ADAPTER_HEADER.replace(
            member, member + "    PyramidEntry held_lot_{};\n")
        self.assert_fails_naming(files, "`PyramidEntry::max_runup`",
                                 "in `PineExecutionAdapter::third_writer`")

    def test_a_trades_excursion_written_outside_the_recut(self) -> None:
        spellings = {
            "a row taken by a lambda":
                "    trades_.modify(index, [&](Trade& row) { row.max_drawdown = 0.0; });\n",
            "a row taken from the ledger":
                "    trades_[index].max_drawdown = 0.0;\n",
        }
        for label, line in spellings.items():
            with self.subTest(label):
                host = EXCURSION_HOST + (
                    "void source::PineStrategyHost::relabel(std::size_t index) {\n"
                    + line + "}\n")
                self.assert_fails_naming(excursion_files(host=host), "`Trade::max_drawdown`",
                                         "in `source::PineStrategyHost::relabel`")

    def test_a_lots_excursion_written_in_the_trade_writer(self) -> None:
        host = EXCURSION_HOST.replace(
            "    visit_slices(",
            "    for (auto& entry : pyramid_entries_) entry.max_runup = 0.0;\n    visit_slices(")
        out = self.assert_fails_naming(
            excursion_files(host=host), "`PyramidEntry::max_runup`",
            "in `source::PineStrategyHost::order_global_exit_children`")
        self.assertIn("1 findings", out)   # the Trade writes there stay the named writer's

    def test_an_object_the_gate_cannot_type(self) -> None:
        body = "    auto& lot = pick(pine);\n    lot.max_runup = 0.0;\n"
        out = self.assert_fails_naming(excursion_files(adapter=adapter_with(body)),
                                       "`max_runup` is written at", "cannot type")
        self.assertIn("excursion ?::max_runup", out)

    def test_a_named_writer_that_writes_neither_field(self) -> None:
        adapter = EXCURSION_ADAPTER
        for line in ("    booked.max_runup = std::min(path[point], bar_favorable);\n",
                     "    booked.max_drawdown = 0.0;\n",
                     "        survivor.max_runup = bar_favorable;\n",
                     "        survivor.max_drawdown = 0.0;\n"):
            self.assertIn(line, adapter)
            adapter = adapter.replace(line, "")
        self.assert_fails_naming(
            excursion_files(adapter=adapter),
            "`PineExecutionAdapter::submit_long_tick_margin_units`", "write neither field")

    def test_the_source_layers_own_field_of_that_name_is_not_a_row(self) -> None:
        body = ("    risk_.max_drawdown = 1.0;\n"
                "    PineReplayRiskConfig saved;\n"
                "    saved.max_drawdown = 2.0;\n")
        code, out = self.run_tree(excursion_files(adapter=adapter_with(body)))
        self.assertEqual(code, 0, out)
        self.assertIn("row excursions: 6 source-layer writes", out)

    def test_a_comment_or_a_string_writes_nothing(self) -> None:
        body = ('    // lot.max_runup = 0.0;\n'
                '    log("lot.max_drawdown = 0.0; }");\n')
        code, out = self.run_tree(excursion_files(adapter=adapter_with(body)))
        self.assertEqual(code, 0, out)
        self.assertIn("row excursions: 6 source-layer writes", out)


class RepositoryExcursionWriters(unittest.TestCase):
    """The same check over this repository's own source layer."""

    PLANTED = ("\nnamespace pineforge::source {\n"
               "void PineExecutionAdapter::planted_third_writer() {\n"
               "    auto* pine = pine_view_of(host_);\n"
               "    for (auto& lot : pine->pyramid_entries_) lot.max_runup = 0.0;\n"
               "}\n"
               "} // namespace pineforge::source\n")

    def test_the_named_writers_are_the_repositorys_only_ones(self) -> None:
        found = gate.excursion_writes(gate.ROOT)
        self.assertEqual(sorted(found.guarded), sorted(gate.EXCURSION_WRITERS))
        self.assertEqual(gate.excursion_findings(found), [])
        self.assertEqual({(write.struct, write.relative, write.function) for write in found.writes},
                         {(struct, *where) for struct, where in gate.EXCURSION_WRITERS.items()})
        for struct in gate.EXCURSION_WRITERS:
            self.assertEqual({write.name for write in found.writes if write.struct == struct},
                             set(gate.EXCURSION_FIELDS), struct)

    def test_a_third_writer_planted_in_a_copy_of_the_adapter(self) -> None:
        adapter = gate.EXCURSION_WRITERS["PyramidEntry"][0]
        copied = [gate.ENGINE, *gate.coverage.SOURCE_CLASSES,
                  *sorted({where[0] for where in gate.EXCURSION_WRITERS.values()})]
        with tempfile.TemporaryDirectory(prefix="pineforge-seam-rows-") as name:
            root = Path(name)
            for relative in copied:
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(gate.ROOT / relative, root / relative)
            with open(root / adapter, "a", encoding="utf-8") as planted:
                planted.write(self.PLANTED)
            findings = gate.excursion_findings(gate.excursion_writes(root))
        self.assertEqual(len(findings), 1, findings)
        self.assertIn("`PyramidEntry::max_runup`", findings[0])
        self.assertIn("in `PineExecutionAdapter::planted_third_writer`", findings[0])


if __name__ == "__main__":
    unittest.main()
