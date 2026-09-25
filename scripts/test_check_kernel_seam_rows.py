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

R5 lane H-DOCGATES (AUDIT4-opus X10).
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
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
    def __init__(self, adr_rows: str, source: str = SOURCE) -> None:
        self.dir = tempfile.TemporaryDirectory(prefix="pineforge-seam-rows-")
        self.root = Path(self.dir.name)
        self.write("include/pineforge/engine.hpp", ENGINE)
        for relative, text in SOURCE_HEADERS.items():
            self.write(relative, text)
        self.write("src/source/pine_strategy_host.cpp", source)
        self.write(gate.ADR, ADR_HEAD + adr_rows)

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
def tree(adr_rows: str, source: str = SOURCE):
    made = Tree(adr_rows, source)
    try:
        yield made
    finally:
        made.close()


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


if __name__ == "__main__":
    unittest.main()
