#!/usr/bin/env python3
"""Generate the POD mirror of pineforge::source::PendingOrder (spec §3.6, ABI v4).

Parses ``struct PendingOrder { ... };`` in include/pineforge/source/pine_pending_intent.hpp and
emits

  * include/pineforge/pending_order_mirror.hpp -- the C-compatible
    ``pf_pending_order_v1_t`` typedef (``uint32_t struct_version`` = 1,
    ``uint32_t size``, then every PendingOrder member in declaration order:
    scalars by value -- ``bool``->``uint8_t``, ``int``/``int8_t``/enums->
    ``int32_t``, ``int64_t``/``uint64_t``/``double`` as-is -- and every
    ``std::string`` as ``char name[64]; uint8_t name_truncated; uint64_t
    name_hash64;`` where the hash is FNV-1a 64 of the FULL string) plus the
    ``pf_field_desc_t`` {name, type, offset, size} descriptor type;
  * src/pending_order_mirror.cpp -- the source-free
    ``pineforge::pending_order_layout`` descriptor table;
  * src/source/pine_pending_mirror.cpp -- the source projection
    ``pineforge::fill_pending_order_mirror``.

Every member of PendingOrder must be either mapped by TYPE_MAP / a
``std::string``, or listed in scripts/pending_order_mirror_waivers.txt
(``name  # reason``) -- otherwise generation FAILS. A declaration the parser
cannot classify (two names on one line, a method, a template type, a static
member, ...) also fails: the point of this generator is that PendingOrder
cannot silently grow a member nobody mirrored.

Run with --check to verify the committed files are byte-identical to what
the current source intent header generates (CI, and a ctest). The parser is also
imported by scripts/check_broker_state_hash_coverage.py (``members()``).
"""
from __future__ import annotations

import re
import json
import sys
from pathlib import Path
from exit_leg_reflection_schema import mapping as lifecycle_mapping, validate as validate_lifecycle_mapping

ROOT = Path(__file__).resolve().parents[1]
HPP = ROOT / "include/pineforge/source/pine_pending_intent.hpp"
OUT_H = ROOT / "include/pineforge/pending_order_mirror.hpp"
OUT_C = ROOT / "src/pending_order_mirror.cpp"
OUT_SOURCE_C = ROOT / "src/source/pine_pending_mirror.cpp"
WAIVERS = ROOT / "scripts/pending_order_mirror_waivers.txt"
STRUCT_NAME = "PendingOrder"
STR_CAP = 64
STRUCT_VERSION = 1

