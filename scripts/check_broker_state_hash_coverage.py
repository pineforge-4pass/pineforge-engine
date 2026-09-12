#!/usr/bin/env python3
"""CI gate: every member declared in any ``// @broker-state begin`` ..
``// @broker-state end`` region in include/pineforge/engine.hpp is either
referenced by name in src/engine_state_hash.cpp (outside comments) or listed
(with a non-empty reason) in scripts/broker_state_hash_waivers.txt. A new
broker-state member that is neither fails the build (spec §3.4). Multiple
marker pairs are supported (e.g. one around the main position/order/risk
block, a second, tighter pair around an isolated member declared far away
in the class).

The same rule covers ``struct PendingOrder`` (task 7, carried from the task-5
review): every scalar/string member -- the list is reflected by
scripts/gen_pending_order_mirror.py's parser, the one source of truth for
what PendingOrder declares -- must be referenced as ``o.<name>`` inside the
hash function's ``for (const auto& o : pending_orders_)`` loop or waived as
``pending_order.<name>  # reason`` in the waivers file.

Nested physical lots are checked separately: every PyramidEntry member must
have its type-appropriate f.<fold>(e.<name>) inside the pyramid_entries_ loop,
or a justified pyramid_entry.<name> waiver. Merely hashing the container name,
mentioning a member, or hashing it outside its owning loop does not cover it.
Both member lists use the same fail-closed named-struct parser. Intraday quota
owners, continuations and due closes likewise require every nested field and
optional-presence discriminator in their owning hash block, without waivers."""
from __future__ import annotations
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_pending_order_mirror import members as struct_members  # noqa: E402
from gen_pending_order_mirror import struct_body  # noqa: E402

PENDING_WAIVER_PREFIX = "pending_order."
PYRAMID_WAIVER_PREFIX = "pyramid_entry."
PYRAMID_FOLD = {
    "double": "d", "int": "i", "int64_t": "i", "uint64_t": "u",
    "bool": "b", "std::string": "s",
}

MEMBER_RE = re.compile(r"^\s+[\w:<>, ]+?\s+(\w+_)\s*(?:=|;|\{)", re.M)
# The marker must be the whole (trimmed) line -- not merely a substring, so a
# prose mention like "the ``// @broker-state begin`` marker" in an unrelated
# comment can never be parsed as a real region boundary.
REGION_RE = re.compile(
    r"^[ \t]*// @broker-state begin[ \t]*$(.*?)"
    r"^[ \t]*// @broker-state end[ \t]*$",
    re.M | re.S,
)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_cpp_comments(src: str) -> str:
    """Strip ``//`` and ``/* */`` comments so a bare mention of a member's
    name in an explanatory comment (e.g. "the per-PASS working state
    (dual_entry_path_) is waived") does not count as hashing it. Good enough
    for this codebase's actual content: no ``//`` or ``/*`` appears inside a
    string/char literal in engine_state_hash.cpp."""
    return LINE_COMMENT_RE.sub("", BLOCK_COMMENT_RE.sub("", src))


def _regions(hpp: str) -> list[str]:
    regions = REGION_RE.findall(hpp)
    if not regions:
        print("check_broker_state_hash_coverage: no // @broker-state begin/end "
              "region found in engine.hpp", file=sys.stderr)
        sys.exit(2)
    return regions


def _members(regions: list[str]) -> set[str]:
    members: set[str] = set()
    for region in regions:
        members |= set(MEMBER_RE.findall(region))
    return members


def _load_waivers(path: Path) -> dict[str, str]:
    waivers: dict[str, str] = {}
    for lineno, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "#" not in raw_line:
            print(f"check_broker_state_hash_coverage: waiver line {lineno} has no "
                  f"'# reason': {raw_line!r}", file=sys.stderr)
            sys.exit(1)
        name, reason = raw_line.split("#", 1)
        name = name.strip()
        reason = reason.strip()
        if not name:
            # A line that is entirely whitespace before '#' is a stray/typo
            # line, not a real waiver -- never silently absorb it as "".
            continue
        if not reason:
            print(f"check_broker_state_hash_coverage: waiver for {name!r} "
                  f"(line {lineno}) has no reason after '#'", file=sys.stderr)
            sys.exit(1)
        waivers[name] = reason
    return waivers


