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

SOURCE_CLASSES = {
    "include/pineforge/source/pine_adapter.hpp": ("PineExecutionAdapter",),
    "include/pineforge/source/pine_scheduler.hpp": ("PineScheduler",),
    "include/pineforge/source/pine_strategy_host.hpp": ("PineStrategyHost",),
    "include/pineforge/source/pine_language_state.hpp": ("PineLanguageState",),
}
NESTED_STRUCTS = {
    "PineStrategyConfig", "StrategyOverrides", "StagedConfiguration",
    "PineExitLevels", "PineCancellationReceipt", "PineSizingSnapshot",
    "PlacementSnapshot", "ShortSeedPlan", "PendingShortSeedPlan",
    "DroppedCloseReceipt", "OpenEntryFeeFact", "SourceDayLedger",
    "PineRiskState", "CohortFacts", "PendingBracketLeg", "PendingEntry",
    "DelayedMarketOrder", "PendingSameBarCommand", "SourceShadowPending",
    "PendingRelativeExit", "PendingCoofRequest", "PendingMarginRevival",
    "NamedEntryCancelToken", "CloseCallsiteState", "RetainedBegin",
    "DeferredBoundaryInput", "PineSecurityEvalState",
}

# These three DELTA-review defects belong to the concurrently landing hash
# projection repair.  Naming their exact bad forms here makes this checker
# reject every *new* void/constant/out-of-region fold now and fail closed as
# soon as the sibling repair removes one (the integration merge then deletes
# the stale debt row rather than silently preserving it).
PINNED_HASH_DEBT: set[str] = set()  # L8g settled the three sibling-lane debts (A41(2)); keep empty

# R4-D L10z review fix 7: explicit waivers for PineScheduler caches that live
# outside the @source-state regions (so `durable_members` never sees them) and
# are pure derived lookups over already-hashed state. Each entry is enforced
# fail-closed below: the member must still exist as a mutable cache field in
# the scheduler header and must NOT be folded into pine_state_hash.cpp (fold it
# and delete the row instead of leaving a stale waiver).
SCHEDULER_CACHE_WAIVERS: dict[str, str] = {
    "broker_bar_cursor": (
        "Derived bar-lookup cursor over the hashed retained_.bars prefix; "
        "broker_bar() rebuilds it by reset-and-rescan "
        "(pine_scheduler.hpp:72-95), so it is never next-decision state "
        "(L10c)."
    ),
}

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


def _named_body(text: str, kind: str, name: str) -> str:
    match = re.search(r"\b" + kind + r"\s+" + re.escape(name)
                      + r"(?:\s+final)?(?:\s*:[^{]+)?\s*\{", text)
    if not match:
        raise ValueError(f"cannot find {kind} {name}")
    start = match.end()
    depth = 1
    index = start
    while index < len(text) and depth:
        depth += (text[index] == "{") - (text[index] == "}")
        index += 1
    if depth:
        raise ValueError(f"unclosed {kind} {name}")
    return text[start:index - 1]


def _top_level_statements(body: str) -> list[str]:
    statements: list[str] = []
    value: list[str] = []
    braces = 0
    for char in body:
        value.append(char)
        braces += (char == "{") - (char == "}")
        if char == ";" and braces == 0:
            statements.append("".join(value))
            value = []
    return statements


def _data_fields(body: str, *, trailing_underscore: bool) -> set[str]:
    result: set[str] = set()
    for statement in _top_level_statements(clean(body)):
        value = re.sub(r"\b(?:public|private|protected)\s*:\s*", "", statement).strip()
        if not value or value.startswith(("using ", "friend ", "static_assert", "return ")):
            continue
        declaration = value.rsplit(";", 1)[0]
        if "=" in declaration:
            declaration = declaration.split("=", 1)[0]
        declaration = re.sub(r"\{[^{};]*\}\s*$", "", declaration).strip()
        # A function declaration has a parenthesis in its declarator. Field
        # initializers may have parentheses only after '=' and remain valid.
        if "(" in declaration:
            continue
        match = re.search(r"([A-Za-z_]\w*)\s*$", declaration, re.S)
        if not match:
            continue
        name = match.group(1)
        if not trailing_underscore or name.endswith("_"):
            result.add(name)
    return result


def source_class_members(root: Path) -> set[str]:
    result: set[str] = set()
    for relative, classes in SOURCE_CLASSES.items():
        text = (root / relative).read_text()
        for name in classes:
            kind = "struct" if name == "PineLanguageState" else "class"
            result.update(_data_fields(_named_body(text, kind, name),
                                       trailing_underscore=True))
    return result


def nested_fields(root: Path) -> set[str]:
    result: set[str] = set()
    texts = "\n".join((root / path).read_text() for path in SOURCE_HEADERS)
    for name in NESTED_STRUCTS:
        # Nested adapter records and namespace records are all structs.
        result.update(_data_fields(_named_body(texts, "struct", name),
                                   trailing_underscore=False))
    return result


def _remove_false_blocks(text: str) -> str:
    result = list(text)
    pattern = re.compile(r"\bif\s*\(\s*false\s*\)\s*\{")
    for match in reversed(list(pattern.finditer(text))):
        depth = 1
        index = match.end()
        while index < len(text) and depth:
            depth += (text[index] == "{") - (text[index] == "}")
            index += 1
        for position in range(match.start(), index):
            if result[position] != "\n":
                result[position] = " "
    return "".join(result)


