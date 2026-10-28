"""Scratch policy page is published through the existing Doxygen page/nav list."""
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


def test_published_page_keeps_byte_budget_and_honest_scope():
    page = (ROOT / "docs/pages/scratch-tables.md").read_text()
    assert "physical bytes, not rows" in page
    assert "population estimates do not refuse a run" in page
    assert "**planned**" in page
    for term in ("PINEFORGE_RUN_JSON_PROFILE", "scratch_capacity_exceeded", "scratch_io_failed",
                 "SIGXFSZ", "SIG_IGN", "RLIMIT_FSIZE", "sink-ON", "position-tail spill (L18)",
                 "durable runner"):
        assert term in page
    assert ("Scratch tables use a shared byte allowance before the run, with typed failure "
            "on exhaustion.") in page