def _collection_loop_body(src: str, collection: str, variable: str) -> str:
    """Brace-balanced body of one owning collection loop, comments stripped."""
    loop_re = re.compile(
        rf"for\s*\(\s*const\s+auto&\s+{re.escape(variable)}\s*:\s*"
        rf"{re.escape(collection)}\s*\)\s*\{{")
    matches = list(loop_re.finditer(src))
    if not matches:
        print("check_broker_state_hash_coverage: could not find "
              f"`for (const auto& {variable} : {collection}) {{` in engine_state_hash.cpp",
              file=sys.stderr)
        sys.exit(2)
    if len(matches) > 1:
        print("check_broker_state_hash_coverage: found "
              f"{len(matches)} `for (const auto& {variable} : {collection}) {{` loops in "
              "engine_state_hash.cpp; expected exactly one (the nested coverage "
              "rule inspects a single loop body)", file=sys.stderr)
        sys.exit(2)
    depth, start = 1, matches[0].end()
    for i in range(start, len(src)):
        ch = src[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[start:i]
    print(f"check_broker_state_hash_coverage: unbalanced {collection} loop", file=sys.stderr)
    sys.exit(2)


def _pyramid_folded(loop: str, cpp_type: str, member: str) -> bool:
    fold = PYRAMID_FOLD.get(cpp_type)
    if fold is None:
        return False
    # Require an actual typed serialization call, not a read/assignment or an
    # unrelated reference in the same source. Existing integer casts are fine.
    value = rf"e\.{re.escape(member)}"
    if fold == "i":
        value = rf"(?:{value}|static_cast<int64_t>\(\s*{value}\s*\))"
    return re.search(rf"\bf\.{fold}\(\s*{value}\s*\)\s*;", loop) is not None


def _one_braced_body(src: str, pattern: str, label: str) -> str:
    matches = list(re.finditer(pattern, src))
    if len(matches) != 1:
        raise ValueError(f"{label}: expected exactly one body, got {len(matches)}")
    start = matches[0].end()
    depth = 1
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i]
    raise ValueError(f"{label}: unbalanced body")


def _class_fields(src: str, name: str) -> dict[str, str]:
    """Classify the small value classes; refuse unfamiliar declaration shapes.

    Inline method/nested-type bodies are skipped by balanced braces. Every
    remaining data declaration must be one TYPE NAME; adding a new field is
    visible even when its name has no trailing underscore.
    """
    body = _one_braced_body(src, rf"\b(?:class|struct)\s+{name}\s*\{{", name)
    fields = {}
    statement = ""
    i = 0
    while i < len(body):
        ch = body[i]
        if ch == "{":
            prefix = statement.strip()
            nested = re.match(r"(?:struct|class)\s+\w+$", prefix)
            method = "(" in prefix and ("=" not in prefix.split("(", 1)[0]
                                        or "operator=" in prefix.split("(", 1)[0])
            if not nested and not method:
                raise ValueError(f"{name}: unclassified braced declaration {prefix!r}")
            depth = 1
            i += 1
            while i < len(body) and depth:
                depth += (body[i] == "{") - (body[i] == "}")
                i += 1
            if depth:
                raise ValueError(f"{name}: unbalanced member body")
            statement = ""
            continue
        if ch == ";":
            decl = " ".join(statement.split())
            statement = ""
            if re.fullmatch(re.escape(name) + r"\(\)\s*=\s*default", decl):
                continue
            if re.fullmatch(r"(?:bool|ExitLegActivationBounds)\s+\w+\([^;{}]*\)\s+const", decl):
                continue  # declared read-only value query, never a stored field
            reservation_methods = {
                "ReservationExpansion": {
                    "void capture(uint64_t receiver, int64_t cycle, PositionSide side, double capacity)",
                    "void close_population(uint64_t admitted_incarnation)",
                    "void grow(double& qty, int64_t before_cycle, PositionSide before_side, double before_qty, int64_t after_cycle, PositionSide after_side, double after_qty, double epsilon) const",
                },
                "ReservationGrowthSource": {"void assign_capture(uint64_t source, uint64_t receiver)"},
            }
            if decl in reservation_methods.get(name, set()):
                continue  # exact declared operations, never a blanket declaration waiver
            if decl and not decl.startswith("using "):
                match = re.fullmatch(r"((?:(?:static|constexpr|const)\s+)*[\w:<>]+)\s+(\w+)(?:\s*=\s*[^,]+)?", decl)
                if not match or match[2] in fields:
                    raise ValueError(f"{name}: unclassified data declaration {decl!r}")
                fields[match[2]] = match[1]
        else:
            statement += ch
            if statement.strip() in ("public:", "private:", "protected:"):
                statement = ""
        i += 1
    if statement.strip():
        raise ValueError(f"{name}: unterminated declaration")
    return fields


