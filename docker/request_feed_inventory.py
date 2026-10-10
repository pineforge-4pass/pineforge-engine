"""Request-feed inventory admission for selected-window mode (the consumer half).

A selected-window request is admitted only when the request-feed timeframes of
the compiled artifact are known BEFORE any feed is opened, any library is
loaded, any strategy is constructed or any optimizer trial is allocated. The
pinned transpiler emits an inventory beside the artifact; this module reads that
inventory and the artifact's bytes, and nothing else: never a feed or a feed
index, never ctypes, dlopen, an import of the artifact, a strategy or a
subprocess, and no parsing or evaluation of the generated C++. It is a
standalone module and is not wired into run_json here: where it runs (after
request validation, before the first feed or library operation) belongs to the
integration that calls it.

The inventory is one JSON object with exactly these members:

    {"schema": "pineforge-request-feed-inventory/v1",
     "source_sha256": <64 lowercase hex>, "artifact_sha256": <64 lowercase hex>,
     "primary_chart_timeframe": <timeframe token or null>,
     "entries": [{"kind": "token", "timeframe": <string>},
                 {"kind": "input", "key": <string>, "default": <string>},
                 {"kind": "unknown"}, ...]}

A duplicate key, an extra or missing member and a wrong type are refused; nothing
is coerced. An empty "entries" is COMPLETE (the request reads no request feed),
an "unknown" entry is INCOMPLETE. artifact_sha256 must equal the SHA-256 of the
artifact bytes read here (null means unbound); source_sha256 is checked for its
form only, because the generated C++ is not read. A recorded
primary_chart_timeframe must equal the request's canonical primary timeframe. An
"input" entry resolves to validated_inputs[key] when the request validated that
key, else to its declared default; the value must be a string that is a token.

Timeframe tokens: whole minutes without leading zeros ("1", "240", "100000"), or
<n>D, <n>W, <n>M, <n>S with n a positive whole number without leading zeros
("2D", "10000D", "12M", "30S"). The grammar has no length limit, and digits are
never converted to a number: a spelling is compared as written, after the
normalization below. D and 1D are D, W and 1W are W, a bare M or S is 1M or 1S.
No whitespace, no empty string, no case folding.

A refusal is InventoryAdmissionError: code window_mode_unsupported and a detail
dict naming the capability. An incomplete inventory carries only
selected_window_request_feed_inventory, never a timeframe. admit_selected_request
checks the primary timeframe against the support policy FIRST and reads no file
for a primary outside it. It never builds a support policy: a policy that is
missing, or is not exactly the three documented members, supports nothing.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
from collections.abc import Mapping
from typing import Any, List, Optional, Tuple

__all__ = [
    "InventoryAdmissionError",
    "admit_selected_request",
    "resolve_request_feed_inventory",
]

CODE = "window_mode_unsupported"
SCHEMA = "pineforge-request-feed-inventory/v1"
CAPABILITY_CHART = "selected_window_chart_timeframe"
CAPABILITY_FEED = "selected_window_request_feed_timeframe"
CAPABILITY_INVENTORY = "selected_window_request_feed_inventory"

_INVENTORY_KEYS = frozenset(("schema", "source_sha256", "artifact_sha256",
                             "primary_chart_timeframe", "entries"))
_POLICY_KEYS = frozenset(("primary_chart_timeframes", "preroll", "request_feed_timeframes"))
_SHA256_RE = re.compile(r"[0-9a-f]{64}")
_TIMEFRAME_RE = re.compile(r"(?:(?P<minutes>[1-9][0-9]*)"
                           r"|(?P<count>[1-9][0-9]*)?(?P<unit>[DWMS]))")
_INVENTORY_MAX_BYTES = 8 * 1024 * 1024
_READ_CHUNK = 1024 * 1024


class InventoryAdmissionError(Exception):
    """A selected-window request this helper refuses. `code` is
    window_mode_unsupported; `detail` is the code's typed arguments:
    {"capability": "selected_window_chart_timeframe" | "selected_window_request_feed_timeframe",
    "timeframe": <the offending canonical token>}, or {"capability":
    "selected_window_request_feed_inventory"} with no timeframe member when the
    inventory is incomplete. The arguments are `detail` because
    BaseException.args is the exception's own tuple."""

    code = CODE

    def __init__(self, message: str, detail: Mapping[str, str]) -> None:
        super().__init__(message)
        self.message = message
        self.detail = dict(detail)


def _incomplete(reason: str) -> InventoryAdmissionError:
    return InventoryAdmissionError(f"the request-feed inventory is incomplete: {reason}",
                                   {"capability": CAPABILITY_INVENTORY})


