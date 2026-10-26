"""Generate independent reader fault controls, never stage-C release vectors.

Run on the supervisor's lab; this file is intentionally not executed by the
edit-only lane. --check refuses any missing/different artifact. A difference
from a hand column or input record is an error, never a reason to enlarge masks.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import pf_sink
import pf_sink2
import pf_sink2_reference as reference
from sink_v2_cases import (BUNDLE, cases, normalized, summary_frame_refusals, summary_raw_frame, summary_window_frame,
                           v1_file, v1_line, v1_report)


def write_artifacts(artifacts: dict[str, bytes], destination: Path, check: bool) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for name, content in artifacts.items():
        path = destination / name
        if check:
            if not path.is_file() or path.read_bytes() != content:
                raise ValueError(f"{path}: missing/different; generator comparison is a finding")
        else:
            path.write_bytes(content)


def generate(case, destination: Path, *, check: bool = False) -> None:
    if not case.report:
        case.summary, case.report, case.refusal = v1_report(case)
    expected = v1_file(case.head, case.rows)
    frames = io.BytesIO()
    output = io.BytesIO()
    receipt = reference.encode(case.head, case.rows, case.summary, output, block_records=case.caps,
                               compress=case.compress, frame_factory=case.frame_factory, frame_manifest=frames,
                               force_full=case.force_full, summary_frame_factory=case.summary_frame_factory)
    data = output.getvalue()
    # Validate with the independent reader BEFORE exposing any final artifact.
    with pf_sink2.open(data) as reader:
        reader.verify()
        actual_format = reader.head()["format"]
        summary_sizes = {"J": reader.summary_json_length, "C": reader.summary_stored_length,
                         "header_bytes": 52, "stored_region": reader.summary_length,
                         "frame": "one standard zstd frame",
                         "admission": "ACCEPTED", "bounds_spec": "3.7/9.1 round 10"}
        if case.summary_frame_factory is not None:
            canonical_c = len(reference.compress_summary(reference.metadata_bytes(case.summary), actual_format["level"]))
            summary_sizes.update(canonical_level_C=canonical_c,
                                 canonical_nonshrinking=canonical_c >= reader.summary_json_length,
                                 evidence="DECODE-ONLY RAW FRAME; not an incompressibility measurement")
        decoded = [[record.tag, *record.values] for record in reader.iter_merged()]
        if b"".join(map(v1_line, decoded)) != b"".join(map(v1_line, normalized(case.rows, case.head))):
            raise ValueError(f"{case.name}: decoded records differ from the input")
        converted = io.BytesIO()
        reader.to_v1(converted)
        if converted.getvalue() != expected:
            raise ValueError(f"{case.name}: to-v1 differs from independent input text")
        report_output = io.BytesIO()
        if case.refusal is None:
            report_identity = reader.to_report_json(report_output)
            if report_output.getvalue() != case.report:
                raise ValueError(f"{case.name}: to-report-json differs from the EXISTING v1 writer")
            if (report_identity["content_length"] != len(case.report) or
                    report_identity["report_sha256"] != hashlib.sha256(case.report).hexdigest()):
                raise ValueError(f"{case.name}: report length/SHA differs")
            outer = json.dumps({"result": case.report.decode("ascii")})
            if json.loads(outer)["result"].encode("ascii") != report_output.getvalue():
                raise ValueError(f"{case.name}: frozen result string differs")
        else:
            try:
                reader.to_report_json(report_output)
            except pf_sink2.ReportResultInvalid as error:
                if {"code": error.code, "args": error.code_args} != case.refusal:
                    raise ValueError(f"{case.name}: typed v1 report refusal differs") from error
                import run_json
                if run_json.failure_line(str(error), error.code, error.code_args).encode("ascii") != case.report:
                    raise ValueError(f"{case.name}: typed v1 failure line differs")
            else:
                raise ValueError(f"{case.name}: schema-only report conversion unexpectedly succeeded")
            if report_output.getvalue():
                raise ValueError(f"{case.name}: refusal emitted success bytes")
            report_identity = {"refusal": case.refusal}
    with tempfile.TemporaryDirectory() as scratch:
        v1_path = Path(scratch) / "input.jsonl"
        v1_path.write_bytes(expected)
        with pf_sink.SinkReader(v1_path) as reader:
            for _ in reader:
                pass
            if not reader.complete:
                raise ValueError(f"{case.name}: independent v1 receipt did not verify")
    # Hand-column check runs against ENCODER output, with no reader decoding.
    hand = BUNDLE / f"{case.name}.columns.json"
    if hand.exists():
        specs = {d["type"]: d["types"] for d in case.head["records"]}
        for entry in json.loads(hand.read_text(encoding="utf-8")):
            tag, index = entry["tag"], entry["column"]
            values = [row[index + 1] for row in normalized(case.rows, case.head) if row[0] == tag]
            enc, flags, raw = reference.encode_column(values, specs[tag][index])
            if (enc, flags, raw) != (entry["encoding"], entry["flags"], bytes.fromhex(entry["hex"])):
                raise ValueError(f"{case.name}: hand column {tag}.{index} differs; investigate")
    frame_ranges = [item for item in map(json.loads, frames.getvalue().splitlines())
                    if item["tag"] == "SUMMARY" or case.compress]
    # Deterministic raw/RLE frames do not depend on the zstd build.
    mask = {"layout": "sink-v2-round-6", "profile": receipt["profile"], "v2_size": len(data),
            "admission_spec": "sink-v2-round-10",
            "v2_sha256": hashlib.sha256(data).hexdigest(),
            "logical_sha256": receipt["sha256"],
            "report": report_identity,
            "summary": summary_sizes, "zstd_frames": frame_ranges,
            "encoder": {"level": actual_format["level"], "block_records": actual_format["block_records"],
                        "block_bytes": actual_format["block_bytes"],
                        "stored_columns_only": not case.compress and case.frame_factory is None,
                        "decode_only": case.frame_factory is not None or case.summary_frame_factory is not None}}
    artifacts = {
        f"{case.name}.v1.jsonl": expected,
        f"{case.name}.pfs2": data,
        f"{case.name}.v2.hex": ("# sink-v2 round 6; every checksum is real\n" +
                              "\n".join(data[i:i + 32].hex(" ") for i in range(0, len(data), 32)) + "\n").encode(),
        f"{case.name}.records.jsonl": b"".join(map(v1_line, normalized(case.rows, case.head))),
        f"{case.name}.summary.json": reference.metadata_bytes(case.summary),
        f"{case.name}.report.json": case.report,
        f"{case.name}.dispatch-cases.json": (json.dumps({
            "scope": "APP manifest discriminants only (sink-v2.md 14.7)",
            "status": "PENDING-APP-CONFORMANCE", "binary": f"{case.name}.pfs2",
            "report": f"{case.name}.report.json",
            "accept": {"version": 5, "encoding": "pfs2", "sink_version": 2},
            "refuse": [{"version": 6, "encoding": "pfs2", "sink_version": 2},
                       {"version": 5, "encoding": "gzip", "sink_version": 2},
                       {"version": 5, "encoding": "pfs2", "sink_version": 3},
                       {"version": 2, "encoding": "pfs2", "sink_version": 2}],
        }, separators=(",", ":")) + "\n").encode("ascii"),
        f"{case.name}.mask.json": (json.dumps(mask, ensure_ascii=False, indent=2) + "\n").encode(),
    }
    refusal_artifacts = generate_refusals(case, data)
    # A failed negative control also prevents promotion of the parent vector.
    artifacts.update(refusal_artifacts)
    write_artifacts(artifacts, destination, check)


def generate_refusals(case, data: bytes) -> dict[str, bytes]:
    """Actual corrupt whole files; every mutation must fail before JSON output.

    Wire is a test-only literal-offset/resealing helper, not a reader/encoder
    algorithm. The encoder itself never imports these tests or the reader.
    """
    from test_sink_v2 import Source, Wire
    import struct
    import zlib

    mutations, limit_controls = [], {}
    s_max, block = 1024 * 1024, 128 * 1024
    c_max = (4 + 1 + 4) + (s_max // block) * (block + 3)
    if case.name in ("equity4", "equity4-full"):
        content = reference.metadata_bytes(case.summary)
        for name, frame, message in summary_frame_refusals(content):
            wire = Wire(data)
            wire.replace_summary_frame(frame)
            control = "summary-" + name
            mutations.append((control, bytes(wire.data), message))
            if name == "window":
                limit_controls[control] = {"fault": "window", "J": len(content), "C": len(frame),
                                           "window": 2 * s_max, "header_only": False}
        for name, frame, decoded in (
                ("J-zero", b"\x28\xb5\x2f\xfd\x20\x00\x01\x00\x00", b""),
                ("C-zero", b"", None)):
            wire = Wire(data)
            wire.replace_summary_frame(frame, decoded=decoded)
            mutations.append(("summary-" + name, bytes(wire.data), "summary"))
        wire = Wire(data)
        offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
        struct.pack_into("<I", wire.data, offset + 16, 1)
        wire.seal_root()
        mutations.append(("summary-flags", bytes(wire.data), "summary"))
        # Stored-only fault: changing the ceiling raw frame to non-single
        # segment adds one header byte; FCS=J=S_MAX and window=S_MAX still fit.
        padded = json.loads(content)
        padded["input"]["summary_payload"] = ""
        base = json.dumps(padded, separators=(",", ":")).encode("ascii")
        padded["input"]["summary_payload"] = "x" * (s_max - len(base))
        padded_bytes = json.dumps(padded, separators=(",", ":")).encode("ascii")
        raw = summary_raw_frame(padded_bytes)
        frame = raw[:4] + b"\x80\x50" + raw[5:]
        if len(padded_bytes) != s_max or len(frame) != c_max + 1:
            raise ValueError("stored-only control must have J=S_MAX, C=C_max+1")
        wire = Wire(data)
        wire.replace_summary_frame(frame, decoded=padded_bytes)
        control = f"summary-C{c_max + 1}"
        mutations.append((control, bytes(wire.data), "summary stored/decoded length"))
        limit_controls[control] = {"fault": "stored_length", "J": s_max, "C": c_max + 1,
                                   "window": s_max, "header_only": True}
        oversized = padded_bytes.replace(b'"summary_payload":"', b'"summary_payload":"x', 1)
        frame = summary_window_frame(oversized)
        if len(oversized) != s_max + 1 or len(frame) > c_max:
            raise ValueError("J-only control must have J=S_MAX+1 and an admitted C")
        wire = Wire(data)
        wire.replace_summary_frame(frame, decoded=oversized)
        control = f"summary-J{s_max + 1}"
        mutations.append((control, bytes(wire.data), "summary stored/decoded length"))
        limit_controls[control] = {"fault": "decoded_length", "J": s_max + 1, "C": len(frame),
                                   "window": s_max, "header_only": True}
        wire = Wire(data)
        length_offset = wire.root + (16 if wire.compact else 24)
        length = struct.unpack_from("<I", wire.data, length_offset)[0]
        struct.pack_into("<I", wire.data, length_offset, length + 1)
        wire.seal_root()
        mutations.append(("summary-root-length", bytes(wire.data), "summary header"))
        for relative, name, message in ((12, "crc", "summary CRC"), (20, "sha", "summary SHA")):
            wire = Wire(data)
            wire.replace_summary(content)
            offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
            wire.data[offset + relative] ^= 1
            if name == "sha":
                digest_offset = wire.root + (96 if wire.compact else 352)
                wire.data[digest_offset:digest_offset + 32] = wire.data[offset + 20:offset + 52]
            wire.seal_root(logical=name == "sha" and not wire.compact)
            mutations.append(("summary-decoded-" + name, bytes(wire.data), message))
    if case.name in ("summary-max", "summary-max-full"):
        wire = Wire(data)
        content = reference.metadata_bytes(case.summary)
        oversized = content.replace(b'"summary_payload":"', b'"summary_payload":"x', 1)
        if len(oversized) != s_max + 1:
            raise ValueError("oversize SUMMARY negative must be exactly S_MAX+1 bytes")
        # FCS matches J, but a non-single-segment window stays at S_MAX;
        # RLE of the constant padding keeps the stored length admitted.
        frame = summary_window_frame(oversized)
        if len(frame) > c_max:
            raise ValueError("J-only control has a second stored-length fault")
        wire.replace_summary_frame(frame, decoded=oversized)
        control = f"summary-J{s_max + 1}-valid-json"
        mutations.append((control, bytes(wire.data), "summary stored/decoded length"))
        limit_controls[control] = {"fault": "decoded_length", "J": s_max + 1, "C": len(frame),
                                   "window": s_max, "header_only": True}
    if case.name == "equity4":
        mutations.extend((("tail-missing", data[:-160], "tail"), ("tail-truncated", data[:-1], "tail"),
                          ("tail-extra", data + b"x", "tail")))
        for name, relative, message in (("tail-root-sha", 24, "root SHA"),
                                         ("tail-logical-sha", 56, "logical SHA"),
                                         ("tail-stored-sha", 88, "stored-prefix SHA")):
            wire = Wire(data)
            wire.data[wire.tail + relative] ^= 1
            mutations.append((name, bytes(wire.data), message))
        wire = Wire(data)
        wire.data[wire.column_offset("E", 2)] ^= 1
        mutations.append(("column-crc", bytes(wire.data), "column CRC"))
        wire = Wire(data)
        wire.data[wire.block_offset("E") + 28] ^= 1
        mutations.append(("directory-crc", bytes(wire.data), "directory CRC"))
        for name, offset, replacement in (("unknown-version", 4, b"\x03\x00"),
                                           ("profile-length", 6, b"\x00\x00")):
            wire = Wire(data)
            wire.data[wire.root + offset:wire.root + offset + 2] = replacement
            wire.seal_root()
            mutations.append((name, bytes(wire.data), "root"))
        wire = Wire(data)
        offset, _, _, _ = wire.locator(0)
        wire.data[offset + 64] ^= 1
        wire.seal_root()  # keep the original compact table SHA, repair outer hashes
        mutations.append(("compact-index-sha", bytes(wire.data), "COMPACT descriptor table SHA"))
        for name in ("logical", "finite-count", "raw-count"):
            wire = Wire(data)
            if name == "logical":
                wire.data[wire.tail + 56] ^= 1
                wire.seal_root()
                message = "decoded logical SHA"
            elif name == "finite-count":
                struct.pack_into("<Q", wire.data, wire.root + 88, 3)
                wire.seal_root()
                message = "finite_e_count"
            else:
                for tag, index in (("E", 0), ("I", 7)):
                    struct.pack_into("<Q", wire.data, wire.root + 24 + index * 8, 5)
                    struct.pack_into("<I", wire.data, wire.descriptor_offset(tag) + 16, 5)
                wire.seal_table(0)
                message = "header"
            mutations.append(("compact-unverified-" + name, bytes(wire.data), message))
        for key, value in (("E", 10980), ("T", 3314)):
            wire = Wire(data)
            struct.pack_into("<Q", wire.data, wire.root + 24 + "ET".index(key) * 8, value)
            total = sum(struct.unpack_from("<7Q", wire.data, wire.root + 24))
            struct.pack_into("<Q", wire.data, wire.root + 80, total)
            wire.seal_root()
            mutations.append(("compact-ceiling-" + key, bytes(wire.data), "COMPACT population"))
        wire = Wire(data)
        struct.pack_into("<I", wire.data, wire.root + 20, 129)
        wire.seal_root()
        mutations.append(("compact-ceiling-D", bytes(wire.data), "COMPACT population"))
        wire = Wire(data)
        struct.pack_into("<Q", wire.data, wire.root + 168, 96 * 129)
        wire.seal_root()
        mutations.append(("compact-table-length", bytes(wire.data), "COMPACT descriptor table"))
        wire = Wire(data)
        wire.data[wire.root:wire.root] = b"IDX2"
        wire.root += 4
        wire.tail += 4
        struct.pack_into("<Q", wire.data, wire.tail, wire.root)
        struct.pack_into("<Q", wire.data, wire.tail + 16, len(wire.data))
        wire.seal_root()
        mutations.append(("compact-extra-derived-page", bytes(wire.data), "COMPACT descriptor table tiling"))
        wire = Wire(data)
        start = wire.block_offset("E") + 32
        wire.data[start] = 2
        wire.seal_block("E")
        mutations.append(("unknown-codec", bytes(wire.data), "codec"))
        wire = Wire(data)
        start = wire.column_offset("E", 1)
        wire.data[start] = 13
        mutations.append(("decimal-k13", bytes(wire.data), "DECIMAL scale"))
        wire = Wire(data)
        wire.data[wire.column_offset("E", 1) + 8] = 2
        prefix = b"PFS2-COL\0" + struct.pack("<BIQI", ord("E"), 1, 0, 4)
        values = (10000.0, 10005.25, 10010.5, 10015.75)
        crc = zlib.crc32(prefix + b"".join(b"\x01" + struct.pack("<d", v) for v in values))
        struct.pack_into("<I", wire.data, wire.block_offset("E") + 32 + 16 + 12, crc)
        wire.seal_block("E")
        wire.seal_root()
        mutations.append(("column-sha-crc-valid", bytes(wire.data), "column SHA"))
        wire = Wire(data)
        wire.data[wire.block_offset("E") + 32 + 3 * 16] ^= 1
        wire.seal_block("E")
        mutations.append(("column-sha-table", bytes(wire.data), "block SHA table"))
        wire = Wire(data)
        content = reference.metadata_bytes(case.summary)
        for name, before, after, message in (
                ("summary-duplicate", b'"metrics"', b'"summary"', "duplicate"),
                ("summary-missing", b'"metrics"', b'"xetrics"', "required fields"),
                ("summary-engine-type", b'"engine":"pineforge"', b'"engine":12345678901', "engine must")):
            changed = Wire(data)
            changed.replace_summary(content.replace(before, after, 1))
            mutations.append((name, bytes(changed.data), message))
        for name, bad, message in (
                ("summary-utf8", content.replace(b'"pineforge"', b'"\xff"'), "UTF-8"),
                ("summary-surrogate", content.replace(b'"pineforge"', b'"\\ud800"'), "surrogate"),
                ("summary-unsafe-integer", content[:-1] + b',"future":9007199254740992}', "unsafe"),
                ("summary-overflow", content[:-1] + b',"future":1e309}', "non-finite"),
                ("summary-non-object", b"[]", "summary"),
                ("summary-depth65", content[:-1] + b',"future":' + b"[" * 64 + b"0" + b"]" * 64 + b"}", "nesting"),
                ("summary-payload", content[:-1] + b',"trades":[]}', "forbidden"),
                ("summary-whitespace", content + b"\n", "whitespace")):
            changed = Wire(data)
            changed.replace_summary(bad)
            mutations.append((name, bytes(changed.data), message))

        def decimal_overflow(tag, column, encoding, flags, raw, values):
            if tag == "E" and column == 1:
                body = b"\x00" + reference.unsigned_varint(2 * (1 << 50)) + b"\x01" + bytes(len(values) - 1)
                return 3, 0, body, values
            return None

        damaged = reference.encode_bytes(case.head, case.rows, case.summary, compress=False,
                                          column_fixture=decimal_overflow)
        mutations.append(("decimal-scaled-limit", damaged, "DECIMAL scaled"))

        def frame_fixture(mode):
            def frame(tag, column, raw):
                if tag != "E" or column != 0:
                    return None
                body = ((len(raw) << 3) | 1).to_bytes(3, "little") + raw
                prefix = b"\x28\xb5\x2f\xfd"
                if mode == "window":
                    return prefix + b"\x80\x78" + len(raw).to_bytes(4, "little") + body
                if mode == "dictionary":
                    return prefix + b"\x21\x01" + bytes((len(raw),)) + body
                if mode == "missing-size":
                    return prefix + b"\x00\x00" + body
                frame_bytes = prefix + b"\x20" + bytes((len(raw) + int(mode == "wrong-size"),)) + body
                return frame_bytes + (b"x" if mode == "trailing" else b"")
            return frame

        for mode, message in (("window", "window exceeds"), ("dictionary", "dictionary"),
                              ("missing-size", "content size"), ("wrong-size", "content size"),
                              ("trailing", "frame bytes")):
            damaged = reference.encode_bytes(case.head, case.rows, case.summary, compress=False,
                                              frame_factory=frame_fixture(mode))
            mutations.append(("zstd-" + mode, damaged, message))
    if case.name in ("one-trade-full", "equity4-full"):
        table = 8 if case.name == "one-trade-full" else 18
        wire = Wire(data)
        struct.pack_into("<I", wire.data, wire.root + 512 + table * 64 + 28, 0)
        wire.seal_root()
        mutations.append(("full-required-table-absent", bytes(wire.data), "PRESENT"))
        wire = Wire(data)
        page = wire.locator(table)[0]
        wire.data[page + 24] ^= 1
        wire.page_crc(page)
        mutations.append(("index-sha", bytes(wire.data), "SHA"))
    if case.name == "finite-prefix-pages":
        wire = Wire(data)
        start = wire.locator(0)[0]
        struct.pack_into("<I", wire.data, start + 12, 119)
        wire.page_crc(start)
        mutations.append(("short-nonfinal-page", bytes(wire.data), "index page header"))
        wire = Wire(data)
        struct.pack_into("<Q", wire.data, wire.descriptor_offset("E", 120) + 480, 0)
        wire.seal_table(0)
        mutations.append(("finite-prefix", bytes(wire.data), "finite_prefix"))
        wire = Wire(data)
        start = wire.locator(0)[0]
        wire.data[start + 56] ^= 1
        wire.page_crc(start)
        wire.seal_root()
        mutations.append(("bad-proof", bytes(wire.data), "Merkle proof"))
        wire = Wire(data)
        locator = wire.root + 512
        last_bytes = struct.unpack_from("<I", wire.data, locator + 24)[0]
        struct.pack_into("<I", wire.data, locator + 24, last_bytes - 1)
        wire.seal_root()
        mutations.append(("final-page-length", bytes(wire.data), "final page length"))
    if case.name == "integer-boundaries":
        for tag, column, name in (("E", 0, "unsafe-i64"), ("T", 15, "unsafe-u64")):
            wire = Wire(data)
            struct.pack_into("<Q", wire.data, wire.column_offset(tag, column), 1 << 53)
            mutations.append((name, bytes(wire.data), "PLAIN64"))
        for name, body in (
                ("delta-value", reference.unsigned_varint(2 * ((1 << 52) - 1)) + b"\x01\x04"),
                ("delta-quotient", b"\x00\x01" + reference.unsigned_varint((1 << 53) - 1))):
            wire = Wire(data)
            block = wire.block_offset("E")
            start = wire.column_offset("E", 0)
            length = struct.unpack_from("<I", wire.data, block + 32 + 4)[0]
            wire.data[block + 32 + 1] = 1
            wire.data[start:start + length] = body + bytes(length - len(body))
            wire.seal_block("E")
            mutations.append((name, bytes(wire.data), "delta"))
    if case.name == "float-boundaries":
        for name, word in (("nan-word", 0x7FF8000000000001), ("inf-word", 0x7FF0000000000000),
                            ("negative-inf-word", 0xFFF0000000000000)):
            wire = Wire(data)
            start = wire.column_offset("E", 1)
            count = len([row for row in case.rows if row[0] == "E"])
            for plane in range(8):
                wire.data[start + plane * count] = (word >> (8 * plane)) & 255
            mutations.append((name, bytes(wire.data), "non-finite"))
    if case.name == "default-figure-full":
        wire = Wire(data)
        figure = wire.locator(18)[0]
        # A different adjusted witness remains a valid finite word/mask.
        wire.data[figure + 4096 + 16 + 24] ^= 1
        wire.seal_table(18)
        mutations.append(("default-envelope", bytes(wire.data), "figure table mismatch"))
        wire = Wire(data)
        content = json.loads(reference.metadata_bytes(case.summary))
        content["sink_folds"]["open_at_end_pnl"] += 1.0
        wire.replace_summary(json.dumps(content, separators=(",", ":")).encode())
        mutations.append(("fold-pnl", bytes(wire.data), "sink_folds mismatch"))
    if case.name == "x2-1048575":
        def oversize_line(tag, column, encoding, flags, raw, values):
            if tag == "X" and column == 2:
                changed = [values[0] + "x"]
                enc, cflags, body = reference.encode_column(changed, "str")
                return enc, cflags, body, changed
            return None

        # Framing/SHA agree with the over-limit row. The reader must reach X2.
        damaged = reference.encode_bytes(case.head, case.rows, case.summary, compress=False,
                                          column_fixture=oversize_line)
        mutations.append(("x2-1048576", damaged, "v1 line exceeds"))
    artifacts = {}
    for name, damaged, message in mutations:
        if name.startswith("summary-") or name == "fold-pnl":
            wire = Wire(damaged)
            wire.seal_summary_identity(case.head, case.rows)
            damaged = bytes(wire.data)
        output = io.BytesIO()
        limit = limit_controls.get(name)
        source = Source(damaged) if limit is not None else damaged
        profile = ("FULL" if Wire(damaged).root_length == 4096 else "COMPACT") if limit is not None else None
        try:
            with pf_sink2.open(source, profile_hint=profile) as reader:
                if name.startswith("compact-unverified-"):
                    if (reader.logical_identity_state != "UNVERIFIED" or reader.logical_sha256 != "UNVERIFIED" or
                            reader.summary()["report_sink"]["sha256"] != "UNVERIFIED" or
                            reader.head()["summary"]["report_sink"]["sha256"] != "UNVERIFIED"):
                        raise ValueError("COMPACT promoted a declared identity before full verification")
                    try:
                        reader.require_logical_sha256()
                    except pf_sink2.UnverifiedLogicalIdentity as error:
                        if error.code != "sink_logical_identity_unverified":
                            raise ValueError("COMPACT identity refusal lacks its code") from error
                    else:
                        raise ValueError("COMPACT returned an unverified cache/receipt identity")
                reader.to_report_json(output)
        except pf_sink2.SinkFormatError as error:
            if message is not None and message not in str(error):
                raise ValueError(f"{case.name}/{name}: wrong refusal: {error}") from error
            if limit is not None:
                if not isinstance(error, pf_sink2.SummaryLimit) or error.code != "summary_limit":
                    raise ValueError(f"{case.name}/{name}: untyped SUMMARY refusal") from error
                wire = Wire(damaged)
                offset = struct.unpack_from("<Q", damaged, wire.root + (8 if wire.compact else 16))[0]
                expected_reads = [(wire.root, wire.root_length + 160), (offset, 52)]
                if not limit["header_only"]:
                    expected_reads.extend(((offset + 52, 5), (offset + 57, 5)))
                if source.reads != expected_reads:
                    raise ValueError(f"{case.name}/{name}: read past the first failing header check")
        else:
            raise ValueError(f"{case.name}/{name}: corrupt vector accepted")
        if output.getvalue():
            raise ValueError(f"{case.name}/{name}: corrupt vector emitted success JSON")
        stem = case.name + "--" + name
        artifacts[stem + ".pfs2"] = damaged
        artifacts[stem + ".report.json"] = case.report  # good parent bytes, never emit on refusal
        refusal = {"parent": case.name, "stage": "open-unverified-then-full-verify" if name.startswith(
            "compact-unverified-") else "open-or-full-verify", "error_contains": message}
        if limit is not None:
            refusal.update(code="summary_limit", first_failing_check=limit,
                           stage="open-fixed-summary-header" if limit["header_only"] else "open-bounded-frame-header")
        artifacts[stem + ".refusal.json"] = (json.dumps(refusal,
            separators=(",", ":")) + "\n").encode("ascii")
    return artifacts


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=BUNDLE)
    parser.add_argument("--only", help="comma-separated vector names (default: all, including real zstd)")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    requested = None if args.only is None else set(args.only.split(","))
    try:
        selected = [case for case in cases(large=True) if requested is None or case.name in requested]
        if requested is not None and {case.name for case in selected} != requested:
            raise ValueError("unknown vector name")
        for case in selected:
            generate(case, args.output_dir, check=args.check)
            print(f"{case.name}: {'MATCH' if args.check else 'GENERATED AND VERIFIED'}")
    except (ValueError, OSError) as error:
        print(f"sink-v2 vectors: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