def _opening_coverage(events: str, src: str) -> None:
    """No waiver: the opening model has only causal state, all explicitly folded."""
    events = _strip_cpp_comments(events)
    owner_members = struct_members(events, "OpeningOwner")
    if _class_fields(events, "OpeningReceipt") != {
            "owner_": "OpeningOwner", "raw_fill_base_": "double", "decision_": "Decision"}:
        raise ValueError("OpeningReceipt fields changed; classify every field in the hash contract")
    if _class_fields(events, "OpeningObligations") != {
            "pending_": "std::optional<OpeningReceipt>"}:
        raise ValueError("OpeningObligations fields changed; classify every field in the hash contract")
    if struct_members(events, "Check") != [("OpeningContinuation", "continuation")]:
        raise ValueError("OpeningReceipt Check fields changed; update the hash contract")
    if struct_body(events, "Exempt").strip():
        raise ValueError("OpeningReceipt Exempt gained state; update the hash contract")
    if not re.search(r"using\s+Decision\s*=\s*std::variant<Check,\s*Exempt>\s*;", events):
        raise ValueError("OpeningReceipt Decision alternatives changed; update the hash contract")
    for enum, expected in [("OpeningDecision", ["Check", "Exempt"]),
                           ("OpeningContinuation", ["None", "RemainingAdversePath"])]:
        body = _one_braced_body(events, rf"enum\s+class\s+{enum}\s*\{{", enum)
        if [x.strip() for x in body.split(",")] != expected:
            raise ValueError(f"{enum} alternatives changed; update the hash encoding")

    body = _one_braced_body(src,
        r"if\s*\(const auto& receipt = opening_obligations_\.peek\(\)\)\s*\{",
        "opening receipt hash")
    if "{" in body or "}" in body or re.search(r"\b(?:if|switch|for|while)\s*\(", body):
        raise ValueError("opening receipt folds must cover Check and Exempt unconditionally")
    for cpp_type, member in owner_members:
        fold = PYRAMID_FOLD.get(cpp_type)
        if not fold or not re.search(rf"\bf\.{fold}\(owner\.{member}\);", body):
            raise ValueError(f"OpeningOwner.{member} missing its typed fold in the receipt body")
    required = [r"f\.i\(static_cast<int64_t>\(receipt->decision\(\)\)\);",
                r"f\.b\(receipt->requires_adverse_pass\(\)\);",
                r"f\.d\(receipt->raw_fill_base\(\)\);",
                r"const auto& owner = receipt->owner\(\);" ]
    if not all(re.search(pattern, body) for pattern in required):
        raise ValueError("opening receipt is missing decision/continuation/raw/owner binding")
    if not re.search(r"f\.b\(opening_obligations_\.pending\(\)\);\s*if", src):
        raise ValueError("opening receipt presence fold must precede its body")