def executable_hash_text(text: str) -> str:
    value = _remove_false_blocks(clean(text))
    return re.sub(r"\(\s*void\s*\)\s*[A-Za-z_]\w*\s*;", "", value)


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
        engine_hash = executable_hash_text((root / "src/engine_state_hash.cpp").read_text())
        source_hash_raw = (root / "src/source/pine_state_hash.cpp").read_text()
        source_hash = executable_hash_text(source_hash_raw)
        adapter_header = (root / "include/pineforge/source/pine_adapter.hpp").read_text()
        stream_hash = clean((root / "src/engine_stream.cpp").read_text())
        require_once(engine_hash, 'f.s("pineforge-broker-state/v18")', "generic hash domain")
        require_once(adapter_header, 'kSourceAdapterDomain[] = "pineforge-source-adapter/v3"',
                     "source hash domain")
        require_once(stream_hash, "integer(18); integer(broker_state_hash());",
                     "stream v18 fold")
        host_header = (root / "include/pineforge/source/pine_strategy_host.hpp").read_text()
        require_once(host_header,
                     'kSourceSecurityDomain[] = "pineforge-source-security/v2"',
                     "source request.security hash domain")
        require_once(source_hash, "f.s(kSourceSecurityDomain);",
                     "source request.security hash fold")
        if "if (false) { integer(18); integer(broker_state_hash()); }" in stream_hash:
            raise ValueError("stream v18 fold must be unconditional")
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

        # R5 L7b: the generic broker-state hash folds the native request table
        # through the execution continuation hash, so a request-state field
        # that is durable next-decision state must be folded there. The two
        # anchored-leg fields fold only when set (a Raw anchor and a Working
        # child keep their pre-lane digest); the checker fails closed the
        # moment either conditional fold disappears from the consumer.
        consumer_hash = executable_hash_text(
            (root / "src/native_execution_consumer.cpp").read_text())
        for fold in (
            "if (anchor->rounding != native_order::NativeAnchorRounding::Raw) {",
            "f.u(static_cast<uint64_t>(anchor->rounding));",
            "if (value.visibility != native_order::NativeArmVisibility::Working) {",
            "f.u(static_cast<uint64_t>(value.visibility));",
        ):
            require_once(consumer_hash, fold, "native request-state fold `" + fold + "`")

        scheduler_header = (root / "include/pineforge/source/pine_scheduler.hpp").read_text()
        cache_errors: list[str] = []
        for cache_name in SCHEDULER_CACHE_WAIVERS:
            if not re.search(r"\bmutable\b[^;{]*\b" + re.escape(cache_name) + r"\b\s*(?:=|;)",
                             scheduler_header):
                cache_errors.append(
                    cache_name + " is no longer a mutable PineScheduler cache member")
            if re.search(r"\b" + re.escape(cache_name) + r"\b", source_hash):
                cache_errors.append(
                    cache_name + " is folded in pine_state_hash.cpp; remove its waiver")
        generic = durable_members(root, GENERIC_HEADERS)
        source = durable_members(root, SOURCE_HEADERS) | source_class_members(root)
        nested = nested_fields(root)
        waivers = load_waivers(root)
        all_hash = engine_hash + "\n" + source_hash
        missing = sorted(
            member for member in generic | source
            if member not in waivers and member not in PINNED_HASH_DEBT
            and not re.search(rf"\b{re.escape(member)}\b", all_hash)
        )
        unknown = sorted(name for name in waivers if name not in generic | source)
        redundant = sorted(
            name for name in waivers if re.search(rf"\b{re.escape(name)}\b", all_hash)
        )
        nested_missing = sorted(
            field for field in nested
            if not re.search(rf"\.{re.escape(field)}\b", source_hash)
        )
        debt_errors: list[str] = []  # the L8d sibling-lane pins were settled by L8g (A41(2)) and removed at MERGE-L8
        if missing or unknown or redundant or nested_missing or debt_errors or cache_errors:
            print("check_broker_state_hash_coverage: "
                  f"missing={missing}, unknown_waivers={unknown}, redundant_waivers={redundant}",
                  file=sys.stderr)
            if nested_missing:
                print("check_broker_state_hash_coverage: missing nested fields="
                      + repr(nested_missing), file=sys.stderr)
            if debt_errors:
                print("check_broker_state_hash_coverage: pinned debt="
                      + repr(debt_errors), file=sys.stderr)
            if cache_errors:
                print("check_broker_state_hash_coverage: scheduler cache waivers="
                      + repr(cache_errors), file=sys.stderr)
            return 1
        print("check_broker_state_hash_coverage: "
              f"{len(generic)} generic members, {len(source)} source-adapter members, "
              f"{len(nested)} nested fields, {len(waivers)} waivers, "
              f"{len(SCHEDULER_CACHE_WAIVERS)} scheduler cache waivers, "
              f"{len(PINNED_HASH_DEBT)} pinned sibling-lane debts, OK")
        return 0
    except (OSError, ValueError) as error:
        print("check_broker_state_hash_coverage: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
