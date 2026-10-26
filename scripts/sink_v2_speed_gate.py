#!/usr/bin/env python3
"""Independent-build report-sink speed gate; see docs/design/sink-v2-speed-gate.md.

Only the standard library is imported here. The measured children use the
checkout's run_json driver and, outside the backtest timer, its reference codec.
No build, feed derivation, network access, or baseline update is performed.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
import copy
import contextlib
import ctypes
import ctypes.util
from dataclasses import dataclass
from fractions import Fraction
from functools import lru_cache
import hashlib
import importlib.util
import itertools
import json
import math
import mmap
import os
from pathlib import Path
import random
import re
import shlex
import stat
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib


ROOT = Path(__file__).resolve().parents[1]
LEVELS = (1, 2, 3, 5, 7, 9, 12, 15, 19)
SEED = 20261020
MAX_JSON = 2 * 1024 * 1024
MIB = 1024 * 1024
T_FIELDS = (
    "n", "side", "entry_time", "exit_time", "entry_price", "exit_price",
    "qty", "pnl", "pnl_pct", "max_runup", "max_drawdown", "commission",
    "entry_bar_index", "exit_bar_index", "open_at_end", "entry_incarnation",
    "close_cause", "entry_id", "exit_id", "exit_comment",
)
T_TYPES = (
    "i64", "str", "i64", "i64", *(("f64?",) * 8),
    "i64", "i64", "bool", "u64", "i64", "str", "str", "str",
)
E_FIELDS = ("time_ms", "equity", "open_profit")
E_TYPES = ("i64", "f64?", "f64?")
TAGS = "ETOVKXH"
# Spec 15.1 references only: never substituted for measured file bytes.
FIXED_FLOORS = {
    "FULL": {"empty": 4256, "E/I, zero trades": 17824,
             "T/I, one trade, no E": 54472, "E/T/I, one trade": 63432,
             "all types/I, one trade": 86472},
    "COMPACT": {"empty": 480, "E/I, zero trades": 672,
                "T/I, one trade, no E": 672, "E/T/I, one trade": 768,
                "all types/I, one trade": 1248},
}
GRID_SIZES = (100, 1000, 5000, 9000, 10000, 100000, 1000000)
GRID_FRACTIONS = (0.25, 0.5, 1.0)
SUMMARY_HEADER = 52
S_MAX = 1048576  # Spec 3.7: decoded SUMMARY J and zstd window.
SUMMARY_STORED_LIMIT = 1048661  # Includes the 52-byte SUM2 header.
SUMMARY_STORED_CAP = SUMMARY_STORED_LIMIT - SUMMARY_HEADER
SMALL_RUN_K = 3072  # Spec 0.5: fixed for BOTH model and measured verdicts.
CORNER_E = (1, 2, 10, 40, 63, 64, 68, 69, 100, 127, 128)
MODEL_JSON_BYTES = 4096
MODEL_STORED_BYTES = 1024
METADATA_MEMBERS = frozenset((
    "input", "applied_inputs", "applied_overrides", "applied_runtime", "elapsed_seconds",
    "fingerprint", "report_window", "report_shape",
))
METADATA_OUTPUTS = frozenset(("schema_version", "message_format", "manifest_sha256", "manifest"))
COMPARABLE_MEMBERS = frozenset(("engine", "summary", "metrics", "diagnostics", "sink_folds", "outputs"))
FINALIZATION_CLAIM = 1836032
FINALIZATION_CAP = 2097152
WORK_FILE_CAP = 131072
RSS_TIME_SERIES_CAP = 30000  # per-process observations/run; peak tracking continues after truncation
WALL_SCOPE = "child start through exit after finalization/close"


class GateError(ValueError):
    """Missing, inconsistent or invalid gate evidence."""


class SummaryLimit(GateError):
    """Typed SUMMARY admission refusal (spec 0.5/3.7/5.2)."""

    kind = "summary_limit"


class LateModeMismatch(GateError):
    """Same-input late verification modes produced different v2 bytes."""

    kind = "late_mode_mismatch"

    def __init__(self, evidence, left, right):
        self.evidence = evidence
        super().__init__(
            f"{self.kind}: {evidence['cell']}: "
            f"{left['mode']} sha256={left['sha256']} bytes={left['bytes']} vs "
            f"{right['mode']} sha256={right['sha256']} bytes={right['bytes']}; "
            f"outputs retained in {evidence['directory']}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def reject_constant(value):
    raise GateError(f"non-finite JSON token {value}")


DECODER = json.JSONDecoder(object_pairs_hook=pairs, parse_constant=reject_constant)


def load_json(path: Path):
    with path.open(encoding="utf-8") as source:
        return json.load(source, object_pairs_hook=pairs, parse_constant=reject_constant)


def dump_json(path: Path, value) -> None:
    with path.open("w", encoding="utf-8") as target:
        json.dump(value, target, indent=2, allow_nan=False)
        target.write("\n")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(MIB), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    require(spec is not None and spec.loader is not None, f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def canonical(value, wire: str) -> bytes:
    """Typed equality, including signed zero, nullable values and full integers."""
    nullable = wire.endswith("?")
    if value is None:
        require(nullable, f"null in {wire}")
        return b"\0"
    prefix = b"\1" if nullable else b""
    wire = wire.rstrip("?")
    if wire == "f64":
        require(type(value) in (int, float) and math.isfinite(value), "invalid f64")
        body = struct.pack("<d", float(value))
    elif wire in ("i64", "u64"):
        require(type(value) is int, "invalid integer")
        body = struct.pack("<q" if wire == "i64" else "<Q", value)
    elif wire == "bool":
        require(type(value) is bool, "invalid bool")
        body = bytes((int(value),))
    elif wire == "hex64":
        require(type(value) is str and len(value) == 16, "invalid hex64")
        body = struct.pack("<Q", int(value, 16))
    else:
        require(wire == "str" and type(value) is str, "invalid string")
        encoded = value.encode("utf-8")
        body = struct.pack("<I", len(encoded)) + encoded
    return prefix + body


def hash_row(digest, values, wires) -> None:
    require(len(values) == len(wires), "record width differs from schema")
    for value, wire in zip(values, wires):
        digest.update(canonical(value, wire))


def object_digest(value) -> str:
    # Preserve numeric types and f64 bits; sort only object keys.
    digest = hashlib.sha256()

    def visit(item):
        if item is None:
            digest.update(b"N")
        elif type(item) is bool:
            digest.update(b"B" + bytes((item,)))
        elif type(item) is int:
            digest.update(b"I" + str(item).encode("ascii") + b";")
        elif type(item) is float:
            require(math.isfinite(item), "non-finite report value")
            digest.update(b"F" + struct.pack("<d", item))
        elif type(item) is str:
            digest.update(b"S" + canonical(item, "str"))
        elif type(item) is list:
            digest.update(b"[")
            for child in item:
                visit(child)
            digest.update(b"]")
        else:
            require(type(item) is dict, "unsupported report value")
            digest.update(b"{")
            for key in sorted(item):
                visit(key)
                visit(item[key])
            digest.update(b"}")

    visit(value)
    return digest.hexdigest()


class JSONStream:
    """Bounded root/array reader for the potentially very large sink-off report."""

    def __init__(self, source):
        self.source = source
        self.buffer = ""
        self.pos = 0
        self.eof = False

    def more(self):
        self.buffer = self.buffer[self.pos:]
        self.pos = 0
        chunk = self.source.read(65536)
        self.eof = not chunk
        self.buffer += chunk

    def peek(self):
        while True:
            while self.pos < len(self.buffer) and self.buffer[self.pos].isspace():
                self.pos += 1
            if self.pos < len(self.buffer):
                return self.buffer[self.pos]
            if self.eof:
                return ""
            self.more()

    def expect(self, token):
        require(self.peek() == token, f"expected JSON {token!r}")
        self.pos += 1

    def value(self):
        require(bool(self.peek()), "unexpected JSON EOF")
        while True:
            try:
                value, end = DECODER.raw_decode(self.buffer, self.pos)
                # A numeric token can straddle a read boundary.
                if end == len(self.buffer) and not self.eof:
                    self.more()
                    continue
                if (type(value) in (int, float) and end < len(self.buffer)
                        and self.buffer[end] not in ",]} \t\r\n"):
                    require(not self.eof, "incomplete JSON number")
                    require(len(self.buffer) - self.pos < MAX_JSON, "JSON item exceeds 2 MiB")
                    self.more()
                    continue
                require(end - self.pos <= MAX_JSON, "JSON item exceeds 2 MiB")
                self.pos = end
                return value
            except json.JSONDecodeError as error:
                if self.eof:
                    raise GateError(f"invalid driver JSON: {error}") from error
                require(len(self.buffer) - self.pos < MAX_JSON, "JSON item exceeds 2 MiB")
                self.more()

    def array(self):
        self.expect("[")
        if self.peek() != "]":
            while True:
                yield self.value()
                if self.peek() == "]":
                    break
                self.expect(",")
        self.expect("]")

    def skip(self, depth=0):
        require(depth <= 64, "driver JSON nesting exceeds 64")
        token = self.peek()
        if token == "{":
            self.expect("{")
            seen = set()
            if self.peek() != "}":
                while True:
                    key = self.value()
                    require(type(key) is str and key not in seen, "invalid object key")
                    seen.add(key)
                    self.expect(":")
                    self.skip(depth + 1)
                    if self.peek() == "}":
                        break
                    self.expect(",")
            self.expect("}")
        elif token == "[":
            self.expect("[")
            if self.peek() != "]":
                while True:
                    self.skip(depth + 1)
                    if self.peek() == "]":
                        break
                    self.expect(",")
            self.expect("]")
        else:
            self.value()


def read_report(path: Path, off: bool):
    document = {}
    counts = {}
    hashes = {}
    with path.open(encoding="utf-8") as source:
        stream = JSONStream(source)
        stream.expect("{")
        seen = set()
        if stream.peek() != "}":
            while True:
                key = stream.value()
                require(type(key) is str and key not in seen, "invalid root key")
                seen.add(key)
                stream.expect(":")
                if key in ("trades", "equity_curve"):
                    fields, wires = (T_FIELDS[:16], T_TYPES[:16]) if key == "trades" else (E_FIELDS, E_TYPES)
                    digest, count = hashlib.sha256(), 0
                    for record in stream.array():
                        require(type(record) is dict and all(f in record for f in fields),
                                f"incomplete {key} row")
                        hash_row(digest, [record[f] for f in fields], wires)
                        count += 1
                    tag = "T" if key == "trades" else "E"
                    counts[tag], hashes[tag] = count, digest.hexdigest()
                elif key == "outputs":
                    # V1/full stdout may have report-sized arrays here. Keep
                    # only the six SUMMARY metadata members in bounded memory.
                    outputs, output_keys = {}, set()
                    stream.expect("{")
                    if stream.peek() != "}":
                        while True:
                            member = stream.value()
                            require(type(member) is str and member not in output_keys, "invalid outputs key")
                            output_keys.add(member)
                            stream.expect(":")
                            if member in ("schema_version", "message_format", "manifest_sha256",
                                          "manifest", "constants", "hlines"):
                                outputs[member] = stream.value()
                            else:
                                stream.skip()
                            if stream.peek() == "}":
                                break
                            stream.expect(",")
                    stream.expect("}")
                    document[key] = outputs
                else:
                    document[key] = stream.value()
                if stream.peek() == "}":
                    break
                stream.expect(",")
        stream.expect("}")
        require(stream.peek() == "", "bytes after driver JSON")
    require(type(document.get("metrics")) is dict and bool(document["metrics"]), "missing metrics")
    require(type(document.get("summary")) is dict, "missing summary")
    if off:
        require(set(counts) == {"E", "T"}, "missing off E/T arrays")
        require("report_sink" not in document, "sink-off driver installed a sink")
    return document, counts, hashes


def read_v1(path: Path, reader):
    counts = Counter()
    type_bytes = Counter()
    digests = {tag: hashlib.sha256() for tag in TAGS}
    legacy_t = hashlib.sha256()
    merged = hashlib.sha256()
    folds = {"closed_count": 0, "open_at_end_count": 0, "open_at_end_pnl": 0.0}
    active_e = 0
    before = path.stat()
    with reader.SinkReader(path) as sink, path.open("rb") as raw:
        header_line = raw.readline(MAX_JSON)
        schemas = {entry["type"]: entry for entry in sink.header["records"]}
        for record in sink:
            line = raw.readline(MAX_JSON)
            require(line.endswith(b"\n"), "missing bounded v1 record line")
            tag = record.tag
            counts[tag] += 1
            type_bytes[tag] += len(line)
            hash_row(digests[tag], record.values, schemas[tag]["types"])
            merged.update(tag.encode("ascii"))
            hash_row(merged, record.values, schemas[tag]["types"])
            if tag == "E" and record.values[2] is not None and record.values[2] != 0:
                active_e += 1
            if tag == "T":
                require(tuple(record.fields) == T_FIELDS, "unexpected T schema")
                hash_row(legacy_t, record.values[:16], T_TYPES[:16])
                is_open = record.values[14]
                folds["open_at_end_count" if is_open else "closed_count"] += 1
                if is_open and record.values[7] is not None:
                    folds["open_at_end_pnl"] = folds["open_at_end_pnl"] + record.values[7]
        trailer_line = raw.readline(MAX_JSON)
        require(sink.complete and raw.read(1) == b"", "unverified v1 trailer")
        require(all(sink.counts.get(t, 0) == counts[t] for t in TAGS), "v1 count mismatch")
    after = path.stat()
    require((before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns),
            "sink changed during verification")
    shared = len(header_line) + len(trailer_line)
    require(shared + sum(type_bytes.values()) == after.st_size, "v1 byte accounting mismatch")
    return {
        "counts": {t: counts[t] for t in TAGS},
        "record_sha256": {t: digests[t].hexdigest() for t in TAGS},
        "legacy_trade_sha256": legacy_t.hexdigest(),
        "merged_sha256": merged.hexdigest(),
        "schema_tags": list(schemas),
        "type_bytes": {t: type_bytes[t] for t in TAGS},
        "shared_bytes": shared,
        "metadata_bytes": shared,
        "sink_bytes": after.st_size,
        "profile": "V1",
        "sink_folds": folds,
        "fraction": active_e / counts["E"] if counts["E"] else None,
        "fraction_scope": "fraction of raw E with nonzero open_profit (position proxy)",
    }


def page_extent(entries, width):
    """Spec 9.2/15.1: full prefixes and the actual short final page."""
    require(type(entries) is int and entries >= 0 and width in (4, 8, 256, 512),
            "invalid index entries/width")
    if not entries:
        return 0, 0, 0
    capacity = 61440 // width
    pages = (entries + capacity - 1) // capacity
    last = 4096 + width * (entries - (pages - 1) * capacity)
    return pages, last, 65536 * (pages - 1) + last


def canonical_profile(equity, trades, descriptors):
    return "COMPACT" if equity < 10980 and trades < 3314 and descriptors <= 128 else "FULL"


def size_clause(counts, actual_bars):
    """Spec 0.5: (E<128 AND T<4) OR E<64; B/optional streams do not assign clauses."""
    return "(ii-a)" if (counts["E"] < 128 and counts["T"] < 4) or counts["E"] < 64 else "(i)"


def compare_size(v1, v2, counts, actual_bars, decoded, stored, c_q=None, c_m=None):
    """Spec 0.5/3.7: fixed K, one size predicate, independent SUMMARY admission."""
    clause = size_clause(counts, actual_bars)
    literal_empty = all(counts[tag] == 0 for tag in TAGS)
    limit = v1 + SMALL_RUN_K if clause == "(ii-a)" else v1 - 1
    probes = (type(stored) is int and stored > 0 and type(c_q) is int and c_q > 0
              and type(c_m) is int and c_m >= 0)
    c_meta = min(c_m, max(0, stored - c_q)) if probes else None
    comparable = v2 - c_meta if probes else None
    counted = stored - c_meta if probes else None
    maximum = limit - (v2 - stored) if stored is not None else None
    refused = ((type(decoded) is int and decoded > S_MAX)
               or (type(stored) is int and stored > SUMMARY_STORED_CAP))
    size_verdict = "PENDING" if not probes else "PASS" if comparable <= limit else "FAIL"
    return {
        "clause": clause, "v2_file_bytes": v2,
        "small_run_K": SMALL_RUN_K, "small_run_K_source": "FIXED spec 0.5",
        "refusal_kind": "summary_limit" if refused else None,
        "counterfactual_size_verdict": size_verdict if refused else None,
        "C_meta": c_meta, "C_comparable": counted, "COMPARABLE_V2": comparable,
        "metadata_exclusion": "EXCLUDED_CALLER_METADATA" if c_meta else "NONE" if probes else "PENDING",
        "summary_comparable_ratio": counted / decoded if probes and decoded else None,
        "comparison_limit_bytes": limit, "v2_minus_v1_bytes": v2 - v1,
        "comparable_minus_v1_bytes": comparable - v1 if probes else None,
        "diagnostic_verdict": "REFUSED" if refused else "PASS" if v2 <= limit else "FAIL",
        "verdict": "REFUSED" if refused else size_verdict,
        "empty_full_file_limit_bytes": limit if literal_empty else None,
        "empty_full_file_verdict": ("PASS" if v2 <= limit else "FAIL") if literal_empty else None,
        "summary_max_comparable_C": maximum, "summary_max_comparable_ratio": maximum / decoded
        if maximum is not None and decoded else None,
        "summary_headroom_bytes": maximum - counted if maximum is not None and probes else None,
    }


def v1_model_by_terms(equity, trades, fraction):
    """Independent 15.1 printed-byte count, without M1 or blended record rates."""
    f = Fraction(str(fraction))
    terms = (
        810, 138, len(str(equity)), len(str(trades)),  # Header and trailer separately.
        (5 + 13 + 1 + 1 + 2) * equity,  # E fixed bytes.
        (7 + 3) * (1 - f) * equity,  # Flat E numeric text.
        (18 + 18) * f * equity,  # In-position E numeric text.
        (18 + 6 + 5 * 19) * trades,  # T floating-point text.
        (5 + 5 + 7 + 28 + 12 + 6 + 5 + 2 + 10 + 2) * trades,
    )
    return math.ceil(sum(terms))


def size_model(equity, trades, fraction, decoded=MODEL_JSON_BYTES, stored=MODEL_STORED_BYTES,
               c_q=512, c_m=None, *, actual_bars=None, anchor_count=0):
    """Section-15.1 MODEL-ONLY; bytes and break-even have independent routes."""
    bars = equity if actual_bars is None else actual_bars
    e = (equity + 4095) // 4096
    t = (trades + 1023) // 1024
    i = (equity + trades + 65535) // 65536
    descriptors = e + t + i
    profile = canonical_profile(equity, trades, descriptors)
    m1 = 948 + len(str(equity)) + len(str(trades))
    m2 = 947 + SUMMARY_HEADER + stored
    framing = 176 * e + 992 * t + 80 * i
    indexes = (96 * descriptors + 480 if profile == "COMPACT" else
               sum(page_extent(n, 512)[2] for n in (e, t, i))
               + 10 * page_extent(trades, 4)[2] + page_extent(min(1200, equity), 256)[2] + 4256)
    # Exact decimal rational arithmetic also covers measured f outside the grid.
    f = Fraction(str(fraction))
    v1_formula = math.ceil(m1 + (32 + 26 * f) * equity + 201 * trades)
    v1_terms = v1_model_by_terms(equity, trades, fraction)
    require(v1_formula == v1_terms,
            f"MODEL-ONLY v1 arithmetic disagreement N={equity}/T={trades}/f={fraction}: "
            f"formula={v1_formula}, term_by_term={v1_terms}")
    v2_formula = math.ceil(m2 + framing + indexes + (Fraction(1, 5) + 11 * f) * equity
                           + Fraction(551, 10) * trades)
    # Separate regions and E/T/I payloads, rather than the blended payload rate.
    v2_base_terms_raw = (947 + SUMMARY_HEADER + indexes
                         + 176 * e + (Fraction(1, 10) + 11 * f) * equity
                         + 992 * t + 55 * trades + 80 * i + Fraction(1, 10) * (equity + trades))
    v2_terms_raw = v2_base_terms_raw + stored
    v2_terms = math.ceil(v2_terms_raw)
    require(v2_formula == v2_terms,
            f"MODEL-ONLY v2 arithmetic disagreement N={equity}/T={trades}/f={fraction}: "
            f"formula={v2_formula}, term_by_term={v2_terms}")
    counts = {tag: equity if tag == "E" else trades if tag == "T" else 0 for tag in TAGS}
    j_m = decoded - 1024 + 1
    if c_m is None:
        c_m = (j_m + 3) // 4
    model = {
        "source": "MODEL-ONLY", "profile": profile, "fraction": fraction, "N": equity, "T": trades,
        "counts": counts,
        "actual_bars": bars, "raw_E": equity, "anchor_count": anchor_count, "D": descriptors,
        "v1_bytes": v1_formula, "v1_formula_bytes": v1_formula, "v1_term_bytes": v1_terms,
        "v2_bytes": v2_formula, "v2_formula_bytes": v2_formula, "v2_term_bytes": v2_terms,
        "v2_base_formula_bytes": v2_formula - stored, "v2_base_term_bytes": math.ceil(v2_base_terms_raw),
        "v1_metadata_bytes": m1, "v2_metadata_bytes": m2,
        "index_root_tail_bytes": indexes, "block_framing_bytes": framing,
        "summary_decoded_bytes": decoded, "summary_stored_bytes": stored,
        "summary_ratio": stored / decoded,
        "J_Q": 1024, "C_Q": c_q, "J_M": j_m, "C_M": c_m,
        "metadata_scope": ("MODEL-ONLY UNMEASURED 15.1 assumptions: HEAD=947; J=4096/C=1024 including "
                           "frame; J_Q=1024/C_Q=512, J_M=J-J_Q+1, C_M=ceil(J_M/4); no outputs"),
    }
    model.update(compare_size(model["v1_bytes"], model["v2_bytes"], counts, bars,
                              decoded, stored, c_q, c_m))
    counted_terms = min(stored, max(c_q, stored - c_m))
    comparable_terms = math.ceil(v2_terms_raw - stored + counted_terms)
    require(model["COMPARABLE_V2"] == comparable_terms and model["C_comparable"] == counted_terms,
            f"MODEL-ONLY comparable arithmetic disagreement N={equity}/T={trades}/f={fraction}")
    model["comparable_term_bytes"] = comparable_terms
    maximum_terms = ((SMALL_RUN_K + counted_terms if model["clause"] == "(ii-a)" else counted_terms - 1)
                     - (comparable_terms - v1_terms))
    limit_terms = model["v2_base_term_bytes"] + maximum_terms
    require(model["v2_base_formula_bytes"] == model["v2_base_term_bytes"]
            and model["summary_max_comparable_C"] == maximum_terms
            and model["comparison_limit_bytes"] == limit_terms,
            f"MODEL-ONLY break-even arithmetic disagreement N={equity}/T={trades}/f={fraction}")
    model["summary_max_comparable_C_terms"] = maximum_terms
    model["comparison_limit_term_bytes"] = limit_terms
    model["verdict_label"] = "MODEL-ONLY " + model["verdict"] + ("(summary_limit)" if model["refusal_kind"] else "")
    return model


def raw_summary_size(decoded):
    """Spec 9.1: exact at J>=65792; an upper bound below that size."""
    blocks = (decoded + 131071) // 131072
    formula = decoded + 9 + 3 * blocks
    full, remainder = divmod(decoded, 131072)
    terms = 9 + full * (131072 + 3) + (remainder + 3 if remainder else 0)
    require(formula == terms, f"MODEL-ONLY SUMMARY arithmetic disagreement J={decoded}: {formula} != {terms}")
    return formula


def case_model(case):
    population = {"actual_bars": case.get("actual_bars", case["N"]),
                  "anchor_count": case.get("anchor_count", 0)}
    if case["shape"] == "large-compressible-summary":
        model = size_model(case["N"], case["T"], case["fraction"], MIB, MIB // 4, **population)
        model["metadata_scope"] = "MODEL-ONLY UNMEASURED retained J=1MiB caller M example at S_MAX; J_Q=1024/C_Q=512"
        return model
    if case["shape"] == "incompressible-summary":
        model = size_model(case["N"], case["T"], case["fraction"], MIB, raw_summary_size(MIB),
                           c_m=raw_summary_size(MIB - 1024 + 1), **population)
        model["metadata_scope"] = (
            "MODEL-ONLY UNMEASURED raw-block-frame accounting: J=1048576, C=J+33, C_M=J_M+33; "
            "not same-build probes or incompressibility evidence")
        return model
    if case["shape"] in ("ceiling-compressible-summary", "ceiling-raw-summary", "above-ceiling-summary"):
        decoded = S_MAX + int(case["shape"] == "above-ceiling-summary")
        j_m = decoded - 1024 + 1
        raw = case["shape"] == "ceiling-raw-summary"
        stored = raw_summary_size(decoded) if raw else (decoded + 3) // 4
        c_m = raw_summary_size(j_m) if raw else (j_m + 3) // 4
        model = size_model(case["N"], case["T"], case["fraction"], decoded, stored, c_m=c_m, **population)
        full, remainder = divmod(decoded, 131072)
        region_terms = (SUMMARY_HEADER + 9 + full * (131072 + 3) + (remainder + 3 if remainder else 0)
                        if raw else SUMMARY_HEADER + math.ceil(Fraction(decoded, 4)))
        require(SUMMARY_HEADER + stored == region_terms, "MODEL-ONLY SUMMARY region arithmetic disagreement")
        model["summary_region_formula_bytes"] = SUMMARY_HEADER + stored
        model["summary_region_term_bytes"] = region_terms
        model["summary_stored_excess_bytes"] = SUMMARY_HEADER + stored - SUMMARY_STORED_LIMIT
        model["summary_decoded_excess_bytes"] = decoded - S_MAX
        model["metadata_scope"] = (
            "MODEL-ONLY UNMEASURED EXCLUDED_CALLER_METADATA; two-route SUMMARY/frame and file sums; "
            + ("JUST ABOVE S_MAX: REFUSED(summary_limit) on J before encoding; all byte totals are counterfactual"
               if decoded > S_MAX else "at S_MAX, admitted; real compression/probes PENDING"))
        return model
    model = size_model(case["N"], case["T"], case["fraction"], **population)
    if case["shape"] == "selected_report_view":
        model["metadata_scope"] += (f"; selected_report_view: B={population['actual_bars']}, raw E=N; "
                                    f"assumed anchors={population['anchor_count']}")
    return model


def summary_projections(raw):
    """Project token spans, never reserialize values, numbers, keys or escapes."""
    text = raw.decode("utf-8")
    require(type(DECODER.decode(text)) is dict, "SUMMARY projection needs an object")

    def members(document):
        require(document.startswith("{") and document.endswith("}"), "SUMMARY object framing")
        pos = 1
        result = []
        while pos < len(document) - 1:
            start = pos
            while document[pos] in " \t\r\n":
                pos += 1
            if document[pos] == "}":
                break
            key, pos = DECODER.raw_decode(document, pos)
            require(type(key) is str, "SUMMARY member name")
            while document[pos] in " \t\r\n":
                pos += 1
            require(document[pos] == ":", "SUMMARY member separator")
            pos += 1
            while document[pos] in " \t\r\n":
                pos += 1
            value_start = pos
            _, value_end = DECODER.raw_decode(document, pos)
            pos = value_end
            while document[pos] in " \t\r\n":
                pos += 1
            result.append((key, document[start:value_start], document[value_start:value_end],
                           document[value_end:pos]))
            if document[pos] == "}":
                break
            require(document[pos] == ",", "SUMMARY member delimiter")
            pos += 1
        return result

    def object_text(parts):
        return "{" + ",".join(parts) + "}"

    q, m, unknown = [], [], []
    for key, prefix, value, suffix in members(text):
        member = prefix + value + suffix
        if key in METADATA_MEMBERS:
            m.append(member)
        elif key == "outputs":
            output_q, output_m = [], []
            for name, sub_prefix, sub_value, sub_suffix in members(value):
                part = sub_prefix + sub_value + sub_suffix
                (output_m if name in METADATA_OUTPUTS else output_q).append(part)
                if name not in METADATA_OUTPUTS and name not in ("constants", "hlines"):
                    unknown.append("outputs." + name)
            if output_q:
                q.append(prefix + object_text(output_q) + suffix)
            if output_m:
                m.append(prefix + object_text(output_m) + suffix)
        else:
            q.append(member)
            if key not in COMPARABLE_MEMBERS:
                unknown.append(key)
    return object_text(q).encode("utf-8"), object_text(m).encode("utf-8"), len(m), unknown


def check_summary_budget(decoded, stored):
    require(type(decoded) is int and decoded > 0 and type(stored) is int and stored > 0,
            "invalid SUMMARY lengths")
    if decoded > S_MAX or stored > SUMMARY_STORED_CAP:
        raise SummaryLimit(f"summary_limit: J={decoded}, S_MAX={S_MAX}, "
                           f"52+C={SUMMARY_HEADER + stored}, stored_bound={SUMMARY_STORED_LIMIT}")


def check_summary_frame(frame, decoded):
    """Check FCS/window/dictionary before allocating a decoded SUMMARY."""
    require(len(frame) >= 6 and frame[:4] == b"\x28\xb5\x2f\xfd", "SUMMARY is not a zstd frame")
    descriptor = frame[4]
    require(not descriptor & 0x1b, "SUMMARY dictionary/reserved frame flags")
    single = bool(descriptor & 0x20)
    pos = 5
    window = decoded
    if not single:
        descriptor_window = frame[pos]
        pos += 1
        base = 1 << (10 + (descriptor_window >> 3))
        window = base + (base >> 3) * (descriptor_window & 7)
    width = (1 if single else 0, 2, 4, 8)[descriptor >> 6]
    require(width > 0 and pos + width <= len(frame), "SUMMARY missing frame content size")
    content = int.from_bytes(frame[pos:pos + width], "little") + (256 if width == 2 else 0)
    require(content == decoded, "SUMMARY content size differs from J")
    if window > S_MAX:
        raise SummaryLimit(f"summary_limit: window={window}, S_MAX={S_MAX}")


class SummaryZstd:
    """Use the lab's existing libzstd via stdlib ctypes, outside all child timers."""

    def __init__(self, library=None):
        name = str(library) if library else ctypes.util.find_library("zstd")
        require(bool(name), "SUMMARY accounting needs the lab libzstd (--zstd-library)")
        self.lib = ctypes.CDLL(name)
        signatures = {
            "ZSTD_versionString": (ctypes.c_char_p, ()),
            "ZSTD_isError": (ctypes.c_uint, (ctypes.c_size_t,)),
            "ZSTD_getErrorName": (ctypes.c_char_p, (ctypes.c_size_t,)),
            "ZSTD_getFrameContentSize": (ctypes.c_ulonglong, (ctypes.c_void_p, ctypes.c_size_t)),
            "ZSTD_findFrameCompressedSize": (ctypes.c_size_t, (ctypes.c_void_p, ctypes.c_size_t)),
            "ZSTD_decompress": (ctypes.c_size_t, (ctypes.c_void_p, ctypes.c_size_t,
                                               ctypes.c_void_p, ctypes.c_size_t)),
            "ZSTD_compressBound": (ctypes.c_size_t, (ctypes.c_size_t,)),
            "ZSTD_createCCtx": (ctypes.c_void_p, ()),
            "ZSTD_freeCCtx": (ctypes.c_size_t, (ctypes.c_void_p,)),
            "ZSTD_CCtx_setParameter": (ctypes.c_size_t, (ctypes.c_void_p, ctypes.c_int, ctypes.c_int)),
            "ZSTD_CCtx_setPledgedSrcSize": (ctypes.c_size_t, (ctypes.c_void_p, ctypes.c_ulonglong)),
            "ZSTD_compress2": (ctypes.c_size_t, (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                              ctypes.c_void_p, ctypes.c_size_t)),
        }
        for symbol, (result, arguments) in signatures.items():
            function = getattr(self.lib, symbol, None)
            require(function is not None, f"SUMMARY zstd library lacks {symbol}")
            function.restype, function.argtypes = result, arguments
        path = Path(name)
        self.identity = {
            "version": self.lib.ZSTD_versionString().decode("ascii"), "library": name,
            "sha256": sha256_file(path) if path.is_file() else None,
        }

    def checked(self, value):
        require(not self.lib.ZSTD_isError(value),
                "SUMMARY zstd: " + self.lib.ZSTD_getErrorName(value).decode("utf-8"))
        return value

    def decode(self, frame, decoded):
        check_summary_budget(decoded, len(frame))
        check_summary_frame(frame, decoded)
        source = ctypes.create_string_buffer(frame, len(frame))
        require(self.lib.ZSTD_getFrameContentSize(source, len(frame)) == decoded,
                "SUMMARY zstd content size differs")
        require(self.checked(self.lib.ZSTD_findFrameCompressedSize(source, len(frame))) == len(frame),
                "SUMMARY extra frame/trailing bytes")
        output = ctypes.create_string_buffer(decoded)
        require(self.checked(self.lib.ZSTD_decompress(output, decoded, source, len(frame))) == decoded,
                "SUMMARY decoded size differs")
        return output.raw

    def compressed_size(self, raw, level):
        require(0 < len(raw) <= S_MAX, "SUMMARY probe exceeds S_MAX")
        # Observer-only allocation, outside child memory/timing measurements.
        # Its bounded compressBound buffer also identifies a needed raw fallback.
        capacity = self.checked(self.lib.ZSTD_compressBound(len(raw)))
        output = ctypes.create_string_buffer(capacity)
        source = ctypes.create_string_buffer(raw, len(raw))
        context = self.lib.ZSTD_createCCtx()
        require(bool(context), "SUMMARY zstd context allocation failed")
        try:
            # Stable ZSTD_cParameter enum values, zstd.h; exact spec 12.1 settings.
            window_log = min(24, max(10, (len(raw) - 1).bit_length()))
            for parameter, value in ((100, level), (101, window_log), (200, 1),
                                     (201, 0), (202, 0), (400, 0)):
                self.checked(self.lib.ZSTD_CCtx_setParameter(context, parameter, value))
            self.checked(self.lib.ZSTD_CCtx_setPledgedSrcSize(context, len(raw)))
            stored = self.checked(self.lib.ZSTD_compress2(context, output, capacity, source, len(raw)))
            if stored > SUMMARY_STORED_CAP:
                stored = raw_summary_size(len(raw))  # Spec 9.1/12.1 fallback, exact in this size class.
            check_summary_budget(len(raw), stored)
            return stored
        finally:
            self.checked(self.lib.ZSTD_freeCCtx(context))


