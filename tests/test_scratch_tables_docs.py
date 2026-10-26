"""Scratch policy page is published through the existing Doxygen page/nav list.

Red on the frozen base: docs/pages/scratch-tables.md does not exist. Constants
below are hand arithmetic from the brief, not output from a layout run.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def test_scratch_page_is_named_in_the_site_index_and_doxygen_navigation():
    page = (ROOT / "docs/pages/scratch-tables.md").read_text()
    assert page.startswith("# Scratch tables {#scratch_tables}\n")
    doxyfile = (ROOT / "docs/Doxyfile").read_text()
    inputs = re.search(r"(?m)^INPUT[ \t]*=(?:[^\n]*\\\n)*[^\n]*", doxyfile).group(0)
    assert inputs.split().count("pages/scratch-tables.md") == 1
    index = (ROOT / "docs/pages/index.md").read_text()
    assert "[Scratch tables](@ref scratch_tables)" in index
    assert "GENERATE_TREEVIEW      = YES" in doxyfile


def test_published_page_keeps_hand_derived_reservations_and_honest_scope():
    page = (ROOT / "docs/pages/scratch-tables.md").read_text()
    for row in (
        "| 1,000 | 32,000 | 8 | 14,932 | 61,161,472 |",
        "| 100,000 | 3,200,000 | 782 | 15,706 | 64,331,776 |",
        "| 1,000,000 | 32,000,000 | 7,813 | 22,737 | 93,130,752 |",
        "| 100,000,000 | 3,200,000,000 | 781,250 | 796,174 | 3,261,128,704 |",
    ):
        assert row in page
    assert "not measured bounds" in page
    assert "**planned**" in page
    for term in ("PINEFORGE_RUN_JSON_PROFILE", "scratch_capacity_exceeded", "scratch_io_failed",
                 "SIGXFSZ", "SIG_IGN", "RLIMIT_FSIZE", "sink-ON", "position-tail spill (L18)",
                 "durable runner"):
        assert term in page
    assert ("Scratch tables use a single allocation before the run, with enforced count and byte "
            "capacities and typed failure on exhaustion. Historical distinct IDs, ID pairs, "
            "consumed-exit combinations and replacement successors consume that budget; the "
            "default replacement reserve depends on input-bar count. This is not a guarantee "
            "that only OHLCV memory scales.") in page