# C++ member type -> (C field type, copy expression template).
TYPE_MAP: dict[str, tuple[str, str]] = {
    "bool": ("uint8_t", "src.{m} ? 1 : 0"),
    "int": ("int32_t", "(int32_t)src.{m}"),
    "int8_t": ("int32_t", "(int32_t)src.{m}"),
    "int32_t": ("int32_t", "src.{m}"),
    "int64_t": ("int64_t", "src.{m}"),
    "uint64_t": ("uint64_t", "src.{m}"),
    "double": ("double", "src.{m}"),
    # Enums: value-cast to int32 (the enumerator order is the ABI).
    "OrderType": ("int32_t", "(int32_t)src.{m}"),
    "PositionSide": ("int32_t", "(int32_t)src.{m}"),
    "ShortSeedCollisionRole": ("int32_t", "(int32_t)src.{m}"),
    "PineHistoricalBirthReach": ("int32_t", "(int32_t)src.{m}"),
}
# Public v1 is append-only. Removed native fields survive only as one-way
# deprecated output projections at their original offsets.
LEGACY_OUTPUTS = {
    "created_after_position_close_in_bar": "source::placement_has_prior_close(src) ? 1 : 0",
    "over_pyramiding_cap_at_placement": "source::placement_at_entry_capacity(src) ? 1 : 0",
    "reverses_same_bar_market_from_flat": "journal && source::placement_has_opposite_market_predecessor(*journal, src) ? 1 : 0",
    "limit_price": "src.legs.prices().limit_price",
    "stop_price": "src.legs.prices().stop_price",
    "trail_points": "src.legs.prices().trail_points",
    "trail_price": "src.legs.prices().trail_price",
    "trail_offset": "src.legs.prices().trail_offset",
    "profit_ticks": "src.legs.prices().profit_ticks",
    "loss_ticks": "src.legs.prices().loss_ticks",
    "dormant_bracket": "src.legs.dormant() ? 1 : 0",
    "dormant_reissue_pending": "src.legs.pending_replacement() ? 1 : 0",
    "dormant_original_stop_price": "src.legs.original_stop()",
    "dormant_hold_bar": "src.legs.hold_bar()",
    "dormant_reversal_kill_bar": "src.legs.excluded_bar()",
    "dormant_trail_best": "src.legs.trail_best()",
    "dormant_trail_best_start": "src.legs.trail_prefix()",
    "dormant_trail_leg_dead": "src.legs.retired(exit_legs::Leg::Trail) ? 1 : 0",

    "paired_flat_market_candidate": "compat::pine::awaits_pair_review(src.market_admission) ? 1 : 0",
    "default_flat_market_gross_candidate": "compat::pine::awaits_default_review(src.market_admission) ? 1 : 0",
    "opening_affordability_exemption_candidate": "compat::pine::opening_qualification(src.market_admission) ? 1 : 0",
    "explicit_flat_admission_candidate": "compat::pine::explicit_qualification(src.market_admission) ? 1 : 0",
    "pooc_global_full_exit_dynamic_qty": "src.reservation_expansion.population_open() ? 1 : 0",
    "pooc_global_full_exit_tracks_bound_adds": "src.reservation_expansion.capture().has_value() ? 1 : 0",
    "pooc_global_full_exit_bound_add": "src.reservation_growth_source.reservation_owner().has_value() ? 1 : 0",
    "coof_suppress_stop_on_entry_bar": "src.pine_exit_activation.holds_stop() ? 1 : 0",
    "coof_suppress_limit_on_entry_bar": "src.pine_exit_activation.holds_limit() ? 1 : 0",
    "created_during_coof_recalc": "src.birth.from_fill() ? 1 : 0",
    "coof_born_at_close_recalc": "src.birth.at_terminal_fill() ? 1 : 0",
    "coof_born_mid_bar": "compat::pine::historical_cascade_reach(src) ? 1 : 0",
    "created_by_same_id_replacement": "src.type != OrderType::RAW_ORDER && src.replaced_order_incarnation != 0 ? 1 : 0",
    "replaced_exit_order_incarnation": "src.type == OrderType::EXIT ? src.replaced_order_incarnation : 0",
    "created_while_in_position": "src.type == OrderType::EXIT && src.created_position_side != PositionSide::FLAT ? 1 : 0",
    "requested_partial": "src.quantity_request.is_partial(1e-9, 1e-9) ? 1 : 0",
    "full_percent_exit_request": "src.quantity_request.requests_all() ? 1 : 0",
    "sbmt_member": "src.pine_frozen_market_instruction.active() ? 1 : 0",
    "sbmt_own_qty": "src.pine_frozen_market_instruction.transaction() ? src.pine_frozen_market_instruction.transaction()->own_units : std::numeric_limits<double>::quiet_NaN()",
    "sbmt_tx_qty": "src.pine_frozen_market_instruction.transaction() ? src.pine_frozen_market_instruction.transaction()->transaction_units : std::numeric_limits<double>::quiet_NaN()",
    "sbmt_kept_over_cap": "src.pine_frozen_market_instruction.transaction() && source::placement_at_entry_capacity(src) ? 1 : 0",
    "sbmt_close_qty": "src.pine_frozen_market_instruction.targeted_close() ? src.quantity_request.intent()->units() : std::numeric_limits<double>::quiet_NaN()",
    "sbmt_close_buy": "src.pine_frozen_market_instruction.targeted_close() && src.created_position_side == PositionSide::SHORT ? 1 : 0",
    "declined_by_replaced_short_market": "src.cancellation.cause() == CancellationCause::Replacement ? 1 : 0",
    "suppress_as_declined_reversal_close": "src.cancellation.cause() == CancellationCause::Dependency ? 1 : 0",
    "suppressed_close_consumed_ledger_qty": "src.cancellation.close_claim_consumed()",
    "suppressed_close_retired_ledger_qty": "src.cancellation.close_claim_retired()",
    "short_seed_collision_role": "(int32_t)src.short_seed_collision_role",
}
_ADMISSION_FIELDS = json.loads((ROOT / "scripts/market_admission_mirror_fields.json").read_text())


