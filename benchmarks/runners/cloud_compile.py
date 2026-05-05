#!/usr/bin/env python3
"""Cloud-compile all benchmark strategies via the PyneSys `pyne compile`
service, replacing hand-ported `strategy_pyne.py` files with the
cloud-canonical translation.

Pre-requisites:
    1. A PyneSys API key. Set via:
         export PYNESYS_API_KEY=...
    2. A ready PyneCore workdir (created by the first `pyne` invocation).

Usage:
    # compile all strategy folders that have a strategy.pine
    python benchmarks/runners/cloud_compile.py

    # only one folder
    python benchmarks/runners/cloud_compile.py --only 06-liquidity-sweep

    # force re-compile even if strategy_pyne.py is fresher than .pine
    python benchmarks/runners/cloud_compile.py --force

Per strategy this script:
    1. Removes any pre-existing strategy_pyne.py (hand-port).
    2. Invokes `pyne compile <strategy.pine> --output <tmp>.py`.
    3. Renames the cloud output to strategy_pyne.py so the existing
       run_pynecore.py harness picks it up unchanged.
    4. On any compile error the original hand-port (if any) is
       restored so the suite stays runnable.

The cloud compiler is the source of truth for `.pine -> @pyne` Python
in this benchmark — it removes any "did the human port it correctly?"
ambiguity from the comparison results.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH_DIR = REPO_ROOT / "benchmarks"
WORKDIR = BENCH_DIR / "_workdir"


def find_strategies(only: str | None) -> list[Path]:
    root = BENCH_DIR / "strategies"
    out: list[Path] = []
    for d in sorted(root.iterdir()):
        if not d.is_dir() or d.name.startswith("_"):
            continue
        if only and only not in d.name:
            continue
        if (d / "strategy.pine").exists():
            out.append(d)
    return out


def compile_one(strat_dir: Path, *, force: bool) -> tuple[bool, str]:
    pine = strat_dir / "strategy.pine"
    out_py = strat_dir / "strategy_pyne.py"
    backup = strat_dir / "strategy_pyne.py.bak"

    # Default policy: keep the committed cloud-compiled output. Only
    # re-call the PyneSys API when the user explicitly requests it
    # (--force / REFRESH_COMPILE=1) — every compile costs API credits
    # and the committed artefact is what every other engine's results
    # were generated against. Mtime is intentionally NOT consulted
    # here because freshly-cloned files all share the checkout time.
    if out_py.exists() and not force:
        return True, "skip (committed; --force to refresh)"

    if out_py.exists():
        out_py.replace(backup)

    cmd = [
        "pyne", "-w", str(WORKDIR.resolve()),
        "compile", str(pine.resolve()),
        "--output", str(out_py.resolve()),
        "--force",
    ]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        if backup.exists():
            backup.replace(out_py)
        return False, "timeout"

    if res.returncode != 0:
        if backup.exists():
            backup.replace(out_py)
        # Pyne's CLI dumps a fairly noisy traceback. Pull the most
        # meaningful trailing line (typically `Error: ...`).
        err = res.stderr.strip().splitlines()
        msg = err[-1] if err else f"rc={res.returncode}"
        return False, msg[:200]

    if not out_py.exists():
        if backup.exists():
            backup.replace(out_py)
        return False, "compile reported success but output is missing"

    if backup.exists():
        backup.unlink()
    return True, "ok"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="Substring filter — only compile strategies whose folder name contains this")
    ap.add_argument("--force", action="store_true",
                    help="Re-call the PyneSys API even when a committed strategy_pyne.py "
                         "already exists. Costs API credits per strategy.")
    args = ap.parse_args()

    if not (os.environ.get("PYNESYS_API_KEY") or "").strip():
        cfg = WORKDIR / "config" / "api.toml"
        if not cfg.exists():
            print("ERROR: no API key found.", file=sys.stderr)
            print(f"Set PYNESYS_API_KEY env var or fill api_key in {cfg}", file=sys.stderr)
            return 1

    strategies = find_strategies(args.only)
    if not strategies:
        print("no strategies match", file=sys.stderr)
        return 1

    started = time.time()
    n_ok = n_skip = n_fail = 0
    failed: list[tuple[str, str]] = []

    for i, d in enumerate(strategies, 1):
        ok, msg = compile_one(d, force=args.force)
        if ok:
            if msg.startswith("skip"):
                n_skip += 1
                tag = "SKIP"
            else:
                n_ok += 1
                tag = "OK"
        else:
            n_fail += 1
            failed.append((d.name, msg))
            tag = "FAIL"
        print(f"  [{i:3d}/{len(strategies)}] {d.name:38s}  {tag:4s}  {msg}")

    elapsed = time.time() - started
    print()
    print(f"compiled {n_ok}, skipped {n_skip}, failed {n_fail}  in {elapsed:.1f}s")
    if failed:
        print()
        print("failed strategies:")
        for name, err in failed:
            print(f"  {name}: {err}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