def _intraday_coverage(policy: str, budget: str, obligation: str, src: str) -> None:
    """No waivers: serialize the policy, quota and generic obligation owners.

    Reflect every stored child, but exclude value inputs and decision records:
    those are ephemeral call arguments, not persistent engine state. Class
    storage and enum/schema encodings fail closed when their shape changes.
    """
    policy, budget, obligation = map(_strip_cpp_comments, (policy, budget, obligation))
    for source, name, expected in [
        (policy, "IntradayCap", {
            "schema_version": "static constexpr uint64_t",
            "attachment_": "CapAttachment", "configuration_": "CapConfiguration",
            "budget_": "IntradayOrderBudget", "due_cause_": "std::optional<CloseCause>",
            "next_action_": "uint64_t"}),
        (budget, "IntradayOrderBudget", {
            "day_": "std::optional<OrderRiskDay>", "charged_slots_": "int",
            "latched_": "bool", "transfer_": "std::optional<CloseQuotaTransfer>"}),
        (obligation, "PositionCloseObligation", {
            "due_": "std::optional<PositionCloseRequest>"}),
    ]:
        if _class_fields(source, name) != expected:
            raise ValueError(f"{name} fields changed; classify every field in the hash contract")
    attachment = _one_braced_body(policy, r"enum\s+class\s+CapAttachment\s*\{", "CapAttachment")
    if [x.strip() for x in attachment.split(",")] != ["LegacySource", "None"]:
        raise ValueError("CapAttachment alternatives changed; update the hash encoding")

    children = {name: struct_members(source, name) for source, names in [
        (policy, ("CapConfiguration", "CloseCause")),
        (budget, ("OrderRiskDay", "CloseQuotaTransfer")),
        (obligation, ("PositionCloseRequest",)),
    ] for name in names}

    def folds(name: str, expression: str) -> list[tuple[str, str]]:
        result = []
        for cpp_type, member in children[name]:
            value = expression + member
            if cpp_type == "OrderRiskDay":
                if name == "OrderRiskDay":
                    raise ValueError("OrderRiskDay cannot recursively own itself")
                result.extend(folds(cpp_type, value + "."))
            else:
                fold = PYRAMID_FOLD.get(cpp_type)
                if fold is None:
                    raise ValueError(f"{name}.{member}: unclassified causal type {cpp_type}")
                result.append((value, f"f.{fold}({value});"))
        return result

    cap = "max_intraday_filled_orders_"
    pieces = ["f.u(compat::pine::IntradayCap::schema_version);",
              f"f.i(static_cast<int64_t>({cap}.attachment()));"]
    pieces.extend(fold for _member, fold in folds("CapConfiguration", cap + ".configuration()."))
    for label, presence, opening, name, expression in [
        ("risk day", f"f.b({cap}.budget().day().has_value());",
         f"if (const auto& day = {cap}.budget().day()) {{", "OrderRiskDay", "day->"),
        ("close quota transfer", f"f.b({cap}.budget().transfer().has_value());",
         f"if (const auto& transfer = {cap}.budget().transfer()) {{",
         "CloseQuotaTransfer", "transfer->"),
        ("due cap cause", f"f.b({cap}.due_cause().has_value());",
         f"if (const auto& due = {cap}.due_cause()) {{", "CloseCause", "due->"),
        ("position close obligation", "f.b(position_close_obligation_.pending());",
         "if (const auto& request = position_close_obligation_.peek()) {",
         "PositionCloseRequest", "request->"),
    ]:
        body = _one_braced_body(src, re.escape(opening), label + " hash")
        compact_body = re.sub(r"\s+", "", body)
        expected_folds = folds(name, expression)
        for member, fold in expected_folds:
            if fold not in compact_body:
                raise ValueError(f"{name}.{member} missing its typed fold in the {label} body")
        expected_body = "".join(fold for _member, fold in expected_folds)
        if compact_body != expected_body:
            raise ValueError(f"{label} folds must cover every child unconditionally in declaration order")
        pieces.extend([presence, opening, expected_body, "}"])
        if name == "OrderRiskDay":
            pieces.extend([f"f.i({cap}.budget().charged_slots());",
                           f"f.b({cap}.budget().latched());"])
        if name == "CloseCause":
            pieces.append(f"f.u({cap}.next_action());")

    # The entire block must be consecutive and unconditional at function
    # scope: a mention, wrong type, foreign owner or conditional fold fails.
    hash_body = _one_braced_body(src,
        r"uint64_t\s+BacktestEngine::broker_state_hash\(\)\s+const\s*\{", "broker hash")
    compact_hash = re.sub(r"\s+", "", hash_body)
    block = re.sub(r"\s+", "", "".join(pieces))
    if compact_hash.count(block) != 1:
        raise ValueError("intraday hash requires every typed configuration/state fold beside its owning body")
    prefix = compact_hash[:compact_hash.index(block)]
    if prefix.count("{") != prefix.count("}") or (prefix and prefix[-1] not in ";}"):
        raise ValueError("intraday hash block must be unconditional at broker_state_hash function scope")


def _quantity_request_coverage(quantity: str, source: str) -> None:
    quantity = _strip_cpp_comments(quantity)
    if _class_fields(quantity, "QuantityIntent") != {"value_": "Value"}:
        raise ValueError("QuantityIntent data changed; update its complete hash encoding")
    if _class_fields(quantity, "QuantityRequest") != {
            "intent_": "std::optional<QuantityIntent>",
            "reservation_": "std::optional<QuantityReservation>"}:
        raise ValueError("QuantityRequest data changed; update its complete hash encoding")
    if struct_members(quantity, "Units") != [("double", "amount")]:
        raise ValueError("Units intent fields changed")
    if struct_members(quantity, "Fraction") != [("double", "numerator"), ("double", "denominator")]:
        raise ValueError("Fraction intent fields changed")
    if struct_members(quantity, "QuantityReservation") != [("double", "units"), ("double", "basis_units")]:
        raise ValueError("QuantityReservation fields changed")
    if not re.search(r"struct\s+All\s*\{\s*\}", quantity):
        raise ValueError("All intent must have no numeric payload")
    if not re.search(r"enum\s+class\s+QuantityIntentKind\s*\{\s*Units,\s*Fraction,\s*All\s*\}", quantity):
        raise ValueError("QuantityIntentKind hash encoding changed")
    if not re.search(r"using\s+Value\s*=\s*std::variant<Units,\s*Fraction,\s*All>", quantity):
        raise ValueError("QuantityIntent variant discriminator changed")
    loop = _collection_loop_body(source, "pending_orders_", "o")
    expected = """f.b(o.quantity_request.intent().has_value());
        if (const auto& intent = o.quantity_request.intent()) {
            f.i(static_cast<int64_t>(intent->kind()));
            if (intent->kind() == QuantityIntent::Kind::Units) f.d(intent->units());
            else if (intent->kind() == QuantityIntent::Kind::Fraction) {
                f.d(intent->numerator()); f.d(intent->denominator());
            }
        }
        f.b(o.quantity_request.reservation().has_value());
        if (const auto& reservation = o.quantity_request.reservation()) {
            f.d(reservation->units); f.d(reservation->basis_units);
        }"""
    compact = re.sub(r"\s+", "", loop)
    folded = re.sub(r"\s+", "", expected)
    if compact.count(folded) != 1:
        raise ValueError("quantity intent/reservation hash requires every field and presence discriminator")
    prefix = compact[:compact.index(folded)]
    if prefix.count("{") != prefix.count("}"):
        raise ValueError("quantity request hash must be unconditional inside its order loop")