def _canonical_timeframe(value: Any) -> Optional[str]:
    """The wire's canonical spelling of value, or None when value is not a string
    that spells a timeframe token."""
    if not isinstance(value, str):
        return None
    match = _TIMEFRAME_RE.fullmatch(value)
    if match is None:
        return None
    if match.group("minutes") is not None:
        return value
    count, unit = match.group("count"), match.group("unit")
    if unit in ("D", "W") and count in (None, "1"):
        return unit
    return (count or "1") + unit


def _policy_token(value: Any) -> Optional[str]:
    """value when it is already a canonical token, else None: a policy is written
    in the wire's own spelling and an alias in it is a value nobody defined."""
    token = _canonical_timeframe(value)
    return token if token is not None and token == value else None


def _require_primary(primary_timeframe: Any) -> str:
    primary = _canonical_timeframe(primary_timeframe)
    if primary is None:
        raise ValueError(f"primary_timeframe is not a timeframe token: {primary_timeframe!r:.80}")
    return primary


def _policy_members(policy: Any) -> Optional[Tuple[Any, Any]]:
    """The policy's primary and request-feed members, each the word "all" or a
    frozenset of canonical tokens; None when the policy is not a mapping of exactly
    primary_chart_timeframes, preroll and request_feed_timeframes with values the
    the documented support policy defines. preroll is checked for its form only: a
    pre-roll length is no argument of this helper."""
    if not isinstance(policy, Mapping) or set(policy) != _POLICY_KEYS:
        return None
    preroll = policy["preroll"]
    if not isinstance(preroll, str) or preroll not in ("supported", "zero_only"):
        return None
    members = []
    for key in ("primary_chart_timeframes", "request_feed_timeframes"):
        value = policy[key]
        if isinstance(value, str):
            if value != "all":
                return None
            members.append("all")
            continue
        if not isinstance(value, (list, tuple)):
            return None
        tokens = set()
        for item in value:
            token = _policy_token(item)
            if token is None:
                return None
            tokens.add(token)
        members.append(frozenset(tokens))
    return members[0], members[1]


def _read_inventory_bytes(path: Any) -> bytes:
    if not isinstance(path, (str, os.PathLike)):
        raise _incomplete("no inventory path was given")
    try:
        with open(path, "rb") as handle:
            data = handle.read(_INVENTORY_MAX_BYTES + 1)
    except (OSError, ValueError) as error:
        raise _incomplete(f"the inventory cannot be read: {error}") from None
    if len(data) > _INVENTORY_MAX_BYTES:
        raise _incomplete(f"the inventory is over {_INVENTORY_MAX_BYTES} bytes")
    return data


def _no_duplicate_keys(pairs: List[Tuple[str, Any]]) -> dict:
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError("duplicate key")
        out[key] = value
    return out


def _parse_inventory(data: bytes) -> Any:
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=_no_duplicate_keys)
    except (ValueError, RecursionError):
        raise _incomplete("the inventory is not strict JSON (bad UTF-8, syntax or a "
                          "duplicate key)") from None


def _checked_entry(position: int, entry: Any) -> tuple:
    keys = set(entry) if isinstance(entry, dict) else None
    kind = entry.get("kind") if keys is not None else None
    if kind == "token" and keys == {"kind", "timeframe"} and isinstance(entry["timeframe"], str):
        return ("token", entry["timeframe"])
    if (kind == "input" and keys == {"kind", "key", "default"}
            and isinstance(entry["key"], str) and isinstance(entry["default"], str)):
        return ("input", entry["key"], entry["default"])
    if kind == "unknown" and keys == {"kind"}:
        return ("unknown",)
    raise _incomplete(f"entry {position} is not exactly one of the three entry shapes")


def _checked_entries(doc: Any) -> List[tuple]:
    """doc's entries as ("token", timeframe), ("input", key, default) or
    ("unknown",), after the exact-shape checks of the whole object."""
    if not isinstance(doc, dict) or set(doc) != _INVENTORY_KEYS:
        raise _incomplete("the inventory is not one object with exactly the five schema members")
    if not isinstance(doc["schema"], str) or doc["schema"] != SCHEMA:
        raise _incomplete(f"the inventory's schema is not {SCHEMA}")
    if doc["artifact_sha256"] is None:
        raise _incomplete("the inventory is unbound (artifact_sha256 is null)")
    for member in ("source_sha256", "artifact_sha256"):
        value = doc[member]
        if not isinstance(value, str) or _SHA256_RE.fullmatch(value) is None:
            raise _incomplete(f"{member} is not 64 lowercase hex digits")
    binding = doc["primary_chart_timeframe"]
    if binding is not None and _canonical_timeframe(binding) is None:
        raise _incomplete("primary_chart_timeframe is neither null nor a timeframe token")
    if not isinstance(doc["entries"], list):
        raise _incomplete("entries is not an array")
    return [_checked_entry(position, entry) for position, entry in enumerate(doc["entries"])]


