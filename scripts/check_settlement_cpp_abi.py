#!/usr/bin/env python3
"""Audit the v17 surface and execute frozen-v16/live-v17 settlement ABI pairs.

This is intentionally a compile/link control, never a JSON-to-JSON manifest
comparison.  It consumes every historical receipt supplied by CMake and links
the actual host-ab9714b archive in both stale directions, after checking the
authenticated v16-to-v17 relocation manifest and retired surface.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from cpp_abi_pairing import (
    PairingError, audit_prepared_receipt, enforce_receipt_mode,
    execute_v16_v17_pair,
)

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tests/fixtures/native_cpp_abi/host-ab9714b/relocation-manifest-v16-v17.json"
RETIRED_HEADER = "pineforge/source/pine_pending_intent.hpp"
_OLD = "legacy"
_RUN = _OLD + "_run_"
_STREAM = _OLD + "_stream_"
RETIRED_SEAMS = (
    _RUN + "simple", _RUN + "tf", _RUN + "rich",
    _STREAM + "begin", _STREAM + "push_bar", _STREAM + "push_tick",
    _STREAM + "push_ticks", _STREAM + "advance_time", _STREAM + "end",
    "validate_source_lifecycle", "preflight_source_lifecycle",
    "apply_source_pre_close_lifecycle", "apply_source_pending_removals",
)


def verify(include: Path) -> dict:
    engine = (include / "pineforge/engine.hpp").read_text()
    native = (include / "pineforge/native_host.hpp").read_text()
    manifest = json.loads(MANIFEST.read_text())
    if manifest.get("schema") != "pineforge-r4-d-relocation/v1":
        raise RuntimeError("unexpected v16-v17 relocation schema")
    if manifest.get("transition") != {
            "from": "engine_script_run_v16", "to": "engine_script_run_v17"}:
        raise RuntimeError("v16-v17 relocation transition drift")
    if "inline namespace engine_script_run_v17" not in engine:
        raise RuntimeError("current engine epoch is not v17")
    if (include / RETIRED_HEADER).exists():
        raise RuntimeError("retired source order header remains installed")
    present = [name for name in RETIRED_SEAMS if name in engine]
    if present:
        raise RuntimeError("retired engine seams remain: " + ", ".join(present))
    required_virtuals = {
        "prepare_native_begin", "on_native_bar_open", "on_native_input",
        "on_native_tick",
    }
    if not required_virtuals.issubset(set(manifest.get("addedVirtuals", []))):
        raise RuntimeError("relocation manifest omits a native hook")
    if not all("virtual void " + name in native for name in required_virtuals):
        raise RuntimeError("current native host omits a required hook")
    pairs = manifest.get("rejectionPairs")
    if pairs != [["v16-frozen", "v17-current"], ["v17-current", "v16-frozen"]]:
        raise RuntimeError("v16/v17 rejection pairs drift")
    return {"transition": manifest["transition"], "rejectionPairs": pairs,
            "retiredHeader": RETIRED_HEADER}


def verify_pair(args: argparse.Namespace) -> dict:
    surface = verify(args.include)
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
        "v16V17Surface": surface,
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
    receipt_mode = parser.add_mutually_exclusive_group()
    receipt_mode.add_argument("--skip-if-receipt-missing", action="store_true")
    receipt_mode.add_argument("--require-receipts", action="store_true")
    args = parser.parse_args()
    try:
        mode = enforce_receipt_mode(
            (args.base_receipt, args.prior_receipt, args.v13_receipt,
             args.v14_receipt, args.v15_frozen_receipt, args.v16_frozen_receipt),
            skip=args.skip_if_receipt_missing, require=args.require_receipts,
            label="settlement C++ ABI")
        if mode is not None:
            return mode
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
