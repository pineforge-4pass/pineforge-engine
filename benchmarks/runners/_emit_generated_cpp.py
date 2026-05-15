#!/usr/bin/env python3
"""One-shot: transpile all 75 bench strategy.pine -> generated.cpp.

Run this ONLY when the codegen output changes and you need to re-emit all
generated.cpp files before committing them to the assets submodule.

Requires:
  - sibling pineforge-codegen repo at ../pineforge-codegen (or set PINEFORGE_CODEGEN_PATH)

After running, commit the updated generated.cpp files to the assets submodule
(not the engine repo).

Usage:
    PINEFORGE_CODEGEN_PATH=/path/to/pineforge-codegen \\
    uv run python benchmarks/runners/_emit_generated_cpp.py
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BENCH = REPO_ROOT / "benchmarks"

# Add codegen to sys.path before importing strategies.
CODEGEN_PATH = Path(os.environ.get("PINEFORGE_CODEGEN_PATH",
                                   str(REPO_ROOT.parent / "pineforge-codegen")))
if not CODEGEN_PATH.exists():
    print(
        f"ERROR: pineforge-codegen not found at {CODEGEN_PATH}\n"
        "Set PINEFORGE_CODEGEN_PATH env var or check out the sibling repo.",
        file=sys.stderr,
    )
    sys.exit(1)
sys.path.insert(0, str(CODEGEN_PATH))

sys.path.insert(0, str(BENCH))
from paths import STRATEGIES  # noqa: E402

try:
    from pineforge_codegen import transpile  # type: ignore
except ImportError as e:
    print(f"ERROR: cannot import pineforge_codegen: {e}", file=sys.stderr)
    sys.exit(1)

n_ok = n_fail = 0
for d in sorted(STRATEGIES.iterdir()):
    if not d.is_dir() or d.name.startswith("_"):
        continue
    pine_path = d / "strategy.pine"
    if not pine_path.exists():
        print(f"  [{d.name}] SKIP (no strategy.pine)")
        continue
    try:
        pine = pine_path.read_text()
        cpp = transpile(pine)
        (d / "generated.cpp").write_text(cpp)
        print(f"  [{d.name}] OK ({len(cpp)} chars)")
        n_ok += 1
    except Exception as e:
        msg = str(e).splitlines()[0][:200]
        print(f"  [{d.name}] FAIL  {msg}")
        n_fail += 1

print(f"\nemitted {n_ok}, failed {n_fail}")
if n_fail:
    sys.exit(1)