def admission_mirror_expression(ctype: str, path: str) -> str:
    """Read canonical leaves directly: the C accessor must not allocate.

    The separate structured visitor remains the variable-journal/hash API.
    A missing optional emits its existing zero/empty value under its explicit
    presence field; a present numeric leaf preserves its actual NaN payload.
    """
    root = "src.{m}"
    if not path.startswith("draft."):
        _fail(f"invalid admission mirror path {path}")
    parts = path[len("draft."):].split(".")
    owner = parts.pop(0)
    if owner in ("observation_present", "review_present", "sizing_revision_present"):
        return f"{root}.{owner[:-len('_present')]}() ? 1 : 0"
    if owner not in ("observation", "review", "sizing_revision") or not parts:
        _fail(f"unclassified admission mirror path {path}")
    guard = f"{root}.{owner}()"
    expression = guard + "->"
    if parts == ["original_sizing_present"]:
        return f"{guard} && {expression}original_sizing.has_value() ? 1 : 0"
    if parts[0] == "original_sizing":
        guard += f" && {expression}original_sizing"
        expression += "original_sizing->"
        parts.pop(0)
    if parts[0] == "birth":
        parts.pop(0)
        leaf = parts.pop(0)
        if leaf in ("cursor_domain", "cursor_position", "cursor_index", "cursor_count"):
            expression += "birth.cursor()." + leaf[len("cursor_"):] + "()"
        else:
            expression += "birth." + leaf + "()"
        if parts:
            _fail(f"unclassified admission birth path {path}")
    else:
        expression += ".".join(parts)
    if ctype == "std::string":
        return f"{guard} ? std::string_view({expression}) : std::string_view()"
    if ctype not in ("uint64_t", "int64_t", "double"):
        _fail(f"unclassified admission mirror type {ctype}")
    return f"{guard} ? static_cast<{ctype}>({expression}) : 0"