@lru_cache(maxsize=None)
def summary_codec(library=None):
    return SummaryZstd(library)


def v2_accounting(path: Path, zstd_library=None):
    """Physical accounting AFTER full reference verification; decode bounded SUMMARY only."""
    total = path.stat().st_size
    require(total >= 16 + 320 + 160, "short v2 sink")
    by_type, blocks, block_bytes, sha_bytes = Counter(), Counter(), Counter(), Counter()
    tables = []
    with path.open("rb") as source:
        head = source.read(12)
        require(head[:8] == b"\x89PFS2\r\n\x1a", "wrong v2 magic")
        length = struct.unpack_from("<I", head, 8)[0]
        require(0 < length <= MAX_JSON, "v2 head bound")
        header = DECODER.decode(source.read(length).decode("utf-8"))
        require(header.get("version") == 2, "wrong v2 header version")
        source.seek(total - 160)
        tail = source.read(160)
        require(tail[152:] == b"PFS2END\n", "v2 tail missing")
        root_offset, root_length = struct.unpack_from("<QI", tail)
        require(root_length in (320, 4096) and root_offset + root_length + 160 == total, "v2 root extent")
        require(struct.unpack_from("<Q", tail, 16)[0] == total, "v2 file size mismatch")
        source.seek(root_offset)
        root = source.read(root_length)
        require(root[:4] == b"FTR2" and struct.unpack_from("<H", root, 4)[0] == 2, "v2 root magic/version")
        flags = struct.unpack_from("<H", root, 6)[0]
        require(flags in (0, 1) and root_length == (320 if flags else 4096), "v2 profile flag/length mismatch")
        profile = "COMPACT" if flags else "FULL"
        if flags:
            data_end, summary_length, descriptors = struct.unpack_from("<QII", root, 8)
            summary_offset = data_end
            counts = dict(zip(TAGS + "I", struct.unpack_from("<8Q", root, 24)))
            finite_e = struct.unpack_from("<Q", root, 88)[0]
        else:
            data_end, summary_offset, summary_length, descriptors = struct.unpack_from("<QQII", root, 8)
            counts = dict(zip(TAGS + "I", struct.unpack_from("<8Q", root, 32)))
            finite_e, figure_buckets = struct.unpack_from("<QI", root, 416)
            require(figure_buckets == 1200, "v2 figure bucket count")
        # Fixed SUM2/header reads and bounded frame-header inspection precede
        # every stored/output/window/JSON allocation (spec 5.2).
        source.seek(summary_offset)
        summary_head = source.read(SUMMARY_HEADER)
        require(len(summary_head) == SUMMARY_HEADER and summary_head[:4] == b"SUM2", "v2 SUMMARY header")
        stored, decoded, crc, summary_flags = struct.unpack_from("<4I", summary_head, 4)
        require(summary_flags == 0, "v2 SUMMARY flags")
        check_summary_budget(decoded, stored)
        require(data_end == summary_offset and SUMMARY_HEADER < summary_length
                and summary_offset + summary_length <= root_offset, "v2 summary extent")
        require(summary_length == SUMMARY_HEADER + stored, "v2 SUMMARY framing")
        check_summary_frame(source.read(min(stored, 18)), decoded)
        require(finite_e <= counts["E"], "v2 figure population")
        require(counts["I"] == sum(counts[t] for t in TAGS), "v2 interleave count mismatch")
        pos = 16 + length
        seen = Counter()
        while pos < data_end:
            source.seek(pos)
            block = source.read(32)
            require(len(block) == 32 and block[:4] == b"BLK2", "v2 block framing")
            tag = chr(block[4])
            require(tag in TAGS + "I", "unknown v2 block tag")
            n = struct.unpack_from("<I", block, 12)[0]
            ncols = struct.unpack_from("<I", block, 8)[0]
            size = 32 + struct.unpack_from("<I", block, 24)[0]
            schema = next((s for s in header["records"] if s["type"] == tag), None)
            expected_columns = 1 if tag == "I" else len(schema["fields"]) if schema else None
            require(ncols == expected_columns and 0 < n
                    and 32 + 48 * ncols <= size <= 32 * MIB and pos + size <= data_end,
                    "invalid v2 block extent")
            directory = source.read(16 * ncols)
            column_stored = sum(struct.unpack_from("<I", directory, 16 * j + 8)[0] for j in range(ncols))
            require(size == 32 + 48 * ncols + column_stored, "v2 directory/SHA/body extent mismatch")
            seen[tag] += n
            blocks[tag] += 1
            block_bytes[tag] += size
            sha_bytes[tag] += 32 * ncols
            if tag != "I":
                by_type[tag] += size
            pos += size
        require(pos == data_end and all(seen[t] == counts[t] for t in TAGS + "I"),
                "v2 physical counts differ from root")
        require(descriptors == sum(blocks.values()), "v2 descriptor count mismatch")
        source.seek(summary_offset + SUMMARY_HEADER)
        frame = source.read(stored)
        require(len(frame) == stored, "short SUMMARY frame")
        check_summary_frame(frame, decoded)
        zstd = summary_codec(zstd_library)
        raw_summary = zstd.decode(frame, decoded)
        require(len(raw_summary) == decoded and zlib.crc32(raw_summary) == crc
                and hashlib.sha256(raw_summary).digest() == summary_head[20:52], "SUMMARY decoded CRC/SHA")
        summary = DECODER.decode(raw_summary.decode("utf-8"))
        require(type(summary) is dict, "SUMMARY is not an object")
        level = header["format"]["level"]
        require(type(level) is int and 1 <= level <= 19, "invalid SUMMARY HEAD level")
        probe_stored = zstd.compressed_size(raw_summary, level)
        raw_q, raw_m, m_members, unknown = summary_projections(raw_summary)
        c_q = zstd.compressed_size(raw_q, level)
        c_m = zstd.compressed_size(raw_m, level) if m_members else 0
        folds = summary.get("sink_folds")
        require(type(folds) is dict
                and list(folds) == ["closed_count", "open_at_end_count", "open_at_end_pnl"]
                and all(type(folds[k]) is int and folds[k] >= 0
                        for k in ("closed_count", "open_at_end_count"))
                and folds["closed_count"] + folds["open_at_end_count"] == counts["T"]
                and type(folds["open_at_end_pnl"]) in (int, float)
                and math.isfinite(folds["open_at_end_pnl"]), "v2 SUMMARY sink_folds missing/invalid")
        pos = summary_offset + summary_length
        rank_width = 4 if counts["T"] <= 2**32 else 8
        if profile == "COMPACT":
            require(canonical_profile(counts["E"], counts["T"], descriptors) == "COMPACT",
                    "COMPACT exceeds raw E/T/block ceiling")
            offset, size = struct.unpack_from("<QQ", root, 160)
            require(offset == pos and size == 96 * descriptors, "compact descriptor extent")
            require(tuple(blocks[t] for t in TAGS + "I") == struct.unpack_from("<8I", root, 208),
                    "compact block counts differ")
            for table_id, tag in enumerate(TAGS + "I"):
                table_size = 96 * blocks[tag]
                if tag != "I":
                    by_type[tag] += table_size
                tables.append({"id": table_id, "entry_count": blocks[tag], "entry_width": 96,
                               "page_count": 0, "last_page_bytes": 0, "bytes": table_size})
            pos += size
        for table_id in (range(19) if profile == "FULL" else ()):
            offset, pages, width, entries, last, flags = struct.unpack_from(
                "<QIIQII", root, 512 + 64 * table_id)
            expected_width = 512 if table_id < 8 else 256 if table_id == 18 else rank_width
            expected_entries = (blocks[(TAGS + "I")[table_id]] if table_id < 8 else
                                min(1200, finite_e) if table_id == 18 else counts["T"])
            expected_pages, expected_last, size = page_extent(expected_entries, expected_width)
            require((width, entries, pages, last, flags) == (
                expected_width, expected_entries, expected_pages, expected_last, int(bool(entries))),
                f"v2 FULL table {table_id} locator differs from short-page layout")
            if pages:
                require(offset == pos, "v2 table order/extent")
                tag = (TAGS + "I")[table_id] if table_id < 8 else "E" if table_id == 18 else "T"
                if tag != "I":
                    by_type[tag] += size
                pos += size
            else:
                require(offset == 0, "empty v2 table has offset")
            tables.append({"id": table_id, "entry_count": entries, "entry_width": width,
                           "page_count": pages, "last_page_bytes": last, "bytes": size})
        require(pos == root_offset, "unaccounted v2 index extent")
    shared = total - sum(by_type.values())
    require(shared >= 16 + length + summary_length + root_length + 160, "negative v2 shared accounting")
    pages = sum(table["page_count"] for table in tables)
    index_root_tail = sum(table["bytes"] for table in tables) + root_length + 160
    # Minimum for this mix: one descriptor per nonempty type, one T per rank,
    # one finite E bucket if present. The actual populations remain above.
    fixed_floor = (480 + 96 * len(blocks) if profile == "COMPACT" else
                   4256 + 4608 * len(blocks) + (41000 if counts["T"] else 0) + (4352 if finite_e else 0))
    return {
        "type_bytes": {t: by_type[t] for t in TAGS},
        "shared_bytes": shared, "sink_bytes": total,
        "counts": {t: counts[t] for t in TAGS},
        "level": level, "run_id": header["run_id"],
        "profile": profile, "canonical_profile": canonical_profile(counts["E"], counts["T"], descriptors),
        "root_bytes": root_length,
        "metadata_bytes": 16 + length + summary_length,
        "summary_bytes": summary_length, "sink_folds": folds,
        "summary_decoded_bytes": decoded, "summary_stored_bytes": stored, "summary_ratio": stored / decoded,
        "summary_expansion_bytes": stored - decoded, "summary_zstd_frame": True,
        "summary_json_sha256": summary_head[20:52].hex(),
        "summary_metadata_sha256": object_digest(summary),
        "summary_probe_stored_bytes": probe_stored, "summary_probe_level": level,
        "summary_probe_zstd_build": zstd.identity,
        "J_Q": len(raw_q), "C_Q": c_q, "J_M": len(raw_m), "C_M": c_m,
        "summary_m_members": m_members, "conservatively_counted_members": unknown,
        "summary_q_sha256": hashlib.sha256(raw_q).hexdigest(),
        "summary_m_sha256": hashlib.sha256(raw_m).hexdigest(),
        "block_counts": dict(blocks), "block_bytes": dict(block_bytes),
        "column_sha_bytes": dict(sha_bytes), "column_sha_total_bytes": sum(sha_bytes.values()),
        "index_tables": tables, "index_page_count": pages, "descriptor_count": descriptors,
        "rank_width": rank_width, "rank_bytes": sum(t["bytes"] for t in tables[8:18]),
        "figure_bytes": tables[18]["bytes"] if profile == "FULL" else 0, "finite_e_count": finite_e,
        "index_root_tail_bytes": index_root_tail, "fixed_floor_bytes": fixed_floor,
        "finalization_claim_bytes": FINALIZATION_CLAIM, "finalization_cap_bytes": FINALIZATION_CAP,
        "extra_work_file_cap_bytes": WORK_FILE_CAP,
        "output_plus_work_file_cap_bytes": total + WORK_FILE_CAP,
    }


@dataclass(frozen=True)
class Plan:
    builds: tuple[str, ...]
    sizes: tuple[int, ...]
    strategies: tuple[str, ...]
    replicates: int = 5
    warmups: int = 1
    levels: tuple[int, ...] = LEVELS

    @property
    def arms(self):
        return ("off", "v1", "v2-default") + tuple(f"v2-L{level}" for level in self.levels)

    def validate(self):
        require(len(self.builds) >= 4 and len(set(self.builds)) == len(self.builds),
                "at least 4 distinct independent builds are required")
        require(len(self.sizes) >= 3 and tuple(sorted(set(self.sizes))) == self.sizes
                and self.sizes[0] > 0, "sizes must contain at least 3 increasing positive bar counts")
        require(self.strategies and len(set(self.strategies)) == len(self.strategies),
                "strategies must be nonempty and distinct")
        require(self.replicates >= 5 and self.warmups >= 1, "need >=5 timed runs and >=1 warm-up")
        require(self.levels and len(set(self.levels)) == len(self.levels)
                and all(level in LEVELS for level in self.levels), "invalid candidate levels")


@dataclass(frozen=True)
class Job:
    kind: str
    build: str
    bars: int
    strategy: str
    arm: str
    run: int
    warmup: bool

    @property
    def key(self):
        return (self.kind, self.build, self.bars, self.strategy, self.arm, self.run, self.warmup)


def schedule(plan: Plan):
    """Latin-style rotation: every slot visits all builds, with rotating arms."""
    plan.validate()
    backtests = tuple(("backtest", arm) for arm in plan.arms)
    encoders = tuple(("encode", f"v2-L{level}") for level in plan.levels)
    cells = tuple(itertools.product(plan.sizes, plan.strategies))

    def stage(options, run, warmup, rotation):
        for cell_slot in range(len(cells)):
            bars, strategy = cells[(cell_slot + rotation) % len(cells)]
            for slot in range(len(options)):
                for build_slot in range(len(plan.builds)):
                    build = plan.builds[(build_slot + rotation + cell_slot) % len(plan.builds)]
                    kind, arm = options[(slot + build_slot + rotation + cell_slot) % len(options)]
                    yield Job(kind, build, bars, strategy, arm, run, warmup)

    # The v1 warm-ups supply immutable inputs for every later reference encode.
    for run in range(plan.warmups):
        yield from stage(backtests, run, True, run)
    for run in range(plan.warmups):
        yield from stage(encoders, run, True, run)
    for run in range(plan.replicates):
        yield from stage(backtests + encoders, run, False, run + plan.warmups)


def percentile(values, fraction):
    ordered = sorted(values)
    require(bool(ordered), "empty percentile")
    position = fraction * (len(ordered) - 1)
    lo, hi = math.floor(position), math.ceil(position)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def build_median(groups, rng=None):
    """groups = build -> run values; build, then run resampling."""
    builds = sorted(groups)
    require(bool(builds) and all(groups[b] for b in builds), "empty build group")
    selected = builds if rng is None else [rng.choice(builds) for _ in builds]
    medians = []
    for build in selected:
        values = groups[build]
        sample = values if rng is None else [rng.choice(values) for _ in values]
        medians.append(statistics.median(sample))
    return statistics.median(medians)


def ratio_ci(numerator, denominator, iterations=2000, seed=SEED):
    """Two-stage percentile bootstrap; builds sampled independently within arm."""
    require(iterations >= 200, "bootstrap needs at least 200 iterations")
    estimate = build_median(numerator) / build_median(denominator)
    rng = random.Random(seed)
    draws = [build_median(numerator, rng) / build_median(denominator, rng)
             for _ in range(iterations)]
    return {"ratio": estimate, "ci95": [percentile(draws, 0.025), percentile(draws, 0.975)]}