def _artifact_digest(path: Any) -> str:
    """The SHA-256 of the artifact's bytes, streamed. The artifact is hashed, never
    loaded."""
    if not isinstance(path, (str, os.PathLike)):
        raise _incomplete("no artifact path was given")
    digest = hashlib.sha256()
    try:
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(_READ_CHUNK), b""):
                digest.update(chunk)
    except (OSError, ValueError) as error:
        raise _incomplete(f"the artifact cannot be read: {error}") from None
    return digest.hexdigest()


def resolve_request_feed_inventory(inventory_path: Any, artifact_path: Any,
                                   validated_inputs: Any, *,
                                   primary_timeframe: Any) -> List[str]:
    """The sorted, unique canonical request-feed timeframes of one request, from
    the artifact's inventory. Reads the inventory file, then the artifact's bytes
    for their digest, and nothing else.

    Raises InventoryAdmissionError (code window_mode_unsupported, detail
    {"capability": "selected_window_request_feed_inventory"}) when the inventory
    is incomplete for this request: absent, unreadable, not strict JSON, not
    exactly the schema, unbound, bound to another artifact, bound to another
    primary chart timeframe, holding an unknown entry, or holding an input
    reference that does not resolve to a token (an input value that is not a
    string included: it is not coerced, and a present value never falls back to
    the default). ValueError when primary_timeframe is not a timeframe token: the
    request was validated before this call."""
    primary = _require_primary(primary_timeframe)
    if not isinstance(validated_inputs, Mapping):
        raise _incomplete("the request's validated inputs are not a mapping")
    doc = _parse_inventory(_read_inventory_bytes(inventory_path))
    entries = _checked_entries(doc)
    if _artifact_digest(artifact_path) != doc["artifact_sha256"]:
        raise _incomplete("the inventory is bound to another artifact")
    binding = doc["primary_chart_timeframe"]
    if binding is not None and _canonical_timeframe(binding) != primary:
        raise _incomplete("the inventory's chart binding is not the request's primary timeframe")
    tokens = set()
    for entry in entries:
        if entry[0] == "unknown":
            raise _incomplete("an entry is not known before the strategy runs")
        if entry[0] == "token":
            token = _canonical_timeframe(entry[1])
            if token is None:
                raise _incomplete("a token entry does not spell a timeframe token")
        else:
            key, default = entry[1], entry[2]
            token = _canonical_timeframe(validated_inputs[key] if key in validated_inputs
                                         else default)
            if token is None:
                raise _incomplete(f"input {key!r:.80} does not resolve to a timeframe token")
        tokens.add(token)
    return sorted(tokens)


def admit_selected_request(primary_timeframe: Any, inventory_path: Any, artifact_path: Any,
                           validated_inputs: Any, support_policy: Any) -> Optional[List[str]]:
    """Admit a selected-window request against the build's support policy, or
    raise InventoryAdmissionError. In this order: canonicalize the primary
    timeframe; enforce the policy's primary list (a policy that is missing or not
    exactly the three documented members supports no primary: nothing is
    synthesized); resolve the inventory; check each resolved token against the
    policy's request-feed list.

    A primary outside the policy is {"capability": "selected_window_chart_timeframe",
    "timeframe": <canonical primary>} and reads no file. A resolved token outside
    the list is {"capability": "selected_window_request_feed_timeframe",
    "timeframe": <that canonical token>}, the first in sorted order when several
    are outside. An incomplete inventory is the inventory capability, as in
    resolve_request_feed_inventory. Returns the sorted canonical tokens. When the
    policy says "all" for request feeds the set does not enter the decision: the
    inventory is not read and the result is None, not an empty set. ValueError
    when primary_timeframe is not a timeframe token."""
    primary = _require_primary(primary_timeframe)
    members = _policy_members(support_policy)
    if members is None:
        raise InventoryAdmissionError(
            "the support policy is missing or is not exactly primary_chart_timeframes, "
            f"preroll and request_feed_timeframes, so primary timeframe {primary} is not supported",
            {"capability": CAPABILITY_CHART, "timeframe": primary})
    if members[0] != "all" and primary not in members[0]:
        raise InventoryAdmissionError(
            f"selected-window mode is not supported for primary timeframe {primary}",
            {"capability": CAPABILITY_CHART, "timeframe": primary})
    if members[1] == "all":
        return None
    tokens = resolve_request_feed_inventory(inventory_path, artifact_path, validated_inputs,
                                            primary_timeframe=primary)
    for token in tokens:
        if token not in members[1]:
            raise InventoryAdmissionError(
                f"selected-window mode is not supported for request-feed timeframe {token}",
                {"capability": CAPABILITY_FEED, "timeframe": token})
    return tokens