COMPOSITE_MAP = {
    "ExitLegLifecycle": lifecycle_mapping(),

    "MarketAdmissionDraft": [(suffix, ct, admission_mirror_expression(ct, path)) for suffix, ct, path in _ADMISSION_FIELDS],
    "ReservationExpansion": [
        # An eight-byte first field preserves ff54's entire 142-field object,
        # including trailing padding, before any appended smaller fields.
        ("position_cycle", "int64_t", "src.{m}.capture() ? src.{m}.capture()->position_cycle : 0"),
        ("present", "uint8_t", "src.{m}.capture().has_value() ? 1 : 0"),
        ("side", "int32_t", "src.{m}.capture() ? static_cast<int32_t>(src.{m}.capture()->side) : 0"),
        ("first_later_admission_present", "uint8_t", "src.{m}.capture() && src.{m}.capture()->first_later_admission ? 1 : 0"),
        ("first_later_admission", "uint64_t", "src.{m}.capture() && src.{m}.capture()->first_later_admission ? *src.{m}.capture()->first_later_admission : 0"),
    ],
    "ReservationGrowthSource": [
        ("present", "uint8_t", "src.{m}.reservation_owner().has_value() ? 1 : 0"),
        ("reservation_owner", "uint64_t", "src.{m}.reservation_owner() ? *src.{m}.reservation_owner() : 0"),
    ],
    "PineFrozenMarketInstruction": [
        # Start after the full ff54 142-field prefix, including trailing padding.
        ("kind", "uint64_t", "static_cast<uint64_t>(src.{m}.kind())"),
        ("own_units", "double", "src.{m}.transaction() ? src.{m}.transaction()->own_units : 0.0"),
        ("transaction_units", "double", "src.{m}.transaction() ? src.{m}.transaction()->transaction_units : 0.0"),
        ("target_id", "std::string", "src.{m}.targeted_close() ? std::string_view(src.{m}.targeted_close()->target_id) : std::string_view()"),
    ],
    "ExitLegActivation": [
        ("owner_cycle", "int64_t", "src.{m}.bounds() ? src.{m}.bounds()->position_cycle : 0"),
        ("present", "uint8_t", "src.{m}.bounds().has_value() ? 1 : 0"),
        ("stop_first_bar", "int64_t", "src.{m}.bounds() ? src.{m}.bounds()->stop_first_bar : 0"),
        ("limit_first_bar", "int64_t", "src.{m}.bounds() ? src.{m}.bounds()->limit_first_bar : 0"),
    ],
    "PineExitActivationPolicy": [
        ("owner_cycle_at_birth", "int64_t", "src.{m}.evidence() ? src.{m}.evidence()->position_cycle : 0"),
        ("present", "uint8_t", "src.{m}.evidence().has_value() ? 1 : 0"),
        ("entry_bar_at_birth", "int32_t", "src.{m}.evidence() ? src.{m}.evidence()->entry_bar : 0"),
        ("direction_at_birth", "int32_t", "src.{m}.evidence() ? src.{m}.evidence()->direction : 0"),
        ("cursor_price_at_birth", "double", "src.{m}.evidence() ? src.{m}.evidence()->cursor_price : 0.0"),
        ("stop_level_at_birth", "double", "src.{m}.evidence() ? src.{m}.evidence()->stop_level : 0.0"),
        ("limit_level_at_birth", "double", "src.{m}.evidence() ? src.{m}.evidence()->limit_level : 0.0"),
        ("limit_continuation_present", "uint8_t", "src.{m}.evidence() && src.{m}.evidence()->limit_continuation ? 1 : 0"),
        ("limit_continuation_cause", "int32_t", "src.{m}.evidence() && src.{m}.evidence()->limit_continuation ? static_cast<int32_t>(src.{m}.evidence()->limit_continuation->cause) : 0"),
        ("limit_continuation_fill", "uint64_t", "src.{m}.evidence() && src.{m}.evidence()->limit_continuation ? src.{m}.evidence()->limit_continuation->observed_fill_sequence : 0"),
    ],
    "QuantityRequest": [
        ("intent_kind", "uint64_t", "src.{m}.intent() ? static_cast<uint64_t>(src.{m}.intent()->kind()) + 1 : 0"),
        ("intent_units", "double", "src.{m}.intent() && src.{m}.intent()->kind() == QuantityIntent::Kind::Units ? src.{m}.intent()->units() : 0.0"),
        ("intent_numerator", "double", "src.{m}.intent() && src.{m}.intent()->kind() == QuantityIntent::Kind::Fraction ? src.{m}.intent()->numerator() : 0.0"),
        ("intent_denominator", "double", "src.{m}.intent() && src.{m}.intent()->kind() == QuantityIntent::Kind::Fraction ? src.{m}.intent()->denominator() : 0.0"),
        ("reservation_present", "uint8_t", "src.{m}.reservation().has_value() ? 1 : 0"),
        ("reservation_units", "double", "src.{m}.reservation() ? src.{m}.reservation()->units : 0.0"),
        ("reservation_basis_units", "double", "src.{m}.reservation() ? src.{m}.reservation()->basis_units : 0.0"),
    ],
    "OrderBirth": [
        # Start appended facts at the v1 struct's 8-byte boundary, preserving
        # its trailing padding as well as all 108 field offsets.
        ("timestamp", "int64_t", "src.{m}.timestamp()"),
        ("cause", "int32_t", "(int32_t)src.{m}.cause()"),
        ("bar", "int32_t", "src.{m}.bar()"),
        ("cursor_domain", "int32_t", "(int32_t)src.{m}.cursor().domain()"),
        ("cursor_position", "int32_t", "(int32_t)src.{m}.cursor().position()"),
        ("cursor_index", "int32_t", "src.{m}.cursor().index()"),
        ("cursor_count", "int32_t", "src.{m}.cursor().count()"),
        ("cursor_price", "double", "src.{m}.cursor_price()"),
        ("first_fill", "uint64_t", "src.{m}.first_fill()"),
        ("last_fill", "uint64_t", "src.{m}.last_fill()"),
        ("evaluation_ordinal", "uint64_t", "src.{m}.evaluation_ordinal()"),
    ],
    "OrderCancellationReceipt": [
        ("cause", "int32_t", "static_cast<int32_t>(src.{m}.cause())"),
        ("state", "int32_t", "static_cast<int32_t>(src.{m}.state())"),
        ("close_claim_release", "int32_t", "static_cast<int32_t>(src.{m}.close_claim_release())"),
        ("source_incarnation", "uint64_t", "src.{m}.source_incarnation()"),
        ("source_sequence", "int64_t", "src.{m}.source_sequence()"),
        ("target_incarnation", "uint64_t", "src.{m}.target_incarnation()"),
        ("target_owner", "int64_t", "src.{m}.target_owner()"),
        ("target_revision", "uint64_t", "src.{m}.target_revision()"),
        ("close_claim_consumed", "double", "src.{m}.close_claim_consumed()"),
        ("close_claim_retired", "double", "src.{m}.close_claim_retired()"),
    ],
}
STRING_TYPES = frozenset({"std::string"})

BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
# One declaration, comments stripped and whitespace collapsed:
#   TYPE NAME [= initialiser]
# TYPE is a single (optionally std::-qualified) identifier; anything else
# (template args, two names, cv-qualifiers, `static`, a method's `(`) fails
# to match and the caller reports it.
DECL_RE = re.compile(r"^((?:std::)?[A-Za-z_]\w*) ([A-Za-z_]\w*)(?: = .+)?$", re.S)


def _fail(msg: str) -> "NoReturn":  # noqa: F821
    sys.exit(f"gen_pending_order_mirror: {msg}")


def struct_body(text: str, name: str = STRUCT_NAME) -> str:
    """Return the text between the braces of ``struct <name> { ... };``.
    Comments are stripped BEFORE anchoring, so a prose mention of
    ``struct PendingOrder {`` in a comment cannot mis-anchor the parser, and
    the anchor must occur exactly once in what remains."""
    text = LINE_COMMENT_RE.sub("", BLOCK_COMMENT_RE.sub("", text))
    anchors = list(re.finditer(rf"\bstruct\s+{re.escape(name)}\s*\{{", text))
    if not anchors:
        _fail(f"struct {name} not found in {HPP}")
    if len(anchors) > 1:
        _fail(f"struct {name} {{ appears {len(anchors)} times in {HPP} (outside comments); "
              "expected exactly one definition")
    start = anchors[0].end()
    depth = 1
    for i in range(start, len(text)):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[start:i]
    _fail(f"struct {name}: unbalanced braces")


def members(text: str | None = None, name: str = STRUCT_NAME) -> list[tuple[str, str]]:
    """Return [(cpp_type, name)] for every data member of the named struct, in
    declaration order. Comments are stripped and multi-line declarations
    (a member whose initialiser wraps onto the next line) are joined before
    parsing. Any declaration that is not exactly ``TYPE NAME [= init];``
    aborts."""
    if text is None:
        text = HPP.read_text(encoding="utf-8")
    body = struct_body(text, name)   # already comment-stripped
    out: list[tuple[str, str]] = []
    seen: set[str] = set()
    for raw in body.split(";"):
        decl = " ".join(raw.split())
        if not decl:
            continue
        m = DECL_RE.match(decl)
        if not m or "," in decl:
            _fail(f"cannot classify declaration in struct {name}: {decl!r} "
                  "(expected exactly `TYPE NAME [= init];`; split multi-name "
                  "declarations, and mirror-waive methods/templates explicitly)")
        t, n = m.group(1), m.group(2)
        if n in seen:
            _fail(f"duplicate member name {n}")
        seen.add(n)
        out.append((t, n))
    if not out:
        _fail(f"struct {name} has no members?")
    return out


def load_waivers(path: Path = WAIVERS) -> dict[str, str]:
    waivers: dict[str, str] = {}
    if not path.is_file():
        return waivers
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "#" not in raw:
            _fail(f"waiver line {lineno} has no '# reason': {raw!r}")
        name, reason = raw.split("#", 1)
        name, reason = name.strip(), reason.strip()
        if not name:
            continue
        if not reason:
            _fail(f"waiver for {name!r} (line {lineno}) has no reason after '#'")
        waivers[name] = reason
    return waivers


def classify(ms: list[tuple[str, str]], waivers: dict[str, str]):
    """Return (mirrored, waived) where mirrored = [(cpp_type, name)] kept in
    the POD and waived = [(cpp_type, name, reason)]. Aborts on an unmapped,
    unwaived type or a waiver naming a non-member."""
    if "legs" in waivers:
        _fail("canonical exit lifecycle cannot be mirror-waived")
    if {"reservation_expansion", "reservation_growth_source"} & waivers.keys():
        _fail("reservation expansion and source receipts cannot be waived")
    if "market_admission" in waivers:
        _fail("market admission cannot be waived")
    names = {n for _, n in ms}
    orphans = sorted(w for w in waivers if w not in names)
    if orphans:
        _fail(f"waiver(s) naming a member not in struct {STRUCT_NAME}: {orphans}")
    mirrored, waived = [], []
    for t, n in ms:
        if n in waivers:
            waived.append((t, n, waivers[n]))
        elif t in STRING_TYPES or t in TYPE_MAP or t in COMPOSITE_MAP:
            mirrored.append((t, n))
        else:
            _fail(f"member {n} has unmapped type {t}; add it to TYPE_MAP or "
                  f"waive it in {WAIVERS.relative_to(ROOT)}")
    return mirrored, waived