def _pine_frozen_market_instruction_coverage(header: str, source: str) -> None:
    """An exclusive instruction, with each live payload folded in its owner loop."""
    header = _strip_cpp_comments(header)
    if _class_fields(header, "FrozenMarketInstruction") != {"value_": "Value"}:
        raise ValueError("FrozenMarketInstruction fields changed; classify every frozen fact")
    for name, fields in [
        ("Transaction", [("double", "own_units"), ("double", "transaction_units")]),
        ("TargetedClose", [("std::string", "target_id")]),
    ]:
        if struct_members(header, name) != fields:
            raise ValueError(name + " instruction payload changed; update hash and mirror")
    if not re.search(r"using\s+Value\s*=\s*std::variant<std::monostate,\s*Transaction,\s*TargetedClose>\s*;", header):
        raise ValueError("FrozenMarketInstruction variant discriminator changed")
    body = _one_braced_body(header,
        r"enum\s+class\s+FrozenMarketInstructionKind\s*\{", "FrozenMarketInstructionKind")
    if [x.strip() for x in body.split(",")] != ["Ordinary", "Transaction", "TargetedClose"]:
        raise ValueError("FrozenMarketInstructionKind hash encoding changed")
    loop = _collection_loop_body(source, "pending_orders_", "o")
    expected = """f.i(static_cast<int64_t>(o.pine_frozen_market_instruction.kind()));
        if (const auto* transaction = o.pine_frozen_market_instruction.transaction()) {
            f.d(transaction->own_units); f.d(transaction->transaction_units);
        }
        if (const auto* close = o.pine_frozen_market_instruction.targeted_close()) {
            f.s(close->target_id);
        }"""
    compact = re.sub(r"\s+", "", loop)
    folded = re.sub(r"\s+", "", expected)
    if compact.count(folded) != 1:
        raise ValueError("frozen market instruction requires every live field and role discriminator")
    prefix = compact[:compact.index(folded)]
    if prefix.count("{") != prefix.count("}") or (prefix and prefix[-1] not in ";}"):
        raise ValueError("frozen market instruction hash must be unconditional in its order loop")


def _birth_coverage(header: str, source: str) -> None:
    header = _strip_cpp_comments(header)
    expected = {
        "BirthCursor": {"domain_": "BirthCursorDomain", "position_": "BirthCursorPosition", "index_": "int", "count_": "int"},
        "OrderBirth": {"cause_": "OrderBirthCause", "bar_": "int", "timestamp_": "int64_t", "cursor_": "BirthCursor", "cursor_price_": "double", "first_fill_": "uint64_t", "last_fill_": "uint64_t", "evaluation_ordinal_": "uint64_t"},
    }
    for name, fields in expected.items():
        if _class_fields(header, name) != fields:
            raise ValueError(f"{name} fields changed; classify every nested birth fact")
    loop = _collection_loop_body(source, "pending_orders_", "o")
    compact = re.sub(r"\s+", "", loop)
    expressions = [
        "f.i(static_cast<int64_t>(o.birth.cause()));",
        "f.i(o.birth.bar());", "f.i(o.birth.timestamp());",
        "f.i(static_cast<int64_t>(o.birth.cursor().domain()));",
        "f.i(static_cast<int64_t>(o.birth.cursor().position()));",
        "f.i(o.birth.cursor().index());", "f.i(o.birth.cursor().count());",
        "f.d(o.birth.cursor_price());", "f.u(o.birth.first_fill());",
        "f.u(o.birth.last_fill());", "f.u(o.birth.evaluation_ordinal());",
        "f.i(static_cast<int64_t>(o.pine_birth_reach));",
    ]
    expected = "".join(expressions)
    if compact.count(expected) != 1:
        raise ValueError("birth facts and Pine reach require one complete contiguous hash block")
    prefix = compact[:compact.index(expected)]
    if prefix.count("{") != prefix.count("}"):
        raise ValueError("birth facts must be unconditionally hashed at order-loop scope")


