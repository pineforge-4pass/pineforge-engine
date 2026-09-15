#!/usr/bin/env python3
"""Fail closed when durable generic or source-adapter state loses its hash fold."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
REGION = re.compile(
    r"^[ \t]*// @(?:broker-state|source-state) begin[ \t]*$(.*?)"
    r"^[ \t]*// @(?:broker-state|source-state) end[ \t]*$", re.M | re.S)
MEMBER = re.compile(r"^\s*(?:[\w:<>, ]+?)\s+(\w+_)\s*(?:=|;|\{)", re.M)

GENERIC_HEADERS = ("include/pineforge/engine.hpp",)
SOURCE_HEADERS = (
    "include/pineforge/source/pine_adapter.hpp",
    "include/pineforge/source/pine_scheduler.hpp",
    "include/pineforge/source/pine_strategy_host.hpp",
    "include/pineforge/source/pine_language_state.hpp",
)


def clean(text: str) -> str:
    return LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", text))


def durable_members(root: Path, paths: tuple[str, ...]) -> set[str]:
    result: set[str] = set()
    for relative in paths:
        text = (root / relative).read_text()
        regions = REGION.findall(text)
        if not regions:
            continue
        for region in regions:
            result.update(MEMBER.findall(region))
    return result


def load_waivers(root: Path) -> dict[str, str]:
    path = root / "scripts/broker_state_hash_waivers.txt"
    result: dict[str, str] = {}
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "#" not in raw:
            raise ValueError(f"waiver line {number} has no reason")
        name, reason = raw.split("#", 1)
        name, reason = name.strip(), reason.strip()
        if not name or not reason:
            raise ValueError(f"invalid waiver line {number}")
        result[name] = reason
    return result


def require_once(text: str, value: str, label: str) -> None:
    if text.count(value) != 1:
        raise ValueError(label + " must appear exactly once")


def main(root: Path = ROOT) -> int:
    try:
        engine_hash = clean((root / "src/engine_state_hash.cpp").read_text())
        source_hash = clean((root / "src/source/pine_state_hash.cpp").read_text())
        adapter_header = (root / "include/pineforge/source/pine_adapter.hpp").read_text()
        stream_hash = clean((root / "src/engine_stream.cpp").read_text())
        require_once(engine_hash, 'f.s("pineforge-broker-state/v17")', "generic hash domain")
        require_once(adapter_header, 'kSourceAdapterDomain[] = "pineforge-source-adapter/v2"',
                     "source hash domain")
        require_once(stream_hash, "integer(17); integer(broker_state_hash());",
                     "stream v17 fold")
        if "if (false) { integer(17); integer(broker_state_hash()); }" in stream_hash:
            raise ValueError("stream v17 fold must be unconditional")
        if "void source::PineStrategyHost::hash_source_extension" not in source_hash:
            raise ValueError("source host hash extension is missing")
        if "void source::PineExecutionAdapter::hash_state" not in source_hash:
            raise ValueError("adapter durable-state hash is missing")
        if "void source::PineScheduler::hash_state" not in source_hash:
            raise ValueError("scheduler durable-state hash is missing")
        if "adapter_.hash_state(f);" not in source_hash:
            raise ValueError("source extension does not fold adapter state")
        if "scheduler_.hash_state(f);" not in source_hash:
            raise ValueError("source extension does not fold scheduler state")
        # L4c restores durable adapter policy receipts.  The generic core
        # remains source-blind, so their complete fold must be visible in the
        # source placement traversal rather than waived as retired book state.
        for fold in (
            "value.legs.visit(f);",
            "value.reservation_expansion.capture()",
            "value.reservation_growth_source.reservation_owner()",
            "value.stop_limit_activated",
            "value.cancellation.cause",
            "last_applied_ordinal_",
        ):
            if fold not in source_hash:
                raise ValueError("L4c adapter policy hash fold is missing: " + fold)

        generic = durable_members(root, GENERIC_HEADERS)
        source = durable_members(root, SOURCE_HEADERS)
        waivers = load_waivers(root)
        all_hash = engine_hash + "\n" + source_hash
        missing = sorted(
            member for member in generic | source
            if member not in waivers and not re.search(rf"\b{re.escape(member)}\b", all_hash)
        )
        unknown = sorted(name for name in waivers if name not in generic | source)
        redundant = sorted(
            name for name in waivers if re.search(rf"\b{re.escape(name)}\b", all_hash)
        )
        if missing or unknown or redundant:
            print("check_broker_state_hash_coverage: "
                  f"missing={missing}, unknown_waivers={unknown}, redundant_waivers={redundant}",
                  file=sys.stderr)
            return 1
        print("check_broker_state_hash_coverage: "
              f"{len(generic)} generic members, {len(source)} source-adapter members, "
              f"{len(waivers)} waivers, OK")
        return 0
    except (OSError, ValueError) as error:
        print("check_broker_state_hash_coverage: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