def aa_spread(groups):
    medians = {b: statistics.median(v) for b, v in sorted(groups.items())}
    values = list(medians.values())
    spreads = [max(a, b) / min(a, b) for a, b in itertools.combinations(values, 2)]
    return {
        "build_medians_seconds": medians,
        "median_pair_ratio": statistics.median(spreads),
        "max_over_min": max(values) / min(values),
        "comparison": "distinct builds within this arm; no same-binary A/A samples",
    }


def timing_passes(ratio):
    # Equality passes. The CI is descriptive, not a stricter replacement for 1.10.
    return type(ratio) in (int, float) and math.isfinite(ratio) and 0 < ratio <= 1.10


def is_digest(value):
    return (type(value) is str and len(value) == 64
            and all(char in "0123456789abcdef" for char in value))


def identity_problem(row, reference):
    if row["counts"]["E"] != reference["counts"]["E"] or row["counts"]["T"] != reference["counts"]["T"]:
        return "E/T counts differ between arms/builds/runs"
    if row.get("script_bars") != reference.get("script_bars"):
        return "script-bar counts differ between arms/builds/runs"
    for field in ("legacy_trade_sha256", "equity_sha256", "metrics_sha256"):
        if row[field] != reference[field]:
            return f"{field} differs between arms/builds/runs"
    if row["arm"] != "off" and reference["arm"] != "off":
        for field in ("record_sha256", "merged_sha256"):
            if row[field] != reference[field]:
                return f"{field} differs between sink arms/builds/runs"
    return None


def validate_rows(plan: Plan, rows):
    expected = Counter(job.key for job in schedule(plan))
    observed = Counter()
    problems = []
    core_references, sink_references = {}, {}
    default_levels = set()
    for row in rows:
        key = tuple(row.get(k) for k in ("kind", "build", "bars", "strategy", "arm", "run", "warmup"))
        observed[key] += 1
        if not row.get("ok") or row.get("exit_code") != 0:
            problems.append(f"run {row.get('sequence')}: {row.get('error', 'failed run')}")
            continue
        wall = row.get("wall_seconds")
        if type(wall) not in (float, int) or not math.isfinite(wall) or wall <= 0:
            problems.append(f"run {row.get('sequence')}: missing positive wall time")
            continue
        try:
            require(type(row["counts"]["T"]) is int and row["counts"]["T"] >= 0
                    and type(row["counts"]["E"]) is int and row["counts"]["E"] >= 0, "invalid E/T counts")
            require(row["wall_scope"] == WALL_SCOPE
                    and type(row["script_bars"]) is int and row["script_bars"] > 0,
                    "missing complete wall scope/script-bar count")
            require(all(is_digest(row[f])
                        for f in ("legacy_trade_sha256", "equity_sha256", "metrics_sha256")),
                    "missing identity digest")
            cell = (row["bars"], row["strategy"])
            refs = [core_references.setdefault(cell, row)]
            if row["arm"] != "off":
                require(set(row["record_sha256"]) == set(TAGS)
                        and all(is_digest(d) for d in row["record_sha256"].values())
                        and is_digest(row["merged_sha256"]), "missing full sink identity")
                require(type(row["sink_bytes"]) is int and row["sink_bytes"] > 0
                        and type(row["shared_bytes"]) is int and row["shared_bytes"] >= 0
                        and set(row["type_bytes"]) == set(TAGS)
                        and all(type(n) is int and n >= 0 for n in row["type_bytes"].values())
                        and sum(row["type_bytes"].values()) + row["shared_bytes"] == row["sink_bytes"],
                        "invalid byte accounting")
                require(row["bytes_per_equity"] == (
                    row["type_bytes"]["E"] / row["counts"]["E"] if row["counts"]["E"] else None),
                        "inconsistent bytes per equity point")
                require(row["bytes_per_trade"] == (
                    row["type_bytes"]["T"] / row["counts"]["T"] if row["counts"]["T"] else None),
                    "inconsistent bytes per trade")
                if row["arm"].startswith("v2"):
                    require(type(row["level"]) is int and 1 <= row["level"] <= 19, "missing actual zstd level")
                    require(row["finalization_included"] is True
                            and type(row["metadata_bytes"]) is int and row["metadata_bytes"] > 0,
                            "missing v2 finalization/metadata evidence")
                    require(row["profile"] in ("COMPACT", "FULL")
                            and (row["profile"] != "COMPACT" or canonical_profile(
                                row["counts"]["E"], row["counts"]["T"], row["descriptor_count"]) == "COMPACT"),
                            "invalid/missing v2 profile")
                    require(row["finalization_claim_bytes"] == FINALIZATION_CLAIM
                            and row["extra_work_file_cap_bytes"] == WORK_FILE_CAP,
                            "missing round-4 fixed finalization claim")
                    if row["arm"] == "v2-default":
                        default_levels.add(row["level"])
                    else:
                        require(row["level"] == int(row["arm"][4:]), "candidate level mismatch")
                if row["kind"] == "encode":
                    require(type(row["cpu_seconds_per_million_e"]) in (int, float)
                            and math.isfinite(row["cpu_seconds_per_million_e"])
                            and row["cpu_seconds_per_million_e"] >= 0, "missing encoder CPU")
                refs.append(sink_references.setdefault(cell, row))
            for ref in refs:
                problem = identity_problem(row, ref)
                require(problem is None, problem or "")
        except (KeyError, TypeError, GateError) as error:
            problems.append(f"run {row.get('sequence')}: {error}")
    missing, extra = expected - observed, observed - expected
    if missing:
        problems.append(f"missing {sum(missing.values())} planned rows; first {next(iter(missing))}")
    if extra:
        problems.append(f"unexpected/duplicate {sum(extra.values())} rows; first {next(iter(extra))}")
    if len(default_levels) > 1:
        problems.append("runtime default zstd level differs across builds/runs")
    return problems


def median_field(rows, field):
    groups = defaultdict(list)
    for row in rows:
        value = row.get(field)
        require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
                f"missing {field}")
        groups[row["build"]].append(value)
    return build_median(groups)


def memory_problems(plan, rows, evidence, artifacts):
    """L0b live-after-feed evidence is separate from process high-water RSS."""
    problems, gaps = defaultdict(list), []

    def record(arm, message, gap):
        problems[arm].append(message)
        if gap:
            gaps.append(message)

    for row in rows:
        if row.get("ok") and (type(row.get("peak_rss_bytes")) is not int or row["peak_rss_bytes"] <= 0):
            record(row["arm"], f"missing child peak RSS: run {row.get('sequence')}", True)
    if type(evidence) is not dict or evidence.get("schema") != "sink-v2-memory/v2":
        evidence = {"rows": []}
        gaps.append("missing sink-v2-memory/v2 evidence for the hard live-memory gates")
    if type(evidence.get("rows")) is not list or not all(type(p) is dict for p in evidence["rows"]):
        gaps.append("invalid memory proof rows")
        evidence = {"rows": []}
    proofs = defaultdict(list)
    for proof in evidence.get("rows", []):
        proofs[(proof.get("build"), proof.get("strategy"), proof.get("arm"), proof.get("axis"))].append(proof)
    for build, strategy, arm, axis in itertools.product(
            plan.builds, plan.strategies, plan.arms[2:], ("bars", "trades")):
        matches = proofs[(build, strategy, arm, axis)]
        try:
            require(len(matches) == 1, "missing/duplicate memory proof")
            proof = matches[0]
            artifact = artifacts[build]["strategies"][strategy]
            require(proof["library_sha256"] == artifacts[build]["library_sha256"]
                    and proof["module_sha256"] == artifact["module_sha256"], "memory proof artifact mismatch")
            populations, live = proof["populations"], proof["live_bytes"]
            require(type(populations) is list and type(live) is list
                    and len(populations) == len(live) and len(populations) >= (3 if axis == "bars" else 2)
                    and all(type(n) is int and n >= 0 for n in populations + live)
                    and populations == sorted(set(populations)), "invalid memory ladder")
            require(set((1000, 100000, 1000000) if axis == "bars" else (10, 10000)).issubset(populations),
                    "missing DESIGN 7(a)/(b) populations")
            require(set(proof["file_backings"]) >= {"scratch", "output", "temporary"}
                    and all(v in ("disk", "tmpfs") for v in proof["file_backings"].values())
                    and type(proof["counter"]) is str and bool(proof["counter"])
                    and proof["includes_resident_files"] is True and proof["excludes_loaded_ohlcv"] is True,
                    "live-memory proof must account for resident scratch/output without double counting")
            measured_backings = [
                {k: r.get("file_backings", {}).get(k, {}).get("backing") for k in ("scratch", "output", "temporary")}
                for r in rows if r["kind"] == "backtest" and not r["warmup"]
                and (r["build"], r["strategy"], r["arm"]) == (build, strategy, arm)]
            require(all(all(proof["file_backings"][k] == b[k] for k in b) for b in measured_backings),
                    "slope proof backing differs from this session")
            require(type(proof.get("log")) is str and bool(proof["log"]), "memory proof has no log locator")
            if max(live) - min(live) > MIB:
                record(arm, f"memory {build} {strategy} {arm} {axis}: "
                       "live memory changes by more than 1 MiB after OHLCV load", False)
        except (KeyError, TypeError, GateError) as error:
            record(arm, f"memory {build} {strategy} {arm} {axis}: {error}", True)
    for build, strategy, bars in itertools.product(plan.builds, plan.strategies, plan.sizes):
        cell = [r for r in rows if r["kind"] == "backtest" and not r["warmup"]
                and (r["build"], r["strategy"], r["bars"]) == (build, strategy, bars)]
        base = [r.get("peak_accounted_rss_bytes") for r in cell if r["arm"] == "v1"]
        for arm in plan.arms[2:]:
            values = [r.get("peak_accounted_rss_bytes") for r in cell if r["arm"] == arm]
            pair_rows = [r for r in cell if r["arm"] in ("v1", arm)]
            if any(not {"scratch", "output", "temporary"}.issubset(r.get("file_backings", {}))
                   or any(b.get("backing") not in ("disk", "tmpfs")
                          for b in r.get("file_backings", {}).values())
                   or r.get("memory_accounting_error")
                   or not r.get("rss_counter")
                   or r.get("rss_includes_resident_files") is not True
                   or type(r.get("peak_rss_bytes")) is not int
                   or type(r.get("peak_accounted_rss_bytes")) is not int
                   or r["peak_accounted_rss_bytes"] < r["peak_rss_bytes"] for r in pair_rows):
                record(arm, f"missing inclusive RSS/backing/counter {build} {strategy} {bars} {arm}", True)
            if (len(base) != plan.replicates or len(values) != plan.replicates
                    or any(type(v) is not int or v <= 0 for v in base + values)):
                record(arm, f"missing accounted RSS {build} {strategy} {bars} {arm}", True)
            elif max(values) > max(base) + 16 * MIB:
                record(arm, f"inclusive peak RSS exceeds v1 + 16 MiB: {build} {strategy} {bars} {arm}", False)
    return {"arms": {arm: {"pass": not problems[arm], "problems": problems[arm]} for arm in plan.arms[2:]},
            "gaps": sorted(set(gaps))}


def choose_level(table):
    """Start with fewest bytes; fall back to the next cheaper passing candidate."""
    eligible = sorted((r for r in table if r["eligible"]), key=lambda r: r["level"])
    if not eligible:
        return None
    best = min(range(len(eligible)), key=lambda i: (eligible[i]["total_bytes"], eligible[i]["level"]))
    while best > 0 and eligible[best]["total_bytes"] > eligible[best - 1]["total_bytes"] * 0.98:
        best -= 1
    return eligible[best]["level"]


def summarize(plan, rows, evidence=None, artifacts=None, iterations=2000, seed=SEED):
    plan.validate()
    problems = validate_rows(plan, rows)
    result = {
        "schema": "sink-v2-speed-gate/v7", "seed": seed, "bootstrap_iterations": iterations,
        "bootstrap": "95% percentile; resample builds independently within arm, then runs within build",
        "large_bars": max(plan.sizes), "builds": list(plan.builds),
        "replicates": plan.replicates, "warmups": plan.warmups,
        "sizes": [], "level_table": [], "selected_level": None, "problems": problems,
        "fixed_floor_references": FIXED_FLOORS,
        "size_planning_references": [case_model(c) for c in size_grid_cases()
                                     if c["shape"] in ("grid", "ordinary-corner", "selected_report_view")],
        "size_comparisons": [], "size_expansions": [], "size_counterexamples": [],
        "v2_le_v1_at_every_measured_size": None,
        "finalization_claim_bytes": FINALIZATION_CLAIM, "finalization_cap_bytes": FINALIZATION_CAP,
        "memory_gaps": [],
        "speed_pass": False, "memory_pass": False, "gate": "FAIL",
    }
    # A bad/missing row invalidates the experiment, never just its contribution.
    if problems:
        update_size_verdict(result, rows)
        return result
    timed = [r for r in rows if not r["warmup"]]
    indexed = defaultdict(lambda: defaultdict(list))
    for row in timed:
        if row["kind"] == "backtest":
            indexed[(row["bars"], row["strategy"], row["arm"])][row["build"]].append(row["wall_seconds"])
    cells = {}
    for bars in plan.sizes:
        size_rows = []
        for strategy in plan.strategies:
            base_groups = indexed[(bars, strategy, "v1")]
            arms = {}
            for arm in plan.arms:
                sample = [r for r in timed if r["kind"] == "backtest"
                          and (r["bars"], r["strategy"], r["arm"]) == (bars, strategy, arm)]
                groups = indexed[(bars, strategy, arm)]
                stats = ratio_ci(groups, base_groups, iterations, seed)
                stats.update(wall_seconds=build_median(groups), aa=aa_spread(groups))
                stats["levels_observed"] = sorted({r["level"] for r in sample if "level" in r})
                for field in (
                        "sink_bytes", "bytes_per_trade", "bytes_per_equity", "shared_bytes", "metadata_bytes",
                        "index_root_tail_bytes", "fixed_floor_bytes", "figure_bytes", "rank_bytes",
                        "column_sha_total_bytes", "engine_only_seconds", "finalization_seconds",
                        "engine_thread_cpu_seconds", "writer_thread_cpu_seconds"):
                    stats[field] = (None if any(r.get(field) is None for r in sample)
                                    else median_field(sample, field))
                for field in ("peak_rss_bytes", "peak_accounted_rss_bytes", "peak_tmpfs_resident_bytes",
                              "peak_scratch_bytes", "peak_output_bytes", "peak_temporary_bytes",
                              "peak_work_file_bytes", "finalization_peak_added_bytes"):
                    stats[field] = (None if any(r.get(field) is None for r in sample)
                                    else max(r[field] for r in sample))
                stats["profiles"] = sorted({r.get("profile", "OFF") for r in sample})
                stats["finalization_claim_checks"] = sorted({r.get("finalization_claim_check", "unmeasured")
                                                             for r in sample})
                stats["counts"] = sample[0]["counts"]
                stats["script_bars"] = sample[0]["script_bars"]
                stats["bars_per_second"] = stats["script_bars"] / stats["wall_seconds"]
                stats["file_backings"] = sample[0].get("file_backings", {})
                stats["memory_gaps"] = sorted({r["memory_accounting_error"] for r in sample
                                               if r.get("memory_accounting_error")})
                stats["added_peak_vs_v1_bytes"] = None
                stats["median_peak_accounted_rss_bytes"] = (
                    None if any(type(r.get("peak_accounted_rss_bytes")) is not int for r in sample)
                    else median_field(sample, "peak_accounted_rss_bytes"))
                arms[arm] = stats
            for arm, stats in arms.items():
                baseline = arms["v1"]
                if stats["peak_accounted_rss_bytes"] is not None and baseline["peak_accounted_rss_bytes"] is not None:
                    stats["added_peak_vs_v1_bytes"] = stats["peak_accounted_rss_bytes"] - baseline["peak_accounted_rss_bytes"]
                a, b = stats["median_peak_accounted_rss_bytes"], baseline["median_peak_accounted_rss_bytes"]
                stats["median_added_peak_vs_v1_bytes"] = None if a is None or b is None else a - b
                stats["l6_reported_memory_band_bytes"] = 16 * MIB
            cell = {"strategy": strategy, "bars": bars, "arms": arms}
            cells[(bars, strategy)] = cell
            size_rows.append(cell)
        # Bootstrap the size statistic as a median of strategy ratios, preserving
        # each sampled build across all strategy strata within an arm.
        def size_stat(arm, rng=None):
            selected = {}
            for name in (arm, "v1"):
                selected[name] = (list(plan.builds) if rng is None else
                                  [rng.choice(plan.builds) for _ in plan.builds])
            ratios = []
            for strategy in plan.strategies:
                estimates = {}
                for name in (arm, "v1"):
                    per_build = []
                    for build in selected[name]:
                        values = indexed[(bars, strategy, name)][build]
                        sample = values if rng is None else [rng.choice(values) for _ in values]
                        per_build.append(statistics.median(sample))
                    estimates[name] = statistics.median(per_build)
                ratios.append(estimates[arm] / estimates["v1"])
            return statistics.median(ratios)

        rng = random.Random(seed)
        draws = [size_stat("v2-default", rng) for _ in range(iterations)]
        result["sizes"].append({
            "bars": bars, "ratio": size_stat("v2-default"),
            "ci95": [percentile(draws, 0.025), percentile(draws, 0.975)], "strategies": size_rows,
        })
    # Whole-file acceptance is evaluated per file below, never from byte medians.
    result["speed_pass"] = all(
        timing_passes(cells[(max(plan.sizes), s)]["arms"]["v2-default"]["ratio"])
        for s in plan.strategies)
    memory = memory_problems(plan, rows, evidence, artifacts or {})
    memory_errors = memory["arms"]["v2-default"]["problems"]
    result["memory_pass"] = not memory_errors and not memory["gaps"]
    result["memory_gaps"] = memory["gaps"]
    result["problems"].extend(sorted(set(memory_errors + memory["gaps"])))
    for level in plan.levels:
        arm = f"v2-L{level}"
        encodes = [r for r in timed if r["kind"] == "encode" and r["arm"] == arm]
        comparisons = [cells[(max(plan.sizes), s)]["arms"][arm] for s in plan.strategies]
        # Equal weight to each size/strategy input, not to its number of runs.
        groups = [[r for r in encodes if (r["bars"], r["strategy"]) == (b, s)]
                  for b, s in itertools.product(plan.sizes, plan.strategies)]
        seconds_per_million = statistics.median(median_field(g, "cpu_seconds_per_million_e") for g in groups)
        wall_pass = all(timing_passes(c["ratio"]) for c in comparisons)
        cell_table = []
        for (bars, strategy), group in zip(itertools.product(plan.sizes, plan.strategies), groups):
            wall = cells[(bars, strategy)]["arms"][arm]
            baseline = cells[(bars, strategy)]["arms"]["v1"]
            large_rate = cells[(max(plan.sizes), strategy)]["arms"][arm]["bars_per_second"]
            cpu_per_million = median_field(group, "cpu_seconds_per_million_e")
            cell_table.append({
                "bars": bars, "strategy": strategy, "total_bytes": median_field(group, "sink_bytes"),
                "profiles": sorted({r["profile"] for r in group}),
                "bytes_per_equity": (None if any(r["bytes_per_equity"] is None for r in group)
                                     else median_field(group, "bytes_per_equity")),
                "bytes_per_trade": (None if any(r["bytes_per_trade"] is None for r in group)
                                    else median_field(group, "bytes_per_trade")),
                "encode_cpu_seconds_per_million_e": cpu_per_million,
                "measured_large_bars_per_second": large_rate,
                "writer_load": cpu_per_million * large_rate / 1e6,
                "v1_wall_seconds": baseline["wall_seconds"], "v2_wall_seconds": wall["wall_seconds"],
                "wall_ratio": wall["ratio"], "ci95": wall["ci95"],
            })
        result["level_table"].append({
            "level": level, "total_bytes": sum(median_field(g, "sink_bytes") for g in groups),
            "bytes_per_equity": (None if any(r["bytes_per_equity"] is None for r in encodes) else
                                 statistics.median(median_field(g, "bytes_per_equity") for g in groups)),
            "bytes_per_trade": (None if any(r["bytes_per_trade"] is None for r in encodes) else
                                statistics.median(median_field(g, "bytes_per_trade") for g in groups)),
            "encode_cpu_seconds_per_million_e": seconds_per_million,
            "measured_large_bars_per_second": {
                s: cells[(max(plan.sizes), s)]["arms"][arm]["bars_per_second"] for s in plan.strategies},
            "writer_load": max(c["writer_load"] for c in cell_table),
            "writer_load_source": "encoder child CPU/E times same-strategy LARGE script bars/full child wall",
            "large_ratios": [c["ratio"] for c in comparisons],
            "wall_pass": wall_pass,
            "memory_pass": memory["arms"][arm]["pass"],
            "memory_problems": memory["arms"][arm]["problems"],
            "eligible": wall_pass and all(c["writer_load"] <= 0.25 for c in cell_table)
                        and memory["arms"][arm]["pass"],
            "cells": cell_table,
        })
    result["selected_level"] = choose_level(result["level_table"])
    if set(plan.levels) != set(LEVELS):
        result["problems"].append("incomplete candidate level set; exploratory measurements cannot select a shipped level")
        result["selected_level"] = None
    if not result["speed_pass"]:
        result["problems"].append("LARGE default v2/v1 median exceeds 1.10 in at least one strategy")
    if result["selected_level"] is None:
        result["problems"].append("no compression level satisfies wall, memory and <=25% encoder-load criteria")
    result["gate"] = "PASS" if not result["problems"] else "FAIL"
    update_size_verdict(result, rows)
    return result