def _exit_activation_coverage(activation: str, policy: str, source: str) -> None:
    activation = _strip_cpp_comments(activation)
    policy = _strip_cpp_comments(policy)
    expected = [
        (activation, "ExitLegActivation", {"bounds_": "std::optional<ExitLegActivationBounds>"}),
        (activation, "ExitLegActivationBounds", {"position_cycle": "int64_t", "stop_first_bar": "int64_t", "limit_first_bar": "int64_t"}),
        (policy, "ExitActivationPolicy", {"evidence_": "std::optional<ExitPlacementEvidence>"}),
        (policy, "ExitPlacementEvidence", {"position_cycle": "int64_t", "entry_bar": "int", "direction": "int", "cursor_price": "double", "stop_level": "double", "limit_level": "double", "limit_continuation": "std::optional<LimitContinuation>"}),
        (policy, "LimitContinuation", {"cause": "LimitContinuationCause", "observed_fill_sequence": "uint64_t"}),
    ]
    for text, name, fields in expected:
        if _class_fields(text, name) != fields:
            raise ValueError(name + " activation fields changed; every value must be hashed")
    loop = _collection_loop_body(source, "pending_orders_", "o")
    expected = """f.b(o.leg_activation.bounds().has_value());
        if (const auto& bounds = o.leg_activation.bounds()) {
            f.i(bounds->position_cycle); f.i(bounds->stop_first_bar); f.i(bounds->limit_first_bar);
        }
        f.b(o.pine_exit_activation.evidence().has_value());
        if (const auto& evidence = o.pine_exit_activation.evidence()) {
            f.i(evidence->position_cycle); f.i(evidence->entry_bar); f.i(evidence->direction);
            f.d(evidence->cursor_price); f.d(evidence->stop_level); f.d(evidence->limit_level);
            f.b(evidence->limit_continuation.has_value());
            if (const auto& continuation = evidence->limit_continuation) {
                f.i(static_cast<int64_t>(continuation->cause)); f.u(continuation->observed_fill_sequence);
            }
        }"""
    compact = re.sub(r"\s+", "", loop)
    folded = re.sub(r"\s+", "", expected)
    if compact.count(folded) != 1:
        raise ValueError("exit activation needs every nested fact and optional discriminator")
    prefix = compact[:compact.index(folded)]
    if prefix.count("{") != prefix.count("}"):
        raise ValueError("exit activation hash block must be unconditional")


def _reservation_expansion_fields(header: str) -> None:
    header = _strip_cpp_comments(header)
    expected = {
        "ReservationExpansion": {"capture_": "std::optional<ReservationExpansionCapture>"},
        "ReservationExpansionCapture": {"position_cycle": "int64_t", "side": "PositionSide", "first_later_admission": "std::optional<uint64_t>"},
        "ReservationGrowthSource": {"reservation_owner_": "std::optional<uint64_t>"},
    }
    for name, fields in expected.items():
        if _class_fields(header, name) != fields:
            raise ValueError(name + " fields changed; every capture/source fact must be hashed")

def _reservation_expansion_coverage(header: str, source: str) -> None:
    _reservation_expansion_fields(header)
    loop = _collection_loop_body(source, "pending_orders_", "o")
    expected = """f.b(o.reservation_expansion.capture().has_value());
        if (const auto& capture = o.reservation_expansion.capture()) {
            f.i(capture->position_cycle);
            f.i(static_cast<int64_t>(capture->side));
            f.b(capture->first_later_admission.has_value());
            if (const auto& admission = capture->first_later_admission) {
                f.u(*admission);
            }
        }
        f.b(o.reservation_growth_source.reservation_owner().has_value());
        if (const auto& receiver = o.reservation_growth_source.reservation_owner()) {
            f.u(*receiver);
        }"""
    compact = re.sub(r"\s+", "", loop)
    folded = re.sub(r"\s+", "", expected)
    if compact.count(folded) != 1:
        raise ValueError("reservation capture/source encoding needs every nested fact and discriminator once")
    prefix = compact[:compact.index(folded)]
    if prefix.count("{") != prefix.count("}") or (prefix and prefix[-1] not in ";}"):
        raise ValueError("reservation capture/source hash block must be unconditional")


