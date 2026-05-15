#!/usr/bin/env python3
"""Compare cloud-compiled strategy_pyne.py vs prior committed version.

Diffs current working tree against HEAD (the committed baseline snapshot)
for each strategies/*/strategy_pyne.py, emits markdown summary to stdout.

Note: The plan called for diffing against stash@{0}, but since the assets
submodule working tree was clean at the start of the refresh (no local
modifications), the committed HEAD is the equivalent baseline.
"""
from __future__ import annotations
import subprocess
import sys
from pathlib import Path

STRATEGIES = Path(__file__).resolve().parent.parent / "assets" / "strategies"


def diff_one(rel: Path) -> tuple[bool, int, int]:
    """Return (changed, added_lines, removed_lines)."""
    try:
        out = subprocess.run(
            ["git", "-C", str(STRATEGIES.parent), "diff", "--numstat", "HEAD", "--",
             str(rel.relative_to(STRATEGIES.parent))],
            check=True, capture_output=True, text=True,
        )
    except subprocess.CalledProcessError:
        return False, 0, 0
    if not out.stdout.strip():
        return False, 0, 0
    added, removed, _ = out.stdout.strip().split()
    return True, int(added), int(removed)


def main() -> None:
    print("# Cloud-compiler drift vs prior pin\n")
    print("| Strategy | Changed | +lines | -lines |")
    print("|---|---|---|---|")
    changed = 0
    for d in sorted(STRATEGIES.iterdir()):
        if not d.is_dir() or d.name.startswith("_"):
            continue
        f = d / "strategy_pyne.py"
        if not f.exists():
            continue
        ch, a, r = diff_one(f)
        if ch:
            changed += 1
        flag = "🟡 yes" if ch else "🟢 no"
        print(f"| {d.name} | {flag} | {a} | {r} |")
    print(f"\n**Total changed:** {changed}")


if __name__ == "__main__":
    main()
