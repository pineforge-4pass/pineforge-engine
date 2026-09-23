#!/usr/bin/env python3
"""Audit the v19 surface and execute frozen-v18/live-v19 settlement ABI pairs.

This is intentionally a compile/link control, never a JSON-to-JSON manifest
comparison.  It consumes every historical receipt supplied by CMake and links
the actual host-fc7aad6 archive in both stale directions, after checking the
authenticated v18-to-v19 relocation manifest, the retired surface the
v16-to-v18 relocation removed, and the host hooks it added.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from cpp_abi_pairing import (
    CURRENT_EPOCH, PairingError, audit_prepared_receipt, enforce_receipt_mode,
    execute_frozen_pair,
)

ROOT = Path(__file__).resolve().parents[1]
# The active transition, and the historical one whose retired surface and
# added hooks the live tree must still show.
MANIFEST = ROOT / "tests/fixtures/native_cpp_abi/host-fc7aad6/relocation-manifest-v18-v19.json"
V16_V18_MANIFEST = ROOT / "tests/fixtures/native_cpp_abi/host-ab9714b/relocation-manifest-v16-v18.json"
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
    if manifest.get("schema") != "pineforge-epoch-relocation/v1":
        raise RuntimeError("unexpected v18-v19 relocation schema")
    if manifest.get("transition") != {
            "from": "engine_script_run_v18", "to": "engine_script_run_v19"}:
        raise RuntimeError("v18-v19 relocation transition drift")
    if "inline namespace " + CURRENT_EPOCH not in engine:
        raise RuntimeError("current engine epoch is not v19")
    capability = manifest.get("hostCapability") or {}
    if capability != {"from": "PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V18",
                      "to": "PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19"}:
        raise RuntimeError("v18-v19 host capability transition drift")
    if "#define " + capability["to"] + " 1" not in native:
        raise RuntimeError("current native host omits its v19 capability macro")
    if capability["from"] in native:
        raise RuntimeError("current native host still defines the v18 capability macro")
    historical = json.loads(V16_V18_MANIFEST.read_text())
    if historical.get("schema") != "pineforge-r4-d-relocation/v1" or historical.get(
            "transition") != {"from": "engine_script_run_v16", "to": "engine_script_run_v18"}:
        raise RuntimeError("historical v16-v18 relocation manifest drift")
    if (include / RETIRED_HEADER).exists():
        raise RuntimeError("retired source order header remains installed")
    present = [name for name in RETIRED_SEAMS if name in engine]
    if present:
        raise RuntimeError("retired engine seams remain: " + ", ".join(present))
    required_virtuals = {
        "prepare_native_begin", "on_native_bar_open", "on_native_input",
        "on_native_tick", "on_native_timeframe_bar", "on_native_margin_call",
        "on_native_recalculate", "on_native_sub_bar",
    }
    # Policy hooks answer a value, so their declaration is not "virtual void".
    required_answering_virtuals = {
        "resolve_margin_requirement", "margin_check_allowed",
        "resolve_anchored_level",
    }
    declared = set(historical.get("addedVirtuals", []))
    if not (required_virtuals | required_answering_virtuals).issubset(declared):
        raise RuntimeError("relocation manifest omits a native hook")
    if not all("virtual void " + name in native for name in required_virtuals):
        raise RuntimeError("current native host omits a required hook")
    if not all(re.search(r"virtual\s+[^;{]+?\b" + name + r"\s*\(", native)
               for name in required_answering_virtuals):
        raise RuntimeError("current native host omits a required policy hook")
    # N5: the generic host hash extension is a BacktestEngine virtual, so the
    # engine header carries it, beside the deprecated spelling it forwards to.
    required_engine_virtuals = {"hash_host_extension"}
    if not required_engine_virtuals.issubset(declared):
        raise RuntimeError("relocation manifest omits an engine hook")
    if not all("virtual void " + name + "(BrokerStateHashSink&) const;" in engine
               for name in required_engine_virtuals | {"hash_source_extension"}):
        raise RuntimeError("current engine omits a required hash extension hook")
    for key in ("addedVirtuals", "removedVirtuals"):
        if manifest.get(key) != []:
            raise RuntimeError("v18-v19 relocation " + key + " drift: v19 moves no host hook")
    pairs = manifest.get("rejectionPairs")
    if pairs != [["v18-frozen", "v19-current"], ["v19-current", "v18-frozen"]]:
        raise RuntimeError("v18/v19 rejection pairs drift")
    return {"transition": manifest["transition"], "rejectionPairs": pairs,
            "addedStorage": manifest.get("addedStorage", []),
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
        audit_prepared_receipt(args.v18_frozen_receipt, "frozen v18"),
    ]
    pair = execute_frozen_pair(
        compiler=args.compiler,
        extra_flags=args.extra_flag,
        current_library=args.library,
        current_include=args.include,
        generated_include=args.generated_include,
        frozen_receipt=args.v18_frozen_receipt,
        kind="native",
        artifact_directory=args.receipt.parent,
    )
    return {
        "schemaVersion": "pineforge-settlement-abi/v2",
        "v18V19Surface": surface,
        "historicalInputs": inputs,
        "v18V19Pair": pair,
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
    parser.add_argument("--v18-frozen-receipt", type=Path, required=True)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--receipt", type=Path, required=True)
    receipt_mode = parser.add_mutually_exclusive_group()
    receipt_mode.add_argument("--skip-if-receipt-missing", action="store_true")
    receipt_mode.add_argument("--require-receipts", action="store_true")
    args = parser.parse_args()
    try:
        mode = enforce_receipt_mode(
            (args.base_receipt, args.prior_receipt, args.v13_receipt,
             args.v14_receipt, args.v15_frozen_receipt, args.v16_frozen_receipt,
             args.v18_frozen_receipt),
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
          f"{summary['linked']} positive links; {summary['rejected']} v18/v19 rejections; "
          "no executable run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
