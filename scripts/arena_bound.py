"""Pre-start ScratchArena arithmetic, shared with APP/COMPUTE.

This module performs no I/O and uses integer arithmetic only.  Its input is a
trusted compiler/lowering facts record plus a data-plan record, NOT a report.
See docs/design/arena-bound.md for the proof obligations on the facts producer.
An absent proof is not a zero.  It produces an explicit uncertified result.
"""

from __future__ import annotations

from collections.abc import Mapping


CHUNK_BYTES = 1_048_576
ROUNDING_REGIONS = 14
COMMAND_STORES = 3
COMMAND_SHARE_DENOMINATOR = 16  # kCommandScratchArenaShare = 1/16.
MAX_U64 = (1 << 64) - 1
# Shape::chunk_bytes defaults to 1024 and o1_store.cpp's object header is
# 16 bytes.  The source cadence uses the same 1<<20 platform guard for an
# unbounded/calc-on-order-fills per-execution receipt.
OBJECT_HEADER_BYTES = 16
OBJECT_CHUNK_BYTES = 1024
OBJECTS_PER_EXECUTION_PLATFORM_MAX = 1 << 20
# ScratchArena::Impl rejects chunks > uint32_t::max(), not >= 2**32.
# This is a representation limit, NEVER a substitute bound for an unknown run.
MAX_ARENA_BYTES = ((1 << 32) - 1) * CHUNK_BYTES
FACTS_SCHEMA = "pineforge-arena-facts/v1"
WIRE_LINE_BYTES = 1_048_576  # Includes LF, report_sink_writer.cpp check_line.


class FactsError(ValueError):
    """A facts record is malformed, as opposed to explicitly uncertified."""


class MissingProof(FactsError):
    """A required pre-start fact/proof is absent, never implicitly zero."""


class BoundOverflow(OverflowError):
    """The finite mathematical result exceeds the supported integer range."""


def _integer(value, name: str) -> int:
    if type(value) is not int or value < 0:
        raise FactsError(f"{name} must be a nonnegative integer (not bool/float)")
    if value > MAX_U64:
        raise BoundOverflow(f"{name} exceeds uint64")
    return value


def _add(*values: int) -> int:
    result = 0
    for value in values:
        if value > MAX_U64 - result:
            raise BoundOverflow("addition exceeds uint64")
        result += value
    return result


def _mul(left: int, right: int) -> int:
    if right != 0 and left > MAX_U64 // right:
        raise BoundOverflow("multiplication exceeds uint64")
    return left * right