def render_summary(summary):
    def number(value):
        return "unmeasured" if value is None else f"{value:.6g}"

    real = summary.get("real_summary", {
        "status": "PENDING-REAL-SUMMARY", "reason": "no production SUMMARY evidence supplied",
    })
    lines = real_summary_lines(real) + binding_summary_lines(summary.get("binding_summary") or binding_summary(real)) + [
        "",
        "Report sink v2 speed gate",
        f"bootstrap seed={summary['seed']}; draws={summary['bootstrap_iterations']}; {summary['bootstrap']}",
        "Wall estimates: median of build medians; size ratio: median of strategy ratios.",
        "LARGE must pass for every strategy. CI describes uncertainty; the limit remains ratio <= 1.10.",
        "B/T and B/E include actual blocks/SHA/descriptors/short pages; FULL T includes all ten ranks.",
        "Shared = interleave blocks/pages + head + summary + root + tail.",
        f"Finalization CLAIM F={FINALIZATION_CLAIM} B; cap={FINALIZATION_CAP} B; work files <= {WORK_FILE_CAP} B total.",
        "F is not a measured RSS allowance. Inclusive added peak <= 16777216 B; live slope range <= 1048576 B.",
        "",
        "bars strategy arm level wall_s v1_wall_s ratio CI95 B/T(v1->arm) B/E(v1->arm) profile file_B shared_B A/A(max/min)",
    ]
    if "v2_late_verify" in summary:
        lines.insert(len(lines) - 1, f"v2_mode={summary['v2_mode']} "
                     f"v2_late_verify={summary['v2_late_verify']} (resolved)")
    for cell in summary.get("late_mode_comparison", {}).get("cells", []):
        lines.append("UNTIMED LATE-MODE BYTES " + json.dumps(cell, separators=(",", ":")))
    for size in summary["sizes"]:
        for cell in size["strategies"]:
            baseline = cell["arms"]["v1"]
            for arm, stats in cell["arms"].items():
                lines.append(
                    f"{cell['bars']} {cell['strategy']} {arm} "
                    f"{','.join(str(n) for n in stats['levels_observed']) or '-'} {number(stats['wall_seconds'])} "
                    f"{number(baseline['wall_seconds'])} {number(stats['ratio'])} "
                    f"[{number(stats['ci95'][0])},{number(stats['ci95'][1])}] "
                    f"{number(baseline['bytes_per_trade'])}->{number(stats['bytes_per_trade'])} "
                    f"{number(baseline['bytes_per_equity'])}->{number(stats['bytes_per_equity'])} "
                    f"{'/'.join(stats['profiles'])} {number(stats['sink_bytes'])} "
                    f"{number(stats['shared_bytes'])} {number(stats['aa']['max_over_min'])}")
                lines.append(
                    f"  memory process_peak_B={number(stats['peak_rss_bytes'])} "
                    f"inclusive_peak_B={number(stats['peak_accounted_rss_bytes'])} "
                    f"added_vs_v1_B={number(stats['added_peak_vs_v1_bytes'])} "
                    f"median_added_vs_v1_B={number(stats['median_added_peak_vs_v1_bytes'])} "
                    f"L6_reported_band_B={stats['l6_reported_memory_band_bytes']} "
                    f"tmpfs_resident_peak_B={number(stats['peak_tmpfs_resident_bytes'])} "
                    f"scratch_peak_B={number(stats['peak_scratch_bytes'])} "
                    f"work_peak_B={number(stats['peak_work_file_bytes'])} "
                    f"output_peak_B={number(stats['peak_output_bytes'])} "
                    f"curve_spill_temp_peak_B={number(stats['peak_temporary_bytes'])}")
                lines.append(
                    f"  phases engine_s={number(stats['engine_only_seconds'])} "
                    f"finalization_s={number(stats['finalization_seconds'])} "
                    f"engine_thread_cpu_s={number(stats['engine_thread_cpu_seconds'])} "
                    f"writer_thread_cpu_s={number(stats['writer_thread_cpu_seconds'])} "
                    f"isolated_finalization_added_B={number(stats['finalization_peak_added_bytes'])} "
                    f"claim_check={'; '.join(stats['finalization_claim_checks'])}")
                lines.append(f"  backing={json.dumps(stats['file_backings'], separators=(',', ':'))}")
                for gap in stats["memory_gaps"]:
                    lines.append(f"  memory GAP: {gap}")
        lines.append(f"size {size['bars']} median v2/v1={number(size['ratio'])} "
                     f"CI95=[{number(size['ci95'][0])},{number(size['ci95'][1])}]")
    lines.extend(("", "level total_B B/E B/T encode_cpu_s/1M_E measured_LARGE_bars/s writer_load LARGE_ratios wall_pass memory_pass eligible"))
    for row in summary["level_table"]:
        lines.append(
            f"{row['level']} {number(row['total_bytes'])} {number(row['bytes_per_equity'])} "
            f"{number(row['bytes_per_trade'])} {number(row['encode_cpu_seconds_per_million_e'])} "
            f"{json.dumps(row['measured_large_bars_per_second'], separators=(',', ':'))} "
            f"{number(row['writer_load'])} {','.join(number(r) for r in row['large_ratios'])} "
            f"{row['wall_pass']} {row['memory_pass']} {row['eligible']}")
        for problem in row["memory_problems"]:
            lines.append(f"  level {row['level']} memory rejection: {problem}")
        for cell in row["cells"]:
            lines.append(f"  {cell['bars']} {cell['strategy']} {'/'.join(cell['profiles'])} "
                         f"file_B={number(cell['total_bytes'])} "
                         f"v1_wall_s={number(cell['v1_wall_seconds'])} v2_wall_s={number(cell['v2_wall_seconds'])} "
                         f"writer_load={number(cell['writer_load'])}")
    lines.append("Index/root/tail floor REFERENCES (not measurements): " + json.dumps(FIXED_FLOORS))
    lines.append("Synthetic size probes: v1 generation and v2 reference encode wall times are not backtest speed ratios.")
    lines.append(f"Fixed small-run allowance K={SMALL_RUN_K} B for MODEL-ONLY and MEASURED verdicts (0.5).")
    lines.append("MODEL-ONLY assumptions: J=4096/C=1024; J_Q=1024/C_Q=512; J_M=3073/C_M=769; C_meta=512.")
    lines.append("MODEL-ONLY retained 1MiB caller M examples, admitted S_MAX variants and S_MAX+1 refusal are UNMEASURED.")
    lines.append("Size rule: (E<128 AND T<4) OR E<64 uses (ii-a), comparable<=v1+fixed K; all others (i), comparable<v1.")
    lines.append("Each MEASURED file judges COMPARABLE_V2; equality FAILS(i). Only SUMMARY frame payload is deductible.")
    for cell in summary.get("size_comparisons", []):
        def exact(value):
            return "PENDING" if value is None else str(value)

        def signed(value):
            return "PENDING" if value is None else f"{value:+d}"

        model = cell.get("model")
        modeled = ("MODEL-ONLY PENDING" if model is None else
                   f"MODEL-ONLY B={model['actual_bars']} N={model['N']} "
                   f"raw_E={model['raw_E']} anchors={model['anchor_count']} T={model['T']} "
                   f"f={model['fraction']} profile={model['profile']} "
                   f"v1_B={model['v1_bytes']} full_v2_B={model['v2_bytes']} "
                   f"EXCLUDED_CALLER_METADATA C_meta={model['C_meta']} COMPARABLE_V2={model['COMPARABLE_V2']} "
                   f"full_delta_B={model['v2_minus_v1_bytes']:+d} "
                   f"comparable_delta_B={model['comparable_minus_v1_bytes']:+d} "
                   f"limit_B={model['comparison_limit_bytes']} {model['clause']} {model['verdict_label']} "
                   f"full_diagnostic={model['diagnostic_verdict']} "
                   f"J={model['summary_decoded_bytes']} C={model['summary_stored_bytes']} "
                   f"C/J={model['summary_ratio']:.12g} J_Q={model['J_Q']} C_Q={model['C_Q']} "
                   f"J_M={model['J_M']} C_M={model['C_M']} C_comparable={model['C_comparable']} "
                   f"counted_Cmax={model['summary_max_comparable_C']}")
        lines.append(
            f"SIZE {cell['cell']} N={exact(cell['N'])} actual_bars={exact(cell['actual_bars'])} "
            f"raw_E={exact(cell['raw_E'])} anchors={exact(cell['anchor_count'])} T={exact(cell['T'])} "
            f"counts={json.dumps(cell['counts'], separators=(',', ':'))} "
            f"f={number(cell['fraction'])} D={exact(cell['D'])} profile={exact(cell['profile'])} | "
            f"MEASURED v1_B={exact(cell['v1_bytes'])} full_v2_B={exact(cell['v2_bytes'])} "
            f"{cell['metadata_exclusion']} C_meta={exact(cell['C_meta'])} "
            f"COMPARABLE_V2={exact(cell['COMPARABLE_V2'])} "
            f"full_delta_B={signed(cell['v2_minus_v1_bytes'])} "
            f"comparable_delta_B={signed(cell['comparable_minus_v1_bytes'])} "
            f"limit_B={exact(cell['comparison_limit_bytes'])} "
            f"{cell['clause'] or 'PENDING'} {cell['verdict']} full_diagnostic={cell['diagnostic_verdict']} | {modeled}")
        lines.append(
            f"  MEASURED SUMMARY J={exact(cell['summary_decoded_bytes'])} C={exact(cell['summary_stored_bytes'])} "
            f"C/J={number(cell['summary_ratio'])} expansion_B={signed(cell['summary_expansion_bytes'])} "
            f"header_plus_frame_B={exact(cell['summary_bytes'])} always_zstd={cell['summary_zstd_frame']} "
            f"HEAD_level={exact(cell['level'])}; J_Q={exact(cell['J_Q'])} C_Q={exact(cell['C_Q'])} "
            f"J_M={exact(cell['J_M'])} C_M={exact(cell['C_M'])} C_comparable={exact(cell['C_comparable'])} "
            f"C_comparable/J={number(cell['summary_comparable_ratio'])}; "
            f"counted_Cmax={exact(cell['summary_max_comparable_C'])} "
            f"rmax={exact(cell['summary_max_comparable_C'])}/{exact(cell['summary_decoded_bytes'])}="
            f"{number(cell['summary_max_comparable_ratio'])} counted_headroom_B={exact(cell['summary_headroom_bytes'])}")
        lines.append(
            f"  v1_wall_s={number(cell['v1_wall_seconds'])} v2_wall_s={number(cell['v2_wall_seconds'])}; "
            f"f_scope={cell['fraction_scope']}; requested_f={cell.get('requested_fraction', 'not synthetic')}; "
            f"zstd_build={json.dumps(cell['zstd_build'], separators=(',', ':'))}")
        lines.append(
            f"  SUMMARY probe_C={exact(cell['summary_probe_stored_bytes'])} "
            f"probe_level={exact(cell['summary_probe_level'])} "
            f"probe_build={json.dumps(cell['summary_probe_zstd_build'], separators=(',', ':'))} "
            f"matches_producer={cell['summary_probe_matches_producer']}; "
            f"JSON_provenance={json.dumps(cell['metadata_provenance'], separators=(',', ':'))}")
        lines.append(f"  Q_sha256={exact(cell['summary_q_sha256'])} M_sha256={exact(cell['summary_m_sha256'])} "
                     f"conservatively_counted={json.dumps(cell['conservatively_counted_members'])}")
        if cell["empty_full_file_verdict"]:
            lines.append(f"  EMPTY complete-file check (no deduction): full_v2_B={cell['v2_bytes']} "
                         f"v1_plus_K={cell['empty_full_file_limit_bytes']} {cell['empty_full_file_verdict']}")
        if model:
            lines.append(f"  MODEL-ONLY scope={model['metadata_scope']}")
            lines.append(f"  MODEL-ONLY v1 cross-check: formula_B={model['v1_formula_bytes']} "
                         f"term_by_term_B={model['v1_term_bytes']}")
            lines.append(f"  MODEL-ONLY v2 cross-check: formula_B={model['v2_formula_bytes']} "
                         f"term_by_term_B={model['v2_term_bytes']} comparable_term_B={model['comparable_term_bytes']}")
            if "summary_region_formula_bytes" in model:
                lines.append(f"  MODEL-ONLY SUMMARY ceiling: S_MAX={S_MAX} stored_bound={SUMMARY_STORED_LIMIT} "
                             f"region_formula_B={model['summary_region_formula_bytes']} "
                             f"region_term_B={model['summary_region_term_bytes']} "
                             f"decoded_excess_B={model['summary_decoded_excess_bytes']:+d} "
                             f"stored_excess_B={model['summary_stored_excess_bytes']:+d}; "
                             f"counterfactual_size_verdict={model['counterfactual_size_verdict']}")
            if model["empty_full_file_verdict"]:
                lines.append(f"  MODEL-ONLY EMPTY complete-file check (no deduction): "
                             f"full_v2_B={model['v2_bytes']} v1_plus_K={model['empty_full_file_limit_bytes']} "
                             f"MODEL-ONLY {model['empty_full_file_verdict']}")
            lines.append("  MODEL-ONLY break-even against REAL FIRST SUMMARY (model file base): "
                         + json.dumps(model.get("real_summary_break_even", {
                             "source": "MODEL-ONLY", "status": "PENDING-REAL-SUMMARY",
                         }), separators=(",", ":")))
        for gap in cell["evidence_gaps"]:
            lines.append(f"  PENDING: {gap}")
        for gap in cell["coverage_gaps"]:
            lines.append(f"  required-case PENDING: {gap}")
    lines.append("Probe physical components: cell profile measured_v1_B measured_full_v2_B MODEL-ONLY_v1_B MODEL-ONLY_full_v2_B "
                 "v1_wall_s v2_wall_s B/T(v1->v2) B/E(v1->v2)")
    for observation in summary.get("size_grid", []):
        if observation.get("writer_refusal"):
            lines.append(f"size REFUSED(summary_limit) {observation['case']['id']}; invalid artifact retained")
            continue
        if not observation["ok"]:
            lines.append(f"size PENDING {observation['case']['id']}: {observation['error']}")
            continue
        base, current, model = observation["v1"], observation["v2"], observation["model"]
        lines.append(f"{observation['case']['id']} {current['profile']} {base['sink_bytes']} {current['sink_bytes']} "
                     f"{model['v1_bytes']} {model['v2_bytes']} "
                     f"{number(base['wall_seconds'])} {number(current['wall_seconds'])} "
                     f"{number(base['bytes_per_trade'])}->{number(current['bytes_per_trade'])} "
                     f"{number(base['bytes_per_equity'])}->{number(current['bytes_per_equity'])}")
        lines.append(f"  metadata v1={base['metadata_bytes']} v2={current['metadata_bytes']} "
                     f"model_v1={model['v1_metadata_bytes']} model_v2={model['v2_metadata_bytes']}; "
                     f"index/root/tail={current['index_root_tail_bytes']} floor={current['fixed_floor_bytes']} "
                     f"SHA={current['column_sha_total_bytes']} "
                     f"scope={observation.get('model_scope', model['metadata_scope'])}")
        for arm, info in (("v1", base), ("v2", current)):
            lines.append(
                f"  {arm} process_peak_B={number(info['peak_rss_bytes'])} "
                f"inclusive_peak_B={number(info.get('peak_accounted_rss_bytes'))} "
                f"tmpfs_resident_peak_B={number(info.get('peak_tmpfs_resident_bytes'))} "
                f"scratch_peak_B={number(info.get('peak_scratch_bytes'))} "
                f"work_peak_B={number(info.get('peak_work_file_bytes'))} "
                f"output_peak_B={number(info.get('peak_output_bytes'))} "
                f"memory_gap={info.get('memory_accounting_error') or 'none'}")
    for cell in summary.get("size_expansions", []):
        lines.append(f"SIZE EXPANSION {cell['cell']} {cell['profile']}: "
                     f"v1={cell['v1_bytes']} v2={cell['v2_bytes']} ({cell['v2_minus_v1_bytes']:+d} B) "
                     f"C_meta={cell['C_meta']} COMPARABLE_V2={cell['COMPARABLE_V2']} "
                     f"{cell['clause']} {cell['verdict']} full_diagnostic={cell['diagnostic_verdict']}")
    for cell in summary.get("size_failures", []):
        lines.append(f"SIZE COUNTEREXAMPLE {cell['cell']} FAIL{cell['clause']}: "
                     f"v1={cell['v1_bytes']} full_v2={cell['v2_bytes']} "
                     f"C_meta={cell['C_meta']} COMPARABLE_V2={cell['COMPARABLE_V2']} "
                     f"({cell['comparable_minus_v1_bytes']:+d} comparable B)")
    for cell in summary.get("size_refusals", []):
        lines.append(f"SIZE REFUSED {cell['cell']} summary_limit {cell['clause'] or 'PENDING'}: "
                     f"writer={json.dumps(cell['writer_refusal'], separators=(',', ':'))}; "
                     f"invalid_artifact={cell['invalid_sink_path']} invalid_bytes={cell['invalid_sink_bytes']}; "
                     "no accepted file or size PASS/FAIL")
    lines.append("MODEL-ONLY refused candidates (excluded from admitted-model verdict): "
                 + (",".join(summary.get("model_size_refusals", [])) or "none"))
    lines.append(f"SIZE acceptance={summary.get('size_acceptance', 'PENDING')} "
                 f"{summary.get('model_size_verdict', 'MODEL-ONLY PENDING')} "
                 f"MEASURED={summary.get('measured_size_verdict', 'PENDING')}; "
                 "MODEL-ONLY assumptions are not production measurements.")
    lines.append(f"selected level: {summary['selected_level']}")
    for problem in summary["problems"]:
        lines.append(problem if problem.startswith("REFUSED ") else f"FAIL: {problem}")
    large = next((s for s in summary["sizes"] if s["bars"] == summary["large_bars"]), None)
    numbers = ("unavailable (invalid/incomplete experiment)" if large is None else
               f"median v2/v1={number(large['ratio'])} "
               f"CI95=[{number(large['ci95'][0])},{number(large['ci95'][1])}]")
    lines.append(f"GATE {summary['gate']} / LARGE bars={summary['large_bars']}: {numbers}; "
                 f"speed={summary['speed_pass']} memory={summary['memory_pass']}; "
                 f"v2<=v1 at every measured size={summary['v2_le_v1_at_every_measured_size']}; "
                 f"measured_size={summary.get('measured_size_verdict', 'PENDING')}; "
                 f"size acceptance={summary.get('size_acceptance', 'PENDING')}; "
                 f"measured clause failures={','.join(c['cell'] for c in summary.get('size_failures', [])) or 'none'}; "
                 f"refused runs={len(summary.get('size_refusals', []))}; "
                 "measured FAIL(i)/(ii-a), REFUSED admission and PENDING evidence prevent GATE PASS")
    return "\n".join(lines) + "\n"


def filesystem_backing(path: Path):
    """Recognized Linux mounts only; opaque overlay/FUSE is UNKNOWN (10.7)."""
    result = {"path": str(path.resolve()), "backing": "unknown", "filesystem": None}
    try:
        matches = []
        for line in Path("/proc/self/mountinfo").read_text().splitlines():
            left, right = line.split(" - ", 1)
            mount = left.split()[4]
            for escaped, literal in ((r"\040", " "), (r"\011", "\t"), (r"\012", "\n"), (r"\134", "\\")):
                mount = mount.replace(escaped, literal)
            if path.resolve().is_relative_to(Path(mount)):
                matches.append((len(mount), right.split()[0]))
        if matches:
            kind = max(matches)[1]
            result["filesystem"] = kind
            if kind in ("tmpfs", "ramfs"):
                result["backing"] = "tmpfs"
            elif kind in ("ext2", "ext3", "ext4", "xfs", "btrfs", "zfs"):
                result["backing"] = "disk"
    except (OSError, ValueError, IndexError):
        pass
    return result