def generate_parts() -> tuple[str, str, str]:
    # Share the strict nested storage census; newly stored fields cannot hide
    # behind an unchanged composite-map name. Imported lazily (checker also
    # uses this module's PendingOrder parser).
    validate_lifecycle_mapping(COMPOSITE_MAP["ExitLegLifecycle"])
    from check_exit_leg_lifecycle import check as check_exit_lifecycle
    check_exit_lifecycle((ROOT / "include/pineforge/exit_leg_lifecycle.hpp").read_text())
    from check_broker_state_hash_coverage import _reservation_expansion_fields
    _reservation_expansion_fields((ROOT / "include/pineforge/reservation_expansion.hpp").read_text())
    from check_market_admission_schema import check as market_admission_coverage
    market_admission_coverage(ROOT)
    mirrored, waived = classify(members(), load_waivers())
    fields: list[str] = []
    copies: list[str] = []
    descs: list[tuple[str, str]] = [("struct_version", "uint32_t"), ("size", "uint32_t")]
    prefix = json.loads((ROOT / "scripts/pending_order_v1_prefix.json").read_text())["members"]
    native = dict((name, kind) for kind, name in mirrored)
    for kind, name in prefix:
        if name not in LEGACY_OUTPUTS and native.get(name) != kind:
            _fail(f"public v1 prefix member {name} needs an explicit derived projection")
    prefix_names = {name for _, name in prefix}
    # Preserve all 142 ff54 fields, including activation, before new composites.
    existing_extension = ["replaced_order_incarnation", "birth", "pine_birth_reach", "quantity_request", "leg_activation", "pine_exit_activation", "reservation_expansion", "reservation_growth_source", "pine_frozen_market_instruction"]
    tail = [(native[name], name) for name in existing_extension]
    # Cancellation is the first new native member after the shipped v1
    # contract. Keep every pre-cancellation field byte-for-byte in place and
    # append the receipt's leaves after the complete 396-field object.
    tail += [(kind, name) for kind, name in mirrored
             if name not in prefix_names and name not in existing_extension
             and name != "cancellation"]
    if "cancellation" in native:
        tail.append((native["cancellation"], "cancellation"))
    ordered = prefix + tail
    for t, m in ordered:
        if m in LEGACY_OUTPUTS:
            ct = TYPE_MAP[t][0]
            fields.append(
                f"    {ct} {m};" if native.get(m) == t
                else f"    {ct} {m};  // deprecated, derived output only")
            copies.append(f"    out->{m} = {LEGACY_OUTPUTS[m]};")
            descs.append((m, ct))
        elif t in COMPOSITE_MAP:
            for suffix, ct, expr in COMPOSITE_MAP[t]:
                prefix = "quantity" if t == "QuantityRequest" else m
                name = f"{prefix}_{suffix}"
                if ct in STRING_TYPES:
                    fields += [f"    char {name}[{STR_CAP}];",
                               f"    uint8_t {name}_truncated;",
                               f"    uint64_t {name}_hash64;"]
                    copies.append(f"    copy_str({expr.format(m=m)}, out->{name}, &out->{name}_truncated, &out->{name}_hash64);")
                    descs += [(name, f"char[{STR_CAP}]"),
                              (f"{name}_truncated", "uint8_t"),
                              (f"{name}_hash64", "uint64_t")]
                else:
                    fields.append(f"    {ct} {name};")
                    copies.append(f"    out->{name} = {expr.format(m=m)};")
                    descs.append((name, ct))
        elif t in STRING_TYPES:
            fields += [f"    char {m}[{STR_CAP}];",
                       f"    uint8_t {m}_truncated;",
                       f"    uint64_t {m}_hash64;"]
            copies.append(f"    copy_str(src.{m}, out->{m}, &out->{m}_truncated, &out->{m}_hash64);")
            descs += [(m, f"char[{STR_CAP}]"), (f"{m}_truncated", "uint8_t"), (f"{m}_hash64", "uint64_t")]
        else:
            ct, expr = TYPE_MAP[t]
            fields.append(f"    {ct} {m};")
            copies.append(f"    out->{m} = {expr.format(m=m)};")
            descs.append((m, ct))

    banner = "// GENERATED by scripts/gen_pending_order_mirror.py from include/pineforge/engine.hpp -- do not edit."
    waived_note = ([f"// Not mirrored (scripts/pending_order_mirror_waivers.txt): "
                    + ", ".join(f"{n} ({r})" for _, n, r in waived)]
                   if waived else [])
    h = [
        banner,
        f"// {len(mirrored)} PendingOrder members mirrored ({len(descs)} POD fields incl. struct_version/size).",
        *waived_note,
        "#pragma once",
        "#include <stdint.h>",
        "",
        "/* C-compatible value snapshot of one resting pineforge::PendingOrder",
        " * (spec 3.6). struct_version identifies the field set (this file:",
        f" * {STRUCT_VERSION}); size is sizeof(pf_pending_order_v1_t) as the producer",
        " * compiled it. Strings are copied into a NUL-terminated char[64]",
        " * (name_truncated = 1 when the source was longer than 63 bytes) with",
        " * name_hash64 = FNV-1a 64 of the FULL source string. Enums are their",
        " * int32 value; bool is 0/1 in a uint8_t. Append-only, like every",
        " * pineforge.h POD. */",
        "typedef struct pf_pending_order_v1_s {",
        "    uint32_t struct_version;",
        "    uint32_t size;",
        *fields,
        "} pf_pending_order_v1_t;",
        "",
        "/* One row of the self-describing layout table returned by",
        " * strategy_pending_order_layout(): field name, C type spelling",
        f' * ("uint8_t", "int32_t", "int64_t", "uint64_t", "double", "char[{STR_CAP}]",',
        ' * "uint32_t"), byte offset inside pf_pending_order_v1_t, byte size. */',
        "typedef struct pf_field_desc_s {",
        "    const char* name;",
        "    const char* type;",
        "    uint32_t offset;",
        "    uint32_t size;",
        "} pf_field_desc_t;",
        f"#define PF_PENDING_ORDER_STRUCT_VERSION {STRUCT_VERSION}",
        f"#define PF_PENDING_ORDER_FIELD_COUNT {len(descs)}",
        f"#define PF_PENDING_ORDER_STR_CAP {STR_CAP}",
        "",
    ]
    projection = [
        banner,
        "#include <pineforge/source/pine_pending_intent.hpp>",
        "#include <pineforge/pending_order_mirror.hpp>",
        "#include <pineforge/compat/pine/market_admission.hpp>",
        "",
        "#include <cstddef>",
        "#include <cstring>",
        "#include <stdexcept>",
        "#include <string_view>",
        "#include <type_traits>",
        "",
        "static_assert(std::is_standard_layout<pf_pending_order_v1_t>::value,",
        '              "pf_pending_order_v1_t must be standard-layout");',
        "static_assert(std::is_trivial<pf_pending_order_v1_t>::value,",
        '              "pf_pending_order_v1_t must be trivial (memcpy-able across the C ABI)");',
        "",
        "namespace pineforge {",
        "namespace {",
        "",
        "// NUL-terminated copy of the first STR_CAP-1 bytes + FNV-1a 64 of the",
        "// whole string, so a consumer can still match an over-long id exactly.",
        "void copy_str(std::string_view s, char* dst, uint8_t* truncated, uint64_t* hash) {",
        "    uint64_t h = 1469598103934665603ULL;",
        "    for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ULL; }",
        "    *hash = h;",
        f"    const size_t n = s.size() < {STR_CAP - 1} ? s.size() : {STR_CAP - 1};",
        "    if (n != 0) std::memcpy(dst, s.data(), n);",
        "    dst[n] = 0;",
        f"    *truncated = s.size() > {STR_CAP - 1} ? 1 : 0;",
        "}",
        "",
        "}  // namespace",
        "",
        "void fill_pending_order_mirror(const source::PendingOrder& src, const MarketAdmissionJournal* journal, pf_pending_order_v1_t* out) {",
        "    const auto& origin = src.market_admission.observation();",
        "    if (!journal && src.type == OrderType::ENTRY && origin",
        "        && origin->kind == admission::CommandKind::Entry",
        "        && origin->placement_side == static_cast<int>(PositionSide::FLAT)",
        "        && (!std::isnan(origin->prices.limit) || !std::isnan(origin->prices.stop)))",
        '        throw std::logic_error("bound priced order mirror requires its admission journal");',
        "    std::memset(out, 0, sizeof(*out));",
        f"    out->struct_version = {STRUCT_VERSION};",
        "    out->size = (uint32_t)sizeof(*out);",
        *copies,
        "}",
        "",
        "void fill_pending_order_mirror(const source::PendingOrder& src, pf_pending_order_v1_t* out) {",
        "    fill_pending_order_mirror(src, nullptr, out);",
        "}",
        "",
        "}  // namespace pineforge",
        "",
    ]
    descriptor = [
        banner,
        "#include <pineforge/pending_order_mirror.hpp>",
        "",
        "#include <cstddef>",
        "#include <type_traits>",
        "",
        "static_assert(std::is_standard_layout<pf_pending_order_v1_t>::value,",
        '              "pf_pending_order_v1_t must be standard-layout");',
        "static_assert(std::is_trivial<pf_pending_order_v1_t>::value,",
        '              "pf_pending_order_v1_t must be trivial (memcpy-able across the C ABI)");',
        "",
        "namespace pineforge {",
        "namespace {",
        "",
        "#define PF_PO_FIELD(name, type) \\",
        "    { #name, type, (uint32_t)offsetof(pf_pending_order_v1_t, name), \\",
        "      (uint32_t)sizeof(((pf_pending_order_v1_t*)0)->name) }",
        "",
        "const pf_field_desc_t kLayout[] = {",
        *[f'    PF_PO_FIELD({n}, "{t}"),' for n, t in descs],
        "};",
        "",
        "#undef PF_PO_FIELD",
        "",
        "}  // namespace",
        "",
        "const pf_field_desc_t* pending_order_layout(int* count) {",
        "    if (count) *count = (int)(sizeof(kLayout) / sizeof(kLayout[0]));",
        "    return kLayout;",
        "}",
        "",
        "}  // namespace pineforge",
        "",
    ]
    return "\n".join(h), "\n".join(descriptor), "\n".join(projection)


