#!/usr/bin/env python3
"""Execute the authenticated frozen-v16/live-v17 settlement ABI pairs.

This is intentionally a compile/link control, never a JSON-to-JSON manifest
comparison.  It consumes every historical receipt supplied by CMake and links
the actual host-ab9714b archive in both stale directions.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from cpp_abi_pairing import PairingError, audit_prepared_receipt, execute_v16_v17_pair


def verify_pair(args: argparse.Namespace) -> dict:
    inputs = [
        audit_prepared_receipt(args.base_receipt, "R2 base"),
        audit_prepared_receipt(args.prior_receipt, "R3 prior"),
        audit_prepared_receipt(args.v13_receipt, "native v13"),
        audit_prepared_receipt(args.v14_receipt, "native v14"),
        audit_prepared_receipt(args.v15_frozen_receipt, "frozen v15"),
        audit_prepared_receipt(args.v16_frozen_receipt, "frozen v16"),
    ]
    pair = execute_v16_v17_pair(
        compiler=args.compiler,
        extra_flags=args.extra_flag,
        current_library=args.library,
        current_include=args.include,
        generated_include=args.generated_include,
        v16_receipt=args.v16_frozen_receipt,
        kind="native",
        artifact_directory=args.receipt.parent,
    )
    return {
        "schemaVersion": "pineforge-settlement-abi/v2",
        "historicalInputs": inputs,
        "v16V17Pair": pair,
        "summary": {
            "compiled": len(pair["compiles"]),
            "linked": sum(row["outcome"] == "linked" for row in pair["links"]),
            "rejected": sum(row["outcome"] == "expected-rejection" for row in pair["links"]),
            "executedBinaries": 0,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--include", type=Path, required=True)
    parser.add_argument("--generated-include", type=Path, required=True)
    parser.add_argument("--base-receipt", type=Path, required=True)
    parser.add_argument("--prior-receipt", type=Path, required=True)
    parser.add_argument("--v13-receipt", type=Path, required=True)
    parser.add_argument("--v14-receipt", type=Path, required=True)
    parser.add_argument("--v15-frozen-receipt", type=Path, required=True)
    parser.add_argument("--v16-frozen-receipt", type=Path, required=True)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = verify_pair(args)
    except PairingError as error:
        raise SystemExit("settlement C++ ABI: " + str(error))
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    summary = result["summary"]
    print(f"settlement C++ ABI: {summary['compiled']} callers compiled; "
          f"{summary['linked']} positive links; {summary['rejected']} v16/v17 rejections; "
          "no executable run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