def resident_file_bytes(path: Path):
    """Linux mincore without faulting pages in; map at most 64 MiB at a time."""
    require(sys.platform.startswith("linux"), "file residency requires Linux mincore")
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                          ctypes.c_int, ctypes.c_int, ctypes.c_longlong)
    libc.mmap.restype = ctypes.c_void_p
    libc.mincore.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_ubyte))
    libc.mincore.restype = ctypes.c_int
    libc.munmap.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
    libc.munmap.restype = ctypes.c_int
    page = mmap.PAGESIZE
    total = 0
    with path.open("rb", buffering=0) as source:
        size = os.fstat(source.fileno()).st_size
        for offset in range(0, size, 64 * MIB):
            length = min(64 * MIB, size - offset)
            address = libc.mmap(None, length, 0, mmap.MAP_SHARED, source.fileno(), offset)
            require(address != ctypes.c_void_p(-1).value, f"mmap residency failed errno={ctypes.get_errno()}")
            try:
                vector = (ctypes.c_ubyte * ((length + page - 1) // page))()
                require(libc.mincore(address, length, vector) == 0,
                        f"mincore failed errno={ctypes.get_errno()}")
                total += page * sum(bool(value & 1) for value in vector)
            finally:
                libc.munmap(address, length)
    return total


def process_memory(pid, resident_inodes):
    """Read current RSS and subtract watched file mappings once per sample."""
    rss, mapped, current = 0, 0, None
    for line in Path(f"/proc/{pid}/smaps").read_text().splitlines():
        parts = line.split()
        if len(parts) >= 5 and "-" in parts[0] and ":" in parts[3]:
            major, minor = (int(s, 16) for s in parts[3].split(":"))
            current = (os.makedev(major, minor), int(parts[4]))
        elif parts[:1] == ["Rss:"]:
            value = int(parts[1]) * 1024
            rss += value
            if current in resident_inodes:
                mapped += value
    if not rss:
        raise ProcessLookupError("child has exited or has no readable RSS yet")
    return rss, mapped


def process_family(pid):
    """Every live descendant; a verifier/encoder is part of this measured run."""
    family, pending = set(), [pid]
    while pending:
        member = pending.pop()
        if member in family:
            continue
        family.add(member)
        try:
            for task in Path(f"/proc/{member}/task").iterdir():
                pending.extend(int(value) for value in (task / "children").read_text().split())
        except FileNotFoundError:
            pass  # Exit race. Failed accounting is still caught by the sampler.
    return sorted(family)


def process_role(member, engine_pid, root_role):
    if member == engine_pid:
        return root_role
    try:
        # Inspect only the measured family. Never emit argv or persist it in evidence.
        arguments = Path(f"/proc/{member}/cmdline").read_bytes().split(b"\0")
        names = {Path(os.fsdecode(arg)).name for arg in arguments if arg}
        if "pineforge_sink_finalize.py" in names:
            return "A"
        if names & {"pf_sink_transcode_v2", "pineforge_sink_encode_v2.py", "phase-transcoder"}:
            return "C"
        if "run_json.py" in names:
            return "engine"
        if "pf_sink_tee" in names:
            return "replay" if b"--replay" in arguments else "relay"
    except OSError:
        pass  # Role discovery is diagnostic, separate from inclusive RSS validity.
    return "other"


class MemorySampler:
    """Sample this run's files, including unlinked curve/spill/work fds.

    RSS + tmpfs residency - those same file mappings' RSS avoids double counting.
    This is a sampled peak, not a claim about allocator-only finalization state.
    """

    def __init__(self, paths, interval=0.02, root_role="engine"):
        self.paths = {k: Path(v).resolve() for k, v in paths.items()}
        self.backings = {k: filesystem_backing(v) for k, v in self.paths.items()}
        self.interval = interval
        self.root_role = root_role
        self.stop = threading.Event()
        self.errors = set()
        self.peaks = Counter()
        self.samples = 0
        self.rss_time_series = []
        self.rss_time_series_dropped = 0
        self.engine_peak_rss_bytes = 0
        self.engine_peak_monotonic_ns = None
        self.process_vm_hwm = {}

    def record_process_rss(self, member, engine_pid, rss):
        observed = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        role = process_role(member, engine_pid, self.root_role)
        # VmHWM resets on exec; wait4/phase ru_maxrss can inherit the forking
        # driver's peak. Per-process evidence must come from this kernel field.
        hwm = None
        try:
            for line in Path(f"/proc/{member}/status").read_text().splitlines():
                if line.startswith("VmHWM:"):
                    hwm = int(line.split()[1]) * 1024
                    key = str(member)
                    previous = self.process_vm_hwm.get(key, {})
                    old = previous.get("vm_hwm_bytes", 0) if previous.get("role") == role else 0
                    self.process_vm_hwm[key] = {"pid": member, "role": role,
                                                "vm_hwm_bytes": max(old, hwm),
                                                "counter": "/proc/<pid>/status VmHWM"}
                    break
        except FileNotFoundError:
            pass
        if role == "engine" and rss > self.engine_peak_rss_bytes:
            self.engine_peak_rss_bytes = rss
            self.engine_peak_monotonic_ns = observed
        if len(self.rss_time_series) < RSS_TIME_SERIES_CAP:
            self.rss_time_series.append({"monotonic_ns": observed, "pid": member,
                                         "role": role, "rss_bytes": rss, "vm_hwm_bytes": hwm})
        else:
            self.rss_time_series_dropped += 1

    def snapshot(self, pid=None):
        files = {}
        for role, root in self.paths.items():
            candidates = root.rglob("*") if root.is_dir() else (root,)
            if role == "output" and not root.is_dir():
                # Owner staging/captures are siblings on the publication
                # filesystem, not hidden tmpfs or untracked writable child fds.
                candidates = itertools.chain(candidates, (
                    path for private in root.parent.glob(".pf-tee-run-*") for path in private.rglob("*")))
            for path in candidates:
                try:
                    info = path.stat()
                    if stat.S_ISREG(info.st_mode):
                        files[(info.st_dev, info.st_ino)] = (path, role, info.st_size, str(path))
                except FileNotFoundError:
                    continue  # Concurrent creation/cleanup; open fds below catch unlinked files.
        family = process_family(pid) if pid is not None else []
        for member in family:
            for fd in itertools.chain(Path(f"/proc/{member}/fd").iterdir(),
                                      Path(f"/proc/{member}/map_files").iterdir()):
                try:
                    name = os.readlink(fd).removesuffix(" (deleted)")
                    resolved = Path(name)
                    role = next((k for k, root in self.paths.items()
                                 if resolved == root or resolved.is_relative_to(root)), None)
                    if role is None and "output" in self.paths:
                        out = self.paths["output"]
                        try:
                            relative = resolved.relative_to(out.parent)
                            if relative.parts and relative.parts[0].startswith(".pf-tee-run-"):
                                role = "output"
                        except ValueError:
                            pass
                    if role is not None:
                        if "/map_files/" in str(fd) and any(item[3] == name for item in files.values()):
                            continue
                        info = fd.stat()
                        if stat.S_ISREG(info.st_mode):
                            key = (info.st_dev, info.st_ino)
                            # Prefer a regular path or open fd over map_files,
                            # which can require extra permissions on some hosts.
                            files.setdefault(key, (fd, role, info.st_size, name))
                    elif fd.parent.name == "fd":
                        info = fd.stat()
                        if stat.S_ISREG(info.st_mode):
                            details = Path(f"/proc/{member}/fdinfo/{fd.name}").read_text()
                            flags = next((int(line.split()[1], 8) for line in details.splitlines()
                                          if line.startswith("flags:")), None)
                            if flags is None or flags & os.O_ACCMODE:
                                self.errors.add(f"untracked writable child file: {name}")
                except FileNotFoundError:
                    continue
        amounts = Counter()
        resident_inodes = set()
        work_files = 0
        for inode, (path, role, size, name) in files.items():
            amounts[f"peak_{role}_bytes"] += size
            if ".pfs2-" in name:
                work_files += 1
                amounts["peak_work_file_bytes"] += size
                if Path(name).name not in ("work-00.bin", "work-01.bin") or size > 65536:
                    self.errors.add("10.7 work-file name/size exceeds fixed policy")
            if self.backings[role]["backing"] == "tmpfs":
                amounts["peak_tmpfs_resident_bytes"] += resident_file_bytes(path)
                resident_inodes.add(inode)
        amounts["peak_work_file_count"] = work_files
        if work_files > 2:
            self.errors.add("10.7 more than two work files")
        if pid is not None:
            observations = []
            for member in family:
                value = process_memory(member, resident_inodes)
                observations.append(value)
                self.record_process_rss(member, pid, value[0])
            rss = sum(value[0] for value in observations)
            mapped = sum(value[1] for value in observations)
            amounts["peak_sampled_process_rss_bytes"] = rss
            amounts["peak_accounted_rss_bytes"] = rss + max(0, amounts["peak_tmpfs_resident_bytes"] - mapped)
            amounts["peak_mapped_file_rss_bytes"] = mapped
            self.samples += 1
        for field, value in amounts.items():
            self.peaks[field] = max(self.peaks[field], value)

    def observe(self, pid):
        while not self.stop.is_set():
            try:
                self.snapshot(pid)
            except (FileNotFoundError, ProcessLookupError):
                pass  # Child exit race; zero successful samples is a gap below.
            except (OSError, ValueError) as error:
                self.errors.add(f"{type(error).__name__}: {error}")
            self.stop.wait(self.interval)

    def result(self, peak_rss):
        if any(v["backing"] == "unknown" for v in self.backings.values()):
            self.errors.add("unknown backing for scratch/output/curve/spill path")
        if not self.samples:
            self.errors.add("no inclusive RSS samples on this platform/run")
        if peak_rss is None:
            self.errors.add("child peak RSS unavailable")
        result = {field: self.peaks.get(field, 0) for field in (
            "peak_output_bytes", "peak_scratch_bytes", "peak_temporary_bytes",
            "peak_work_file_bytes", "peak_work_file_count", "peak_tmpfs_resident_bytes",
            "peak_sampled_process_rss_bytes", "peak_mapped_file_rss_bytes")}
        result.update(
            file_backings=self.backings, memory_samples=self.samples,
            memory_sample_interval_seconds=self.interval,
            rss_time_series=self.rss_time_series,
            rss_time_series_cap=RSS_TIME_SERIES_CAP,
            rss_time_series_dropped=self.rss_time_series_dropped,
            rss_time_series_clock="CLOCK_MONOTONIC",
            process_vm_hwm=list(self.process_vm_hwm.values()),
            engine_peak_sample_rss_bytes=self.engine_peak_rss_bytes or None,
            engine_peak_sample_monotonic_ns=self.engine_peak_monotonic_ns,
            peak_accounted_rss_bytes=(max(self.peaks.get("peak_accounted_rss_bytes", 0), peak_rss,
                                           self.peaks.get("peak_tmpfs_resident_bytes", 0))
                                      if self.samples and peak_rss is not None else None),
            rss_counter="wait4 HWM; sampled process-family smaps + mincore(tmpfs) minus mapped file RSS",
            rss_includes_resident_files=not self.errors,
            memory_accounting_error="; ".join(sorted(self.errors)) or None,
            memory_peak_scope="maximum sampled inclusive RSS, process HWM or final resident files; 20ms sampling can miss short peaks",
        )
        if not self.samples:
            for field in tuple(result):
                if field.startswith("peak_"):
                    result[field] = None
        if any(v["backing"] == "unknown" for v in self.backings.values()):
            result["peak_tmpfs_resident_bytes"] = None
        return result


def run_child(command, stdout: Path, stderr: Path, memory_paths=None, child_env=None, *, pass_fds=()):
    """wait4 provides THIS child's CPU/RSS, including its finalization/close."""
    require(hasattr(os, "wait4"), "the gate requires a Unix lab host with wait4")
    env = dict(os.environ, CCACHE_DISABLE="1", PYTHONDONTWRITEBYTECODE="1")
    env.update(child_env or {})
    root_role = "driver" if any(str(arg) in ("from-v1", "stage-a", "--tee") for arg in command) else "engine"
    sampler = MemorySampler(dict(memory_paths, diagnostic=stderr), root_role=root_role) if memory_paths else None
    with stdout.open("wb") as out, stderr.open("wb") as err:
        start = time.monotonic_ns()
        child = subprocess.Popen(command, stdout=out, stderr=err, env=env, pass_fds=pass_fds)
        observer = threading.Thread(target=sampler.observe, args=(child.pid,), daemon=True) if sampler else None
        if observer:
            observer.start()
        try:
            while True:
                try:
                    _, status, usage = os.wait4(child.pid, 0)
                    break
                except InterruptedError:
                    continue
            child.returncode = os.waitstatus_to_exitcode(status)
        except BaseException:
            child.kill()
            child.wait()
            raise
        finally:
            end = time.monotonic_ns()
            if sampler:
                sampler.stop.set()
                observer.join()
        elapsed = (end - start) / 1e9
    peak_rss = (int(usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024))
                if sys.platform in ("darwin", "linux") and usage.ru_maxrss > 0 else None)
    result = {
        "exit_code": child.returncode, "wall_seconds": elapsed,
        "start_monotonic_ns": start,
        "cpu_seconds": usage.ru_utime + usage.ru_stime,
        "peak_rss_bytes": peak_rss, "wall_scope": WALL_SCOPE, "finalization_included": True,
        "command": [str(part) for part in command],
    }
    if sampler:
        try:
            sampler.snapshot()  # Include the sealed output's final size/residency.
        except (OSError, ValueError) as error:
            sampler.errors.add(f"final file sample: {error}")
        result.update(sampler.result(peak_rss))
    return result


def worker(driver: Path, off: bool, argv):
    """The same run_json path for all arms, with its sink factory disabled for off."""
    sys.path.insert(0, str(driver.parent))
    module = load_module(driver, "_sink_speed_driver")
    if off:
        require(callable(getattr(module, "_new_report_sink", None)), "driver has no sink factory for off")
        module._new_report_sink = lambda *_args, **_kwargs: None
    require(callable(getattr(module, "main", None)), "driver has no main(argv)")
    return module.main(argv)


def default_strategies(corpus: Path, subset: Path):
    candidates = []
    for line in subset.read_text(encoding="utf-8").splitlines():
        name = line.partition("#")[0].strip()
        if not name:
            continue
        directory = corpus / name
        metadata_path = directory / "inputs.json"
        meta = load_json(metadata_path) if metadata_path.is_file() else {}
        tape = directory / meta.get("tv_trades_csv", "tv_trades.csv")
        with tape.open(encoding="utf-8-sig", newline="") as source:
            reader = csv.DictReader(source)
            require(reader.fieldnames is not None, f"empty TV tape {tape}")
            column = next((c for c in ("Trade #", "Trade number") if c in reader.fieldnames), None)
            require(column is not None, f"no trade-number column in {tape}")
            count = max((int(row[column]) for row in reader if row[column]), default=0)
        candidates.append((count, name))
    ordered = sorted(set(candidates))
    require(len(ordered) >= 3, "subset needs at least 3 strategies with TV tapes")
    return tuple(ordered[i][1] for i in (0, len(ordered) // 2, len(ordered) - 1))


def inspect_builds(roots, strategies, configs, library_relative):
    artifacts, used = {}, {}
    for root_text in roots:
        root = Path(root_text)
        cache_paths = [p for p in (root / "CMakeCache.txt", root / "build/CMakeCache.txt") if p.is_file()]
        require(len(cache_paths) == 1, f"need one CMakeCache.txt at {root} or {root}/build")
        cache = {}
        for line in cache_paths[0].read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.startswith(("//", "#")):
                name, value = line.split("=", 1)
                cache[name.split(":", 1)[0]] = value
        for key in ("CMAKE_CXX_COMPILER_LAUNCHER", "CMAKE_C_COMPILER_LAUNCHER", "RULE_LAUNCH_COMPILE"):
            require(not cache.get(key), f"{root}: {key} must be empty (ccache off)")
        require("ccache" not in cache.get("CMAKE_CXX_COMPILER", ""), "ccache compiler wrapper refused")
        info = {"cache": str(cache_paths[0]), "cache_sha256": sha256_file(cache_paths[0]),
                "flags": {k: v for k, v in cache.items() if k.startswith((
                    "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS", "CMAKE_BUILD_TYPE",
                    "CMAKE_CXX_COMPILER", "CMAKE_C_COMPILER", "PINEFORGE_WITH_ZSTD",
                    "PINEFORGE_VERSION_SOURCE"))},
                "strategies": {}}

        def artifact(path):
            path = path.resolve()
            require(path.is_relative_to(root) and path.is_file(), f"artifact outside build root or missing: {path}")
            stat = path.stat()
            key = (stat.st_dev, stat.st_ino)
            require(key not in used, f"shared/hardlinked artifact: {path} and {used.get(key)}")
            used[key] = str(path)
            return str(path), sha256_file(path)

        info["library"], info["library_sha256"] = artifact(root / library_relative)
        for strategy in strategies:
            cfg = configs.get(strategy, {})
            if "module" in cfg:
                module = root / cfg["module"]
            else:
                hits = [p for p in root.rglob("strategy.*") if p.suffix in (".so", ".dylib")
                        and strategy in p.parent.as_posix()]
                require(len(hits) == 1, f"{root}: need exactly one module for {strategy}; use --strategy-config")
                module = hits[0]
            module_path, digest = artifact(module)
            cpp = root / cfg["generated_cpp"] if "generated_cpp" in cfg else module.parent / "generated.cpp"
            require(cpp.is_file(), f"missing generated.cpp beside {module}")
            info["strategies"][strategy] = {"module": module_path, "module_sha256": digest,
                                            "generated_cpp": str(cpp.resolve()), "source_sha256": sha256_file(cpp)}
        artifacts[root_text] = info
    flag_sets = [info["flags"] for info in artifacts.values()]
    require(all(flags == flag_sets[0] for flags in flag_sets), "compiler/flags differ across builds")
    for strategy in strategies:
        require(len({a["strategies"][strategy]["source_sha256"] for a in artifacts.values()}) == 1,
                f"generated source differs across builds for {strategy}")
    return artifacts


def inspect_split_proofs(roots, artifacts, tee=False):
    """Bind the default hard gate to four clean builds of one exact tree."""
    trees, identities, codec_builds = set(), set(), set()
    for root in roots:
        def git(*argv):
            result = subprocess.run(["git", "-C", root, *argv], capture_output=True, text=True, check=False)
            require(result.returncode == 0, f"cannot bind proof checkout {root}: {result.stderr}")
            return result.stdout.strip()
        require(not git("status", "--porcelain=v1"), f"dirty proof checkout: {root}")
        tree = git("rev-parse", "HEAD^{tree}")
        trees.add(tree)
        info = artifacts[root]
        require(info["flags"].get("PINEFORGE_VERSION_SOURCE") == "FILE",
                "stage-C proof requires PINEFORGE_VERSION_SOURCE=FILE")
        require("engine_tree" not in info or info["engine_tree"] == tree,
                "source tree changed during experiment")
        info["engine_head"] = git("rev-parse", "HEAD")
        info["engine_tree"] = tree
        closure = [("transcoder", "build/bin/pf_sink_transcode_v2"),
                              ("kernel_library", "build/lib/libpineforge_kernel.a"),
                              ("sink_library", "build/lib/libpineforge_sink_v2.a")]
        if tee:
            closure.append(("relay", "build/bin/pf_sink_tee"))
        for key, relative in closure:
            path = Path(root) / relative
            require(path.is_file(), f"missing split artifact {path}")
            identity = (path.stat().st_dev, path.stat().st_ino)
            require(identity not in identities, "shared/hardlinked stage-C build artifact")
            identities.add(identity)
            info[key], info[key + "_sha256"] = str(path), sha256_file(path)
        engine_sources = [Path(root) / "CMakeLists.txt"]
        for directory in ("src", "include"):
            engine_sources.extend(path for path in (Path(root) / directory).rglob("*") if path.is_file())
        newest_engine_source = max(path.stat().st_mtime_ns for path in engine_sources)
        for key in ("library", "kernel_library"):
            require(Path(info[key]).stat().st_mtime_ns > newest_engine_source,
                    "stale engine archive; perform a full build before proof")
        tool_sources = [Path(root) / "tools/pf_sink_transcode_v2.cpp",
                        Path(root) / "cmake/PineForgeSinkV2.cmake", *engine_sources]
        tool_sources.extend(path for path in (Path(root) / "tools/sink_v2").rglob("*") if path.is_file())
        require(Path(info["sink_library"]).stat().st_mtime_ns > max(path.stat().st_mtime_ns for path in tool_sources),
                "stale stage-C archive")
        if tee:
            require(Path(info["relay"]).stat().st_mtime_ns > max(
                (Path(root) / p).stat().st_mtime_ns for p in
                ("tools/sink_v2/report_sink_tee.cpp", "cmake/PineForgeSinkV2.cmake")),
                "stale relay executable")
        version = subprocess.run([info["transcoder"], "--version"], capture_output=True, check=False)
        require(version.returncode == 0, "cannot read stage-C build identity")
        identity = json.loads(version.stdout)
        require(type(identity.get("zstd_build")) is str and identity["zstd_build"].startswith("zstd-1.5.7:"),
                "stage-C codec is not the pinned build")
        codec_builds.add(identity["zstd_build"])
        info["transcoder_version"] = identity
    require(len(trees) == 1, "build replicates have different source trees")
    require(len(codec_builds) == 1, "build replicates use different zstd configurations")


def check_artifacts_unchanged(artifacts):
    for info in artifacts.values():
        for key in ("transcoder", "kernel_library", "sink_library", "relay"):
            if key in info:
                require(sha256_file(Path(info[key])) == info[key + "_sha256"], "stage-C artifact changed during experiment")
        require(sha256_file(Path(info["library"])) == info["library_sha256"], "library changed during experiment")
        for asset in info["strategies"].values():
            require(sha256_file(Path(asset["module"])) == asset["module_sha256"],
                    "strategy module changed during experiment")
            require(sha256_file(Path(asset["generated_cpp"])) == asset["source_sha256"],
                    "generated source changed during experiment")


def prepare_feeds(output, sizes, strategies, configs, default_feed):
    """Exact prefixes only; never repeat/extend history or accept a short LARGE."""
    feeds, cached = {}, {}
    for strategy in strategies:
        source = Path(configs.get(strategy, {}).get("ohlcv", default_feed)).resolve()
        require(source.is_file(), f"missing OHLCV feed {source}")
        if source not in cached:
            destinations = {n: output / f"feed-{len(cached)}-{n}.csv" for n in sizes}
            handles = {n: p.open("wb") for n, p in destinations.items()}
            try:
                with source.open("rb") as inp:
                    header = inp.readline()
                    require(b"timestamp" in header.lower() or b"time" in header.lower(), "OHLCV header lacks time")
                    for handle in handles.values():
                        handle.write(header)
                    count = 0
                    for line in inp:
                        require(bool(line.strip()), "blank OHLCV row")
                        count += 1
                        for n, handle in handles.items():
                            if count <= n:
                                handle.write(line)
                        if count == max(sizes):
                            break
                    require(count == max(sizes), f"{source}: only {count} bars; need {max(sizes)}")
            finally:
                for handle in handles.values():
                    handle.close()
            cached[source] = destinations
        for n, path in cached[source].items():
            feeds[(strategy, n)] = path
    return feeds


def check_driver_args(arguments):
    require(type(arguments) is list and all(type(a) is str for a in arguments), "driver_args must be strings")
    reserved = ("--so", "--ohlcv", "--report-sink", "--report-sink-format", "--report-sink-level",
                "--spill-dir", "--run-id", "--bench", "--warmup", "--repeats", "--generated-cpp",
                "--v1-a-mode", "--v1-a-stdout-fd", "--v1-a-max-bytes", "--phase-log-out",
                "--tee", "--tee-state-out", "--tee-progress-out", "--tee-late-verify")
    require(all(a.split("=", 1)[0] not in reserved for a in arguments), "driver_args overrides gate-owned option")
    # run_json accepts legacy abbreviations. Do not let an abbreviation alter an arm.
    require(all(not any(flag.startswith(a.split("=", 1)[0]) for flag in reserved)
                for a in arguments if a.startswith("--")), "gate-owned option abbreviation refused")


def enrich(row, report, sink_info):
    row.update(sink_info)
    require(row["counts"]["E"] >= 0 and row["counts"]["T"] >= 0, "invalid sink population")
    row["metrics_sha256"] = object_digest(report["metrics"])
    require(report["summary"].get("total_trades") == row["counts"]["T"], "summary/T count mismatch")
    row["equity_sha256"] = row["record_sha256"]["E"]
    row["bytes_per_trade"] = row["type_bytes"]["T"] / row["counts"]["T"] if row["counts"]["T"] else None
    row["bytes_per_equity"] = row["type_bytes"]["E"] / row["counts"]["E"] if row["counts"]["E"] else None


def run_storage(directory, sink, stdout):
    """Use isolated children of the requested mounts, for every arm alike."""
    scratch_base = Path(os.environ.get("PF_SCRATCH_DIR") or directory)
    temporary_base = Path(os.environ.get("TMPDIR") or tempfile.gettempdir())
    scratch = Path(tempfile.mkdtemp(prefix="sink-speed-scratch-", dir=scratch_base))
    temporary = Path(tempfile.mkdtemp(prefix="sink-speed-temp-", dir=temporary_base))
    return ({"output": sink, "scratch": scratch, "temporary": temporary, "capture": stdout},
            {"PF_SCRATCH_DIR": str(scratch), "TMPDIR": str(temporary)})


def report_timings(row, report):
    diagnostics = report.get("diagnostics", {})
    row["script_bars"] = diagnostics.get("script_bars_processed")
    require(type(row["script_bars"]) is int and row["script_bars"] > 0, "missing measured script-bar count")
    window = report.get("report_window")
    selected = window.get("counts", {}) if type(window) is dict else None
    row["actual_bars"] = selected.get("window_script_bars") if selected is not None else row["script_bars"]
    row["anchor_count"] = selected.get("anchor_points") if selected is not None else 0
    require(type(row["actual_bars"]) is int and row["actual_bars"] >= 0
            and type(row["anchor_count"]) is int and row["anchor_count"] in (0, 1),
            "missing actual selected bars/anchor count")
    row["engine_only_seconds"] = report.get("elapsed_seconds")
    # Optional lab instrumentation; loading/startup/serialization prevents
    # inferring finalization duration by subtracting from whole-child time.
    timing = report.get("sink_stage_c_timing", diagnostics.get("sink_timing", {}))
    for field in ("finalization_seconds", "engine_thread_cpu_seconds", "writer_thread_cpu_seconds",
                  "finalization_peak_added_bytes"):
        value = timing.get(field)
        require(value is None or (type(value) in (int, float) and math.isfinite(value) and value >= 0),
                f"invalid measured {field}")
        row[field] = value
    # Per-child wait4 in tee mode; legacy column meanings stay runner user CPU
    # and inline encoder user+system CPU, never overlapping CHILDREN deltas.
    for field in ("stage_a_child", "stage_c_child", "runner_child", "relay_child",
                  "stage_a_cpu_seconds", "relay_cpu_seconds", "v2_mode", "stage_a_wall_seconds",
                  "stage_a_standalone_seconds", "stage_c_standalone_seconds",
                  "engine_start_monotonic_ns", "engine_finished_observed_ns",
                  "engine_sink_fd_closed_observed_ns", "driver_sink_fd_closed_ns"):
        if field in timing:
            row[field] = timing[field]
    row["finalization_claim_check"] = (
        "unmeasured: isolated finalization allocation counter unavailable"
        if row["finalization_peak_added_bytes"] is None else
        "within claim" if row["finalization_peak_added_bytes"] <= FINALIZATION_CLAIM else
        "above claim, within cap" if row["finalization_peak_added_bytes"] <= FINALIZATION_CAP else "above cap")


def bind_summary_evidence(info, args, producer, artifact_sha256, provenance, expected_summary):
    """Bind lab producer-build evidence separately from the observer's zstd build."""
    entry = getattr(args, "zstd_builds", {}).get(producer)
    if entry is not None:
        require(type(entry) is dict and entry.get("artifact_sha256") == artifact_sha256
                and type(entry.get("build")) is str and bool(entry["build"].strip())
                and type(entry.get("log")) is str and bool(entry["log"].strip())
                and is_digest(entry.get("zstd_library_sha256")), "invalid/artifact-mismatched zstd build evidence")
    info["zstd_build"] = entry
    info["metadata_provenance"] = provenance
    expected = {key: value for key, value in expected_summary.items() if key not in ("report_sink", "sink_folds", "sink_stage_c_timing")}
    expected["sink_folds"] = info["sink_folds"]
    require(object_digest(expected) == info["summary_metadata_sha256"], "embedded SUMMARY differs from its source run JSON")
    info["summary_schema_validated"] = True  # Caller has completed the reference verifier.
    info["summary_probe_matches_producer"] = bool(
        entry and entry["zstd_library_sha256"] == info["summary_probe_zstd_build"].get("sha256"))


def check_stage_a_receipts(streaming, file_mode):
    """The timed stdout arm and untimed FILE artifact must certify the same bytes."""
    require(type(streaming) is dict and type(file_mode) is dict
            and streaming.get("complete") is True and file_mode.get("complete") is True,
            "stage A receipt is incomplete")
    for field in ("sha256", "sink_bytes", *("count_" + t.lower() for t in TAGS)):
        require(field in streaming and streaming[field] == file_mode.get(field),
                f"stage A stdout/FILE receipt {field} differs")


def materialize_stdout_v1(command, directory, timed_report, sink, child_env):
    """Outside the timer: retain the same stream for the unchanged value/size checks."""
    replay = list(command)
    for flag in ("--v1-a-mode", "--v1-a-stdout-fd"):
        index = replay.index(flag)
        del replay[index:index + 2]
    if "--phase-log-out" in replay:
        index = replay.index("--phase-log-out")
        del replay[index:index + 2]
    stdout, stderr = directory / "file-validation.json", directory / "file-validation.stderr"
    observation = run_child(replay, stdout, stderr, child_env=child_env)
    require(observation["exit_code"] == 0, "untimed v1 FILE validation replay failed")
    file_report, _, _ = read_report(stdout, False)
    check_stage_a_receipts(timed_report.get("report_sink"), file_report.get("report_sink"))
    require(sink.is_file() and sink.stat().st_size == timed_report["report_sink"]["sink_bytes"],
            "untimed v1 FILE artifact size differs from stdout receipt")
    return {"scope": "untimed FILE replay; receipt-bound artifact for value/size checks",
            "receipt_matches": True, "stdout": str(stdout), **observation}


def stage_a_probe(argv):
    """A standalone real-finalizer rate probe, outside the backtest gate."""
    parser = argparse.ArgumentParser(description=stage_a_probe.__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="FILE artifact, or untimed stdout receipt-check artifact")
    parser.add_argument("--v1-a-mode", choices=("file", "stdout"), default="file")
    parser.add_argument("--phase-log-out", type=Path)
    args = parser.parse_args(argv)
    with tempfile.TemporaryDirectory(prefix="sink-speed-stage-a-") as temporary:
        directory = Path(temporary)
        command = [sys.executable, "-B", str(ROOT / "scripts/sink_v2_split_driver.py"),
                   "stage-a", str(args.source.resolve()), str(args.output.resolve()),
                   "--v1-a-mode", args.v1_a_mode]
        if args.phase_log_out:
            command += ["--phase-log-out", str(args.phase_log_out.resolve())]
        with open(os.devnull, "wb") as null:
            if args.v1_a_mode == "stdout":
                command += ["--v1-a-stdout-fd", str(null.fileno())]
            observation = run_child(command, directory / "receipt.json", directory / "stderr",
                                    pass_fds=(null.fileno(),) if args.v1_a_mode == "stdout" else ())
        require(observation["exit_code"] == 0, (directory / "stderr").read_text())
        receipt = load_json(directory / "receipt.json")
        require(receipt.get("complete") is True, "standalone A receipt is incomplete")
        result = {"v1_a_mode": args.v1_a_mode, "receipt": receipt, "measurement": observation}
        if args.v1_a_mode == "stdout":
            replay = [sys.executable, "-B", str(ROOT / "scripts/sink_v2_split_driver.py"),
                      "stage-a", str(args.source.resolve()), str(args.output.resolve())]
            checked = run_child(replay, directory / "file-receipt.json", directory / "file-stderr")
            require(checked["exit_code"] == 0, "standalone FILE receipt check failed")
            file_receipt = load_json(directory / "file-receipt.json")
            check_stage_a_receipts(receipt, file_receipt)
            result.update(file_mode_receipt=file_receipt, receipt_matches=True)
        print(json.dumps(result, separators=(",", ":"), allow_nan=False))
    return 0


def resolved_late_verify(args):
    return getattr(args, "v2_late_verify", None) or (
        "structure" if getattr(args, "v2_mode", "serial") == "tee" else "full")


def late_mode_receipt(seeds, grid, enabled, seed_cells_planned, *, skip_size_grid):
    grid_cells_planned = (sum(case["N"] >= 100000 for case in size_grid_cases())
                          if enabled and not skip_size_grid else 0)
    expected = seed_cells_planned + grid_cells_planned if enabled else 0
    cells = [row["late_mode_comparison"] for row in itertools.chain(seeds, grid)
             if "late_mode_comparison" in row]
    return {
        "enabled": enabled, "scope": "untimed same-input bytes; no contribution to medians",
        "cells": cells, "cells_compared": len(cells), "missing": max(0, expected - len(cells)),
        "grid_cells_planned": grid_cells_planned,
        "equal": (bool(expected) and len(cells) == expected
                  and all(cell.get("equal") is True for cell in cells)) if enabled else None,
    }


def compare_late_modes(args, seed_sink, seed_meta, directory, build, cell, level, block_records=None,
                       *, timed_outputs):
    """Compare four real replays and recorded timed bytes, outside all observations.

    Mismatches always retain all four outputs. Equal outputs follow
    --keep-artifacts; their receipts/logs/digests remain. Shims change only the
    CLI worker count; the owner/relay/proof paths are real.
    """
    require(getattr(args, "v2_mode", "serial") == "tee",
            "--v2-compare-late-modes requires --v2-mode tee")
    directory = directory / "late-mode-comparison"
    directory.mkdir(mode=0o700)
    real_tool = Path(build).resolve() / "build/bin/pf_sink_transcode_v2"
    relay = real_tool.with_name("pf_sink_tee")
    require(real_tool.is_file() and relay.is_file(), "late-mode comparison needs this build's tool and relay")
    evidence = {
        "cell": cell, "directory": str(directory), "equal": False,
        "started_monotonic_ns": time.monotonic_ns(),
        "scope": "untimed same-seed replays; excluded from all timing/CPU/memory medians",
        "seed": {"path": str(seed_sink), "sha256": sha256_file(seed_sink),
                 "summary_path": str(seed_meta), "summary_sha256": sha256_file(seed_meta)},
        "modes_compared": ["tee-full", "tee-structure-t4", "tee-structure-t1", "serial-full"],
        "outputs": [],
    }
    receipt_path = directory / "receipt.json"
    try:
        for mode, late_verify, threads in (
                ("tee-full", "full", None), ("tee-structure-t4", "structure", 4),
                ("tee-structure-t1", "structure", 1), ("serial-full", "full", None)):
            arm_dir = directory / mode
            arm_dir.mkdir(mode=0o700)
            tool = real_tool
            if threads is not None:
                shim_dir = arm_dir / "exec"
                shim_dir.mkdir(mode=0o700)
                tool = shim_dir / "pf_sink_transcode_v2"
                tool.write_text("#!/bin/sh\nexec " + shlex.quote(str(real_tool))
                                + " --late-threads " + str(threads) + ' "$@"\n', encoding="utf-8")
                tool.chmod(0o700)
                (shim_dir / "pf_sink_tee").symlink_to(relay)
            sink = arm_dir / "sink.pfs2"
            stdout, stderr, phases = (arm_dir / name for name in ("stdout.json", "stderr.txt", "phases.json"))
            command = [sys.executable, "-B", str(args.codec), "from-v1", str(seed_sink), str(sink),
                       "--summary-json", str(seed_meta), "--level", str(level), "--phase-log-out", str(phases)]
            if block_records:
                command += ["--block-records", block_records]
            if mode.startswith("tee-"):
                command += ["--tee", "--tee-late-verify", late_verify]
            env = dict(os.environ, PF_SINK_V2_TRANSCODER=str(tool))
            with stdout.open("wb") as out, stderr.open("wb") as err:
                child = subprocess.run(command, stdout=out, stderr=err, env=env, check=False)
            require(child.returncode == 0, f"late-mode replay {cell}/{mode} exited {child.returncode}; see {stderr}")
            receipt = load_json(stdout)
            require(receipt.get("complete") is True and receipt.get("late_verify", "full") == late_verify,
                    f"late-mode replay {cell}/{mode} receipt disagrees with requested mode")
            require(sink.is_file(), f"late-mode replay {cell}/{mode} has no output")
            evidence["outputs"].append({
                "mode": mode, "late_verify": late_verify, "late_threads_requested": threads,
                "path": str(sink), "sha256": sha256_file(sink), "bytes": sink.stat().st_size,
                "receipt_bytes": receipt.get("sink_bytes"), "retained": True,
                "receipt_path": str(stdout), "phase_log_path": str(phases),
            })
        require(sha256_file(seed_sink) == evidence["seed"]["sha256"]
                and sha256_file(seed_meta) == evidence["seed"]["summary_sha256"],
                f"late-mode comparison {cell} seed changed during replay")
        replays = list(evidence["outputs"])
        oracle = replays[-1]
        # Timed sinks may already be deleted. Never substitute a replay's digest
        # or re-hash a later file for the bytes recorded by the measured job.
        timed_outputs = [dict(output, sha256=output.get("sha256"), bytes=output.get("bytes"))
                         for output in timed_outputs]
        evidence["outputs"].extend(timed_outputs)
        evidence["modes_compared"] = list(dict.fromkeys(output["mode"] for output in evidence["outputs"]))
        for output in itertools.chain(replays[:-1], timed_outputs):
            if (not is_digest(output.get("sha256")) or type(output.get("bytes")) is not int
                    or (output["sha256"], output["bytes"]) != (oracle["sha256"], oracle["bytes"])):
                evidence["refusal_kind"] = LateModeMismatch.kind
                raise LateModeMismatch(evidence, output, oracle)
        require(all(output["receipt_bytes"] == output["bytes"] for output in replays),
                f"late-mode comparison {cell} output length disagrees with its receipt")
        evidence.update(equal=True, sha256=oracle["sha256"], bytes=oracle["bytes"])
        if not args.keep_artifacts:
            for output in replays:
                Path(output["path"]).unlink()
                output["retained"] = False
        return evidence
    finally:
        dump_json(receipt_path, evidence)


def compare_late_mode_seeds(args, plan, seeds, rows, output):
    """Run once per seed only after experiment() has finished its entire loop."""
    root = output / "late-modes"
    root.mkdir()
    results = []
    build_indices = {build: i for i, build in enumerate(plan.builds)}
    with (output / "late-mode-seeds.jsonl").open("w", encoding="utf-8") as raw:
        for (build, bars, strategy), seed in seeds.items():
            directory = root / f"{build_indices.get(build, -1)}-{bars}-{strategy}"
            result = {"build": build, "bars": bars, "strategy": strategy,
                      "level_requested": args.size_grid_level, "ok": False, "artifact_dir": str(directory)}
            try:
                require(build in build_indices, f"late-mode seed {(build, bars, strategy)} has unknown build")
                directory.mkdir(parents=True)
                require(seed["ok"], f"late-mode seed {(build, bars, strategy)} did not finish successfully")
                timed_outputs = [
                    {"mode": "timed-" + row["v2_late_verify"], "late_verify": row["v2_late_verify"],
                     "path": row["sink_path"],
                     "sha256": row.get("metadata_provenance", {}).get("artifact_sha256"),
                     "bytes": row.get("sink_bytes"), "retained": row.get("sink_retained"),
                     **{key: row[key] for key in ("sequence", "run", "warmup")}}
                    for row in rows if row["kind"] == "encode"
                    and (row["build"], row["bars"], row["strategy"]) == (build, bars, strategy)
                    and row["level_requested"] == args.size_grid_level
                ]
                require(timed_outputs, f"late-mode seed {(build, bars, strategy)} has no timed encodes "
                        f"at level {args.size_grid_level}")
                result["late_mode_comparison"] = compare_late_modes(
                    args, Path(seed["sink_path"]), Path(seed["artifact_dir"]) / "summary-meta.json",
                    directory, build, {"build": build, "bars": bars, "strategy": strategy},
                    args.size_grid_level, timed_outputs=timed_outputs)
                result["ok"] = True
            except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
                result["error"] = f"{type(error).__name__}: {error}"
                if isinstance(error, LateModeMismatch):
                    result["refusal_kind"] = error.kind
                    result["late_mode_comparison"] = error.evidence
            raw.write(json.dumps(result, separators=(",", ":"), allow_nan=False) + "\n")
            raw.flush()
            results.append(result)
            if not result["ok"]:
                break
    return results


def experiment(args, plan, artifacts, configs, feeds, output):
    reader = load_module(args.v1_reader, "_sink_speed_v1")
    seeds, references, sink_references = {}, {}, {}
    rows = []
    with (output / "raw.jsonl").open("w", encoding="utf-8") as raw:
        for sequence, job in enumerate(schedule(plan)):
            directory = output / f"run-{sequence:06d}"
            directory.mkdir()
            stdout, stderr = directory / "stdout.json", directory / "stderr.txt"
            sink = directory / ("sink.jsonl" if job.arm == "v1" else "sink.pfs2")
            memory_paths, child_env = run_storage(directory, sink, stdout)
            if args.driver.name == "sink_v2_split_driver.py" or args.codec.name == "sink_v2_split_driver.py":
                child_env["PF_SINK_V2_TRANSCODER"] = str(Path(job.build) / "build/bin/pf_sink_transcode_v2")
            row = dict(zip(("kind", "build", "bars", "strategy", "arm", "run", "warmup"), job.key))
            row.update(sequence=sequence, ok=False, exit_code=None, sink_path=str(sink), artifact_dir=str(directory),
                       v1_a_mode=getattr(args, "v1_a_mode", "file") if job.arm == "v1" else None,
                       v2_mode=getattr(args, "v2_mode", "serial") if job.arm.startswith("v2") else None,
                       v2_late_verify=resolved_late_verify(args) if job.arm.startswith("v2") else None,
                       level_requested=(0 if job.arm == "v2-default" else
                                        int(job.arm[4:]) if job.arm.startswith("v2-L") else None),
                       engine_thread_cpu_seconds=None, writer_thread_cpu_seconds=None,
                       finalization_seconds=None, peak_scratch_bytes=None,
                       storage_paths={k: str(v) for k, v in memory_paths.items()})
            try:
                cfg = configs.get(job.strategy, {})
                asset = artifacts[job.build]["strategies"][job.strategy]
                if job.kind == "backtest":
                    driver_args = [
                        "--so", asset["module"], "--ohlcv", str(feeds[(job.strategy, job.bars)]),
                        "--generated-cpp", asset["generated_cpp"],
                        "--spill-dir", str(memory_paths["temporary"]),
                    ] + cfg.get("driver_args", [])
                    if job.arm != "off":
                        driver_args += ["--report-sink", str(sink), "--report-sink-format",
                                        "1" if job.arm == "v1" else "2",
                                        "--run-id", "sink-speed"]
                        if job.arm != "v1":
                            driver_args += ["--report-sink-level", str(row["level_requested"])]
                    command = [sys.executable, "-B", str(Path(__file__).resolve()), "_worker",
                               str(args.driver), "off" if job.arm == "off" else "on", *driver_args]
                else:
                    seed_sink, seed_meta = seeds[(job.build, job.bars, job.strategy)]
                    command = [sys.executable, "-B", str(args.codec), "from-v1", str(seed_sink), str(sink),
                               "--summary-json", str(seed_meta), "--level", str(row["level_requested"])]
                if job.arm.startswith("v2") and getattr(args, "v2_mode", "serial") == "tee":
                    command += ["--tee", "--tee-late-verify", resolved_late_verify(args)]
                if getattr(args, "phase_log", False) and job.arm != "off":
                    phases = directory / "phases.json"
                    command += ["--phase-log-out", str(phases)]
                    row["phase_log_path"] = str(phases)
                with contextlib.ExitStack() as inherited:
                    pass_fds = ()
                    if job.kind == "backtest" and job.arm == "v1" and row["v1_a_mode"] == "stdout":
                        # stdout goes to this inherited fd, never a FILE-mode --out path.
                        null = inherited.enter_context(open(os.devnull, "wb"))
                        pass_fds = (null.fileno(),)
                        command += ["--v1-a-mode", "stdout", "--v1-a-stdout-fd", str(null.fileno())]
                    row.update(run_child(command, stdout, stderr, memory_paths, child_env, pass_fds=pass_fds))
                if job.arm.startswith("v2") and capture_writer_refusal(row, stdout, stderr, sink):
                    raise SummaryLimit("REFUSED(summary_limit); invalid file retained")
                require(row["exit_code"] == 0, f"child exited {row['exit_code']}; see {stderr}")
                if job.kind == "encode" and getattr(args, "v2_mode", "serial") == "tee":
                    require(load_json(stdout).get("late_verify", "full") == resolved_late_verify(args),
                            "tee encode receipt does not bind requested late verification")
                row["stdout_sha256"] = sha256_file(stdout)
                if job.kind == "backtest":
                    report, counts, hashes = read_report(stdout, job.arm == "off")
                    if job.arm == "v1" and row["v1_a_mode"] == "stdout":
                        row["v1_file_validation"] = materialize_stdout_v1(
                            command, directory, report, sink, child_env)
                    if job.arm.startswith("v2") and "transcoder_version" in artifacts[job.build]:
                        require(report.get("report_sink", {}).get("complete") is True
                                and report["report_sink"].get("zstd_build")
                                    == artifacts[job.build]["transcoder_version"]["zstd_build"],
                                "receipt does not bind the measured transcoder build")
                        if getattr(args, "v2_mode", "serial") == "tee":
                            require(report.get("sink_stage_c_timing", {}).get("v2_mode") == "tee",
                                    "tee backtest did not report the owner/relay mode")
                            require(report["report_sink"].get("late_verify", "full")
                                    == resolved_late_verify(args),
                                    "tee receipt does not bind requested late verification")
                    dump_json(directory / "report-metadata.json", report)
                else:
                    report = load_json(seeds[(job.build, job.bars, job.strategy)][1])
                report_timings(row, report)
                if job.kind == "encode":
                    for field in ("engine_only_seconds", "finalization_seconds", "engine_thread_cpu_seconds",
                                  "writer_thread_cpu_seconds", "finalization_peak_added_bytes"):
                        row[field] = None
                    row["finalization_claim_check"] = "unmeasured: reference encoder has no isolated allocation counter"
                if job.arm == "off":
                    row.update(counts=counts, legacy_trade_sha256=hashes["T"], equity_sha256=hashes["E"],
                               metrics_sha256=object_digest(report["metrics"]), sink_bytes=0,
                               type_bytes={t: 0 for t in TAGS}, shared_bytes=0,
                               bytes_per_trade=None, bytes_per_equity=None)
                    require(report["summary"].get("total_trades") == counts["T"], "off summary/T mismatch")
                else:
                    converted = directory / "export-v1.jsonl"
                    if job.arm == "v1":
                        info = read_v1(sink, reader)
                    else:
                        conversion = run_child(
                            [sys.executable, "-B", str(args.codec), "to-v1", str(sink), str(converted)],
                            directory / "verify.stdout", directory / "verify.stderr")
                        row["verification"] = conversion
                        require(conversion["exit_code"] == 0, "reference v2 verification/export failed")
                        info = read_v1(converted, reader)
                        accounting = v2_accounting(sink, getattr(args, "zstd_library", None))
                        require(accounting["counts"] == info["counts"], "v2/export counts differ")
                        require(object_digest(accounting["sink_folds"]) == object_digest(info["sink_folds"]),
                                "v2/export SUMMARY trade folds differ")
                        require(accounting["profile"] == accounting["canonical_profile"],
                                "writer/reference encoder did not choose canonical profile")
                        info.update(accounting)
                        source_meta = (directory / "report-metadata.json" if job.kind == "backtest"
                                       else seeds[(job.build, job.bars, job.strategy)][1])
                        provenance = {
                            "kind": "same-run production JSON" if job.kind == "backtest" else "verified v1 warm-up JSON",
                            "mode": ("selected" if "report_window" in report else "ordinary")
                                    + ("+outputs" if "outputs" in report else ""),
                            "run": {k: row[k] for k in ("build", "bars", "strategy", "run", "warmup", "kind", "sequence")},
                            "metadata_path": str(source_meta), "metadata_sha256": sha256_file(source_meta),
                            "artifact_path": str(sink), "artifact_sha256": sha256_file(sink),
                        }
                        if job.kind == "encode":
                            provenance["seed_sink_path"] = str(seed_sink)
                            provenance["source_run"] = next(
                                {k: r[k] for k in ("sequence", "build", "bars", "strategy", "run", "warmup")}
                                for r in rows if r.get("sink_path") == str(seed_sink))
                        bind_summary_evidence(info, args, job.build if job.kind == "backtest" else "reference",
                                              artifacts[job.build]["library_sha256"] if job.kind == "backtest"
                                              else sha256_file(args.codec), provenance, report)
                        if row["level_requested"]:
                            require(info["level"] == row["level_requested"], "writer ignored requested level")
                    enrich(row, report, info)
                    if job.kind == "backtest":
                        descriptor = report.get("report_sink", {})
                        require(descriptor.get("version") == (1 if job.arm == "v1" else 2),
                                "driver did not report the requested sink format")
                        require(all(descriptor.get("counts", {}).get(t, 0) == row["counts"][t] for t in TAGS),
                                "driver descriptor counts differ from sink")
                        if job.arm.startswith("v2"):
                            row["scratch_flags"] = descriptor.get("scratch_flags")
                            require(type(row["scratch_flags"]) is int and row["scratch_flags"] in (0, 1, 2),
                                    "missing/invalid v2 scratch_flags")
                    if job.kind == "encode":
                        require(row["counts"]["E"] > 0, "encoder load needs E records")
                        row["cpu_seconds_per_million_e"] = row["cpu_seconds"] * 1e6 / row["counts"]["E"]
                cell = (job.bars, job.strategy)
                problem = identity_problem(row, references.setdefault(cell, row))
                require(problem is None, problem or "")
                if job.arm != "off":
                    problem = identity_problem(row, sink_references.setdefault(cell, row))
                    require(problem is None, problem or "")
                if job.kind == "backtest" and job.arm == "v1" and job.warmup:
                    metadata = directory / "summary-meta.json"
                    seed_metadata = {k: v for k, v in report.items()
                                     if k not in ("report_sink", "sink_stage_c_timing")}
                    seed_metadata["sink_folds"] = row["sink_folds"]
                    # The real encoder reads compact JSON only (indented text is "invalid report number").
                    metadata.write_text(json.dumps(seed_metadata, separators=(",", ":"), allow_nan=False), encoding="utf-8")
                    seeds[(job.build, job.bars, job.strategy)] = (sink, metadata)
                row["ok"] = True
            except (OSError, ValueError, KeyError, TypeError, struct.error, OverflowError) as error:
                row["error"] = f"{type(error).__name__}: {error}"
            if row["ok"]:
                for role in ("scratch", "temporary"):
                    try:
                        memory_paths[role].rmdir()
                    except OSError:
                        row["memory_accounting_error"] = (
                            (row.get("memory_accounting_error") or "") + f"; owned {role} not empty: {memory_paths[role]}")
            # Keep immutable warm-up v1 inputs. All failed artifacts remain inspectable.
            keep_seed = job.kind == "backtest" and job.arm == "v1" and job.warmup
            if row["ok"] and not args.keep_artifacts:
                for path in (sink, directory / "export-v1.jsonl", stdout):
                    if path.exists() and not (path == sink and keep_seed):
                        path.unlink()
            row["sink_retained"] = sink.is_file()
            raw.write(json.dumps(row, separators=(",", ":"), allow_nan=False) + "\n")
            raw.flush()
            rows.append(row)
            print(f"{sequence + 1}: {job.kind} {job.bars} {job.strategy} {job.arm} "
                  f"{'warmup' if job.warmup else 'timed'} {'ok' if row['ok'] else row['error']}", flush=True)
            if not row["ok"]:
                break  # Preserve the first fault; missing planned rows also force FAIL.
    return rows


def size_extreme_cases():
    cases = [{"N": 0, "T": 0, "fraction": 0.25, "shape": "empty"}]
    cases += [{"N": n, "T": t, "fraction": f, "shape": "extreme"}
              for n, t in ((100, 0), (100, 1), (999, 0), (1000, 0)) for f in GRID_FRACTIONS]
    populations = [(n, t) for n in CORNER_E for t in range(4)]
    populations += [(n, 4) for n in (1, 63, 64, 127, 128)]
    cases += [{"N": n, "T": t, "fraction": f, "shape": shape,
               "actual_bars": 1000 if shape == "selected_report_view" else n,
               "anchor_count": int(shape == "selected_report_view")}
              for shape in ("ordinary-corner", "selected_report_view")
              for n, t in populations for f in GRID_FRACTIONS]
    cases += [{"N": 100, "T": 1, "fraction": f, "shape": shape}
              for shape in ("large-compressible-summary", "incompressible-summary",
                            "ceiling-compressible-summary", "ceiling-raw-summary",
                            "above-ceiling-summary") for f in GRID_FRACTIONS]
    return cases


def size_grid_cases():
    cases = [{"N": n, "T": t, "fraction": f, "shape": "grid"}
             for n in GRID_SIZES for t in (0, 1, n // 20) for f in GRID_FRACTIONS]
    cases += [{"N": n, "T": t, "fraction": 0.25, "shape": "count-boundary"}
              for n, t in ((10979, 1), (10980, 1), (1200, 3313), (1200, 3314))]
    cases += [{"N": n, "T": 0, "fraction": 1.0, "shape": "block-boundary",
               "block_records": "E=1,T=1024,I=65536"} for n in (127, 128)]
    cases += size_extreme_cases()
    cases += [{"N": 100, "T": 0, "fraction": 0.25, "shape": "large-summary"},
              {"N": 100, "T": 1, "fraction": 0.25, "shape": "outputs"}]
    for sequence, case in enumerate(cases):
        case["id"] = f"{sequence:03d}-{case['shape']}-N{case['N']}-T{case['T']}-f{case['fraction']}"
    return cases


def write_size_seed(header_path, metadata_path, case, target, summary_path):
    """Streaming synthetic E/T(/O/V/K), with captured production metadata shape.

    These are format-size probes, never engine/backtest timing samples.
    """
    header = load_json(header_path)
    header["records"] = [s for s in header["records"] if s["type"] in "ET"]
    if case["shape"] == "outputs":
        reader = load_module(ROOT / "docker/pf_sink.py", "_size_schema")
        for tag, columns in (
                ("O", reader._O + tuple((f"v{i}", "f64?", "") for i in range(8))),
                ("V", reader._V), ("K", tuple((f"c{i}", "f64?", "") for i in range(16)))):
            header["records"].append({"type": tag, "fields": [c[0] for c in columns],
                                      "types": [c[1] for c in columns], "units": [c[2] for c in columns]})
    counts, digest = Counter(), hashlib.sha256()
    rng = random.Random(SEED)
    n, trades = case["N"], case["T"]
    bars = case.get("actual_bars", n)
    start = 1700000000000
    net = 0.0
    wins = losses = 0
    best = worst = None
    with target.open("wb") as output:
        def emit(value):
            line = (json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n").encode()
            output.write(line)
            digest.update(line)

        emit(header)
        for ordinal in range(n):
            active = ordinal % 4 < round(case["fraction"] * 4)
            equity = 10000.0 + rng.uniform(-500, 500) if active else 10000.0
            profit = rng.uniform(-20, 20) if active else 0.0
            emit(["E", start + ordinal * 60000, equity, profit])
            counts["E"] += 1
            if case["shape"] == "outputs":
                clock = start + ordinal * 60000
                emit(["O", ordinal, clock, clock + 60000, *[rng.random() for _ in range(8)]])
                emit(["V", ordinal, 0, ordinal, clock, clock + 60000, 0, 0, 1,
                      rng.random(), f"{rng.getrandbits(64):016x}", f"event-{ordinal}"])
                counts["O"] += 1
                counts["V"] += 1
        for ordinal in range(trades):
            entry = ordinal % max(1, n)
            clock = start + entry * 60000
            pnl = rng.uniform(-10, 10)
            emit(["T", ordinal + 1, "long", clock, clock + 60000, 100.0, 100.0 + pnl,
                  1.0, pnl, pnl, abs(pnl), abs(pnl), 0.0, entry, entry + 1,
                  False, ordinal + 1, 1, "entry", "exit", ""])
            counts["T"] += 1
            net += pnl
            wins += pnl > 0
            losses += pnl < 0
            best = pnl if best is None else max(best, pnl)
            worst = pnl if worst is None else min(worst, pnl)
        if case["shape"] == "outputs":
            emit(["K", *[float(i) for i in range(16)]])
            counts["K"] = 1
        trailer = {"trailer": "pf-report-sink", "version": 1,
                   "counts": {s["type"]: counts[s["type"]] for s in header["records"]},
                   "sha256": digest.hexdigest()}
        output.write((json.dumps(trailer, separators=(",", ":")) + "\n").encode())
    metadata = copy.deepcopy(load_json(metadata_path))
    for key in ("report_sink", "trades", "equity_curve"):
        metadata.pop(key, None)
    metadata.pop("outputs", None)
    metadata["input"].update(bars=bars)
    metadata["diagnostics"].update(input_bars_processed=bars, script_bars_processed=bars,
                                   sink_gate_size_probe=case)
    metadata["summary"].update(total_trades=trades, wins=wins, losses=losses, net_pnl=net,
                               avg_trade=net / trades if trades else 0.0,
                               best_trade=best or 0.0, worst_trade=worst or 0.0, bars_processed=bars)
    metadata["sink_folds"] = {"closed_count": trades, "open_at_end_count": 0, "open_at_end_pnl": 0.0}
    if case["shape"] == "selected_report_view":
        # Explicit synthetic population facts, never a production selected-run claim.
        metadata.setdefault("report_window", {}).setdefault("counts", {}).update(
            window_script_bars=bars, anchor_points=case["anchor_count"])
    if case["shape"] == "outputs":
        metadata["outputs"] = {"schema_version": 1, "message_format": "synthetic",
                               "manifest_sha256": hashlib.sha256(b"{}").hexdigest(), "manifest": {},
                               "constants": {}, "hlines": []}
    if case["shape"] == "large-summary":
        metadata.setdefault("applied_inputs", {})["sink_gate_summary_probe"] = "x" * 65536
    if case["shape"] in ("large-compressible-summary", "ceiling-compressible-summary", "above-ceiling-summary"):
        # Retain the example, repeat admission at S_MAX and probe its successor.
        # Do not force its measured compression ratio to the MODEL's quarter.
        budget = S_MAX + int(case["shape"] == "above-ceiling-summary")
        metadata.setdefault("applied_inputs", {})["sink_gate_summary_probe"] = ""
        base = json.dumps(metadata, separators=(",", ":"), allow_nan=False).encode("utf-8")
        require(len(base) <= budget, "production metadata exceeds compressible-SUMMARY probe budget")
        metadata["applied_inputs"]["sink_gate_summary_probe"] = "x" * (budget - len(base))
    if case["shape"] in ("incompressible-summary", "ceiling-raw-summary"):
        budget = S_MAX
        caller = Path(case["summary_fixture_path"])
        require(caller.stat().st_size == budget, f"caller SUMMARY must have exactly {budget} decoded bytes")
        raw_summary = caller.read_bytes()
        supplied = DECODER.decode(raw_summary.decode("utf-8"))
        require(supplied["summary"]["total_trades"] == trades
                and supplied["sink_folds"] == metadata["sink_folds"]
                and supplied["diagnostics"]["script_bars_processed"] == n,
                "caller SUMMARY population/folds differ from the synthetic sink")
        summary_path.write_bytes(raw_summary)
        return  # Preserve the actual caller's JSON spelling and order.
    # Exact synthetic metadata spelling is fed to the real reference encoder.
    summary_path.write_text(json.dumps(metadata, separators=(",", ":"), allow_nan=False), encoding="utf-8")


def capture_writer_refusal(observation, stdout, stderr, sink):
    """A failed writer's typed diagnostic is evidence; a model prediction is not.

    Accept WriterError{summary_limit}, {"error":{"kind":"summary_limit"}},
    or 14.4's report_sink_failed/args{} failure line identifying summary_limit.
    Retain its diagnostics and invalid artifact; never verify/export it as a file.
    """
    if observation.get("exit_code") in (None, 0):
        return False
    typed, receipt_complete, failure_code = False, None, None
    for path in (stdout, stderr):
        with path.open(encoding="utf-8", errors="replace") as source:
            diagnostic = source.read(65536)
        typed |= bool(re.search(r"\bWriterError\{summary_limit\}", diagnostic))
        for fragment in (diagnostic, *diagnostic.splitlines()):
            try:
                failure = DECODER.decode(fragment)
            except (ValueError, TypeError):
                continue
            if type(failure) is dict and type(failure.get("error")) is dict:
                typed |= failure["error"].get("kind") == "summary_limit"
                receipt_complete = failure.get("complete")
            if (type(failure) is dict and failure.get("code") == "report_sink_failed"
                    and failure.get("args") == {} and type(failure.get("error")) is str
                    and re.search(r"\bsummary_limit\b", failure["error"])):
                typed = True
                receipt_complete = failure.get("complete")
                failure_code = failure["code"]
            if (type(failure) is dict and failure.get("format") == 2
                    and failure.get("transcoder_version") == 2 and failure.get("complete") is False
                    and failure.get("status") == 4 and failure.get("code") == "report_sink_failed"
                    and failure.get("reason") == "v2_input_mismatch"
                    and type(failure.get("diagnostic")) is str
                    and re.search(r"\bsummary_limit\b", failure["diagnostic"])):
                typed, receipt_complete, failure_code = True, False, failure["code"]
    if not typed:
        return False
    require(receipt_complete in (None, 0), "summary_limit writer claimed complete=1")
    if sink.is_file() and sink.stat().st_size >= 8:
        with sink.open("rb") as source:
            source.seek(-8, os.SEEK_END)
            require(source.read(8) != b"PFS2END\n", "summary_limit writer left a success tail")
    observation.update(
        writer_refusal={"kind": "summary_limit", "exit_code": observation["exit_code"],
                        "stdout": str(stdout), "stderr": str(stderr), "no_valid_tail": True,
                        "receipt_complete": receipt_complete, "run_failure_code": failure_code},
        invalid_sink_path=str(sink), invalid_sink_bytes=sink.stat().st_size if sink.is_file() else 0)
    return True


def measure_size_grid(args, rows, output):
    real = production_summary(rows)
    print("\n".join(real_summary_lines(real) + binding_summary_lines(binding_summary(real))), flush=True)
    seed = next((r for r in rows if r["ok"] and r["kind"] == "backtest" and r["arm"] == "v1" and r["warmup"]), None)
    require(seed is not None, "size grid needs a verified v1 warm-up and captured metadata")
    root = output / "size-grid"
    root.mkdir()
    reader = load_module(args.v1_reader, "_grid_v1")
    with reader.SinkReader(seed["sink_path"]) as source:
        header = source.header
    dump_json(root / "header.json", header)
    metadata = Path(seed["artifact_dir"]) / "summary-meta.json"
    results = []
    with (output / "size-grid.jsonl").open("w", encoding="utf-8") as raw:
        for case in size_grid_cases():
            case = dict(case)
            if case["shape"] == "incompressible-summary" and getattr(args, "incompressible_summary_json", None):
                case["summary_fixture_path"] = str(args.incompressible_summary_json)
            if case["shape"] == "ceiling-raw-summary" and getattr(args, "ceiling_incompressible_summary_json", None):
                case["summary_fixture_path"] = str(args.ceiling_incompressible_summary_json)
            directory = root / case["id"]
            directory.mkdir()
            definition = directory / "case.json"
            dump_json(definition, case)
            v1, v2, summary_path = directory / "sink.jsonl", directory / "sink.pfs2", directory / "summary.json"
            result = {"case": case, "ok": False, "model": case_model(case),
                      "v2_mode": getattr(args, "v2_mode", "serial"),
                      "v2_late_verify": resolved_late_verify(args)}
            if case["shape"] in ("block-boundary", "outputs", "large-summary"):
                result["model_scope"] = "default E/T/I model; custom caps/outputs/metadata are additional measured costs"
            try:
                require(case["shape"] not in ("incompressible-summary", "ceiling-raw-summary")
                        or case.get("summary_fixture_path"),
                        "PENDING: supply the 1MiB/ceiling caller JSON fixture; a raw-block frame is not evidence")
                for arm, sink in (("v1", v1), ("v2", v2)):
                    stdout, stderr = directory / f"{arm}.stdout", directory / f"{arm}.stderr"
                    paths, env = run_storage(directory, sink, stdout)
                    if args.codec.name == "sink_v2_split_driver.py":
                        env["PF_SINK_V2_TRANSCODER"] = str(Path(seed["build"]) / "build/bin/pf_sink_transcode_v2")
                    if arm == "v1":
                        paths["metadata_output"] = summary_path
                    if arm == "v1":
                        command = [sys.executable, "-B", str(Path(__file__).resolve()), "_size_seed",
                                   str(root / "header.json"), str(metadata), str(definition), str(v1), str(summary_path)]
                    else:
                        command = [sys.executable, "-B", str(args.codec), "from-v1", str(v1), str(v2),
                                   "--summary-json", str(summary_path), "--level", str(args.size_grid_level)]
                        if case.get("block_records"):
                            command += ["--block-records", case["block_records"]]
                        if getattr(args, "v2_mode", "serial") == "tee":
                            command += ["--tee", "--tee-late-verify", resolved_late_verify(args)]
                    observation = run_child(command, stdout, stderr, paths, env)
                    result[arm] = observation
                    if arm == "v2" and capture_writer_refusal(observation, stdout, stderr, sink):
                        observation.update(counts=result["v1"]["counts"],
                                           actual_bars=case.get("actual_bars", case["N"]),
                                           anchor_count=case.get("anchor_count", 0),
                                           summary_decoded_bytes=summary_path.stat().st_size)
                        result["writer_refusal"] = observation["writer_refusal"]
                        result["error"] = "REFUSED(summary_limit); invalid file retained, no size verdict"
                        break
                    require(observation["exit_code"] == 0, f"size {arm} child failed; see {stderr}")
                    if arm == "v2" and getattr(args, "v2_compare_late_modes", False) and case["N"] >= 100000:
                        observation.update(sink_sha256=sha256_file(v2), sink_bytes=v2.stat().st_size)
                        result["late_mode_comparison"] = compare_late_modes(
                            args, v1, summary_path, directory, seed["build"],
                            {"size_grid": case["id"], "build": seed["build"], "bars": case["N"],
                             "strategy": seed["strategy"]},
                            args.size_grid_level, case.get("block_records"), timed_outputs=[{
                                "mode": "timed-" + resolved_late_verify(args),
                                "late_verify": resolved_late_verify(args), "path": str(v2),
                                "sha256": observation["sink_sha256"], "bytes": observation["sink_bytes"],
                                "retained": None,
                            }])
                    if arm == "v1":
                        observation.update(read_v1(v1, reader))
                    for role in ("scratch", "temporary"):
                        paths[role].rmdir()
                if result.get("writer_refusal"):
                    raise SummaryLimit(result["error"])
                exported = directory / "export.jsonl"
                result["verification"] = run_child(
                    [sys.executable, "-B", str(args.codec), "to-v1", str(v2), str(exported)],
                    directory / "verify.stdout", directory / "verify.stderr")
                require(result["verification"]["exit_code"] == 0, "size-grid reference verification failed")
                verified = read_v1(exported, reader)
                result["v2"].update(v2_accounting(v2, getattr(args, "zstd_library", None)))
                require(result["v2"]["profile"] == result["v2"]["canonical_profile"],
                        "size encoder did not choose canonical profile")
                require(all(verified[k] == result["v1"][k] for k in ("counts", "record_sha256", "merged_sha256"))
                        and result["v2"]["counts"] == verified["counts"], "size-grid records differ")
                require(object_digest(result["v2"]["sink_folds"]) == object_digest(verified["sink_folds"]),
                        "size-grid SUMMARY trade folds differ")
                provenance = {
                    "kind": "synthetic population using captured production metadata",
                    "source_run": {k: seed.get(k) for k in ("sequence", "build", "bars", "strategy", "run", "warmup")},
                    "source_metadata_path": str(metadata), "source_metadata_sha256": sha256_file(metadata),
                    "metadata_path": str(summary_path), "metadata_sha256": sha256_file(summary_path),
                    "population_changes": case,
                }
                if case.get("summary_fixture_path"):
                    provenance["kind"] = "caller-supplied JSON, unchanged"
                    provenance["header_source_run"] = provenance.pop("source_run")
                    provenance["caller_json_path"] = case["summary_fixture_path"]
                    provenance["caller_json_sha256"] = sha256_file(Path(case["summary_fixture_path"]))
                    provenance["source_run"] = {"caller_fixture": case["summary_fixture_path"],
                                                "sha256": provenance["caller_json_sha256"]}
                bind_summary_evidence(result["v2"], args, "reference", sha256_file(args.codec), provenance,
                                      load_json(summary_path))
                result["v2"].update(actual_bars=case.get("actual_bars", case["N"]),
                                    anchor_count=case.get("anchor_count", 0),
                                    fraction=result["v1"]["fraction"], fraction_scope=result["v1"]["fraction_scope"])
                if case["shape"] in ("incompressible-summary", "ceiling-raw-summary"):
                    result["v2"]["requires_incompressible_evidence"] = True
                for arm in ("v1", "v2"):
                    info = result[arm]
                    for tag, name in (("E", "bytes_per_equity"), ("T", "bytes_per_trade")):
                        info[name] = info["type_bytes"][tag] / info["counts"][tag] if info["counts"][tag] else None
                result["v2_le_v1"] = result["v2"]["sink_bytes"] <= result["v1"]["sink_bytes"]
                result["ok"] = True
            except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
                result["error"] = f"{type(error).__name__}: {error}"
                if isinstance(error, LateModeMismatch):
                    result["refusal_kind"] = error.kind
                    result["late_mode_comparison"] = error.evidence
            result["measured"] = measured_size_cell(
                case["id"], result.get("v2"), result.get("v1"), result["model"],
                None if result["ok"] else result["error"])
            raw.write(json.dumps(result, separators=(",", ":"), allow_nan=False) + "\n")
            raw.flush()
            results.append(result)
            print(f"size {case['id']}: {result['measured']['clause'] or 'PENDING'} "
                  f"{result['measured']['verdict']}; encoding "
                  f"{'ok' if result['ok'] else result['error']}", flush=True)
            if (not result["ok"] and not result.get("writer_refusal")
                    and case["shape"] not in ("incompressible-summary", "ceiling-raw-summary")):
                break
            if (result["ok"] and result["measured"]["verdict"] not in ("FAIL", "PENDING")
                    and not result["measured"]["coverage_gaps"] and not args.keep_artifacts):
                for path in (v1, v2, exported):
                    path.unlink()
    return results


def summary_evidence_gaps(row):
    """A missing or unmatched Q/M probe cannot provide an estimated deduction."""
    gaps = []
    decoded, stored = row.get("summary_decoded_bytes"), row.get("summary_stored_bytes")
    valid = (type(decoded) is int and 1 <= decoded <= S_MAX
             and type(stored) is int and 1 <= stored <= SUMMARY_STORED_CAP)
    if (not valid or not row.get("summary_zstd_frame") or not row.get("summary_schema_validated")
            or (valid and (row.get("summary_ratio") != stored / decoded
                           or row.get("summary_expansion_bytes") != stored - decoded
                           or row.get("summary_bytes") != SUMMARY_HEADER + stored))):
        gaps.append("missing verified always-zstd SUMMARY J/C/schema")
    if type(row.get("level")) is not int or not 1 <= row["level"] <= 19 or not row.get("zstd_build"):
        gaps.append("missing HEAD level/producer zstd build")
    if not row.get("metadata_provenance"):
        gaps.append("missing JSON source-run provenance")
    probe = row.get("summary_probe_stored_bytes")
    if (type(probe) is not int or probe <= 0 or row.get("summary_probe_level") != row.get("level")
            or not row.get("summary_probe_zstd_build") or not row.get("summary_probe_matches_producer")):
        gaps.append("missing compression probes at HEAD level with the producer's zstd build")
    if (not all(type(row.get(k)) is int and 2 <= row[k] <= S_MAX for k in ("J_Q", "J_M"))
            or type(row.get("C_Q")) is not int or row["C_Q"] <= 0
            or type(row.get("C_M")) is not int or row["C_M"] < 0
            or type(row.get("summary_m_members")) is not int or row["summary_m_members"] < 0
            or (type(row.get("C_M")) is int and type(row.get("summary_m_members")) is int
                and (row["C_M"] == 0) != (row["summary_m_members"] == 0))
            or type(row.get("conservatively_counted_members")) is not list):
        gaps.append("missing token-preserving SUMMARY Q/M projection probes")
    return gaps


def production_summary(rows):
    """Only a verified, artifact-bound writer run supplies REAL-first evidence."""
    for row in rows:
        provenance = row.get("metadata_provenance") or {}
        if (not row.get("ok") or row.get("kind") != "backtest"
                or not row.get("arm", "").startswith("v2")
                or provenance.get("kind") != "same-run production JSON"
                or not row.get("run_id") or not provenance.get("run") or not provenance.get("mode")
                or not provenance.get("metadata_path") or not is_digest(provenance.get("metadata_sha256"))
                or not provenance.get("artifact_path") or not is_digest(provenance.get("artifact_sha256"))
                or summary_evidence_gaps(row)):
            continue
        decoded, stored = row["summary_decoded_bytes"], row["summary_stored_bytes"]
        c_meta = min(row["C_M"], max(0, stored - row["C_Q"]))
        return {
            "status": "REAL-PRODUCTION-SUMMARY", "J": decoded, "C": stored, "C/J": stored / decoded,
            "run_id": row["run_id"], "provenance": provenance,
            "HEAD_level": row["level"], "zstd_build": row["zstd_build"],
            "probe_zstd_build": row["summary_probe_zstd_build"],
            "J_Q": row["J_Q"], "C_Q": row["C_Q"], "J_M": row["J_M"], "C_M": row["C_M"],
            "C_meta": c_meta, "C_comparable": stored - c_meta, "C_comparable/J": (stored - c_meta) / decoded,
            "metadata_exclusion": "EXCLUDED_CALLER_METADATA" if c_meta else "NONE",
            "conservatively_counted_members": row["conservatively_counted_members"],
        }
    return {"status": "PENDING-REAL-SUMMARY",
            "reason": "no verified production writer SUMMARY with provenance and matching Q/M probes"}


def real_summary_lines(evidence):
    if evidence["status"] != "REAL-PRODUCTION-SUMMARY":
        return ["PENDING-REAL-SUMMARY: " + evidence["reason"],
                "Every model-derived verdict below is MODEL-ONLY; real SUMMARY evidence is required."]
    return [
        f"REAL-PRODUCTION-SUMMARY J={evidence['J']} C={evidence['C']} C/J={evidence['C/J']:.12g} "
        f"run_id={evidence['run_id']}",
        f"  provenance={json.dumps(evidence['provenance'], separators=(',', ':'))} "
        f"HEAD_level={evidence['HEAD_level']} zstd_build={json.dumps(evidence['zstd_build'], separators=(',', ':'))} "
        f"probe_build={json.dumps(evidence['probe_zstd_build'], separators=(',', ':'))}",
        f"  J_Q={evidence['J_Q']} C_Q={evidence['C_Q']} J_M={evidence['J_M']} C_M={evidence['C_M']} "
        f"{evidence['metadata_exclusion']} C_meta={evidence['C_meta']} C_comparable={evidence['C_comparable']} "
        f"C_comparable/J={evidence['C_comparable/J']:.12g} "
        f"conservatively_counted={json.dumps(evidence['conservatively_counted_members'])}",
    ]


def model_summary_break_even(model, real):
    """Plug the FIRST real counted C into a model base; never infer a measurement."""
    maximum = model["summary_max_comparable_C"]
    result = {
        "source": "MODEL-ONLY", "limit_bytes": maximum,
        "limit_term_bytes": model["summary_max_comparable_C_terms"],
        "status": "PENDING-REAL-SUMMARY", "verdict": "PENDING-REAL-SUMMARY",
    }
    if real["status"] != "REAL-PRODUCTION-SUMMARY":
        return result
    checked = compare_size(model["v1_bytes"], model["v2_base_formula_bytes"] + real["C"],
                           model["counts"], model["actual_bars"], real["J"], real["C"],
                           real["C_Q"], real["C_M"])
    expected = "PASS" if real["C_comparable"] <= maximum else "FAIL"
    require(checked["verdict"] == expected
            and checked["COMPARABLE_V2"] == model["v2_base_term_bytes"] + real["C_comparable"],
            "MODEL-ONLY real SUMMARY break-even arithmetic disagreement")
    result.update(
        status=expected, J_real=real["J"], C_real=real["C"], C_meta_real=real["C_meta"],
        counted_frame_bytes=real["C_comparable"], counted_frame_ratio=real["C_comparable/J"],
        limit_ratio=maximum / real["J"], COMPARABLE_V2=checked["COMPARABLE_V2"],
        headroom_bytes=maximum - real["C_comparable"],
        verdict="MODEL-ONLY " + ("COUNTERFACTUAL " if model["verdict"] == "REFUSED" else "") + expected)
    return result


def binding_summary(real):
    """Spec 0.5/15.1: two f=.25 checks FIRST, with f=.5/1 sensitivities."""
    cells, other_fractions = [], []
    for fraction in GRID_FRACTIONS:
        for equity, trades in ((1, 1), (64, 4)):
            model = size_model(equity, trades, fraction)
            cell = {
                "cell": f"E{equity}/T{trades}", "source": "MODEL-ONLY", "fraction": fraction,
                "E": equity, "T": trades, "B": equity, "anchor_count": 0, "profile": model["profile"],
                "clause": model["clause"], "small_run_K": SMALL_RUN_K,
                "v1_bytes": model["v1_bytes"], "v1_term_bytes": model["v1_term_bytes"],
                "model_full_v2_bytes": model["v2_bytes"],
                "B0": model["v2_base_formula_bytes"], "B0_terms": model["v2_base_term_bytes"],
                "comparison_limit_bytes": model["comparison_limit_bytes"],
                "comparison_limit_term_bytes": model["comparison_limit_term_bytes"],
                "L": model["summary_max_comparable_C"], "L_terms": model["summary_max_comparable_C_terms"],
                **model_summary_break_even(model, real),
            }
            (cells if fraction == 0.25 else other_fractions).append(cell)
    status = ("FAIL" if any(cell["status"] == "FAIL" for cell in cells) else
              "PENDING-REAL-SUMMARY" if real["status"] != "REAL-PRODUCTION-SUMMARY" else "PASS")
    return {"source": "MODEL-ONLY", "status": status, "fixed_K": SMALL_RUN_K,
            "cells": cells, "other_fractions": other_fractions,
            "scope": "FIRST real counted SUMMARY; model payload bases, not measured whole files"}


def binding_summary_lines(binding):
    lines = []
    for label, cells in (("BINDING", binding["cells"]), ("BREAK-EVEN", binding["other_fractions"])):
        for cell in cells:
            real = ("PENDING-REAL-SUMMARY" if cell["status"] == "PENDING-REAL-SUMMARY" else
                    f"J_real={cell['J_real']} C_real={cell['C_real']} C_meta_real={cell['C_meta_real']} "
                    f"counted_C={cell['counted_frame_bytes']} "
                    f"COMPARABLE_V2={cell['COMPARABLE_V2']} headroom_B={cell['headroom_bytes']} "
                    f"counted_ratio={cell['counted_frame_ratio']:.12g} limit_ratio={cell['L']}/{cell['J_real']}")
            lines.append(
                f"{label} {cell['cell']} f={cell['fraction']} {cell['clause']} MODEL-ONLY "
                f"v1_B={cell['v1_bytes']} v1_terms_B={cell['v1_term_bytes']} "
                f"B0={cell['B0']} B0_terms={cell['B0_terms']} "
                f"integer_limit={cell['comparison_limit_bytes']} limit_terms={cell['comparison_limit_term_bytes']} "
                f"L={cell['L']} L_terms={cell['L_terms']} fixed_K={binding['fixed_K']} "
                f"{real} {cell['verdict']}")
    lines.append(f"BINDING verdict={binding['status']} (MODEL-ONLY payload bases; either f=.25 failure means FAIL)")
    return lines


def measured_size_cell(cell, row, base, model=None, error=None):
    """Per-file evidence. No median, model value or absent reading can supply a PASS."""
    row, base = row or {}, base or {}
    refusal = row.get("writer_refusal") or {}
    counts = row.get("counts", base.get("counts", {}) if refusal else {})
    equity, trades = counts.get("E"), counts.get("T")
    bars = row.get("actual_bars", base.get("actual_bars") if refusal else None)
    result = {
        "cell": cell, "source": "MEASURED", "N": equity, "raw_E": equity,
        "actual_bars": bars, "anchor_count": row.get("anchor_count"), "T": trades,
        "counts": {tag: counts.get(tag) for tag in TAGS},
        "fraction": row.get("fraction"), "fraction_scope": row.get("fraction_scope"),
        "D": row.get("descriptor_count"), "profile": row.get("profile"),
        "v1_bytes": base.get("sink_bytes"), "v2_bytes": row.get("sink_bytes"), "model": model,
        "v1_wall_seconds": base.get("wall_seconds"), "v2_wall_seconds": row.get("wall_seconds"),
        "verdict": "PENDING", "clause": None, "diagnostic_verdict": "PENDING",
        "v2_file_bytes": row.get("sink_bytes"), "v2_minus_v1_bytes": None,
        "C_meta": None, "C_comparable": None, "COMPARABLE_V2": None, "metadata_exclusion": "PENDING",
        "summary_comparable_ratio": None, "comparable_minus_v1_bytes": None, "comparison_limit_bytes": None,
        "summary_max_comparable_C": None, "summary_max_comparable_ratio": None, "summary_headroom_bytes": None,
        "empty_full_file_limit_bytes": None, "empty_full_file_verdict": None,
        "refusal_kind": None, "writer_refusal": refusal or None,
        "invalid_sink_path": row.get("invalid_sink_path"), "invalid_sink_bytes": row.get("invalid_sink_bytes"),
        "small_run_K": SMALL_RUN_K, "small_run_K_source": "FIXED spec 0.5",
    }
    for key in ("summary_decoded_bytes", "summary_stored_bytes", "summary_ratio", "summary_expansion_bytes",
                "summary_bytes", "summary_json_sha256", "summary_probe_stored_bytes", "summary_probe_level",
                "summary_probe_zstd_build", "summary_probe_matches_producer", "summary_zstd_frame",
                "summary_schema_validated", "metadata_provenance", "zstd_build", "level", "run_id",
                "J_Q", "C_Q", "J_M", "C_M", "summary_m_members", "conservatively_counted_members",
                "summary_q_sha256", "summary_m_sha256"):
        result[key] = row.get(key)
    if (refusal.get("kind") == "summary_limit" and refusal.get("no_valid_tail") is True
            and type(refusal.get("exit_code")) is int and refusal["exit_code"] != 0):
        # No valid file, stored frame, probes or size inequality can be inferred.
        result.update(verdict="REFUSED", diagnostic_verdict="REFUSED", refusal_kind="summary_limit",
                      v2_bytes=None, v2_file_bytes=None, evidence_gaps=[], coverage_gaps=[])
        if type(equity) is int and equity >= 0 and type(trades) is int and trades >= 0:
            result["clause"] = size_clause(counts, bars)
            if type(result["v1_bytes"]) is int:
                result["comparison_limit_bytes"] = result["v1_bytes"] + (
                    result["small_run_K"] if result["clause"] == "(ii-a)" else -1)
        return result
    gaps = [error] if error else []
    gaps.extend(summary_evidence_gaps(row))
    valid_counts = all(type(value) is int and value >= 0 for value in (*result["counts"].values(), bars))
    if not valid_counts:
        gaps.append("missing actual bars/raw E/T/O/V/K/X/H counts")
    if any(type(base.get("counts", {}).get(tag)) is not int
           or base["counts"][tag] != counts.get(tag) for tag in TAGS):
        gaps.append("paired v1/v2 raw stream counts differ or are missing")
    if not all(type(value) is int and value > 0 for value in (result["v1_bytes"], result["v2_bytes"])):
        gaps.append("missing verified paired file bytes")
    decoded, stored = result["summary_decoded_bytes"], result["summary_stored_bytes"]
    valid_summary = (type(decoded) is int and 1 <= decoded <= S_MAX
                     and type(stored) is int and 1 <= stored <= SUMMARY_STORED_CAP)
    if valid_summary and type(result["v2_bytes"]) is int and result["summary_bytes"] is not None:
        if result["v2_bytes"] < SUMMARY_HEADER + stored:
            gaps.append("SUMMARY header/frame exceeds whole-file bytes")
    if any(type(result[key]) not in (int, float) or not math.isfinite(result[key]) or result[key] <= 0
           for key in ("v1_wall_seconds", "v2_wall_seconds")):
        gaps.append("missing paired child wall measurements")
    if type(row.get("anchor_count")) is not int or row["anchor_count"] not in (0, 1):
        gaps.append("missing anchor count")
    fraction = row.get("fraction")
    if equity and (type(fraction) not in (int, float) or not math.isfinite(fraction) or not 0 <= fraction <= 1):
        gaps.append("missing measured f equivalent")
    if (row.get("profile") not in ("COMPACT", "FULL") or type(row.get("descriptor_count")) is not int
            or (type(equity) is int and type(trades) is int and type(row.get("descriptor_count")) is int
                and row["profile"] != canonical_profile(equity, trades, row["descriptor_count"]))):
        gaps.append("missing/noncanonical profile/D")
    if (type(equity) is int and type(row.get("anchor_count")) is int
            and row["anchor_count"] > equity):
        gaps.append("anchor count exceeds raw E")
    probe = row.get("summary_probe_stored_bytes")
    incompressible = bool(not gaps and valid_summary and stored >= decoded and probe >= decoded)
    coverage_gaps = []
    if row.get("requires_incompressible_evidence") and not incompressible:
        coverage_gaps.append("required caller fixture has not demonstrated incompressibility")
    if valid_counts and all(type(result[key]) is int for key in ("v1_bytes", "v2_bytes")):
        result.update(compare_size(result["v1_bytes"], result["v2_bytes"], counts, bars,
                                   decoded if valid_summary else None, stored if valid_summary else None,
                                   row.get("C_Q") if not gaps else None, row.get("C_M") if not gaps else None))
    if gaps:
        result["verdict"] = "PENDING"
    result["evidence_gaps"] = gaps
    result["coverage_gaps"] = coverage_gaps
    return result


def update_size_verdict(summary, rows, grid=(), expected_cases=None):
    real = production_summary(rows)
    binding = binding_summary(real)
    summary["small_run_allowance"] = {"K": SMALL_RUN_K, "source": "FIXED spec 0.5"}
    baseline = {(r["build"], r["bars"], r["strategy"], r["run"], r["warmup"]): r
                for r in rows if r.get("ok") and r["kind"] == "backtest" and r["arm"] == "v1"}
    comparisons = []
    for row in rows:
        if row.get("arm", "").startswith("v2"):
            base = baseline.get((row["build"], row["bars"], row["strategy"], row["run"], row["warmup"]))
            counts, fraction = row.get("counts", {}), row.get("fraction")
            if counts.get("E") == 0:
                fraction = 0.25  # No E payload; f has no effect on the empty model.
            model = (size_model(counts["E"], counts["T"], fraction,
                                actual_bars=row["actual_bars"], anchor_count=row["anchor_count"])
                     if all(type(counts.get(tag)) is int and counts[tag] >= 0 for tag in ("E", "T"))
                     and type(row.get("actual_bars")) is int and row["actual_bars"] >= 0
                     and type(row.get("anchor_count")) is int and 0 <= row["anchor_count"] <= min(1, counts["E"])
                     and type(fraction) in (int, float) and math.isfinite(fraction) and 0 <= fraction <= 1 else None)
            comparisons.append(measured_size_cell(
                f"run-{row['sequence']:06d}/{row['arm']}/{row['kind']}", row, base, model,
                None if row.get("ok") and base else row.get("error", "missing verified v1/v2 pairing")))
    expected = size_grid_cases() if expected_cases is None else expected_cases
    observed = {result["case"]["id"]: result for result in grid}
    complete_grid = [observed.pop(case["id"], {"case": case, "ok": False, "model": case_model(case),
                                              "error": "measurement not attempted"})
                     for case in expected]
    complete_grid += list(observed.values())
    for result in complete_grid:
        measured = measured_size_cell(result["case"]["id"], result.get("v2"), result.get("v1"),
                                      result["model"], None if result.get("ok") else result.get("error", "unverified"))
        measured["requested_fraction"] = result["case"]["fraction"]
        result["measured"] = measured
        comparisons.append(measured)
    summary["size_grid"] = complete_grid
    summary["size_comparisons"] = comparisons
    for cell in comparisons:
        model = cell["model"]
        if model is None:
            continue
        model["real_summary_break_even"] = model_summary_break_even(model, real)
    measured_bytes = [r for r in comparisons if r["verdict"] != "REFUSED"
                      and type(r["v1_bytes"]) is int and type(r["v2_bytes"]) is int]
    summary["size_refusals"] = [r for r in comparisons if r["verdict"] == "REFUSED"]
    summary["size_files_compared"] = len(measured_bytes)
    summary["size_expansions"] = [r for r in measured_bytes if r["v2_bytes"] > r["v1_bytes"]]
    summary["v2_le_v1_at_every_measured_size"] = (
        not summary["size_expansions"] if measured_bytes else None)
    summary["size_failures"] = [r for r in comparisons if r["verdict"] == "FAIL"
                                and r["clause"] in ("(i)", "(ii-a)")]
    summary["size_counterexamples"] = summary["size_failures"]
    summary["size_pending"] = [r["cell"] for r in comparisons if r["verdict"] == "PENDING" or r["coverage_gaps"]]
    summary["measured_size_verdict"] = ("FAIL" if summary["size_failures"] else
                                        "PENDING" if summary["size_pending"] or not measured_bytes
                                        or real["status"] != "REAL-PRODUCTION-SUMMARY" else
                                        "REFUSED" if summary["size_refusals"] else "PASS")
    models = [r["model"] for r in complete_grid]
    summary["model_size_refusals"] = [r["case"]["id"] for r in complete_grid if r["model"]["verdict"] == "REFUSED"]
    admitted_models = [m for m in models if m["verdict"] != "REFUSED"]
    model_status = ("PENDING" if not admitted_models else
                    "FAIL" if any(m["verdict"] == "FAIL" for m in admitted_models) else "PASS")
    summary["model_size_scope"] = "admitted models only; REFUSED candidates listed separately, never counted PASS/FAIL"
    summary["model_size_verdict"] = "MODEL-ONLY " + model_status
    verdicts = (model_status, summary["measured_size_verdict"],
                "PENDING" if binding["status"] == "PENDING-REAL-SUMMARY" else binding["status"])
    summary["size_acceptance"] = ("FAIL" if "FAIL" in verdicts else "PENDING" if "PENDING" in verdicts else
                                  "REFUSED" if "REFUSED" in verdicts else "PASS")
    problems = [f"measured size {r['cell']} FAIL{r['clause']}: v1={r['v1_bytes']} "
                f"full_v2={r['v2_bytes']} C_meta={r['C_meta']} COMPARABLE_V2={r['COMPARABLE_V2']}"
                for r in summary["size_failures"]]
    if summary["size_pending"]:
        problems.append(f"PENDING size evidence for {len(summary['size_pending'])} files/cells")
    if summary["size_refusals"]:
        problems.append(f"REFUSED SUMMARY admission for {len(summary['size_refusals'])} runs/cells; "
                        "no accepted-file size verdict for these runs")
    for cell in binding["cells"]:
        if cell["status"] == "FAIL":
            problems.append(f"MODEL-ONLY binding {cell['cell']}/f=.25 FAIL{cell['clause']}: "
                            f"real counted SUMMARY={cell['counted_frame_bytes']} > L={cell['L']}; "
                            f"K fixed at {SMALL_RUN_K}")
    if real["status"] != "REAL-PRODUCTION-SUMMARY":
        problems.append("PENDING-REAL-SUMMARY: production SUMMARY and Q/M probes are required")
    if len(observed) or len(grid) != len({r["case"]["id"] for r in grid}):
        problems.append("unexpected/duplicate size-grid cells")
    previous = summary.get("size_problems", [])
    summary["problems"] = [p for p in summary["problems"] if p not in previous] + problems
    summary["size_problems"] = problems
    summary["gate"] = ("PASS" if not summary["problems"] and summary["size_acceptance"] == "PASS"
                        and summary.get("speed_pass") is True and summary.get("memory_pass") is True else "FAIL")
    # Insertion order is the report contract for summary.json as well as text.
    ordered = {"real_summary": real, "binding_summary": binding}
    ordered.update((key, value) for key, value in summary.items() if key not in ordered)
    summary.clear()
    summary.update(ordered)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "stage-a":
        return stage_a_probe(argv[1:])
    if argv and argv[0] == "_worker":
        require(len(argv) >= 4, "worker arguments missing")
        return worker(Path(argv[1]), argv[2] == "off", argv[3:])
    if argv and argv[0] == "_size_seed":
        require(len(argv) == 6, "size seed arguments missing")
        write_size_seed(Path(argv[1]), Path(argv[2]), load_json(Path(argv[3])), Path(argv[4]), Path(argv[5]))
        return 0
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Tee defaults to structure with 4 late threads. A one-strategy 1M-bar probe on four "
               "clean builds measured wall/v1-STDOUT: structure/4=1.073x, structure/1=1.118x, "
               "full=1.337x (about 1.34x). The 1.10 limit held at the default 4; a 1-core host "
               "misses it. Probe measurements only; replace with the gate box's numbers.")
    parser.add_argument("--build-roots", nargs="+", type=Path, required=True)
    parser.add_argument("--sizes", nargs="+", type=int, default=[10000, 1000000, 8000000])
    parser.add_argument("--strategies", nargs="+", help="corpus-relative names; default: subset trade-count extrema/median")
    parser.add_argument("--corpus", type=Path, default=ROOT / "corpus")
    parser.add_argument("--subset", type=Path, default=ROOT / "scripts/corpus_parity_subset.txt")
    parser.add_argument("--strategy-config", type=Path, help="per-strategy module/ohlcv/driver_args JSON map")
    parser.add_argument("--ohlcv", type=Path, required=True, help="CSV with at least LARGE bars, sliced before timing")
    parser.add_argument("--replicates", type=int, default=5, help="timed runs per build/arm/cell, minimum 5")
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--levels", nargs="+", type=int, default=list(LEVELS))
    parser.add_argument("--output", type=Path, required=True, help="new output directory, refuses reuse")
    parser.add_argument("--driver", type=Path, default=ROOT / "scripts/sink_v2_split_driver.py")
    parser.add_argument("--codec", type=Path, default=ROOT / "scripts/sink_v2_split_driver.py")
    parser.add_argument("--v1-reader", type=Path, default=ROOT / "docker/pf_sink.py")
    parser.add_argument("--v1-a-mode", choices=("file", "stdout"), default="file",
                        help="v1 stage A: FILE verifier (default) or stdin -> /dev/null")
    parser.add_argument("--v2-mode", choices=("serial", "tee"), default="serial",
                        help="v2 only: serial oracle or concurrent relay/A/C with owner publication")
    parser.add_argument("--phase-log", action="store_true",
                        help="opt-in driver/tool phase evidence in each run's phases.json")
    parser.add_argument("--v2-late-verify", choices=("full", "structure"), default=None,
                        help="unset resolves to structure for tee, full for serial; explicit structure requires tee")
    parser.add_argument("--v2-compare-late-modes", action="store_true",
                        help="tee only: replay each from-v1 seed untimed as full, structure/4, structure/1 "
                             "and serial/full; refuse any SHA-256 or length difference, retaining outputs")
    parser.add_argument("--library-relative", default="build/lib/libpineforge.a")
    parser.add_argument("--bootstrap-iterations", type=int, default=2000)
    parser.add_argument("--seed", type=int, default=SEED)
    parser.add_argument("--memory-evidence", type=Path, help="artifact-bound L0b live-after-feed memory proof")
    parser.add_argument("--keep-artifacts", action="store_true", help="retain every sink/export/stdout (large disk cost)")
    parser.add_argument("--skip-size-grid", action="store_true", help="exploratory only: omit required 15.1 size evidence")
    parser.add_argument("--size-grid-level", type=int, default=3, choices=LEVELS)
    parser.add_argument("--zstd-library", type=Path, help="same libzstd shared library used by the measured producers")
    parser.add_argument("--zstd-build-info", type=Path, help="artifact-bound producer build/log map; absent entries stay PENDING")
    parser.add_argument("--incompressible-summary-json", type=Path,
                        help="schema-valid 1 MiB caller JSON for N100/T1, tested rather than assumed incompressible")
    parser.add_argument("--ceiling-incompressible-summary-json", type=Path,
                        help="schema-valid J=S_MAX (1 MiB) caller JSON for N100/T1; raw fallback fits admission")
    args = parser.parse_args(argv)
    output_created = False
    try:
        args.v2_late_verify = resolved_late_verify(args)
        require(args.v2_late_verify == "full" or args.v2_mode == "tee",
                "--v2-late-verify structure requires --v2-mode tee")
        require(not args.v2_compare_late_modes or args.v2_mode == "tee",
                "--v2-compare-late-modes requires --v2-mode tee")
        require(not args.v2_compare_late_modes or args.size_grid_level in args.levels,
                f"--v2-compare-late-modes requires --size-grid-level {args.size_grid_level} in --levels")
        require(args.bootstrap_iterations >= 200, "bootstrap needs at least 200 iterations")
        roots = tuple(str(path.resolve()) for path in args.build_roots)
        # Refuse too few builds BEFORE trying to inspect modules or run a driver.
        require(len(roots) >= 4 and len(set(roots)) == len(roots), "at least 4 independent build roots required")
        configs = load_json(args.strategy_config) if args.strategy_config else {}
        require(type(configs) is dict, "strategy config must be an object")
        strategies = tuple(args.strategies or default_strategies(args.corpus, args.subset))
        plan = Plan(roots, tuple(args.sizes), strategies, args.replicates, args.warmups, tuple(args.levels))
        plan.validate()
        for cfg in configs.values():
            require(type(cfg) is dict, "each strategy config must be an object")
            check_driver_args(cfg.get("driver_args", []))
        for name in ("driver", "codec", "v1_reader"):
            path = getattr(args, name).resolve()
            require(path.is_file(), f"missing {name}: {path}")
            setattr(args, name, path)
        if args.zstd_library:
            args.zstd_library = args.zstd_library.resolve()
            require(args.zstd_library.is_file(), "missing --zstd-library")
        args.zstd_builds = load_json(args.zstd_build_info) if args.zstd_build_info else {}
        require(type(args.zstd_builds) is dict, "zstd build evidence must be a producer map")
        if args.incompressible_summary_json:
            args.incompressible_summary_json = args.incompressible_summary_json.resolve()
            require(args.incompressible_summary_json.is_file(), "missing caller SUMMARY JSON")
        if args.ceiling_incompressible_summary_json:
            args.ceiling_incompressible_summary_json = args.ceiling_incompressible_summary_json.resolve()
            require(args.ceiling_incompressible_summary_json.is_file(), "missing ceiling caller SUMMARY JSON")
        artifacts = inspect_builds(roots, strategies, configs, args.library_relative)
        split_proof = args.driver == ROOT / "scripts/sink_v2_split_driver.py" and args.codec == args.driver
        require(split_proof or (args.v1_a_mode == "file" and not args.phase_log and args.v2_mode == "serial"),
                "stdout/tee/phase instrumentation requires the real split driver and codec")
        if split_proof:
            inspect_split_proofs(roots, artifacts, tee=args.v2_mode == "tee")
            require(args.zstd_library is not None, "stage-C proof needs the pinned reference decoder")
            os.environ["PF_SINK_ZSTD_LIBRARY"] = str(args.zstd_library)
        evidence = load_json(args.memory_evidence) if args.memory_evidence else None
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        output_created = True
        manifest = {
            "plan": {"builds": roots, "sizes": plan.sizes, "strategies": strategies, "replicates": plan.replicates,
                     "warmups": plan.warmups, "levels": plan.levels, "v1_a_mode": args.v1_a_mode,
                     "v2_mode": args.v2_mode, "v2_late_verify": args.v2_late_verify,
                     "v2_compare_late_modes": args.v2_compare_late_modes},
            "artifacts": artifacts, "strategy_config": configs,
            "tools": {name: {"path": str(getattr(args, name)), "sha256": sha256_file(getattr(args, name))}
                      for name in ("driver", "codec", "v1_reader")},
            "python": sys.version, "platform": sys.platform, "uname": list(os.uname()),
            "ccache_disable": "1", "argv": argv,
            "storage_environment": {key: os.environ.get(key) for key in ("PF_SCRATCH_DIR", "TMPDIR")},
            "summary_observer_zstd": summary_codec(args.zstd_library).identity,
            "zstd_build_info": {
                "path": str(args.zstd_build_info) if args.zstd_build_info else None,
                "sha256": sha256_file(args.zstd_build_info) if args.zstd_build_info else None,
                "producers": args.zstd_builds,
            },
            "incompressible_summary_json": {
                "path": str(args.incompressible_summary_json) if args.incompressible_summary_json else None,
                "sha256": sha256_file(args.incompressible_summary_json) if args.incompressible_summary_json else None,
            },
            "ceiling_incompressible_summary_json": {
                "path": str(args.ceiling_incompressible_summary_json) if args.ceiling_incompressible_summary_json else None,
                "sha256": sha256_file(args.ceiling_incompressible_summary_json)
                          if args.ceiling_incompressible_summary_json else None,
            },
        }
        dump_json(output / "manifest.json", manifest)
        feeds = prepare_feeds(output, plan.sizes, strategies, configs, args.ohlcv)
        dump_json(output / "feeds.json", {f"{s}:{n}": {"path": str(p), "sha256": sha256_file(p)}
                                        for (s, n), p in feeds.items()})
        rows = experiment(args, plan, artifacts, configs, feeds, output)
        # Match experiment's immutable warm-up selection, including multiple
        # warm-ups: its last v1 warm-up supplies every subsequent encode.
        late_mode_seed_rows = {
            (row["build"], row["bars"], row["strategy"]): row for row in rows
            if row["kind"] == "backtest" and row["arm"] == "v1" and row["warmup"]
        }
        late_mode_seeds = (compare_late_mode_seeds(args, plan, late_mode_seed_rows, rows, output)
                           if args.v2_compare_late_modes else [])
        check_artifacts_unchanged(artifacts)
        if split_proof:
            inspect_split_proofs(roots, artifacts, tee=args.v2_mode == "tee")
        if evidence is not None:
            dump_json(output / "memory-evidence.json", evidence)
        summary = summarize(plan, rows, evidence, artifacts, args.bootstrap_iterations, args.seed)
        summary["v1_a_mode"] = args.v1_a_mode
        summary["v2_mode"] = args.v2_mode
        summary["v2_late_verify"] = args.v2_late_verify
        grid = []
        if (not args.skip_size_grid and not validate_rows(plan, rows)
                and all(seed["ok"] for seed in late_mode_seeds)):
            grid = measure_size_grid(args, rows, output)
        comparisons = late_mode_receipt(
            late_mode_seeds, grid, args.v2_compare_late_modes, len(late_mode_seed_rows),
            skip_size_grid=args.skip_size_grid)
        comparisons.update(v2_mode=args.v2_mode, v2_late_verify=args.v2_late_verify)
        summary["late_mode_seeds"] = late_mode_seeds
        summary["late_mode_comparison"] = comparisons
        dump_json(output / "late-modes-receipt.json", comparisons)
        summary["problems"].extend(seed["error"] for seed in late_mode_seeds if not seed["ok"])
        if args.v2_compare_late_modes and comparisons["equal"] is not True:
            summary["problems"].append("late-mode byte comparison missing or unequal; see late-modes-receipt.json")
        check_artifacts_unchanged(artifacts)
        if split_proof:
            inspect_split_proofs(roots, artifacts, tee=args.v2_mode == "tee")
        update_size_verdict(summary, rows, grid)
        if not split_proof:
            summary["problems"].append("custom/synthetic driver: not a stage-C production proof")
            summary["gate"] = "FAIL"
        if any(type(r.get(arm, {}).get("peak_rss_bytes")) is not int
               for r in grid for arm in ("v1", "v2")):
            summary["problems"].append("size-grid child peak RSS gap; no memory PASS")
            summary["memory_pass"] = False
            summary["gate"] = "FAIL"
        dump_json(output / "summary.json", summary)
        text = render_summary(summary)
        (output / "summary.txt").write_text(text, encoding="utf-8")
        print(text, end="")
        return 0 if summary["gate"] == "PASS" else 1
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        print(f"GATE FAIL: {type(error).__name__}: {error}", file=sys.stderr)
        # Preserve a machine-readable failed verdict for setup/analysis failures too.
        if output_created:
            dump_json(output / "failure.json", {"gate": "FAIL", "error": str(error)})
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
