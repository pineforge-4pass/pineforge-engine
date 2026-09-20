#!/usr/bin/env python3
"""Fail-closed v18 ownership guard, with an optional real ABI pair control."""
from __future__ import annotations

import argparse
from pathlib import Path
import re

from cpp_abi_pairing import PairingError, enforce_receipt_mode, execute_v16_v18_pair


ROOT = Path(__file__).resolve().parents[1]


def clean(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def body(text: str, pattern: str, name: str) -> str:
    matches = list(re.finditer(pattern, text))
    if len(matches) != 1:
        raise ValueError(name + " requires exactly one definition")
    start = matches[0].end()
    depth = 1
    for at in range(start, len(text)):
        depth += (text[at] == "{") - (text[at] == "}")
        if depth == 0:
            return text[start:at]
    raise ValueError(name + " has an unclosed body")


def standalone_scope(text: str, outer: str, version: str) -> str:
    owner = body(clean(text), r"namespace\s+" + re.escape(outer) + r"\s*\{", outer)
    return body(owner, r"inline\s+namespace\s+" + re.escape(version) + r"\s*\{", version)


def check(root: Path = ROOT) -> None:
    engine = (root / "include/pineforge/engine.hpp").read_text()
    native = (root / "include/pineforge/native_host.hpp").read_text()
    adapter = (root / "include/pineforge/source/pine_adapter.hpp").read_text()
    source_hash = clean((root / "src/source/pine_state_hash.cpp").read_text())
    generic_hash = clean((root / "src/engine_state_hash.cpp").read_text())
    stream_hash = clean((root / "src/engine_stream.cpp").read_text())
    epochs = re.findall(r"inline\s+namespace\s+(engine_script_run_v\d+)\s*\{", clean(engine))
    if epochs != ["engine_script_run_v18"]:
        raise ValueError("BacktestEngine requires engine_script_run_v18")
    if "PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V18 1" not in native:
        raise ValueError("native host capability must remain v18")
    if "class PineStrategyHost : public NativeStrategyHost" not in (
            root / "include/pineforge/source/pine_strategy_host.hpp").read_text():
        raise ValueError("source host must remain native-bound")
    if (root / "include/pineforge/source/pine_pending_intent.hpp").exists():
        raise ValueError("retired source PendingOrder header is still installed")
    if 'kSourceAdapterDomain[] = "pineforge-source-adapter/v2"' not in adapter:
        raise ValueError("source adapter domain must remain v2")
    source_body = body(source_hash,
                       r"void\s+source::PineStrategyHost::hash_source_extension\(BrokerStateHashSink&\s+f\)\s+const\s*\{",
                       "source hash")
    if not re.match(r"\s*f\.s\(kSourceAdapterDomain\);", source_body):
        raise ValueError("source hash must begin with its adapter domain")
    generic_body = body(generic_hash,
                        r"(?:std::)?uint64_t\s+BacktestEngine::broker_state_hash_from_execution_hash\(\s*(?:std::)?uint64_t\s+execution_hash\)\s+const\s*\{",
                        "broker hash")
    if not re.match(r"\s*BrokerStateHashSink\s+f;\s*f\.s\(\"pineforge-broker-state/v17\"\);", generic_body):
        raise ValueError("broker hash requires the v17 domain")
    stream_body = body(stream_hash,
                       r"uint64_t\s+BacktestEngine::stream_state_hash\(\)\s+const\s*\{",
                       "stream hash")
    compact = re.sub(r"\s+", "", stream_body)
    if compact.count("integer(17);integer(broker_state_hash());") != 1:
        raise ValueError("stream hash requires one unconditional v17 broker fold")
    if "if(false){integer(17);integer(broker_state_hash());}" in compact:
        raise ValueError("stream v17 fold must be unconditional")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--library", type=Path)
    parser.add_argument("--include", type=Path)
    parser.add_argument("--generated-include", type=Path)
    parser.add_argument("--v16-frozen-receipt", type=Path)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--receipt", type=Path)
    receipt_mode = parser.add_mutually_exclusive_group()
    receipt_mode.add_argument("--skip-if-receipt-missing", action="store_true")
    receipt_mode.add_argument("--require-receipts", action="store_true")
    args = parser.parse_args()
    try:
        mode = enforce_receipt_mode(
            (args.v16_frozen_receipt,), skip=args.skip_if_receipt_missing,
            require=args.require_receipts, label="aggregate C++ versions")
        if mode is not None:
            return mode
        check(args.include.resolve().parent if args.include else ROOT)
        requested = [args.compiler, args.library, args.include, args.generated_include,
                     args.v16_frozen_receipt]
        if any(value is not None for value in requested):
            if not all(value is not None for value in requested):
                raise PairingError("runtime ABI control requires compiler, library, include, generated include, and v16 receipt")
            result = execute_v16_v18_pair(
                compiler=args.compiler,
                extra_flags=args.extra_flag,
                current_library=args.library,
                current_include=args.include,
                generated_include=args.generated_include,
                v16_receipt=args.v16_frozen_receipt,
                kind="native",
                artifact_directory=args.receipt.parent if args.receipt else None,
            )
            if args.receipt:
                import json
                args.receipt.parent.mkdir(parents=True, exist_ok=True)
                args.receipt.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    except (PairingError, ValueError) as error:
        raise SystemExit("aggregate C++ versions: " + str(error))
    print("aggregate v18 ownership" + (" and v16/v18 ABI pairs" if args.compiler else "") + " verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
