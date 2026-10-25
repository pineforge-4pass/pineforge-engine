"""Round 5 controls for active write bounds and the two public policy summaries."""
from collections import Counter
import json
from pathlib import Path
import re

import pytest


ROOT = Path(__file__).resolve().parents[1]
INPUTS = (
    "tests/test_cr257_source_exit_scratch_io.cpp",
    "tests/cr150.d/README.md",
    "docs/pages/run-failure-codes.md",
    "docker/run_failure_codes.json",
    "docker/run_failure_codes_diff.json",
)


@pytest.fixture(scope="module", autouse=True)
def declared_inputs():
    paths = [ROOT / relative for relative in INPUTS]
    print("S4E misc contract inputs:\n" + "\n".join(map(str, paths)), flush=True)
    forbidden = tuple(Path.home() / relative for relative in (
        ".config/zsh/keys", ".ssh", ".aws", ".codex", ".config/pineforge"))

    def refuse(path):
        if path.name.endswith(".env") or any(
                path == prefix or prefix in path.parents for prefix in forbidden):
            raise AssertionError(f"refusing protected input: {path}")

    for path in paths:
        refuse(path)
        refuse(path.resolve())


def read_input(relative):
    assert relative in INPUTS, f"undeclared input: {relative}"
    return (ROOT / relative).read_text(encoding="utf-8")


def failure_code_preamble():
    return read_input("docs/pages/run-failure-codes.md").split(
        "<!-- BEGIN generated from docker/run_failure_codes.json:", 1)[0]


def test_engine_run_write_checks_are_active_bounds():
    # These controls must affect the executable's failure count. An info()
    # call with the same numbers is not a write-amplification assertion.
    source = read_input("tests/test_cr257_source_exit_scratch_io.cpp")
    source = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
    body = source.split("void source_bounds(", 1)[1].split("\nint main(", 1)[0]
    for metric in (
        "command.commits", "command.page_pwrite_calls", "command.total_pwrite_calls",
        "command.page_pwrite_bytes", "command.total_pwrite_bytes",
    ):
        calls = re.findall(r'\b(\w+)\s*\(\s*"' + re.escape(metric) + r'"\s*,', body)
        assert calls == ["bound"], f"{metric} must count a violation: {calls}"


def test_cr150_policy_names_terminal_validation_ownership_and_77():
    text = " ".join(read_input("tests/cr150.d/README.md").split())
    for pattern in (
        r"terminal PR-fast validation",
        r"PINEFORGE_PR_FAST_TESTS",
        r"no (?:lane file|fence).*?may follow",
        r"`set_tests_properties\(\.\.\.\)` may name only tests.*?same lane",
        r"SKIP_RETURN_CODE.*?77.*?receipt",
    ):
        assert re.search(pattern, text, re.I), f"missing policy: {pattern}"


def test_failure_code_preamble_counts_match_catalog():
    codes = json.loads(read_input("docker/run_failure_codes.json"))["codes"]
    preamble = failure_code_preamble()
    total = re.search(r"(\d+) named failure codes", preamble)
    assert total, "document the total named failure-code population"
    assert int(total[1]) == len(codes)
    groups = re.findall(r"(\d+)\s+(?:first released|added) in (\d+\.\d+\.\d+)", preamble)
    assert len(groups) == len({version for _, version in groups}), "duplicate release group"
    assert {version: int(count) for count, version in groups} == dict(
        Counter(entry["since"] for entry in codes.values()))
    registry = re.search(r"(\d+) registry entries", preamble)
    assert registry, "document the registry population including its unnamed sentinel"
    assert int(registry[1]) == len(codes) + 1


def test_upcoming_failure_codes_are_announced():
    codes = json.loads(read_input("docker/run_failure_codes.json"))["codes"]
    diff = json.loads(read_input("docker/run_failure_codes_diff.json"))
    releases = {row["entry"]["since"] for row in diff["added"]
                if row["collection"] == "codes"}
    upcoming = {name for name, entry in codes.items() if entry["since"] in releases}
    # Right after a release the diff is reset (from that release, nothing added): no
    # release is upcoming and the page has nothing to announce, so the population is
    # empty by construction and the comparison below holds for it. (The former
    # "assert upcoming" failed exactly there; an assertion that every added code is
    # in `upcoming` would be true by construction, so none is made.)
    announced = set(re.findall(r"`([a-z][a-z0-9_]+)`", failure_code_preamble()))
    assert upcoming <= announced, f"unannounced failure codes: {sorted(upcoming - announced)}"
