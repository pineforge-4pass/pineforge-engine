#!/usr/bin/env python3
"""Fail-closed v19 ownership guard, with an optional real ABI pair control."""
from __future__ import annotations

import argparse
from pathlib import Path
import re

from cpp_abi_pairing import PairingError, enforce_receipt_mode, execute_frozen_pair


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
    if epochs != ["engine_script_run_v19"]:
        raise ValueError("BacktestEngine requires engine_script_run_v19")
    if "PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19 1" not in native:
        raise ValueError("native host capability must remain v19")
    if "class PineStrategyHost : public NativeStrategyHost" not in (
            root / "include/pineforge/source/pine_strategy_host.hpp").read_text():
        raise ValueError("source host must remain native-bound")
    if (root / "include/pineforge/source/pine_pending_intent.hpp").exists():
        raise ValueError("retired source PendingOrder header is still installed")
    # R5 lane V19-E: the source extension folds live adapter state (v4).
    if 'kSourceAdapterDomain[] = "pineforge-source-adapter/v4"' not in adapter:
        raise ValueError("source adapter domain must remain v4")
    # N5: the fold's host seam is generic. BacktestEngine declares
    # hash_host_extension beside the deprecated hash_source_extension spelling,
    # the projection calls the generic one only, its default forwards to the
    # deprecated one (whose default is the established marker), and the sink a
    # host folds through is a complete public type of this epoch.
    engine_clean = clean(engine)
    for name in ("hash_host_extension", "hash_source_extension"):
        if len(re.findall(r"\bvirtual\s+void\s+" + name
                          + r"\(BrokerStateHashSink&\)\s+const\s*;", engine_clean)) != 1:
            raise ValueError(name + " must be a v19 BacktestEngine virtual")
    if len(re.findall(r"\bclass\s+BrokerStateHashSink\s*\{", engine_clean)) != 1:
        raise ValueError("BrokerStateHashSink must be a complete public type")
    if re.search(r"\bclass\s+BrokerStateHashSink\s*\{", clean(
            (root / "src/broker_state_hash_internal.hpp").read_text())):
        raise ValueError("BrokerStateHashSink must have one definition, the public one")
    host_default = body(generic_hash,
                        r"void\s+BacktestEngine::hash_host_extension\(BrokerStateHashSink&\s+sink\)\s+const\s*\{",
                        "generic host hash default")
    if re.sub(r"\s+", "", host_default) != "hash_source_extension(sink);":
        raise ValueError("the generic host hash default must forward to the deprecated spelling")
    source_default = body(generic_hash,
                          r"void\s+BacktestEngine::hash_source_extension\(BrokerStateHashSink&\s+sink\)\s+const\s*\{",
                          "deprecated host hash default")
    if re.sub(r"\s+", "", source_default) != 'sink.s("source:none");':
        raise ValueError("the deprecated host hash default must fold the established marker")
    source_body = body(source_hash,
                       r"void\s+source::PineStrategyHost::hash_host_extension\(BrokerStateHashSink&\s+f\)\s+const\s*\{",
                       "source hash")
    if not re.match(r"\s*f\.s\(kSourceAdapterDomain\);", source_body):
        raise ValueError("source hash must begin with its adapter domain")
    if "hash_source_extension" in source_hash:
        raise ValueError("the source host must fold through the generic hook")
    source_host = clean((root / "include/pineforge/source/pine_strategy_host.hpp").read_text())
    if len(re.findall(r"void\s+hash_host_extension\(BrokerStateHashSink&\)\s+const\s+override\s*;",
                      source_host)) != 1:
        raise ValueError("the source host must override the generic hash hook")
    if not re.search(r"void\s+hash_source_extension\(BrokerStateHashSink&\s+sink\)\s+const\s+final\s*\{"
                     r"\s*hash_host_extension\(sink\);\s*\}", source_host):
        raise ValueError("the source host must keep the deprecated spelling as a final forward")
    generic_body = body(generic_hash,
                        r"(?:std::)?uint64_t\s+BacktestEngine::broker_state_hash_from_execution_hash\(\s*(?:std::)?uint64_t\s+execution_hash\)\s+const\s*\{",
                        "broker hash")
    if not re.match(r"\s*BrokerStateHashSink\s+f;\s*f\.s\(\"pineforge-broker-state/v19\"\);", generic_body):
        raise ValueError("broker hash requires the v19 domain")
    if not re.search(r"hash_host_extension\(f\);\s*return\s+f\.h;\s*$", generic_body):
        raise ValueError("broker hash must end with the generic host extension")
    if "hash_source_extension" in generic_body:
        raise ValueError("broker hash must not call the deprecated spelling itself")
    stream_body = body(stream_hash,
                       r"uint64_t\s+BacktestEngine::stream_state_hash\(\)\s+const\s*\{",
                       "stream hash")
    compact = re.sub(r"\s+", "", stream_body)
    if compact.count("integer(19);integer(broker_state_hash());") != 1:
        raise ValueError("stream hash requires one unconditional v19 broker fold")
    if "if(false){integer(19);integer(broker_state_hash());}" in compact:
        raise ValueError("stream v19 fold must be unconditional")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--library", type=Path)
    parser.add_argument("--include", type=Path)
    parser.add_argument("--generated-include", type=Path)
    parser.add_argument("--v18-frozen-receipt", type=Path)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--receipt", type=Path)
    receipt_mode = parser.add_mutually_exclusive_group()
    receipt_mode.add_argument("--skip-if-receipt-missing", action="store_true")
    receipt_mode.add_argument("--require-receipts", action="store_true")
    args = parser.parse_args()
    try:
        mode = enforce_receipt_mode(
            (args.v18_frozen_receipt,), skip=args.skip_if_receipt_missing,
            require=args.require_receipts, label="aggregate C++ versions")
        if mode is not None:
            return mode
        check(args.include.resolve().parent if args.include else ROOT)
        requested = [args.compiler, args.library, args.include, args.generated_include,
                     args.v18_frozen_receipt]
        if any(value is not None for value in requested):
            if not all(value is not None for value in requested):
                raise PairingError("runtime ABI control requires compiler, library, include, generated include, and v18 receipt")
            result = execute_frozen_pair(
                compiler=args.compiler,
                extra_flags=args.extra_flag,
                current_library=args.library,
                current_include=args.include,
                generated_include=args.generated_include,
                frozen_receipt=args.v18_frozen_receipt,
                kind="native",
                artifact_directory=args.receipt.parent if args.receipt else None,
            )
            if args.receipt:
                import json
                args.receipt.parent.mkdir(parents=True, exist_ok=True)
                args.receipt.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    except (PairingError, ValueError) as error:
        raise SystemExit("aggregate C++ versions: " + str(error))
    print("aggregate v19 ownership" + (" and v18/v19 ABI pairs" if args.compiler else "") + " verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