def _ceil_chunks(value: int, name: str) -> int:
    value = _integer(value, name)
    return _add(value // CHUNK_BYTES, int(value % CHUNK_BYTES != 0))


def object_store_reserve_bytes(bars: int, objects_per_execution: int,
                               record_bytes: int) -> int:
    """Return the checked object-record byte reservation.

    This is deliberately the small pure arithmetic function shared with the
    C++ ``object_store_reserve_bytes`` helper.  All three inputs are integer
    byte/count facts, so ``ceil(bars * k_obj * record_bytes)`` is the checked
    product itself.  The arena consumer rounds that byte reservation to whole
    arena chunks before taking its maximum with the existing floor.
    """
    bars = _integer(bars, "object-store bars")
    objects_per_execution = _integer(
        objects_per_execution, "objects per execution")
    record_bytes = _integer(record_bytes, "object-store record bytes")
    return _mul(_mul(bars, objects_per_execution), record_bytes)


def combined_arena_chunks(non_command_chunks: int) -> int:
    """Smallest n with n - 3*max(1, n//16) >= non_command_chunks.

    In share block q>=1, n=16*q+r (0<=r<=15) leaves 13*q+r
    non-command chunks. The first block reaching b has
    q=max(1, ceil((b-15)/13))=max(1, floor((b-3)/13)).
    The one-chunk floor also covers n<16. Guard subtraction for b<3;
    arena_bound itself supplies b>=ROUNDING_REGIONS=14.
    """
    non_command_chunks = _integer(non_command_chunks, "non-command chunks")
    share_chunks = (
        max(1, (non_command_chunks - COMMAND_STORES)
            // (COMMAND_SHARE_DENOMINATOR - COMMAND_STORES))
        if non_command_chunks >= COMMAND_STORES else 1
    )
    return _add(non_command_chunks, _mul(COMMAND_STORES, share_chunks))


def _object(value, name: str) -> Mapping:
    if value is None:
        raise MissingProof(f"missing {name}")
    if not isinstance(value, Mapping):
        raise FactsError(f"{name} must be an object")
    return value


def _source(record: Mapping, name: str) -> str:
    if "source" not in record:
        raise MissingProof(f"missing {name}.source")
    value = record.get("source")
    if not isinstance(value, str) or not value.strip():
        raise FactsError(f"{name}.source must identify the pre-start proof")
    return value


def _number(record: Mapping, key: str, name: str) -> int:
    if key not in record:
        raise MissingProof(f"missing {name}.{key}; an absent bound is not zero")
    return _integer(record[key], f"{name}.{key}")


def _derived(value, formula: str, *sources: str) -> dict:
    return {
        "value": value,
        "derivation": {"formula": formula, "sources": list(sources)},
    }


def _unknown_inputs(reason: str) -> dict:
    return {
        name: _derived(None, "no certified pre-start value", reason)
        for name in ("T", "D", "Q", "L", "R", "E")
    }


def slots(keys: int) -> int:
    """Largest IdPairStore index needed for distinct-EVER keys."""
    keys = _integer(keys, "keys")
    if keys == 0:
        return 0
    target = _mul(2, keys)
    result = 64
    while result < target:
        result = _mul(2, result)
    return result


def index_peak(keys: int) -> int:
    """Bytes in the old AND new index during the largest rehash."""
    keys = _integer(keys, "keys")
    if keys == 0:
        return 0
    if keys <= 32:
        return 16 * 64
    return _mul(24, slots(keys))


def fold_opened_segments(rows: int, record_bytes: int,
                        off_max: int = (1 << 63) - 1) -> int:
    """Lazy IdPairFoldFile regions, id_pair_ordered_fold.hpp:59-89.

    off_max is explicit for the existing reduced-offset fixture. Production
    uses signed 64-bit off_t. No per-record padding is added at a boundary.
    """
    rows = _integer(rows, "fold rows")
    record_bytes = _integer(record_bytes, "fold record bytes")
    off_max = _integer(off_max, "fold OFF_MAX")
    if record_bytes == 0 or off_max < record_bytes:
        raise FactsError("a fold record must fit in OFF_MAX")
    rows_per_segment = off_max // record_bytes
    return rows // rows_per_segment + int(rows % rows_per_segment != 0)


def ordered_fold_bytes(keys: int, component_bytes: int) -> int:
    """Distinct-EVER joined keys, including tombstones and staged appends.

    id_pair_ordered_fold.hpp:161-184 pins Leaf=136, Branch=2088 (including
    its 2056-byte map), Snapshot=2096. Its binary splits reuse the old leaf
    and add at most one leaf/branch per new key (:282-336). Thus leaves<=Q,
    branches<=Q-1. Pivots strictly increase (:101-102); the joined key has
    <=2L+1 bytes. The saved mutation path includes its final leaf (:247-258).
    """
    keys = _integer(keys, "fold keys")
    component_bytes = _integer(component_bytes, "fold component bytes")
    if keys == 0:
        return 0
    leaves, branches = keys, keys - 1
    joined_length = _add(_mul(2, component_bytes), 1)
    saved_path = _add(min(branches, _mul(9, _add(joined_length, 1))), 1)
    return _add(_mul(136, leaves), _mul(2088, branches), _mul(2096, saved_path))


def per_user_bytes(inputs: Mapping) -> dict[str, int]:
    """Ten storage contributions plus the explicit stock-source openings zero.

    Accepts the ``inputs`` member returned by arena_bound.  Kept separate so
    arena_bound's public result has EXACTLY the A5 fields. There are still
    eleven entries, but no sequence-minima store: ScratchManifest reserves
    K_q/B_q as zero (scratch_tables.hpp:16, scratch_slab.hpp:78).
    """
    counts = {
        name: _integer(_object(inputs[name], name)["value"], name)
        for name in ("T", "D", "Q", "L", "R", "E")
    }
    t, d, q, length, roots, equity = (
        counts[name] for name in ("T", "D", "Q", "L", "R", "E")
    )
    rank = min(d, 27)
    pair_data = _add(_mul(_add(40, length), d),
                     _mul(_add(40, _mul(2, length)), q))
    pair_index = index_peak(_add(d, q))
    return {
        "equity": _mul(24, equity),
        "positionTail": _mul(_add(30, length), t),
        "cohorts": _add(_mul(_add(96, length), d), int(d != 0)),
        "exitRank": _add(_mul(_add(60, length), rank), int(rank != 0)),
        "erasedData": pair_data,
        "erasedIndex": pair_index,
        "consumedData": _mul(_add(41, _mul(2, length)), q),
        "consumedIndex": index_peak(q),
        "orderedFold": ordered_fold_bytes(q, length),
        "successorRoots": _mul(16, roots),
        # Certified EngineRecorded stock-source profile ONLY: its owners
        # build vectors, never external_openings (pine_adapter.cpp:6983,
        # :6990, :9669). Generic C++ external readbacks are excluded.
        # Each admitted external freeze would instead retain 8*N bytes;
        # N>=2 has 16*N sort peak, plus all other simultaneously retained
        # owners. See native_order.cpp:111-179 and arena-bound.md.
        "openings": 0,
    }


def _arena_term(facts: Mapping) -> dict:
    """Derive the A5 arena sub-result.

    ``unbounded`` means no finite, representable *certified* arena is available.
    Reasons distinguish UNBOUNDED (producer proved no finite bound),
    UNCERTIFIED (missing/unsupported proof), and PLATFORM_RANGE (finite but
    outside this arena representation).  None of these is a capacity refusal.
    Malformed records raise FactsError; overflow is returned explicitly.
    """
    facts = _object(facts, "facts")
    inputs = _unknown_inputs("facts not yet certified")

    def unavailable(reason: str) -> dict:
        return {"arenaBytes": None, "inputs": inputs,
                "unbounded": True, "reason": reason}

    if facts.get("schema") != FACTS_SCHEMA:
        raise FactsError(f"schema must be {FACTS_SCHEMA!r}")
    data = _object(facts.get("data"), "data")
    strategy = _object(facts.get("strategy"), "strategy")
    try:
        data_source = _source(data, "data")
        script_bars = _number(data, "scriptBars", "data")
        selected = _number(data, "selected", "data")
        if selected not in (0, 1):
            raise FactsError("data.selected must be the integer 0 or 1")
        window_bars = _number(data, "windowBars", "data")
        if window_bars > script_bars:
            raise FactsError("data.windowBars must be <= data.scriptBars")
        inputs["E"] = _derived(
            _add(window_bars, 1) if selected else script_bars,
            "selected ? windowBars + 1 : scriptBars", data_source)

        control = _object(strategy.get("controlFlow"), "strategy.controlFlow")
        control_source = _source(control, "strategy.controlFlow")
        status = control.get("status")
        if status not in ("bounded", "unbounded", "unknown"):
            raise FactsError("controlFlow.status must be bounded, unbounded, or unknown")
        if status != "bounded":
            reason = control.get("reason")
            if not isinstance(reason, str) or not reason.strip():
                raise FactsError("uncertified/unbounded control flow requires a reason")
            prefix = "UNBOUNDED" if status == "unbounded" else "UNCERTIFIED"
            return unavailable(f"{prefix}: {reason}; {control_source}")

        fills = _object(strategy.get("fills"), "strategy.fills")
        fill_source = _source(fills, "strategy.fills")
        fill_count = _add(
            _mul(script_bars, _number(fills, "perBar", "strategy.fills")),
            _number(fills, "fixed", "strategy.fills"))
        rows = _number(fills, "rowsPerFill", "strategy.fills")
        terminal = _number(fills, "fixedRows", "strategy.fills")
        inputs["T"] = _derived(
            _add(_mul(fill_count, rows), terminal),
            "(scriptBars * fills.perBar + fills.fixed) * fills.rowsPerFill + fills.fixedRows",
            data_source, fill_source, control_source)

        ids = _object(strategy.get("ids"), "strategy.ids")
        id_source = _source(ids, "strategy.ids")
        literals = ids.get("literals")
        if not isinstance(literals, (list, tuple)) or any(
                not isinstance(value, str) for value in literals):
            raise FactsError("strategy.ids.literals must be a string array")
        # The common domain includes empty from_entry even when Pine omitted it.
        literal_count = _integer(len(set(literals) | {""}), "literal ID population")
        inputs["D"] = _derived(
            _add(literal_count,
                 _mul(script_bars, _number(ids, "freshPerBar", "strategy.ids")),
                 _number(ids, "fixedFresh", "strategy.ids")),
            "|unique(ids.literals union {empty})| + scriptBars * ids.freshPerBar + ids.fixedFresh",
            data_source, id_source, control_source)
        length = _number(ids, "maxBytes", "strategy.ids")
        if any(len(value.encode("utf-8")) > length for value in literals):
            raise FactsError("ids.maxBytes is shorter than a literal in UTF-8 bytes")
        inputs["L"] = _derived(
            length, "ids.maxBytes (complete source/generated ID component closure)",
            id_source, control_source)

        # Each pair domain has at most D**2 keys.  A separately proved count
        # of *key opportunities*, including attempts with no accepted order,
        # can tighten it.  Placement count alone does not cover consumed keys.
        pairs = _object(strategy.get("pairKeys"), "strategy.pairKeys")
        pair_source = _source(pairs, "strategy.pairKeys")
        d = inputs["D"]["value"]
        if pairs.get("mode") == "cartesian":
            pair_count = _mul(d, d)
            formula = "D * D"
        elif pairs.get("mode") == "opportunities":
            opportunities = _add(
                _mul(script_bars, _number(pairs, "perBar", "strategy.pairKeys")),
                _number(pairs, "fixed", "strategy.pairKeys"))
            # D**2 may exceed u64 even when the tighter opportunity bound and
            # the complete resulting arena are representable.
            pair_count = (opportunities if d != 0 and d > MAX_U64 // d
                          else min(_mul(d, d), opportunities))
            formula = "min(D * D, scriptBars * pairKeys.perBar + pairKeys.fixed)"
        else:
            raise FactsError("pairKeys.mode must be cartesian or opportunities")
        inputs["Q"] = _derived(pair_count, formula, data_source, id_source, pair_source)

        successors = _object(strategy.get("successors"), "strategy.successors")
        successor_source = _source(successors, "strategy.successors")
        inputs["R"] = _derived(
            _add(_mul(script_bars, _number(successors, "perBar", "strategy.successors")),
                 _number(successors, "fixed", "strategy.successors")),
            "scriptBars * successors.perBar + successors.fixed",
            data_source, successor_source, control_source)

        objects_record = strategy.get("objects")
        if objects_record is None:
            objects_per_execution = 1
            payload_bytes = 0
        else:
            objects_record = _object(objects_record, "strategy.objects")
            _source(objects_record, "strategy.objects")
            per_execution = objects_record.get("perExecution", 1)
            if per_execution in ("unbounded", "calc_on_order_fills"):
                objects_per_execution = OBJECTS_PER_EXECUTION_PLATFORM_MAX
            elif type(per_execution) is int:
                objects_per_execution = _integer(
                    per_execution, "strategy.objects.perExecution")
            else:
                raise FactsError(
                    "strategy.objects.perExecution must be a nonnegative integer, "
                    "'unbounded', or 'calc_on_order_fills'")
            payload_bytes = (
                _number(objects_record, "payloadBytes", "strategy.objects")
                if "payloadBytes" in objects_record else 0
            )
        object_record_bytes = _add(
            OBJECT_HEADER_BYTES, OBJECT_CHUNK_BYTES, payload_bytes)
        object_reserve = object_store_reserve_bytes(
            script_bars, objects_per_execution, object_record_bytes)

        placement = _object(
            strategy.get("placementStore"), "strategy.placementStore")
        _source(placement, "strategy.placementStore")
        placement_bytes = _number(
            placement, "arenaBytes", "strategy.placementStore")
        placement_chunks = _ceil_chunks(
            placement_bytes, "strategy.placementStore.arenaBytes")
        total = _add(*per_user_bytes(inputs).values())
        # Quotient/remainder avoids overflowing total + CHUNK_BYTES - 1.
        chunks = _add(_ceil_chunks(total, "non-command bytes"),
                      ROUNDING_REGIONS, placement_chunks)
        floor_arena_bytes = _mul(combined_arena_chunks(chunks), CHUNK_BYTES)
        # The object reservation is a separate host floor.  It is rounded to
        # allocatable arena chunks and can only raise the existing floor.
        object_arena_bytes = _mul(
            _ceil_chunks(object_reserve, "object-store reservation"),
            CHUNK_BYTES)
        arena_bytes = max(floor_arena_bytes, object_arena_bytes)
        if arena_bytes > MAX_ARENA_BYTES:
            return unavailable(
                "PLATFORM_RANGE: finite bound exceeds ScratchArena's uint32 chunk "
                f"table limit ({MAX_ARENA_BYTES} bytes); RETURN TO CORE-TOP")
        return {"arenaBytes": arena_bytes, "inputs": inputs, "unbounded": False}
    except BoundOverflow as error:
        return unavailable(f"PLATFORM_RANGE: {error}; RETURN TO CORE-TOP")
    except MissingProof as error:
        return unavailable(f"UNCERTIFIED: {error}")


def _unavailable_term(reason: str, **fields) -> dict:
    return {**fields, "unbounded": True,
            "derivation": {"formula": "no certified pre-start value", "sources": [reason]}}


def _ohlcv_term(record) -> dict:
    if record is None:
        return _unavailable_term("missing complete pre-start feed/mapping inventory",
                                 residentBytes=None, asBytes=None)
    record = _object(record, "ohlcv")
    source = _source(record, "ohlcv")
    page = _number(record, "pageBytes", "ohlcv")
    if page == 0:
        raise FactsError("ohlcv.pageBytes must be positive")
    feeds = record.get("feeds")
    if feeds is None:
        raise MissingProof("missing ohlcv.feeds physical buffer inventory")
    if not isinstance(feeds, (list, tuple)):
        raise FactsError("ohlcv.feeds must enumerate every physical bar/close/column buffer")
    resident = address_space = 0
    for index, item in enumerate(feeds):
        name = f"ohlcv.feeds[{index}]"
        feed = _object(item, name)
        count = _number(feed, "bars", name)
        columns = _number(feed, "columns", name)
        closes = _number(feed, "closeTimes", name)
        if closes not in (0, 1):
            raise FactsError(f"{name}.closeTimes must be 0 or 1")
        resident_copies = _number(feed, "residentCopies", name)
        as_copies = _number(feed, "asCopies", name)
        overhead = _number(feed, "overheadBytes", name)
        # Bar: five doubles + int64_t. Instrument closes/columns: 8 B/row.
        def page_ceil(payload: int) -> int:
            return _mul(_add(payload // page, int(payload % page != 0)), page)

        # The bar buffer, close-time buffer, and EACH value column can be
        # separate mappings/allocations. Rounding their sum would undercount
        # small feeds. overheadBytes belongs to the bar buffer's file layout.
        mapped = _add(page_ceil(_add(_mul(count, 48), overhead)),
                      _mul(_add(columns, closes), page_ceil(_mul(count, 8))))
        resident = _add(resident, _mul(mapped, resident_copies))
        address_space = _add(address_space, _mul(mapped, as_copies))
    return {
        "residentBytes": resident, "asBytes": address_space, "unbounded": False,
        "derivation": {
            "formula": ("sum((pageCeil(48 * bars + overheadBytes) + "
                        "(columns + closeTimes) * pageCeil(8 * bars)) "
                        "* residentCopies/asCopies)"),
            "sources": [source, "bar.hpp Bar; native_bar_views.hpp borrowed arrays; "
                        "native_run_spec.hpp NativeInstrumentColumn"],
        },
    }


def _stream_term(facts: Mapping, arena: Mapping) -> dict:
    record = facts.get("stream")
    if record is None:
        return _unavailable_term("missing pre-start open-at-end and O/V/K/X/H counts", bytes=None)
    record = _object(record, "stream")
    source = _source(record, "stream")
    t = arena["inputs"]["T"]["value"]
    e = arena["inputs"]["E"]["value"]
    length = arena["inputs"]["L"]["value"]
    if t is None or e is None or length is None:
        return _unavailable_term("missing certified T/E/L for the sink stream", bytes=None)
    bars = _number(_object(facts.get("data"), "data"), "scriptBars", "data")
    n_t = _add(t, _number(record, "openAtEndRows", "stream"))
    # Counts cover the complete run, including pre-roll and COOF callbacks.
    counts = {"T": n_t, "E": e}
    for tag, flag, per_bar, fixed in (
            ("O", "outputs", "outputRowsPerBar", "outputRowsFixed"),
            ("V", "outputs", "eventsPerBar", "eventsFixed"),
            ("X", "trace", "tracePerBar", "traceFixed"),
            ("H", "hashes", "hashesPerBar", "hashesFixed")):
        enabled = _number(record, flag, "stream")
        if enabled not in (0, 1):
            raise FactsError(f"stream.{flag} must be 0 or 1")
        counts[tag] = (_add(_mul(bars, _number(record, per_bar, "stream")),
                            _number(record, fixed, "stream")) if enabled else 0)
    counts["K"] = _number(record, "outputs", "stream")  # Writer permits at most one.

    def string_line(key: str, fixed: int, factor: int) -> int:
        if key not in record or record[key] is None:
            # A finite count still has a finite successful-wire bound without
            # a string-length proof: the writer enforces its 1 MiB line ceiling.
            return WIRE_LINE_BYTES
        return min(WIRE_LINE_BYTES,
                   _add(fixed, _mul(factor, _number(record, key, "stream"))))

    # A signed safe JSON integer <= 17 bytes; f64 conservatively <= 64.
    # escaped_string emits at most six bytes per input byte, plus quotes.
    trade_fixed = _add(675, _mul(12, length))
    sizes = {
        "E": 154, "T": string_line("commentBytes", trade_fixed, 6),
        "O": (min(WIRE_LINE_BYTES, _add(
            60, _mul(65, _number(record, "seriesSlots", "stream"))))
              if record["outputs"] else 0),
        "K": (min(WIRE_LINE_BYTES, _add(
            6, _mul(65, _number(record, "constantSlots", "stream"))))
              if record["outputs"] else 0),
        "V": string_line("messageBytes", 239, 6),
        "X": string_line("traceNameBytes", 110, 6),
        "H": 25,
    }
    # Header and trailer each obey the same line ceiling. Their descriptor
    # fields, run_id, version, seven counts and digest need no guessed length.
    total = _add(_mul(2, WIRE_LINE_BYTES),
                 *(_mul(counts[tag], sizes[tag]) for tag in ("E", "T", "O", "V", "K", "X", "H")))
    return {
        "bytes": total, "unbounded": False,
        "derivation": {
            "formula": "2 * 1048576 + sum(recordCount[tag] * maxLineBytes[tag])",
            "sources": [source, "report_sink_writer.cpp line builders and 1 MiB line ceiling"],
            "recordCounts": counts, "maxLineBytes": sizes,
        },
    }


def _private_term(record) -> dict:
    if record is None:
        return _unavailable_term(
            "missing applicable private RSS proof and private address-space proof",
            rssBytes=None, asBytes=None, measured=False)
    record = _object(record, "childPrivate")
    source = _source(record, "childPrivate")
    if "measured" not in record:
        raise MissingProof("missing childPrivate.measured RSS provenance")
    measured = record.get("measured")
    if type(measured) is not bool:
        raise FactsError("childPrivate.measured must be bool (RSS measurement only)")
    result = {}
    for key in ("rssBytes", "asBytes"):
        value = record.get(key)
        result[key] = None if value is None else _integer(value, f"childPrivate.{key}")
    certified = []
    for key in ("rssCertified", "asCertified"):
        if key not in record:
            raise MissingProof(f"missing childPrivate.{key}")
        value = record.get(key)
        if type(value) is not bool:
            raise FactsError(f"childPrivate.{key} must be bool")
        certified.append(value)
    unbounded = any(value is None for value in result.values()) or not all(certified)
    return {
        **result, "unbounded": unbounded, "measured": measured,
        "derivation": {
            "formula": "supplied private RSS/AS proofs; measured applies to RSS only",
            "sources": [source],
            "rssCertified": certified[0], "asCertified": certified[1],
        },
    }


def arena_bound(facts: Mapping) -> dict:
    """One A5-WIDER call; only arena/OHLCV/stream affect the sizing verdict.

    A measured RSS observation is not a private-memory bound. An unmeasured AS
    term is never filled in from RSS. childPrivate and its unbounded flag are
    informational: RLIMIT_AS caps the child, with typed memory_limit charged
    to the strategy. An unbounded sizing term means no attempt-0 tightening;
    COMPUTE uses its default arena and trusted escalation ladder. See the
    exact interface and TS BigInt formulas in docs/design/arena-bound.md.
    """
    facts = _object(facts, "facts")
    try:
        arena = _arena_term(facts)
    except MissingProof as error:
        reason = f"UNCERTIFIED: {error}"
        arena = {"arenaBytes": None, "inputs": _unknown_inputs(reason),
                 "unbounded": True, "reason": reason}
    arena["bytes"] = arena["arenaBytes"]  # A5-WIDER and earlier A5 names agree.
    arena["derivation"] = {
        "formula": "chunkCeil(sum(perUserBytes)) + 14 * 1048576",
        "sources": ["docs/design/arena-bound.md storage derivation"],
    }
    if not arena["unbounded"]:
        arena["derivation"] = {
            "formula": (
                "b = ceil(sum(perUserBytes) / 1048576) + 14; "
                "o = ceil(strategy.placementStore.arenaBytes / 1048576); "
                "m = b + o; q = max(1, floor((m - 3) / (16 - 3))); "
                "floorArenaBytes = (m + 3 * q) * 1048576; "
                "objectReserveBytes = ceil(scriptBars * objects.perExecution * "
                "recordBytes); arenaBytes = max(floorArenaBytes, "
                "ceil(objectReserveBytes / 1048576) * 1048576); f = 1/16"
            ),
            "sources": [
                "docs/design/arena-bound.md storage derivation",
                "src/command_scratch_production.hpp kCommandScratchArenaShare",
                "src/command_scratch_production.cpp command_scratch_arena_capacity_bytes",
            ],
        }
    terms = {"arena": arena}
    for name, calculate, record in (
            ("ohlcv", _ohlcv_term, facts.get("ohlcv")),
            ("finalizerStream", lambda _: _stream_term(facts, arena), None),
            ("childPrivate", _private_term, facts.get("childPrivate"))):
        try:
            terms[name] = calculate(record)
        except (BoundOverflow, MissingProof) as error:
            fields = ({"residentBytes": None, "asBytes": None} if name == "ohlcv"
                      else {"bytes": None} if name == "finalizerStream"
                      else {"rssBytes": None, "asBytes": None, "measured": False})
            prefix = "PLATFORM_RANGE" if isinstance(error, BoundOverflow) else "UNCERTIFIED"
            terms[name] = _unavailable_term(f"{prefix}: {error}", **fields)
    missing = [name for name in ("arena", "ohlcv", "finalizerStream")
               if terms[name]["unbounded"]]
    return {
        "terms": terms, "unbounded": bool(missing), "unboundedTerms": missing,
        "reason": "no certified finite bound for: " + ", ".join(missing) if missing else "",
    }