def generate() -> tuple[str, str]:
    """Compatibility surface for checker self-tests: header + descriptor TU."""
    header, descriptor, _ = generate_parts()
    return header, descriptor


def census() -> str:
    ms = members()
    mirrored, waived = classify(ms, load_waivers())
    by_type: dict[str, int] = {}
    for t, _ in ms:
        by_type[t] = by_type.get(t, 0) + 1
    lines = [f"{STRUCT_NAME}: {len(ms)} members, {len(mirrored)} mirrored, {len(waived)} waived"]
    lines += [f"  {t:<24} {c}" for t, c in sorted(by_type.items(), key=lambda kv: (-kv[1], kv[0]))]
    lines += [f"  waived: {n} ({r})" for _, n, r in waived]
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if "--census" in argv:
        print(census())
        return 0
    h, c, source_c = generate_parts()
    if "--check" in argv:
        cur_h = OUT_H.read_text(encoding="utf-8") if OUT_H.is_file() else None
        cur_c = OUT_C.read_text(encoding="utf-8") if OUT_C.is_file() else None
        cur_source_c = OUT_SOURCE_C.read_text(encoding="utf-8") if OUT_SOURCE_C.is_file() else None
        ok = cur_h == h and cur_c == c and cur_source_c == source_c
        print("pending_order_mirror: up to date" if ok else
              "pending_order_mirror: STALE -- run python3 scripts/gen_pending_order_mirror.py "
              "and commit include/pineforge/pending_order_mirror.hpp + src/pending_order_mirror.cpp "
              "+ src/source/pine_pending_mirror.cpp")
        return 0 if ok else 1
    OUT_H.write_text(h, encoding="utf-8")
    OUT_C.write_text(c, encoding="utf-8")
    OUT_SOURCE_C.write_text(source_c, encoding="utf-8")
    print(f"wrote {OUT_H.relative_to(ROOT)}, {OUT_C.relative_to(ROOT)}, "
          f"{OUT_SOURCE_C.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
