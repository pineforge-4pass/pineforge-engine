#!/usr/bin/env python3
"""One-shot: prepend Apache-2.0 SPDX license header to bench strategy.pine files.

Skips files that already contain 'SPDX-License-Identifier: Apache-2.0' in the
first 5 lines.  For all others, prepends a 6-line header block then the
original file content (including the original //@version=6 line).

Header format:
    // This Pine Script® code is licensed under Apache-2.0
    // SPDX-License-Identifier: Apache-2.0
    // © PineForge contributors 2026
    //
    // PF probe <NN> — <slug-derived description>
    //

Run from the engine repo root:
    uv run python benchmarks/runners/_add_pine_license.py

After running, commit the updated strategy.pine files to the assets submodule.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BENCH = REPO_ROOT / "benchmarks"
sys.path.insert(0, str(BENCH))
from paths import STRATEGIES  # noqa: E402

_SPDX_MARKER = "SPDX-License-Identifier: Apache-2.0"


def _slug_to_description(slug: str) -> str:
    """Convert '01-sma-cross' -> 'sma cross strategy'."""
    # Strip leading NN-
    without_num = re.sub(r"^\d+-", "", slug)
    # Remove trailing '-NN' probe suffix like '-01' or '-01a' or '-01b'
    without_suffix = re.sub(r"-\d+[a-z]?$", "", without_num)
    return without_suffix.replace("-", " ") + " strategy"


def _probe_num(slug: str) -> str:
    """Extract leading number from slug: '01-sma-cross' -> '01'."""
    m = re.match(r"^(\d+)", slug)
    return m.group(1) if m else "??"


def add_header(pine_path: Path) -> bool:
    """Add SPDX header to file if missing. Returns True if modified."""
    text = pine_path.read_text(encoding="utf-8")
    first_lines = text.splitlines()[:5]
    if any(_SPDX_MARKER in line for line in first_lines):
        return False

    slug = pine_path.parent.name
    num = _probe_num(slug)
    desc = _slug_to_description(slug)

    header = (
        "// This Pine Script® code is licensed under Apache-2.0\n"
        f"// {_SPDX_MARKER}\n"
        "// © PineForge contributors 2026\n"
        "//\n"
        f"// PF probe {num} — {desc}\n"
        "//\n"
    )
    pine_path.write_text(header + text, encoding="utf-8")
    return True


def main() -> int:
    n_added = n_skipped = n_fail = 0
    for d in sorted(STRATEGIES.iterdir()):
        if not d.is_dir() or d.name.startswith("_"):
            continue
        pine_path = d / "strategy.pine"
        if not pine_path.exists():
            print(f"  [{d.name}] SKIP (no strategy.pine)")
            continue
        try:
            modified = add_header(pine_path)
            if modified:
                print(f"  [{d.name}] ADDED header")
                n_added += 1
            else:
                print(f"  [{d.name}] already has header (skipped)")
                n_skipped += 1
        except Exception as e:
            print(f"  [{d.name}] FAIL  {e}")
            n_fail += 1

    print(f"\nadded {n_added}, already-present {n_skipped}, failed {n_fail}")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
