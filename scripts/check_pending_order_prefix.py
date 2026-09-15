#!/usr/bin/env python3
"""Check the complete append-only intent row v1 prefix.

This is a source-only ABI control. It authenticates the frozen header closure
from 79921099, then compares every one of its 396 C fields with the current
mirror by type, order, offset, and size. It also proves that cancellation_v1
starts at the old 3120-byte boundary instead of inserting into the prefix.
No compiler, executable, strategy, feed, or reference engine is invoked.
"""
from __future__ import annotations

import gzip
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/script_cpp_abi/basev8"
COMMIT = "79921099a9357cb5bbace907a9319479f6640d89"
TREE = "e141657c572b4a3855dfee607f9951e331b961d6"

DECL = re.compile(
    r"^\s*(uint8_t|uint32_t|uint64_t|int32_t|int64_t|double|char)"
    r"\s+(\w+)(?:\[(\d+)\])?\s*;"
)


def _header_from_fixture(fixture: Path = FIXTURE,
                         commit: str = COMMIT,
                         tree: str = TREE) -> str:
    manifest = json.loads((fixture / "manifest.json").read_text())
    if manifest["source_commit"] != commit or manifest.get("source_tree") != tree:
        raise SystemExit(f"{fixture.name} fixture provenance changed")
    archive = (fixture / manifest["archive"]).read_bytes()
    if hashlib.sha256(archive).hexdigest() != manifest["archive_sha256"]:
        raise SystemExit(f"{fixture.name} fixture archive digest mismatch")
    contents = json.loads(gzip.decompress(archive))
    expected = manifest["files"]["pineforge/pending_order_mirror.hpp"]
    raw = contents["pineforge/pending_order_mirror.hpp"].encode()
    blob = b"blob " + str(len(raw)).encode() + b"\0" + raw
    if (hashlib.sha256(raw).hexdigest() != expected["sha256"]
            or hashlib.sha1(blob).hexdigest() != expected["git_blob"]):
        raise SystemExit(f"{fixture.name} pending-order fixture digest mismatch")
    return raw.decode()


def _struct_fields(text: str) -> list[tuple[str, str, int, int]]:
    marker = "typedef struct pf_pending_order_v1_s {"
    if marker not in text:
        raise SystemExit("pending-order mirror struct marker missing")
    body = text.split(marker, 1)[1].split("} pf_pending_order_v1_t;", 1)[0]
    fields: list[tuple[str, str, int, int]] = []
    for line in body.splitlines():
        line = line.split("//", 1)[0]
        match = DECL.match(line)
        if not match:
            if line.strip():
                raise SystemExit("unclassified mirror declaration: " + line.strip())
            continue
        ctype, name, count = match.groups()
        if count:
            size, align = int(count), 1
        else:
            size = {"uint8_t": 1, "uint32_t": 4, "uint64_t": 8,
                    "int32_t": 4, "int64_t": 8, "double": 8}[ctype]
            align = size
        fields.append((name, ctype + (f"[{count}]" if count else ""), size, align))
    return fields


def _layout(fields: list[tuple[str, str, int, int]]) -> tuple[list[tuple[str, str, int, int]], int]:
    offset = 0
    laid_out = []
    for name, ctype, size, align in fields:
        offset = (offset + align - 1) // align * align
        laid_out.append((name, ctype, offset, size))
        offset += size
    total = (offset + 7) // 8 * 8
    return laid_out, total


def check() -> None:
    frozen = _layout(_struct_fields(_header_from_fixture()))
    current_text = (ROOT / "include/pineforge/pending_order_mirror.hpp").read_text()
    current = _layout(_struct_fields(current_text))
    old_fields, old_size = frozen
    current_fields, current_size = current
    if len(old_fields) != 396:
        raise SystemExit(f"basev8 field count changed: {len(old_fields)}")
    if len(current_fields) != 406:
        raise SystemExit(f"current field count changed: {len(current_fields)}")
    if old_size != 3120:
        raise SystemExit(f"basev8 size changed: {old_size}")
    if current_size != 3192:
        raise SystemExit(f"current size changed: {current_size}")
    if current_fields[:len(old_fields)] != old_fields:
        for index, (before, after) in enumerate(zip(old_fields, current_fields), 1):
            if before != after:
                raise SystemExit(f"prefix mismatch at field {index}: {before!r} != {after!r}")
        raise SystemExit("current mirror is missing a basev8 prefix field")
    if current_fields[len(old_fields)][0] != "cancellation_cause":
        raise SystemExit("cancellation fields are not appended after the basev8 prefix")
    if current_fields[len(old_fields)][2] != old_size:
        raise SystemExit("cancellation_v1 does not start at the basev8 size boundary")
    print("pending-order v1 prefix: 396 fields, type/order/offset/size exact; cancellation_v1 append begins at 3120")


if __name__ == "__main__":
    check()
