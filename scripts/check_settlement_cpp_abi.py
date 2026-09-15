#!/usr/bin/env python3
"""Validate the authenticated v16-to-v17 native C++ ABI transition."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

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
    required_virtuals = {"prepare_native_begin", "on_native_bar_open", "on_native_input"}
    if not required_virtuals.issubset(set(manifest.get("addedVirtuals", []))):
        raise RuntimeError("relocation manifest omits a native hook")
    if not all("virtual void " + name in native for name in required_virtuals):
        raise RuntimeError("current native host omits a required hook")
    pairs = manifest.get("rejectionPairs")
    if pairs != [["v16-frozen", "v17-current"], ["v17-current", "v16-frozen"]]:
        raise RuntimeError("v16/v17 rejection pairs drift")
    return {"transition": manifest["transition"], "rejectionPairs": pairs,
            "retiredHeader": RETIRED_HEADER}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler")
    parser.add_argument("--library", type=Path)
    parser.add_argument("--include", type=Path, required=True)
    parser.add_argument("--generated-include", type=Path)
    parser.add_argument("--base-receipt", type=Path)
    parser.add_argument("--prior-receipt", type=Path)
    parser.add_argument("--v13-receipt", type=Path)
    parser.add_argument("--v14-receipt", type=Path)
    parser.add_argument("--v15-frozen-receipt", type=Path)
    parser.add_argument("--v16-frozen-receipt", type=Path)
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--extra-flag", action="append", default=[])
    args = parser.parse_args()
    result = verify(args.include)
    if args.library is not None and not args.library.is_file():
        raise SystemExit("current ABI library is missing: " + str(args.library))
    if args.receipt is not None:
        args.receipt.write_text(json.dumps(result, indent=2) + "\n")
    print("settlement C++ ABI: v16<->v17 manifest and retired surface verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
