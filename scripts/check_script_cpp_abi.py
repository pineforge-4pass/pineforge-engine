#!/usr/bin/env python3
"""Check the v17 source-host C++ boundary after adapter lowering."""
from __future__ import annotations

import re
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler")
    parser.add_argument("--library")
    parser.add_argument("--include")
    parser.add_argument("--generated-include")
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.parse_args()
    engine = (ROOT / "include/pineforge/engine.hpp").read_text()
    host = (ROOT / "include/pineforge/source/pine_strategy_host.hpp").read_text()
    if "class PineStrategyHost : public NativeStrategyHost" not in host:
        raise SystemExit("script ABI: source host is not native-bound")
    if re.search(r"(?<!~)\bBacktestEngine\s*\(\s*\)\s*;", engine):
        raise SystemExit("script ABI: default execution-owner constructor remains")
    if "NativeConsumerBindTag" not in engine:
        raise SystemExit("script ABI: explicit native construction tag is missing")
    if (ROOT / "include/pineforge/source/pine_pending_intent.hpp").exists():
        raise SystemExit("script ABI: retired source order header remains")
    print("script C++ ABI: v17 native-bound source host verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
