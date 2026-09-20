#!/usr/bin/env python3
"""Compile/link the generated source-host ABI against frozen v16 and live v18."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

from cpp_abi_pairing import PairingError, enforce_receipt_mode, execute_v16_v18_pair


def verify_source_shape(include: Path) -> None:
    engine = (include / "pineforge" / "engine.hpp").read_text()
    host = (include / "pineforge" / "source" / "pine_strategy_host.hpp").read_text()
    if "class PineStrategyHost : public NativeStrategyHost" not in host:
        raise PairingError("source host is not native-bound")
    if re.search(r"(?<!~)\bBacktestEngine\s*\(\s*\)\s*;", engine):
        raise PairingError("legacy default execution-owner constructor remains")
    if "NativeConsumerBindTag" not in engine:
        raise PairingError("explicit native construction tag is missing")
    if (include / "pineforge" / "source" / "pine_pending_intent.hpp").exists():
        raise PairingError("retired source order header remains installed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--include", type=Path, required=True)
    parser.add_argument("--generated-include", type=Path, required=True)
    parser.add_argument("--v16-frozen-receipt", type=Path, required=True)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--receipt", type=Path, required=True)
    receipt_mode = parser.add_mutually_exclusive_group()
    receipt_mode.add_argument("--skip-if-receipt-missing", action="store_true")
    receipt_mode.add_argument("--require-receipts", action="store_true")
    args = parser.parse_args()
    try:
        mode = enforce_receipt_mode(
            (args.v16_frozen_receipt,), skip=args.skip_if_receipt_missing,
            require=args.require_receipts, label="script C++ ABI")
        if mode is not None:
            return mode
        verify_source_shape(args.include)
        result = execute_v16_v18_pair(
            compiler=args.compiler,
            extra_flags=args.extra_flag,
            current_library=args.library,
            current_include=args.include,
            generated_include=args.generated_include,
            v16_receipt=args.v16_frozen_receipt,
            kind="script",
            artifact_directory=args.receipt.parent,
        )
    except PairingError as error:
        raise SystemExit("script C++ ABI: " + str(error))
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print("script C++ ABI: source-host v16/v18 acceptance and bidirectional rejection pairs passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
