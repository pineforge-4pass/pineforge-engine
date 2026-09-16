#!/usr/bin/env python3
"""Maintainer tool: refresh committed A29 base/inventory/ledger fixtures."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import check_twin_parity as parity


ROOT = Path(__file__).resolve().parents[1]

OBSERVABLE_REWRITES = {
    "test_direct_short_reversal_affordability": "owner-seeded margin fixtures are rebuilt as public command tapes",
    "test_engine_risk": "protected risk-latch reads are rewritten to public trade and position outcomes",
    "test_entry_bar_margin_path": "owner-seeded margin checkpoints are rewritten to public margin rows",
    "test_exit_activation_routes": "retired pending-leg reads are rewritten to public bracket trades",
    "test_exit_leg_activation": "owner activation bounds are rewritten to public pending/trade projections",
    "test_exit_leg_lifecycle_integration": "private lifecycle drives are rewritten to source commands",
    "test_exit_lifecycle_availability": "private lifecycle availability is rewritten to public trade timing",
    "test_exit_lifecycle_clock": "fixture-owner clock reads are rewritten through the fixture facade",
    "test_exit_lifecycle_reflection": "retired reflection fields are rewritten to the live lifecycle facade",
    "test_integration": "legacy owner reads in the integration TU are rewritten to source-host projections",
    "test_live_pending_order_mirror": "PendingOrder reads are rewritten to the frozen public C row",
    "test_live_state_hash": "retired source-book mutations are rewritten to adapter-owned state transitions",
    "test_margin_admission_gate": "private admission-book mutations are rewritten to public command outcomes",
    "test_margin_call": "owner-seeded margin scenarios are rewritten to public trade and liquidation rows",
    "test_order_birth_provenance": "retired order objects are rewritten to adapter birth receipts",
    "test_percent_equity_open_entry_fee": "owner sizing reads are rewritten to public fills and fee rows",
    "test_reservation_expansion": "private reservation objects are rewritten to pending and trade projections",
    "test_settlement_observation_boundary": "private settlement seams are rewritten to Applied trade observations",
    "test_small_money_margin_residual": "owner-seeded residual state is rewritten to a real opening tape",
}


def git(*args: str) -> str:
    result = subprocess.run(["git", "-C", str(ROOT), *args], text=True,
                            capture_output=True, timeout=60)
    if result.returncode:
        raise SystemExit(result.stderr.strip())
    return result.stdout


def assertion_lines(path: Path) -> list[int]:
    return [row.line for row in parity.extract_assertions(
        path.read_text(), "tests/" + path.name)]


def rewrite_covering_lines(section: str) -> str:
    counters: dict[str, int] = {}
    cache: dict[str, list[int]] = {}
    pattern = re.compile(
        r"(?P<path>tests/test_[A-Za-z0-9_]+_l[4-9][A-Za-z0-9_]*\.cpp):\d+")

    def replace(match: re.Match[str]) -> str:
        relative = match["path"]
        lines = cache.setdefault(relative, assertion_lines(ROOT / relative))
        if not lines:
            raise SystemExit("covering twin has no assertions: " + relative)
        index = counters.get(relative, 0)
        counters[relative] = index + 1
        return relative + ":" + str(lines[index % len(lines)])

    return pattern.sub(replace, section)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--ledger", type=Path, required=True)
    args = parser.parse_args()
    inventory = json.loads(args.inventory.read_text())
    inventory["schema"] = "pineforge-r4-d-twin-inventory/v2"
    inventory["base"] = parity.BASE
    names = [name for name in inventory["removed"] if name not in parity.A24_NAMES]
    tests: dict[str, dict] = {}
    for name in names:
        source = git("show", f"{parity.BASE}:tests/{name}.cpp")
        rows = parity.extract_assertions(source, "tests/" + name + ".cpp")
        tests[name] = {
            "sourceSha256": hashlib.sha256(source.encode()).hexdigest(),
            "assertions": [{"line": row.line, "text": row.text} for row in rows],
        }
    rewrites = {}
    for name, reason in OBSERVABLE_REWRITES.items():
        twin = parity.find_twin(ROOT / "tests", name)
        twin_rows = parity.extract_assertions(twin.read_text(), "tests/" + twin.name)
        base_rows = tests[name]["assertions"]
        rewrites[name] = {
            "twin": twin.name,
            "baseAssertions": len(base_rows),
            "twinAssertions": len(twin_rows),
            "twinAssertionSha256": parity._assertion_digest(twin_rows),
            "reason": reason,
        }
    inventory["observableRewrites"] = rewrites
    tree = git("rev-parse", parity.BASE + "^{tree}").strip()
    manifest = {
        "schema": "pineforge-r4-d-twin-base/v2",
        "base": parity.BASE,
        "tree": tree,
        "assertionSyntax": "CHECK*/REQUIRE*/EXPECT*/assert; definitions excluded",
        "tests": tests,
    }
    ledger = args.ledger.read_text()
    start = ledger.find(parity.APPENDIX_HEADING)
    if start < 0:
        raise SystemExit("source ledger lacks Appendix 5")
    section = ledger[start:]
    following = re.search(r"^##\s+", section[len(parity.APPENDIX_HEADING):], re.M)
    if following:
        section = section[:len(parity.APPENDIX_HEADING) + following.start()]
    section = rewrite_covering_lines(section)
    identity_twin = ROOT / "tests/test_pending_order_identity_l4d.cpp"
    identity_lines = assertion_lines(identity_twin)
    if len(identity_lines) < 3:
        raise SystemExit("pending-order identity twin lacks public assertions")
    section = section.rstrip() + (
        "\n| tests/test_pending_order_identity.cpp:93-463 | 97 CHECKs | "
        "retired pending-owner/OCA mutation matrix | the base drives the deleted "
        "PendingOrder book and private matcher directly; the switched twin uses "
        "real source commands and observes request incarnations, OCA identity, "
        "trade rows and live lots | "
        f"tests/test_pending_order_identity_l4d.cpp:{identity_lines[0]} public request identity; "
        f"tests/test_pending_order_identity_l4d.cpp:{identity_lines[1]} public incarnation; "
        f"tests/test_pending_order_identity_l4d.cpp:{identity_lines[2]} public cohort identity |\n")
    preamble = (
        "# R4-D twin-parity ledger (repo-local CI fixture)\n\n"
        "Extracted from the root-approved deletion ledger for base `" + parity.BASE
        + "`. This file is consumed directly by CI; it has no campaign-path dependency.\n\n")
    (ROOT / "tests/twin_parity_inventory.json").write_text(
        json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    (ROOT / "tests/twin_parity_base.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (ROOT / "tests/twin_parity_ledger.md").write_text(preamble + section.rstrip() + "\n")
    print("wrote repo-local twin parity fixtures for", len(names), "tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