def _reservation_expansion_version_coverage(header: str, source: str) -> None:
    """Standalone layout-sensitive types and methods own their first ABI."""
    declaration = _one_braced_body(_strip_cpp_comments(header),
        r"inline\s+namespace\s+reservation_expansion_v1\s*\{", "reservation ABI")
    implementation = _one_braced_body(_strip_cpp_comments(source),
        r"inline\s+namespace\s+reservation_expansion_v1\s*\{", "reservation implementation ABI")
    for name in ("ReservationExpansionCapture", "ReservationExpansion", "ReservationGrowthSource"):
        if not re.search(r"\b(?:class|struct)\s+" + name + r"\s*\{", declaration):
            raise ValueError(name + " must belong to reservation_expansion_v1")
    for name in ("ReservationExpansion::capture", "ReservationExpansion::close_population",
                 "ReservationExpansion::owns_exposure", "ReservationExpansion::grow",
                 "ReservationGrowthSource::assign_capture"):
        if not re.search(r"\b" + re.escape(name) + r"\s*\(", implementation):
            raise ValueError(name + " must be implemented in reservation_expansion_v1")


def _runtime_version_coverage(header: str, source: str, stream: str) -> None:
    """The v13 layout and serialized-state contracts must advance together.

    Pin the actual hash entry points, rather than accepting a version string
    mentioned in a comment or an unrelated helper. Public C ABI versions have
    a separate contract and are not changed by this internal epoch.
    """
    header = _strip_cpp_comments(header)
    namespaces = re.findall(r"inline\s+namespace\s+(engine_script_run_v\d+)\s*\{", header)
    if namespaces != ["engine_script_run_v13", "engine_script_run_v13"]:
        raise ValueError("PendingOrder and BacktestEngine layouts require internal namespace engine_script_run_v13")
    broker = _one_braced_body(source,
        r"uint64_t\s+BacktestEngine::broker_state_hash\(\)\s+const\s*\{", "broker hash")
    if not re.match(r'\s*Fnv\s+f;\s*f\.s\("pineforge-broker-state/v13"\);', broker):
        raise ValueError("broker hash must start with pineforge-broker-state/v13")
    stream_body = _one_braced_body(_strip_cpp_comments(stream),
        r"uint64_t\s+BacktestEngine::stream_state_hash\(\)\s+const\s*\{", "stream hash")
    compact = re.sub(r"\s+", "", stream_body)
    fold = "integer(13);integer(broker_state_hash());"
    if compact.count(fold) != 1:
        raise ValueError("stream hash requires version 13 followed by the broker hash")
    prefix = compact[:compact.index(fold)]
    if prefix.count("{") != prefix.count("}") or (prefix and prefix[-1] not in ";}"):
        raise ValueError("stream v13 version fold must be unconditional at function scope")


def main(root: Path = ROOT) -> int:
    hpp = (root / "include/pineforge/engine.hpp").read_text(encoding="utf-8")
    regions = _regions(hpp)
    members = _members(regions)

    src_raw = (root / "src/engine_state_hash.cpp").read_text(encoding="utf-8")
    src = _strip_cpp_comments(src_raw)
    try:
        from check_exit_leg_lifecycle import check as check_exit_lifecycle
        check_exit_lifecycle((root / "include/pineforge/exit_leg_lifecycle.hpp").read_text(), src)
        from check_market_admission_schema import check as market_admission_coverage
        market_admission_coverage(root)
        _runtime_version_coverage(hpp, src, (root / "src/engine_stream.cpp").read_text())
        _reservation_expansion_version_coverage(
            (root / "include/pineforge/reservation_expansion.hpp").read_text(),
            (root / "src/reservation_expansion.cpp").read_text())
        _reservation_expansion_coverage((root / "include/pineforge/reservation_expansion.hpp").read_text(), src)
        _pine_frozen_market_instruction_coverage(
            (root / "include/pineforge/compat/pine/frozen_market_instruction.hpp").read_text(), src)
        _birth_coverage((root / "include/pineforge/order_birth.hpp").read_text(), src)
        _exit_activation_coverage((root / "include/pineforge/leg_activation.hpp").read_text(),
            (root / "include/pineforge/compat/pine/exit_activation.hpp").read_text(), src)
        _opening_coverage((root / "include/pineforge/broker_events.hpp").read_text(), src)
        _quantity_request_coverage((root / "include/pineforge/quantity_intent.hpp").read_text(), src)
        _intraday_coverage(
            (root / "include/pineforge/compat/pine/intraday_cap.hpp").read_text(),
            (root / "include/pineforge/compat/pine/intraday_order_budget.hpp").read_text(),
            (root / "include/pineforge/position_close_obligation.hpp").read_text(), src)
    except (ValueError, OSError) as exc:
        print(f"check_broker_state_hash_coverage: {exc}", file=sys.stderr)
        return 1

    all_waivers = _load_waivers(root / "scripts/broker_state_hash_waivers.txt")
    if {"max_intraday_filled_orders_", "position_close_obligation_"} & all_waivers.keys():
        print("check_broker_state_hash_coverage: Pine cap and generic close owners cannot be waived",
              file=sys.stderr)
        return 1
    waivers = {k: v for k, v in all_waivers.items()
               if not k.startswith((PENDING_WAIVER_PREFIX, PYRAMID_WAIVER_PREFIX))}
    if {"pending_order.reservation_expansion", "pending_order.reservation_growth_source"} & all_waivers.keys():
        print("check_broker_state_hash_coverage: reservation capture/source cannot be waived", file=sys.stderr)
        return 1
    if {"pending_order.quantity_request", "pending_order.pine_frozen_market_instruction", "pending_order.legs"} & all_waivers.keys():
        print("check_broker_state_hash_coverage: quantity_request and pine_frozen_market_instruction cannot be waived", file=sys.stderr)
        return 1
    po_waivers = {k[len(PENDING_WAIVER_PREFIX):]: v
                  for k, v in all_waivers.items() if k.startswith(PENDING_WAIVER_PREFIX)}
    pe_waivers = {k[len(PYRAMID_WAIVER_PREFIX):]: v
                  for k, v in all_waivers.items() if k.startswith(PYRAMID_WAIVER_PREFIX)}

    orphans = sorted(w for w in waivers if w not in members)
    if orphans:
        print("check_broker_state_hash_coverage: waiver(s) naming a member not "
              f"in any // @broker-state region: {orphans}", file=sys.stderr)
        return 1

    missing = sorted(
        m for m in members
        if not re.search(rf"\b{re.escape(m)}\b", src) and m not in waivers
    )
    if missing:
        print("check_broker_state_hash_coverage: unhashed, unwaived broker-state members:", missing)
        return 1

    # --- struct PendingOrder: every scalar/string member, o.<name> in the loop ---
    po_members = [n for _t, n in struct_members(hpp)]
    po_orphans = sorted(w for w in po_waivers if w not in po_members)
    if po_orphans:
        print("check_broker_state_hash_coverage: pending_order.* waiver(s) naming a "
              f"member not in struct PendingOrder: {po_orphans}", file=sys.stderr)
        return 1
    loop = _collection_loop_body(src, "pending_orders_", "o")
    po_missing = sorted(
        m for m in po_members
        if not re.search(rf"\bo\.{re.escape(m)}\b", loop) and m not in po_waivers
    )
    if po_missing:
        print("check_broker_state_hash_coverage: PendingOrder members neither hashed "
              "(o.<name> in the pending_orders_ loop) nor waived (pending_order.<name>):",
              po_missing)
        return 1
    # --- struct PyramidEntry: inspect every physical-lot field recursively ---
    pe_members = struct_members(hpp, "PyramidEntry")
    pe_orphans = sorted(set(pe_waivers) - {n for _t, n in pe_members})
    if pe_orphans:
        print("check_broker_state_hash_coverage: pyramid_entry.* waiver(s) naming a "
              f"member not in struct PyramidEntry: {pe_orphans}", file=sys.stderr)
        return 1
    pe_loop = _collection_loop_body(src, "pyramid_entries_", "e")
    pe_missing = sorted(n for t, n in pe_members
                        if n not in pe_waivers and not _pyramid_folded(pe_loop, t, n))
    pe_redundant = sorted(n for t, n in pe_members
                          if n in pe_waivers and _pyramid_folded(pe_loop, t, n))
    if pe_missing or pe_redundant:
        print("check_broker_state_hash_coverage: PyramidEntry requires one typed fold "
              "inside its loop or a justified pyramid_entry.* waiver; "
              f"missing={pe_missing}, redundant_waivers={pe_redundant}")
        return 1
    print(f"check_broker_state_hash_coverage: {len(members)} members in {len(regions)} "
          f"region(s), {len(waivers)} waived, OK; PendingOrder {len(po_members)} members, "
          f"{len(po_waivers)} waived, OK; PyramidEntry {len(pe_members)} members, "
          f"{len(pe_waivers)} waived, OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
