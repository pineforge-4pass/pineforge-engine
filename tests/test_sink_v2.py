"""CR150 L21R: independent input/scan oracles for sink-v2 sections 3-11.

No observed reader values are harvested as expectations. The reference encoder
does not import the reader. Mutation helpers independently reseal framing so
semantic negatives cannot pass merely because an unrelated outer CRC failed.
"""
from __future__ import annotations

import builtins
from dataclasses import replace
from functools import cmp_to_key
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
sys.path.insert(0, str(ROOT / "tests"))

import pf_sink as v1
import pf_sink2 as reader
import pf_sink2_reference as encoder
from sink_v2_cases import (Case, cases, header, metadata, population_rows, report_trade, sink_folds,
                           normalized, query_rows, summary_frame_refusals, summary_raw_frame, summary_window_frame, trade,
                           v1_file, v1_line, v1_report)


def has_zstd() -> bool:
    if os.environ.get("PF_SINK_ZSTD_LIBRARY"):
        from pf_sink2_zstd import library
        return library() is not None
    try:
        from compression import zstd
        return zstd is not None
    except ImportError:
        try:
            import zstandard
            return zstandard is not None
        except ImportError:
            return False


def zstd_required(test: unittest.TestCase) -> None:
    if not has_zstd():
        test.fail("sink-v2 conformance needs PF_SINK_ZSTD_LIBRARY (the pinned codec), "
                  "compression.zstd or zstandard")


def bits(value):
    return None if value is None else struct.pack("<d", value)


def rows_bytes(records):
    return b"".join(v1_line([record.tag, *record.values]) for record in records)


class Source:
    """Instrumented immutable range source; mutations model an adapter failure."""
    def __init__(self, data: bytes):
        self.data, self.size, self.generation = data, len(data), "generation-one"
        self.locator = "fixture://sink-v2"
        self.reads = []
        self.after_read = None

    def readRange(self, offset, length, abortSignal=None):
        if abortSignal and abortSignal():
            raise reader.SinkCancelled("source cancelled")
        self.reads.append((offset, length))
        if self.after_read:
            self.after_read()
        return self.data[offset:offset + length]


def fixture(name="equity4", **options):
    case = next(case for case in cases() if case.name == name)
    # Legacy physical-page negatives deliberately exercise FULL. Canonical
    # selection and both profiles are independently tested below.
    kwargs = {"block_records": case.caps, "compress": case.compress,
              "frame_factory": case.frame_factory, "force_full": True}
    kwargs.update(options)
    return case, encoder.encode_bytes(case.head, case.rows, case.summary, **kwargs)


class Wire:
    """Literal offsets independently transcribed from spec 9, for negatives."""
    def __init__(self, data):
        self.data = bytearray(data)
        self.tail = len(data) - 160
        self.root = struct.unpack_from("<Q", data, self.tail)[0]
        self.root_length = struct.unpack_from("<I", data, self.tail + 8)[0]
        self.compact = bool(struct.unpack_from("<H", data, self.root + 6)[0] & 1)

    def locator(self, table):
        if self.compact:
            if not 0 <= table < 8:
                raise ValueError("COMPACT has no rank/figure locator")
            counts = struct.unpack_from("<8I", self.data, self.root + 208)
            start = struct.unpack_from("<Q", self.data, self.root + 160)[0]
            return start + sum(counts[:table]) * 96, 0, 96, counts[table]
        return struct.unpack_from("<QIIQ", self.data, self.root + 512 + table * 64)

    def descriptor_offset(self, tag, ordinal=0):
        table = "ETOVKXHI".index(tag)
        offset, _, _, _ = self.locator(table)
        if self.compact:
            return offset + ordinal * 96
        page, slot = divmod(ordinal, 120)
        return offset + page * 65536 + 4096 + slot * 512

    def block_offset(self, tag, ordinal=0):
        return struct.unpack_from("<Q", self.data, self.descriptor_offset(tag, ordinal))[0]

    def column_offset(self, tag, column, ordinal=0):
        block = self.block_offset(tag, ordinal)
        ncols = struct.unpack_from("<I", self.data, block + 8)[0]
        start = block + 32 + ncols * 48
        for i in range(column):
            start += struct.unpack_from("<I", self.data, block + 32 + i * 16 + 8)[0]
        return start

    def seal_head(self):
        length = struct.unpack_from("<I", self.data, 8)[0]
        struct.pack_into("<I", self.data, 12 + length, zlib.crc32(self.data[:12 + length]))

    def seal_block(self, tag, ordinal=0):
        offset = self.block_offset(tag, ordinal)
        ncols = struct.unpack_from("<I", self.data, offset + 8)[0]
        crc = zlib.crc32(self.data[offset + 32:offset + 32 + 48 * ncols],
                         zlib.crc32(self.data[offset:offset + 28]))
        struct.pack_into("<I", self.data, offset + 28, crc)

    def seal_root(self, *, logical=False):
        if logical:
            if self.compact:
                raise ValueError("compact logical resealing needs input stream hashes")
            root = self.data[self.root:self.root + 4096]
            digest = hashlib.sha256(b"PFS2-LOGICAL\0" + root[384:416] + root[352:384])
            for i, tag in enumerate(b"ETOVKXHI"):
                digest.update(bytes((tag,)) + root[32 + i * 8:40 + i * 8])
                digest.update(root[96 + i * 32:128 + i * 32])
            self.data[self.tail + 56:self.tail + 88] = digest.digest()
        crc_offset = self.root_length - 4
        struct.pack_into("<I", self.data, self.root + crc_offset,
                         zlib.crc32(self.data[self.root:self.root + crc_offset]))
        self.data[self.tail + 24:self.tail + 56] = hashlib.sha256(
            self.data[self.root:self.root + self.root_length]).digest()
        flags = struct.unpack_from("<I", self.data, self.tail + 12)[0]
        if flags & 1:
            self.data[self.tail + 88:self.tail + 120] = hashlib.sha256(self.data[:self.tail]).digest()

    def seal_summary_identity(self, head, rows):
        """Reseal changed metadata using INPUT rows, including COMPACT's I.

        COMPACT has no root stream digests. Never harvest a decoder's declared
        identity as the expected logical digest for a SUMMARY mutation.
        """
        if self.compact:
            types = {entry["type"]: entry["types"] for entry in head["records"]}
            streams = {tag: hashlib.sha256(b"PFS2-STREAM\0" + tag.encode("ascii")) for tag in "ETOVKXHI"}
            for row in normalized(rows, head):
                for value, wire in zip(row[1:], types[row[0]]):
                    streams[row[0]].update(encoder.canonical_cell(value, wire))
                streams["I"].update(row[0].encode("ascii"))
            digest = hashlib.sha256(b"PFS2-LOGICAL\0" + self.data[self.root + 128:self.root + 160] +
                                    self.data[self.root + 96:self.root + 128])
            for i, tag in enumerate("ETOVKXHI"):
                digest.update(tag.encode("ascii") + self.data[self.root + 24 + i * 8:self.root + 32 + i * 8])
                digest.update(streams[tag].digest())
            self.data[self.tail + 56:self.tail + 88] = digest.digest()
        self.seal_root(logical=not self.compact)

    def seal_table(self, table):
        if self.compact:
            offset, length = struct.unpack_from("<QQ", self.data, self.root + 160)
            self.data[self.root + 176:self.root + 208] = hashlib.sha256(
                b"PFS2-CINDEX\0" + self.data[offset:offset + length]).digest()
            self.seal_root()
            return
        offset, pages, _, _ = self.locator(table)
        leaves = []
        for page in range(pages):
            start = offset + page * 65536
            leaf = hashlib.sha256(b"PFS2-INDEX\0" + self.data[start:start + 24] +
                                  self.data[start + 4096:start + self.page_length(start)]).digest()
            leaves.append(leaf)
        depth = (pages - 1).bit_length()
        levels = [leaves + [leaves[-1]] * ((1 << depth) - pages)]
        while len(levels[-1]) > 1:
            nodes = levels[-1]
            levels.append([hashlib.sha256(b"PFS2-NODE\0" + nodes[i] + nodes[i + 1]).digest()
                           for i in range(0, len(nodes), 2)])
        for page in range(pages):
            start = offset + page * 65536
            self.data[start + 24:start + 56] = leaves[page]
            for level in range(depth):
                begin = start + 56 + level * 32
                self.data[begin:begin + 32] = levels[level][(page >> level) ^ 1]
            self.page_crc(start)
        begin = self.root + 512 + table * 64 + 32
        self.data[begin:begin + 32] = levels[-1][0]
        self.seal_root()

    def page_crc(self, start):
        crc = zlib.crc32(self.data[start + 4096:start + self.page_length(start)],
                         zlib.crc32(self.data[start:start + 4092]))
        struct.pack_into("<I", self.data, start + 4092, crc)

    def page_length(self, start):
        used, width = struct.unpack_from("<II", self.data, start + 12)
        return 4096 + used * width

    def reseal_decoded_rows(self, tag, rows, head, *, input_rows=None):
        """Repair hashes from deliberately altered INPUT rows, not a decode.

        The caller has replaced encoded bytes without changing their lengths.
        This lets full verification reach order/multiplicity invariants.
        COMPACT needs all tagged input rows because it stores no stream hashes.
        """
        if self.compact and input_rows is None:
            raise ValueError("compact decoded-row resealing needs all tagged input rows")
        table = "ETOVKXHI".index(tag)
        _, _, _, count = self.locator(table)
        wires = ("order",) if tag == "I" else next(
            tuple(d["types"]) for d in head["records"] if d["type"] == tag)
        for ordinal in range(count):
            desc = self.descriptor_offset(tag, ordinal)
            block = self.block_offset(tag, ordinal)
            n = struct.unpack_from("<I", self.data, desc + 16)[0]
            rec0 = struct.unpack_from("<Q", self.data, desc + 24)[0]
            part = rows[rec0:rec0 + n]
            hashes = bytearray()
            for column, wire in enumerate(wires):
                domain = b"PFS2-COL\0" + struct.pack("<BIQI", ord(tag), column, rec0, n)
                digest, sha = zlib.crc32(domain), hashlib.sha256(domain)
                for row in part:
                    canonical = encoder.canonical_cell(row[column], wire)
                    digest = zlib.crc32(canonical, digest)
                    sha.update(canonical)
                struct.pack_into("<I", self.data, block + 32 + column * 16 + 12, digest)
                hashes.extend(sha.digest())
            ncols = len(wires)
            self.data[block + 32 + 16 * ncols:block + 32 + 48 * ncols] = hashes
            self.data[desc + 64:desc + 96] = hashlib.sha256(
                b"PFS2-BLOCK\0" + struct.pack("<BQI", ord(tag), rec0, n) + hashes).digest()
            if tag == "E" and not self.compact:
                self.data[desc + 96:desc + 480] = encoder.series_bytes(part, rec0)
            self.seal_block(tag, ordinal)
        stream = hashlib.sha256(b"PFS2-STREAM\0" + tag.encode())
        for row in rows:
            for value, wire in zip(row, wires):
                stream.update(encoder.canonical_cell(value, wire))
        if not self.compact:
            start = self.root + 96 + table * 32
            self.data[start:start + 32] = stream.digest()
        self.seal_table(table)
        if self.compact:
            self.seal_summary_identity(head, input_rows)
        else:
            self.seal_root(logical=True)

    def replace_summary(self, content):
        self.replace_summary_frame(summary_raw_frame(content), decoded=content)

    def replace_summary_frame(self, frame, *, decoded=None):
        offset = struct.unpack_from("<Q", self.data, self.root + (8 if self.compact else 16))[0]
        length = struct.unpack_from("<I", self.data, offset + 4)[0]
        delta = len(frame) - length
        self.data[offset + 52:offset + 52 + length] = frame
        self.root += delta
        self.tail += delta
        struct.pack_into("<I", self.data, offset + 4, len(frame))
        if decoded is not None:
            struct.pack_into("<II", self.data, offset + 8, len(decoded), zlib.crc32(decoded))
            digest = hashlib.sha256(decoded).digest()
            self.data[offset + 20:offset + 52] = digest
            sha_offset = 96 if self.compact else 352
            self.data[self.root + sha_offset:self.root + sha_offset + 32] = digest
        summary_length_offset = 16 if self.compact else 24
        struct.pack_into("<I", self.data, self.root + summary_length_offset, 52 + len(frame))
        if self.compact:
            table_offset = struct.unpack_from("<Q", self.data, self.root + 160)[0]
            struct.pack_into("<Q", self.data, self.root + 160, table_offset + delta)
        else:
            for table in range(19):
                base = self.root + 512 + table * 64
                table_offset = struct.unpack_from("<Q", self.data, base)[0]
                if table_offset:
                    struct.pack_into("<Q", self.data, base, table_offset + delta)
        struct.pack_into("<Q", self.data, self.tail, self.root)
        struct.pack_into("<Q", self.data, self.tail + 16, len(self.data))
        self.seal_root(logical=not self.compact)


class RoundTripTests(unittest.TestCase):
    def test_every_stored_type_null_signed_zero_escapes_and_safe_extremes(self):
        for case in cases():
            if case.frame_factory:
                continue  # its separate codec-1 CTest row may explicitly skip
            with self.subTest(case=case.name):
                data = encoder.encode_bytes(case.head, case.rows, case.summary,
                                            block_records=case.caps, compress=False, force_full=case.force_full)
                with reader.open(data) as sink:
                    self.assertFalse(sink.complete)
                    expected = normalized(case.rows, case.head)
                    self.assertEqual(rows_bytes(sink.iter_merged()), b"".join(map(v1_line, expected)))
                    self.assertFalse(sink.complete, "iterator exhaustion is not whole-file verification")
                    for descriptor in case.head["records"]:
                        tag = descriptor["type"]
                        self.assertEqual(rows_bytes(sink.iter_type(tag)),
                                         b"".join(v1_line(row) for row in expected if row[0] == tag))
                    receipt = sink.verify()
                    self.assertTrue(sink.complete)
                    self.assertEqual(receipt["verification"], "verified_whole_file")
                    self.assertEqual(sink.summary()["report_sink"]["counts"], sink.counts())
                    embedded = sink.summary()
                    del embedded["report_sink"]
                    self.assertEqual(v1_line(embedded), v1_line(case.summary))

    def test_column_and_block_verification_scopes(self):
        _, data = fixture()
        with reader.open(data) as sink:
            entry = sink.descriptor("E", 0)
            selected = sink.read_columns(entry, (2,))
            self.assertEqual(selected.verification, "verified_columns")
            self.assertEqual(tuple(selected.columns), (2,))
            self.assertEqual(sink.read_block(entry).verification, "verified_block")
            self.assertFalse(sink.complete)
            with self.assertRaises(reader.SinkFormatError):
                sink.read_columns(entry, (0, 0))
            with self.assertRaisesRegex(reader.SinkFormatError, "not bound"):
                sink.read_block(replace(entry, offset=entry.offset + 1))

    def test_head_and_summary_are_defensive_copies_with_explicit_scope(self):
        case, data = fixture("selected-anchor")
        with reader.open(data) as sink:
            self.assertEqual(sink.verification, "provisional")
            exposed = sink.head()
            exposed["records"].clear()
            exposed["summary"]["report_window"]["additive_future_member"].clear()
            self.assertEqual(sink.head()["records"], case.head["records"])
            self.assertEqual(sink.summary()["report_window"], case.summary["report_window"])
            sink.verify()
            self.assertEqual(sink.verification, "verified_whole_file")

    def test_zero_slot_O_and_zero_column_K(self):
        head = header(outputs=True, slots=0, constants=0)
        rows = [["O", 0, None, None], ["K"]]
        data = encoder.encode_bytes(head, rows, metadata("zero-width", outputs=True), compress=False)
        with reader.open(data) as sink:
            sink.verify()
            self.assertEqual(rows_bytes(sink.iter_merged()), b"".join(map(v1_line, rows)))
            self.assertEqual(sink.read_block(sink.descriptor("K", 0)).columns, {})

    def test_full_width_integer_inputs_are_refused_not_rounded(self):
        for value in (-(1 << 63), (1 << 63) - 1, 1 << 53, (1 << 64) - 1):
            with self.subTest(value=value):
                with self.assertRaises(v1.SinkFormatError):
                    encoder.encode_bytes(header(), [["E", value, 1.0, 0.0]], metadata("unsafe"), compress=False)
                row = trade(1)
                row[16] = value  # entry_incarnation: u64, still limited to 2^53-1
                with self.assertRaises(v1.SinkFormatError):
                    encoder.encode_bytes(header(), [row], metadata("unsafe-u64"), compress=False)

    def test_writer_byte_sanitizing_is_one_replacement_per_invalid_byte(self):
        self.assertEqual(encoder.sanitize_utf8(b"a\xe1\x80\xff\xc0\xafz"), "a\ufffd\ufffd\ufffd\ufffd\ufffdz")
        row = ["X", 0, 0, b"\xe1\x80\xff", 1.0]
        data = encoder.encode_bytes(header(extras=True), [row], metadata("bad-utf8-input"), compress=False)
        with reader.open(data) as sink:
            sink.verify()
            self.assertEqual(next(sink.iter_type("X")).values[2], "\ufffd\ufffd\ufffd")

    def test_three_page_merkle_padding_and_two_page_cache(self):
        rows = [["E", i, float(i), 0.0] for i in range(241)]
        data = encoder.encode_bytes(header(), rows, metadata("three-pages"),
                                    block_records={"E": 1, "I": 64}, compress=False, force_full=True)
        source = Source(data)
        with reader.open(source) as sink:
            index_start = min(table.offset for table in sink.tables if table.pages)
            self.assertFalse(any(index_start <= offset < sink.root_offset for offset, _ in source.reads))
            source.reads.clear()
            for j in (0, 120, 240, 0, 240, 120):
                self.assertEqual(sink.descriptor("E", j).rec0, j)
                self.assertLessEqual(len(sink._pages), 2)
            self.assertEqual(len(source.reads), 5)  # A,B,C,A,(C cached),B
            sink.verify()

    def test_soft_byte_cap_closes_before_append(self):
        rows = [["X", i, i, "x" * 2500, float(i)] for i in range(3)]
        data = encoder.encode_bytes(header(extras=True), rows, metadata("soft-cap"),
                                    block_bytes=4096, compress=False)
        with reader.open(data) as sink:
            sink.verify()
            self.assertEqual([entry.n for entry in sink.entries("X")], [1 for _ in rows])
            self.assertEqual(rows_bytes(sink.iter_merged()), b"".join(map(v1_line, rows)))

    def test_hand_multiblock_closing_order(self):
        _, data = fixture("multiblock")
        wire = Wire(data)
        cursor = 16 + struct.unpack_from("<I", data, 8)[0]
        end = struct.unpack_from("<Q", data, wire.root + 8)[0]
        actual = []
        while cursor < end:
            tag = chr(data[cursor + 4])
            n = struct.unpack_from("<I", data, cursor + 12)[0]
            rec0 = struct.unpack_from("<Q", data, cursor + 16)[0]
            body = struct.unpack_from("<I", data, cursor + 24)[0]
            actual.append((tag, rec0, n))
            cursor += 32 + body
        # Independently follow spec 5.4 over E E T E T E E, caps 2/1/4.
        self.assertEqual(actual, [("E", 0, 2), ("T", 0, 1), ("I", 0, 4), ("T", 1, 1),
                                  ("E", 2, 2), ("E", 4, 1), ("I", 4, 3)])
        self.assertEqual(cursor, end)

    def test_worked_four_point_layout_ledger_from_spec_in_both_profiles(self):
        case = next(case for case in cases() if case.name == "equity4")
        # Spec 13.1's framing-only metadata, not the full report sidecar.
        summary = {"engine": "pineforge", "input": {}, "applied_inputs": {}, "applied_overrides": {},
                   "applied_runtime": {}, "elapsed_seconds": 0.0, "summary": {}, "metrics": {},
                   "diagnostics": {}, "fingerprint": None,
                   "sink_folds": {"closed_count": 0, "open_at_end_count": 0, "open_at_end_pnl": 0.0}}
        raw = json.dumps(summary, separators=(",", ":")).encode("ascii")
        self.assertEqual(len(raw), 257)
        self.assertEqual(summary_raw_frame(raw), b"\x28\xb5\x2f\xfd\x60\x01\x00\x09\x08\x00" + raw)
        for full, expected_size, root_offset in ((False, 2251, 1771), (True, 20171, 15915)):
            data = encoder.encode_bytes(case.head, case.rows, summary, compress=False,
                                        stored_digest=False, force_full=full, summary_frame_factory=summary_raw_frame)
            with reader.open(data) as sink:
                sink.verify()
                self.assertEqual((sink.size, sink.root_offset, sink.data_start, sink.data_end, sink.summary_length),
                                 (expected_size, root_offset, 947, 1260, 319))
                self.assertEqual((sink.descriptor("E", 0).offset, sink.descriptor("E", 0).length), (947, 229))
                self.assertEqual((sink.descriptor("I", 0).offset, sink.descriptor("I", 0).length), (1176, 84))
                if full:
                    self.assertEqual([(sink.tables[i].offset, sink.tables[i].byte_length) for i in (0, 7, 18)],
                                     [(1579, 4608), (6187, 4608), (10795, 5120)])
                else:
                    self.assertEqual(sink.tables[0].offset, 1579)
                    self.assertEqual(sum(t.byte_length for t in sink.tables), 192)

    def test_optional_stored_digest_does_not_change_logical_identity(self):
        _, with_digest = fixture(stored_digest=True)
        _, without_digest = fixture(stored_digest=False)
        identities = []
        for data in (with_digest, without_digest):
            with reader.open(data) as sink:
                sink.verify()
                identities.append(sink.logical_sha256)
        self.assertEqual(identities[0], identities[1])
        self.assertEqual(without_digest[-160 + 88:-160 + 120], bytes(32))

    def test_encoder_has_no_reader_import_or_shared_algorithms(self):
        import ast
        tree = ast.parse((ROOT / "docker" / "pf_sink2_reference.py").read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                self.assertNotIn("pf_sink2", [alias.name for alias in node.names])
            elif isinstance(node, ast.ImportFrom):
                self.assertNotEqual(node.module, "pf_sink2")


class ConversionTests(unittest.TestCase):
    def test_report_json_equals_existing_v1_writer_in_both_profiles(self):
        for case in cases():
            if case.frame_factory:
                continue
            for force_full in (False, True):
                with self.subTest(case=case.name, force_full=force_full):
                    data = encoder.encode_bytes(case.head, case.rows, case.summary, block_records=case.caps,
                                                compress=False, force_full=force_full)
                    output = io.BytesIO()
                    with reader.open(data) as sink:
                        if case.refusal is not None:
                            with self.assertRaises(reader.ReportResultInvalid) as caught:
                                sink.to_report_json(output)
                            error = caught.exception
                            self.assertEqual({"code": error.code, "args": error.code_args}, case.refusal)
                            import run_json
                            self.assertEqual(run_json.failure_line(str(error), error.code, error.code_args).encode("ascii"),
                                             case.report)
                            self.assertEqual(output.getvalue(), b"")
                            continue
                        counted = sink.report_json_identity()
                        emitted = sink.to_report_json(output)
                        self.assertTrue(sink.complete)  # these accessors verify before returning
                        self.assertEqual(counted.file_verification, "verified_whole_file")
                        self.assertEqual(emitted.file_verification, "verified_whole_file")
                    expected = case.report  # existing run_json._stream_json, never this inverse
                    self.assertEqual(output.getvalue(), expected)
                    self.assertTrue(expected.endswith(b"\n"))
                    self.assertFalse(expected.endswith(b"\n\n"))
                    self.assertEqual(counted, emitted)
                    self.assertEqual(emitted["content_length"], len(expected))
                    self.assertEqual(emitted["report_sha256"], hashlib.sha256(expected).hexdigest())
                    self.assertEqual(json.loads(json.dumps({"result": expected.decode("ascii")}))["result"].encode("ascii"),
                                     expected)
                    report_object = json.loads(expected)
                    self.assertIsNone(report_object["metrics"]["all"]["profit_factor"])
                    self.assertIsNone(report_object["metrics"]["equity"]["sharpe_tv"])
                    self.assertNotIn("sink_folds", report_object)
                    self.assertNotIn("report_sink", report_object)
                    keys = list(report_object)
                    self.assertEqual(keys[keys.index("diagnostics") + 1], "trades")
                    self.assertEqual(keys[keys.index("metrics") + 1], "equity_curve")
                    raw_e = [dict(zip(("time_ms", "equity", "open_profit"), row[1:]))
                             for row in normalized(case.rows, case.head) if row[0] == "E"]
                    self.assertEqual(v1_line(report_object["equity_curve"]), v1_line(raw_e))

    def test_report_escaping_outputs_encodings_order_and_raw_manifest_sha(self):
        case = next(case for case in cases() if case.name == "utf8-controls")
        # A second input tests negative rgba truncation; no u32 clamp is invented.
        rows = normalized(case.rows, case.head)
        next(row for row in rows if row[0] == "K")[1] = -3.75
        next(row for row in rows if row[0] == "O" and row[1] == 1)[4] = -3.75
        modified = Case("rgba", case.head, rows, metadata("rgba", rows, outputs=True))
        modified.summary, expected, refusal = v1_report(modified)
        self.assertIsNone(refusal)
        for full in (False, True):
            data = encoder.encode_bytes(modified.head, rows, modified.summary, compress=False, force_full=full)
            output = io.BytesIO()
            reader.to_report_json(data, output)
            self.assertEqual(output.getvalue(), expected)
            doc = json.loads(expected)
            outputs = doc["outputs"]
            self.assertEqual(tuple(outputs), ("schema_version", "message_format", "manifest_sha256", "manifest",
                                              "bars", "series", "constants", "hlines", "events"))
            self.assertEqual(outputs["constants"][0], -3)
            self.assertEqual(outputs["series"][0]["values"][1], -3)
            from sink_v2_cases import output_manifest
            self.assertEqual(outputs["manifest_sha256"], hashlib.sha256(output_manifest()).hexdigest())
            self.assertNotEqual(outputs["manifest_sha256"], hashlib.sha256(
                json.dumps(outputs["manifest"], separators=(",", ":")).encode()).hexdigest())
            self.assertEqual([series["slot"] for series in outputs["series"]], [0, 1, 1])
            self.assertEqual(outputs["events"][0]["message"], next(row[-1] for row in rows if row[0] == "V"))
            self.assertTrue(outputs["events"][0]["message"].endswith("\ufffd" * 3))
            self.assertEqual(outputs["events"][1]["message"], "")
            self.assertNotIn("freq", outputs["events"][1])
            self.assertEqual(outputs["events"][1]["phase"], "-7")
            self.assertEqual(outputs["events"][0]["freq"], "all")
            self.assertEqual(outputs["hlines"][0]["price"], outputs["constants"][1])  # bool constant index
            self.assertNotIn("trace", doc)
            self.assertNotIn("broker_hashes", doc)
            self.assertIn(b"\\ud83d\\ude00", expected)
            self.assertIn(b"\\u0000", expected)

    def test_report_refuses_framing_only_metadata_and_null_summary_before_writing(self):
        data = encoder.encode_bytes(header(), [], metadata("framing-only"), compress=False)
        output = io.BytesIO()
        with reader.open(data) as sink:
            with self.assertRaisesRegex(reader.SinkFormatError, "complete v1 report metadata"):
                sink.to_report_json(output)
        self.assertEqual(output.getvalue(), b"")
        case = next(case for case in cases() if case.name == "one-trade")
        case.summary["summary"]["net_pnl"] = None
        data = encoder.encode_bytes(case.head, case.rows, case.summary, compress=False)
        with reader.open(data) as sink:
            with self.assertRaises(reader.ReportResultInvalid) as caught:
                sink.to_report_json(output)
        self.assertEqual(caught.exception.code_args, {"field": "summary.net_pnl", "reason": "nonfinite"})
        self.assertEqual(output.getvalue(), b"")

    def test_report_outputs_reject_invalid_manifest_indices_missing_K_and_metadata_drift(self):
        import copy
        import run_json
        case = next(case for case in cases() if case.name == "outputs")
        for key, field, bad_value in (("outputs", "index", True), ("outputs", "id", ""),
                                      ("series", "slot", False), ("constants", "index", -1)):
            saved = copy.deepcopy(case.summary)
            saved["outputs"]["manifest"][key][0][field] = bad_value
            fields = {"outputs": ("index", "id"), "series": ("slot", "output"), "constants": ("index",)}[key]
            with self.assertRaises(run_json.RunFailure):
                run_json._manifest_entries(saved["outputs"]["manifest"], key, fields)
            data = encoder.encode_bytes(case.head, case.rows, saved, compress=False)
            output = io.BytesIO()
            with reader.open(data) as sink:
                with self.assertRaisesRegex(reader.SinkFormatError, "manifest"):
                    sink.to_report_json(output)
            self.assertEqual(output.getvalue(), b"")
        rows = [row for row in case.rows if row[0] != "K"]
        data = encoder.encode_bytes(case.head, rows, case.summary, compress=False)
        output = io.BytesIO()
        with reader.open(data) as sink:
            with self.assertRaisesRegex(reader.SinkFormatError, "one K row"):
                sink.to_report_json(output)
        self.assertEqual(output.getvalue(), b"")
        saved = copy.deepcopy(case.summary)
        saved["outputs"]["constants"][1] = 1.0
        data = encoder.encode_bytes(case.head, case.rows, saved, compress=False)
        with reader.open(data) as sink:
            with self.assertRaisesRegex(reader.SinkFormatError, "constants/hlines"):
                sink.to_report_json(output)
        self.assertEqual(output.getvalue(), b"")

    def test_report_cli_and_same_generation_count_emit(self):
        case = next(case for case in cases() if case.name == "one-trade")
        data = encoder.encode_bytes(case.head, case.rows, case.summary, compress=False)
        with tempfile.TemporaryDirectory() as scratch:
            source, destination = Path(scratch) / "in.pfs2", Path(scratch) / "out.report.json"
            source.write_bytes(data)
            with mock.patch("sys.stdout", io.StringIO()):
                self.assertEqual(reader.main(["to-report-json", str(source), str(destination)]), 0)
            self.assertEqual(destination.read_bytes(), case.report)
        source = Source(data)
        output = io.BytesIO()

        class ChangingOutput(io.BytesIO):
            def write(self, content):
                size = super().write(content)
                source.generation = "changed-during-emission"
                return size

        with reader.open(source) as sink:
            sink.report_json_identity()
            output = ChangingOutput()
            with self.assertRaisesRegex(reader.SinkFormatError, "generation"):
                sink.to_report_json(output)
            self.assertFalse(sink._complete)
        self.assertFalse(output.getvalue().endswith(b"\n"))

    def test_byte_identical_v1_header_records_trailer_and_content_sha(self):
        for case in cases():
            if case.frame_factory:
                continue
            with self.subTest(case=case.name):
                expected = v1_file(case.head, case.rows)
                data = encoder.encode_bytes(case.head, case.rows, case.summary,
                                            block_records=case.caps, compress=False, force_full=case.force_full)
                with reader.open(data) as sink:
                    output = io.BytesIO()
                    receipt = sink.to_v1(output)
                    self.assertEqual(receipt.file_verification, "verified_whole_file")
                    self.assertEqual(output.getvalue(), expected)
                    body = b"".join(expected.splitlines(keepends=True)[:-1])
                    digest = hashlib.sha256(body).hexdigest()
                    self.assertEqual(receipt, {"v1_sha256": digest, "bytes": len(expected)})
                    content_identity = sink.content_sha256()
                    self.assertEqual(content_identity, digest)
                    self.assertEqual(content_identity.file_verification, "verified_whole_file")
                with tempfile.TemporaryDirectory() as scratch:
                    path = Path(scratch) / "reference.jsonl"
                    path.write_bytes(expected)
                    with v1.SinkReader(path) as sink:
                        self.assertEqual(rows_bytes(sink), b"".join(map(v1_line, normalized(case.rows, case.head))))
                        self.assertTrue(sink.complete)

    def test_float_repr_thresholds_subnormals_and_ties(self):
        values = (5e-324, -5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, -0.0,
                  1e-5, 1e-4, 1e15, 1e16, 1e22, 9007199254740992.0,
                  1.0000000000000002, 1.08342, 9.5367431640625e-07)
        rows = [["E", i, value, 0.0] for i, value in enumerate(values)]
        head = header('quote"\\\x00\n雪')
        data = encoder.encode_bytes(head, rows, metadata("float-repr"), compress=False)
        with reader.open(data) as sink:
            output = io.BytesIO()
            sink.to_v1(output)
            self.assertEqual(output.getvalue(), v1_file(head, rows))

    def test_from_v1_requires_valid_receipt_and_preserves_metadata_bytes(self):
        case, _ = fixture("trades3")
        expected = v1_file(case.head, case.rows)
        # Compact metadata bytes (including raw Unicode spelling) are retained.
        summary = json.dumps(case.summary, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        with tempfile.TemporaryDirectory() as scratch:
            source, output = Path(scratch) / "input.jsonl", Path(scratch) / "output.pfs2"
            source.write_bytes(expected)
            encoder.from_v1(source, output, summary, compress=False)
            with reader.open(output) as sink:
                sink.verify()
                converted = io.BytesIO()
                sink.to_v1(converted)
                self.assertEqual(converted.getvalue(), expected)
                self.assertEqual(sink.summary_json_bytes(), summary)
                self.assertEqual(sink.summary_json_length, len(summary))
                self.assertEqual(sink.summary_length, 52 + sink.summary_stored_length)
                self.assertEqual(output.read_bytes()[sink.summary_offset + 52:sink.summary_offset + 56],
                                 b"\x28\xb5\x2f\xfd")
            source.write_bytes(expected[:expected.rfind(b'{"trailer"')])
            with self.assertRaises(v1.SinkFormatError):
                encoder.from_v1(source, output, summary, compress=False)
            self.assertFalse(output.read_bytes().endswith(b"PFS2END\n"))

    def test_no_output_or_success_trailer_for_corrupt_input(self):
        _, data = fixture()
        damaged = Wire(data)
        damaged.data[damaged.column_offset("E", 1)] ^= 1
        output = io.BytesIO()
        with reader.open(bytes(damaged.data)) as sink:
            with self.assertRaises(reader.SinkFormatError):
                sink.to_v1(output)
            self.assertEqual(output.getvalue(), b"")
            self.assertFalse(sink.complete)

    def test_cancelled_export_withholds_v1_trailer(self):
        _, data = fixture()
        cancelled = [False]

        class Output(io.BytesIO):
            def write(self, content):
                count = super().write(content)
                cancelled[0] = True
                return count

        output = Output()
        with reader.open(data, abortSignal=lambda: cancelled[0]) as sink:
            sink.verify()
            with self.assertRaises(reader.SinkCancelled):
                sink.to_v1(output)
            self.assertNotIn(b'{"trailer"', output.getvalue())
            cancelled[0] = False
            self.assertFalse(sink.complete)


class ProfileTests(unittest.TestCase):
    def test_logical_identity_state_and_coded_demand_in_both_profiles(self):
        for full in (False, True):
            case, data = fixture(force_full=full)
            with reader.open(data) as sink:
                self.assertEqual(sink.logical_identity_state, "UNVERIFIED")
                for obj in (sink.summary(), sink.head()["summary"]):
                    receipt = obj["report_sink"]
                    self.assertEqual(receipt["logical_identity_state"], "UNVERIFIED")
                    self.assertEqual(receipt["counts_verification"], "provisional" if full else "UNVERIFIED")
                    self.assertEqual(receipt["sha256"], sink.logical_sha256)
                self.assertEqual(sink.counts(), {"E": len(case.rows), "T": 0})
                if not full:
                    self.assertEqual(sink.logical_sha256, "UNVERIFIED")
                with self.assertRaises(reader.UnverifiedLogicalIdentity) as failure:
                    sink.require_logical_sha256()
                self.assertEqual(failure.exception.code, "sink_logical_identity_unverified")
                receipt = sink.verify()
                self.assertEqual(sink.logical_identity_state, "VERIFIED")
                self.assertEqual(sink.require_logical_sha256(), receipt["sha256"])
                self.assertEqual(sink.counts_verification, "verified_whole_file")
                self.assertEqual(sink.summary()["report_sink"]["sha256"], receipt["sha256"])

    def test_every_accessor_class_exposes_file_state_before_and_after_verify(self):
        for full in (False, True):
            case, data = fixture("selected-anchor", force_full=full)
            expected = normalized(case.rows, case.head)
            with self.subTest(full=full), reader.open(data) as sink:
                for verified in (False, True):
                    if verified:
                        receipt = sink.verify()
                        self.assertEqual(receipt.file_verification, "verified_whole_file")
                    state = "verified_whole_file" if verified else "provisional" if full else "UNVERIFIED"
                    self.assertEqual(sink.verification, state)
                    self.assertEqual(sink.counts_verification, state)
                    for name in ("compact", "profile", "size", "generation", "root_offset", "root_length",
                                 "data_start", "data_end",
                                 "summary_offset", "summary_length", "summary_json_length",
                                 "summary_stored_length", "nblocks", "finite_e_count", "tables"):
                        with self.subTest(accessor=name, verified=verified):
                            self.assertEqual(getattr(sink, name).file_verification, state)
                    metadata_results = (sink.head(), sink.summary(), sink.summary_json_bytes(),
                                        sink.summary_frame_bytes(), sink.counts(),
                                        sink.counts(include_interleave=True), sink.logical_sha256)
                    for value in metadata_results:
                        self.assertEqual(value.file_verification, state)
                    self.assertEqual(sink.counts()["E"], sum(row[0] == "E" for row in expected))
                    self.assertEqual(sink.finite_e_count,
                                     sum(row[0] == "E" and row[2] is not None for row in expected))
                    self.assertEqual(sink.summary_json_bytes(),
                                     json.dumps(case.summary, separators=(",", ":"), ensure_ascii=True,
                                                allow_nan=False).encode("ascii"))
                    self.assertEqual(sink.tables[0].file_verification, state)
                    self.assertEqual(sink.tables[0].capacity.file_verification, state)
                    self.assertEqual(sink.tables[0].byte_length.file_verification, state)
                    entry = sink.descriptor("E", 0)
                    self.assertEqual(entry.file_verification, state)
                    layout = sink.block_layout(entry)
                    self.assertEqual(layout.file_verification, state)
                    self.assertEqual(layout[0].file_verification, state)
                    columns = sink.read_columns(entry, (0,))
                    self.assertEqual(columns.file_verification, state)
                    self.assertEqual(columns.verification, "verified_columns")
                    self.assertEqual(columns.verified_columns, (0,))
                    self.assertEqual(columns.verified_columns.file_verification, state)
                    block = sink.read_block(entry)
                    self.assertEqual(block.file_verification, state)
                    self.assertEqual(block.verification, "verified_block")
                    self.assertEqual(block.values(0).file_verification, state)
                    begin = min(row[1] for row in expected if row[0] == "E")
                    end = max(row[1] for row in expected if row[0] == "E") + 1
                    cursors = (sink.entries("E"), sink.iter_type("E"), sink.iter_merged(),
                               sink.blocks_for_time_range("E", "A", begin, end),
                               sink.iter_time_range("E", "A", begin, end),
                               sink.trade_slice(1, 1), sink.equity_page(0, 1))
                    for cursor in cursors:
                        self.assertEqual(cursor.file_verification, state)
                        self.assertEqual(next(cursor).file_verification, state)
                        cursor.close()
                    # An empty iterator must still carry state.
                    empty = sink.trade_slice(1000000, 1000001)
                    self.assertEqual(empty.file_verification, state)
                    self.assertEqual(list(empty), [])
                    page = sink.trade_rank_page("pnl", size=1)
                    self.assertEqual(page.file_verification, state)
                    self.assertEqual(page.rows.file_verification, state)
                    self.assertEqual(page.rows[0].file_verification, state)
                    self.assertEqual(page.total, sum(row[0] == "T" for row in expected))
                    buckets = sink.equity_summary(1)
                    self.assertEqual(buckets.file_verification, state)
                    self.assertEqual(buckets[0].file_verification, state)
                    self.assertEqual(buckets[0].mtm.file_verification, state)
                    self.assertEqual(buckets[0].points.file_verification, state)
                    if full:
                        self.assertEqual(sink.index_page("E", 0).file_verification, state)
                    else:
                        with self.assertRaisesRegex(reader.SinkFormatError, "no physical index pages"):
                            sink.index_page("E", 0)
                        # Unsupported access revokes a prior verified receipt.
                        if verified:
                            self.assertEqual(sink.verification, "UNVERIFIED")
                            sink.verify()
                    self.assertEqual(sink.complete, verified)

        # The source protocol also permits opaque immutable generation tokens.
        _, data = fixture(force_full=False)
        source = Source(data)
        source.generation = frozenset(("immutable", "generation"))
        with reader.open(source) as sink:
            self.assertEqual(sink.generation, source.generation)
            self.assertEqual(sink.generation.file_verification, "UNVERIFIED")
            sink.verify()
            self.assertEqual(sink.generation.file_verification, "verified_whole_file")

    def test_resealed_compact_digest_and_counts_open_unverified_then_refuse(self):
        _, data = fixture(force_full=False)
        for mutation in ("logical", "finite-count", "raw-count"):
            wire = Wire(data)
            if mutation == "logical":
                wire.data[wire.tail + 56] ^= 1
                wire.seal_root()
                message = "decoded logical SHA"
            elif mutation == "finite-count":
                struct.pack_into("<Q", wire.data, wire.root + 88, 3)
                wire.seal_root()
                message = "finite_e_count"
            else:
                for tag, index in (("E", 0), ("I", 7)):
                    struct.pack_into("<Q", wire.data, wire.root + 24 + index * 8, 5)
                    struct.pack_into("<I", wire.data, wire.descriptor_offset(tag) + 16, 5)
                wire.seal_table(0)
                message = "header"
            with self.subTest(mutation=mutation), reader.open(bytes(wire.data)) as sink:
                self.assertEqual(sink.logical_sha256, "UNVERIFIED")
                self.assertEqual(sink.counts().file_verification, "UNVERIFIED")
                self.assertEqual(sink.finite_e_count.file_verification, "UNVERIFIED")
                self.assertEqual(sink.root_length.file_verification, "UNVERIFIED")
                self.assertEqual(sink.summary_json_bytes().file_verification, "UNVERIFIED")
                self.assertEqual(sink.head()["summary"]["report_sink"]["sha256"], "UNVERIFIED")
                with self.assertRaises(reader.UnverifiedLogicalIdentity):
                    sink.require_logical_sha256()
                with self.assertRaisesRegex(reader.SinkFormatError, message):
                    sink.verify()
                self.assertFalse(sink.complete)
                self.assertEqual(sink.logical_identity_state, "UNVERIFIED")
                self.assertEqual(sink.counts_verification, "UNVERIFIED")
                output = io.BytesIO()
                with self.assertRaises(reader.SinkFormatError):
                    sink.to_report_json(output)
                self.assertEqual(output.getvalue(), b"")

    def test_canonical_profile_boundaries_and_forced_full(self):
        for n, t, caps, expected in (
                (10979, 1, None, "COMPACT"), (10980, 1, None, "FULL"),
                (1200, 3313, None, "COMPACT"), (1200, 3314, None, "FULL"),
                (127, 0, {"E": 1}, "COMPACT"), (128, 0, {"E": 1}, "FULL")):
            with self.subTest(n=n, t=t, caps=caps):
                rows = population_rows(n, t)
                data = encoder.encode_bytes(header(), rows, metadata("boundary", rows),
                                            block_records=caps, compress=False)
                with reader.open(data) as sink:
                    self.assertEqual(sink.profile, expected)
                    self.assertEqual(sink.root_length, 320 if expected == "COMPACT" else 4096)
                    self.assertEqual(len(sink.descriptor("E", 0).wire), 96 if expected == "COMPACT" else 512)
                    sink.verify()
        rows = [["E", i, None, None] for i in range(10980)]
        data = encoder.encode_bytes(header(), rows, metadata("raw-null"), compress=False)
        with reader.open(data) as sink:
            sink.verify()
            self.assertEqual((sink.profile, sink.counts()["E"], sink.finite_e_count), ("FULL", len(rows), 0))
        rows = [report_trade(1)]
        for full in (False, True):
            data = encoder.encode_bytes(header(), rows, metadata("one", rows), compress=False, force_full=full)
            with reader.open(data) as sink:
                sink.verify()
                self.assertEqual(sink.profile, "FULL" if full else "COMPACT")
                for table in sink.tables[8:18]:
                    self.assertEqual((table.count, table.flags, table.byte_length),
                                     (1, 1, 4100) if full else (0, 0, 0))

    def test_same_population_has_identical_queries_across_profiles(self):
        rows = query_rows()
        summary = metadata("both-profiles", rows, selected=True)
        variants = [encoder.encode_bytes(header(), rows, summary, block_records={"E": 7, "T": 4, "I": 11},
                                         compress=False, force_full=full) for full in (False, True)]
        with reader.open(variants[0]) as compact, reader.open(variants[1]) as full:
            compact.verify()
            full.verify()
            self.assertTrue(compact.compact)
            self.assertFalse(full.compact)
            self.assertEqual(compact.logical_sha256, full.logical_sha256)
            self.assertEqual(rows_bytes(compact.iter_merged()), b"".join(map(v1_line, normalized(rows, header()))))
            self.assertEqual(rows_bytes(compact.iter_merged()), rows_bytes(full.iter_merged()))
            for key in ("n", "exit", "pnl", "pct", "ru", "dd"):
                for direction in ("asc", "desc"):
                    for page in (-1, 1, 3, 999):
                        a = compact.trade_rank_page(key, direction, page=page, size=6)
                        b = full.trade_rank_page(key, direction, page=page, size=6)
                        self.assertEqual(rows_bytes(a.rows), rows_bytes(b.rows))
                        self.assertEqual((a.total, a.page_count, a.effective_page, a.from_row, a.to_row),
                                         (b.total, b.page_count, b.effective_page, b.from_row, b.to_row))
            for first, last in ((1, 1), (2, 11), (4, 8), (35, 100), (100, 101)):
                self.assertEqual(rows_bytes(compact.trade_slice(first, last)), rows_bytes(full.trade_slice(first, last)))
            for buckets in (1, 5, 23, 1200):
                for window in ({}, {"t0": 1700000000000, "t1": 1700000660000}):
                    a, b = compact.equity_summary(buckets, **window), full.equity_summary(buckets, **window)
                    self.assertEqual([(v.begin, v.end, tuple(p.pack() for p in v.points)) for v in a],
                                     [(v.begin, v.end, tuple(p.pack() for p in v.points)) for v in b])
                    for left, right in zip(a, b):
                        for name in ("mtm", "book"):
                            witnesses = lambda value: tuple(witness_tuple(getattr(value, key))
                                                            for key in ("first", "minimum", "maximum", "last"))
                            self.assertEqual(witnesses(getattr(left, name)), witnesses(getattr(right, name)))
                            self.assertEqual(getattr(left, name).finite_count, getattr(right, name).finite_count)
            for begin, end in ((0, 1), (1, 10), (7, 18), (20, 100), (100, 101)):
                self.assertEqual(rows_bytes(compact.equity_page(begin, end)), rows_bytes(full.equity_page(begin, end)))

    def test_discovery_is_tail_first_suffix_then_one_index_request_with_reuse(self):
        rows = [["E", i, float(i), 0.0] for i in range(127)]
        for full in (False, True):
            data = encoder.encode_bytes(header(), rows, metadata("discovery"), block_records={"E": 1},
                                        compress=False, force_full=full)
            source = Source(data)
            with reader.open(source) as sink:
                self.assertEqual(source.reads[0], (len(data) - 4256, 4256))
                if full:
                    self.assertFalse(any(table.offset <= offset < table.offset + table.byte_length
                                         for table in sink.tables if table.pages for offset, _ in source.reads))
                    source.reads.clear()
                    sink.descriptor("E", 0)
                    self.assertEqual(source.reads, [(sink.tables[0].offset, 65536)])
                else:
                    table_offset = struct.unpack_from("<Q", data, sink.root_offset + 160)[0]
                    self.assertEqual(source.reads[4], (table_offset, 96 * 128))
                    source.reads.clear()
                    sink.descriptor("E", 126)
                    self.assertEqual(source.reads, [])
                    with self.assertRaisesRegex(reader.SinkFormatError, "no physical index pages"):
                        sink.index_page("E", 0)
        _, tiny = fixture(force_full=False)
        source = Source(tiny)
        with reader.open(source) as sink:
            self.assertEqual(source.reads[0], (max(0, len(tiny) - 4256), min(len(tiny), 4256)))
            table_offset = struct.unpack_from("<Q", tiny, sink.root_offset + 160)[0]
            # The complete small table is already authenticated from the suffix.
            self.assertNotIn((table_offset, 192), source.reads[1:])
        source = Source(tiny)
        with reader.open(source, profile_hint="COMPACT") as sink:
            self.assertEqual(source.reads[0], (len(tiny) - 480, 480))
            self.assertEqual(source.reads[4], (table_offset, 192))
            sink.verify()

    def test_short_transport_read_cannot_terminate_a_nonfinal_full_page(self):
        rows = [["E", i, float(i), 0.0] for i in range(121)]
        data = encoder.encode_bytes(header(), rows, metadata("nonfinal-read"), block_records={"E": 1},
                                    compress=False, force_full=True)
        source = Source(data)
        with reader.open(source) as sink:
            original = source.readRange
            offset = sink.tables[0].offset

            def shortened(start, length, abortSignal=None):
                result = original(start, length, abortSignal)
                return result[:-512] if start == offset else result

            source.readRange = shortened
            with self.assertRaisesRegex(reader.SinkFormatError, "short range read"):
                sink.descriptor("E", 0)
            self.assertEqual(source.reads[-1], (offset, 65536))
            self.assertFalse(sink.complete)

    def test_compact_ceilings_extra_pages_sha_and_full_absence_are_refusals(self):
        _, data = fixture(force_full=False)
        for field, value in ((24, 10980), (32, 3314)):
            wire = Wire(data)
            struct.pack_into("<Q", wire.data, wire.root + field, value)
            counts = struct.unpack_from("<7Q", wire.data, wire.root + 24)
            struct.pack_into("<Q", wire.data, wire.root + 80, sum(counts))
            wire.seal_root()
            with self.assertRaisesRegex(reader.SinkFormatError, "COMPACT population"):
                reader.open(bytes(wire.data))
        wire = Wire(data)
        struct.pack_into("<I", wire.data, wire.root + 20, 129)
        wire.seal_root()
        with self.assertRaisesRegex(reader.SinkFormatError, "COMPACT population"):
            reader.open(bytes(wire.data))
        wire = Wire(data)
        wire.data[wire.root:wire.root] = b"IDX2"
        wire.root += 4
        wire.tail += 4
        struct.pack_into("<Q", wire.data, wire.tail, wire.root)
        struct.pack_into("<Q", wire.data, wire.tail + 16, len(wire.data))
        wire.seal_root()
        with self.assertRaisesRegex(reader.SinkFormatError, "COMPACT descriptor table tiling"):
            reader.open(bytes(wire.data))
        wire = Wire(data)
        wire.data[wire.locator(0)[0] + 64] ^= 1
        wire.seal_root()
        with self.assertRaisesRegex(reader.SinkFormatError, "descriptor table SHA"):
            reader.open(bytes(wire.data))
        for name, table in (("one-trade", 8), ("equity4", 18)):
            _, full_data = fixture(name)
            wire = Wire(full_data)
            struct.pack_into("<I", wire.data, wire.root + 512 + table * 64 + 28, 0)
            wire.seal_root()
            with self.assertRaisesRegex(reader.SinkFormatError, "PRESENT"):
                reader.open(bytes(wire.data))

    def test_id18_and_compact_scan_equal_brute_force_including_removed_extreme(self):
        case = next(case for case in cases() if case.name == "default-figure")
        for full in (False, True):
            data = encoder.encode_bytes(case.head, case.rows, case.summary, compress=False, force_full=full)
            source = Source(data)
            with reader.open(source) as sink:
                sink.verify()
                decoded = list(sink.iter_merged())
                entries = list(sink.entries("E"))
                source.reads.clear()
                QueryTests.check_envelope(self, sink, decoded, 1200)
                payload_reads = [(offset, size) for offset, size in source.reads
                                 if any(entry.offset <= offset < entry.offset + entry.length for entry in entries)]
                if full:
                    self.assertEqual(payload_reads, [])
                else:
                    self.assertTrue(payload_reads)
                # Raw E still includes the former maximum; the APP page is adjusted.
                raw = [row for row in decoded if row.tag == "E" and row.values[1] is not None]
                page = list(sink.equity_page(0, len(raw) + 1))
                expected = []
                for i, row in enumerate(raw):
                    time_ms, equity, profit = row.values
                    profit = 0.0 if profit is None else profit
                    if i == len(raw) - 1 and abs(profit) <= 1e-9:
                        profit = sink_folds(case.rows)["open_at_end_pnl"]
                    expected.append(["E", time_ms, equity, profit])
                self.assertEqual(rows_bytes(page), b"".join(map(v1_line, expected)))
                self.assertEqual(bits(raw[-1].values[2]), bits(0.0))
                self.assertEqual(bits(page[-1].values[2]), bits(sink_folds(case.rows)["open_at_end_pnl"]))

    def test_large_default_witness_union_has_identical_count_scope_in_both_profiles(self):
        rows = population_rows(10000, 1)
        summary = metadata("default-count-scope", rows)
        results = []
        for full in (False, True):
            data = encoder.encode_bytes(header(), rows, summary, compress=False, force_full=full)
            with reader.open(data) as sink:
                sink.verify()
                decoded = list(sink.iter_merged())
                QueryTests.check_envelope(self, sink, decoded, 1200)
                buckets = sink.equity_summary(1200)
                self.assertTrue(any(bucket.book.finite_count is None for bucket in buckets))
                results.append([(bucket.begin, bucket.end, bucket.mtm.finite_count, bucket.book.finite_count,
                                 tuple(point.pack() for point in bucket.points)) for bucket in buckets])
        self.assertEqual(results[0], results[1])


class RefusalTests(unittest.TestCase):
    def refuses(self, data, message=None):
        with self.assertRaisesRegex(reader.SinkFormatError, message or "."):
            with reader.open(bytes(data)) as sink:
                sink.verify()

    def test_truncated_extra_missing_and_wrong_tail(self):
        _, data = fixture("empty0")
        for cut in (0, 1, 11, 159, len(data) - 160, len(data) - 1):
            with self.subTest(cut=cut):
                self.refuses(data[:cut])
        self.refuses(data + b"\x00")
        self.refuses(data[:-1] + b"!")

    def test_head_magic_version_fixed_schema_duplicate_key_and_crc(self):
        _, data = fixture()
        for before, after in ((b"\x89PFS2", b"\x88PFS2"), (b'"version":2', b'"version":3'),
                              (b'"unix_ms"', b'"unix_Ms"'), (b'"engine":', b'"schema":')):
            with self.subTest(before=before):
                wire = Wire(data)
                pos = wire.data.index(before)
                wire.data[pos:pos + len(before)] = after
                wire.seal_head()
                self.refuses(wire.data)
        wire = Wire(data)
        wire.data[12] ^= 1
        self.refuses(wire.data, "head CRC")

    def test_root_tail_digests_crc_flags_reserved_lengths_and_unsafe_offsets(self):
        _, data = fixture()
        for field in (24, 56, 88):
            with self.subTest(tail_digest=field):
                wire = Wire(data)
                wire.data[wire.tail + field] ^= 1
                self.refuses(wire.data, "SHA")
        for relative in (4, 6, 416, 428, 1728, 4092):
            with self.subTest(root_field=relative):
                wire = Wire(data)
                wire.data[wire.root + relative] ^= 1
                # Repair outer root SHA, keeping a bad root CRC for that row.
                if relative != 4092:
                    wire.seal_root()
                else:
                    wire.data[wire.tail + 24:wire.tail + 56] = hashlib.sha256(
                        wire.data[wire.root:wire.root + 4096]).digest()
                self.refuses(wire.data)
        wire = Wire(data)
        wire.data[wire.root + 6] |= 2
        wire.seal_root()
        self.refuses(wire.data, "root magic/version/flags")
        for field in (0, 16):
            wire = Wire(data)
            struct.pack_into("<Q", wire.data, wire.tail + field, 1 << 53)
            self.refuses(wire.data, "unsafe")
        for field in (8, 12, 120):
            wire = Wire(data)
            wire.data[wire.tail + field] ^= 2
            self.refuses(wire.data)

    def test_summary_missing_bad_crc_bad_digest_and_duplicate_json(self):
        case, data = fixture()
        for relative in (0, 4, 8, 12, 16, 20, 52):
            wire = Wire(data)
            summary = struct.unpack_from("<Q", wire.data, wire.root + 16)[0]
            wire.data[summary + relative] ^= 1
            self.refuses(wire.data)
        with self.assertRaises(reader.SinkFormatError):
            reader._json(b'{"a":1,"a":2}')
        for value in (b'{"a":"\\ud800"}', b'{"a":Infinity}', b'\xef\xbb\xbf{}',
                      b"[" * 65 + b"0" + b"]" * 65, b'{"a":"\xff"}'):
            with self.subTest(json=value[:20]):
                with self.assertRaises(reader.SinkFormatError):
                    reader._json(value)
        for before, after in ((b'"metrics"', b'"tradesx"'), (b'"metrics"', b'"summary"'),
                              (b'"elapsed_seconds":-0.0', b'"elapsed_seconds":NaN ')):
            wire = Wire(data)
            content = json.dumps(case.summary, separators=(",", ":")).encode("ascii")
            wire.replace_summary(content.replace(before, after, 1))
            self.refuses(wire.data)

    def test_summary_frame_and_lengths_refuse_before_codec_allocation_in_both_profiles(self):
        original = builtins.__import__

        def no_codec(name, *args, **kwargs):
            if name in ("pf_sink2_zstd", "compression", "zstandard"):
                raise AssertionError("SUMMARY decoder imported before framing/caps were checked")
            return original(name, *args, **kwargs)

        for full in (False, True):
            case, data = fixture(force_full=full)
            content = json.dumps(case.summary, separators=(",", ":")).encode("ascii")
            for name, frame, message in summary_frame_refusals(content):
                wire = Wire(data)
                wire.replace_summary_frame(frame)
                with self.subTest(full=full, mode=name), mock.patch("builtins.__import__", side_effect=no_codec):
                    with self.assertRaisesRegex(reader.SinkFormatError, message):
                        reader.open(bytes(wire.data))
            s_max, block = 1024 * 1024, 128 * 1024
            c_max = 9 + (s_max // block) * (block + 3)
            for field, value in ((4, 0), (4, c_max + 1),
                                 (8, 0), (8, s_max + 1), (16, 1)):
                wire = Wire(data)
                offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
                struct.pack_into("<I", wire.data, offset + field, value)
                wire.seal_root()
                with self.subTest(full=full, field=field, value=value):
                    with mock.patch("builtins.__import__", side_effect=no_codec):
                        with self.assertRaisesRegex(reader.SinkFormatError, "summary"):
                            reader.open(bytes(wire.data))
            # Keep the complete file tiled, but disagree with stored_len by 1.
            wire = Wire(data)
            offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
            c = struct.unpack_from("<I", wire.data, offset + 4)[0]
            struct.pack_into("<I", wire.data, offset + 4, c - 1)
            wire.seal_root()
            with mock.patch("builtins.__import__", side_effect=no_codec):
                with self.assertRaisesRegex(reader.SinkFormatError, "summary header"):
                    reader.open(bytes(wire.data))
            wire = Wire(data)
            struct.pack_into("<I", wire.data, wire.root + (16 if wire.compact else 24), 52 + c_max + 1)
            wire.seal_root()
            with mock.patch("builtins.__import__", side_effect=no_codec):
                with self.assertRaisesRegex(reader.SinkFormatError, "summary header"):
                    reader.open(bytes(wire.data))

    def test_summary_admission_stops_at_fixed_header_or_frame_header_before_any_payload_read(self):
        s_max, block = 1024 * 1024, 128 * 1024
        c_max = 9 + (s_max // block) * (block + 3)
        for full in (False, True):
            case, data = fixture(force_full=full)
            profile = "FULL" if full else "COMPACT"
            for field, value in ((8, s_max + 1), (4, c_max + 1)):
                wire = Wire(data)
                offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
                struct.pack_into("<I", wire.data, offset + field, value)
                source = Source(bytes(wire.data))
                with self.subTest(profile=profile, field=field, value=value):
                    with mock.patch.object(reader, "_inflate", side_effect=AssertionError("unchecked allocation")):
                        with mock.patch.object(reader, "_json", side_effect=AssertionError("unchecked JSON")):
                            with self.assertRaises(reader.SummaryLimit) as refused:
                                reader.open(source, profile_hint=profile)
                    self.assertEqual(refused.exception.code, "summary_limit")
                    self.assertEqual(source.reads, [(len(data) - wire.root_length - 160, wire.root_length + 160),
                                                    (offset, 52)])
            # Malformed SUM2 flags precede the typed length checks.
            wire = Wire(data)
            offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
            struct.pack_into("<I", wire.data, offset + 8, s_max + 1)
            struct.pack_into("<I", wire.data, offset + 16, 1)
            source = Source(bytes(wire.data))
            with self.assertRaisesRegex(reader.SinkFormatError, "invalid summary header") as refused:
                reader.open(source, profile_hint=profile)
            self.assertNotIsInstance(refused.exception, reader.SummaryLimit)
            self.assertEqual(source.reads, [(wire.root, wire.root_length + 160), (offset, 52)])
            content = json.dumps(case.summary, separators=(",", ":")).encode("ascii")
            raw = summary_raw_frame(content)
            hlen = 6 if len(content) < 256 else 7 if len(content) < 65792 else 9
            frames = (
                ("window", b"\x28\xb5\x2f\xfd\x80\x58" + len(content).to_bytes(4, "little") + raw[hlen:],
                 "window exceeds", True, 10),
                # These multi-fault headers test first-failure precedence,
                # independently of 17.4's single-fault generated controls.
                ("single-FCS-before-window", b"\x28\xb5\x2f\xfd\xa0" +
                 (s_max + 1).to_bytes(4, "little") + raw[hlen:], "content size", False, 9),
                ("missing-size-before-window", b"\x28\xb5\x2f\xfd\x00\x58" + raw[hlen:],
                 "content size", False, 6),
                ("FCS-before-window", b"\x28\xb5\x2f\xfd\x80\x58" +
                 (len(content) + 1).to_bytes(4, "little") + raw[hlen:], "content size", False, 10),
                ("mismatched-size", summary_raw_frame(content + b"x"), "content size", False, hlen),
            )
            for name, frame, message, limited, frame_header_length in frames:
                wire = Wire(data)
                wire.replace_summary_frame(frame)
                offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
                source = Source(bytes(wire.data))
                with self.subTest(profile=profile, frame=name):
                    with mock.patch.object(reader, "_inflate", side_effect=AssertionError("unchecked allocation")):
                        with mock.patch.object(reader, "_json", side_effect=AssertionError("unchecked JSON")):
                            with self.assertRaisesRegex(reader.SinkFormatError, message) as refused:
                                reader.open(source, profile_hint=profile)
                    self.assertEqual(isinstance(refused.exception, reader.SummaryLimit), limited)
                    if limited:
                        self.assertEqual(refused.exception.code, "summary_limit")
                    self.assertEqual(source.reads[1:], [(offset, 52), (offset + 52, 5),
                                                        (offset + 57, frame_header_length - 5)])
            # A valid frame header must never be read before the exact
            # summary-to-next-region range is checked (5.2 step 1).
            wire = Wire(data)
            if wire.compact:
                struct.pack_into("<Q", wire.data, wire.root + 160,
                                 struct.unpack_from("<Q", wire.data, wire.root + 160)[0] + 1)
            else:
                locator = wire.root + 512
                struct.pack_into("<Q", wire.data, locator,
                                 struct.unpack_from("<Q", wire.data, locator)[0] + 1)
            wire.seal_root()
            source = Source(bytes(wire.data))
            with mock.patch.object(reader, "_check_zstd_header", side_effect=AssertionError("range not checked")):
                with self.assertRaisesRegex(reader.SinkFormatError, "summary range"):
                    reader.open(source, profile_hint=profile)
            offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
            self.assertEqual(source.reads, [(wire.root, wire.root_length + 160), (offset, 52)])

    def test_summary_single_fault_boundaries_have_valid_frames_ranges_and_repaired_controls(self):
        s_max, block = 1024 * 1024, 128 * 1024
        c_max = 9 + (s_max // block) * (block + 3)
        for full in (False, True):
            case, data = fixture(force_full=full)
            summary = json.loads(json.dumps(case.summary))
            summary["input"]["padding"] = ""
            base = json.dumps(summary, separators=(",", ":")).encode("ascii")
            summary["input"]["padding"] = "x" * (s_max - len(base))
            content = json.dumps(summary, separators=(",", ":")).encode("ascii")
            self.assertEqual(len(content), s_max)
            raw = summary_raw_frame(content)
            self.assertEqual(len(raw), c_max)
            enlarged = content.replace(b'"padding":"', b'"padding":"x', 1)
            j_frame = summary_window_frame(enlarged)
            self.assertLessEqual(len(j_frame), c_max)
            stored_frame = raw[:4] + b"\x80\x50" + raw[5:]
            self.assertEqual(len(stored_frame), c_max + 1)
            window_frame = summary_window_frame(content)
            window_frame = window_frame[:5] + b"\x58" + window_frame[6:]
            controls = (("decoded", enlarged, j_frame, content, summary_window_frame(content)),
                        ("stored", content, stored_frame, content, raw),
                        ("window", content, window_frame, content, summary_window_frame(content)))
            for fault, decoded, frame, repaired_json, repaired_frame in controls:
                with self.subTest(full=full, fault=fault):
                    # Independently decode with only the window bound relaxed:
                    # FCS, frame consumption, payload, CRC/SHA inputs and JSON
                    # are valid; each generated admission control has one fault.
                    self.assertEqual(reader._inflate(frame, len(decoded), window_limit=2 * s_max), decoded)
                    self.assertEqual(reader._metadata(decoded, len([row for row in case.rows if row[0] == "T"]))[
                        "input"]["padding"], summary["input"]["padding"] + ("x" if fault == "decoded" else ""))
                    wire = Wire(data)
                    wire.replace_summary_frame(frame, decoded=decoded)
                    wire.seal_summary_identity(case.head, case.rows)
                    offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
                    source = Source(bytes(wire.data))
                    with mock.patch.object(reader, "_inflate", side_effect=AssertionError("payload allocation")):
                        with mock.patch.object(reader, "_json", side_effect=AssertionError("JSON allocation")):
                            with self.assertRaises(reader.SummaryLimit) as refused:
                                reader.open(source, profile_hint="FULL" if full else "COMPACT")
                    self.assertEqual(refused.exception.code, "summary_limit")
                    expected_reads = [(wire.root, wire.root_length + 160), (offset, 52)]
                    if fault == "window":
                        expected_reads.extend(((offset + 52, 5), (offset + 57, 5)))
                    self.assertEqual(source.reads, expected_reads)
                    # Repair just that admission fault and require the complete
                    # same-input file to verify under the normal reader bounds.
                    repaired = Wire(data)
                    repaired.replace_summary_frame(repaired_frame, decoded=repaired_json)
                    repaired.seal_summary_identity(case.head, case.rows)
                    with reader.open(bytes(repaired.data)) as sink:
                        sink.verify()
                        self.assertEqual(sink.summary_json_bytes(), content)

    def test_summary_decoded_crc_sha_and_schema_checked_after_a_valid_frame(self):
        for full in (False, True):
            case, data = fixture(force_full=full)
            content = json.dumps(case.summary, separators=(",", ":")).encode("ascii")
            for field, message in ((12, "summary CRC"), (20, "summary SHA")):
                wire = Wire(data)
                wire.replace_summary(content)  # valid standard frame, hashes from input JSON
                offset = struct.unpack_from("<Q", wire.data, wire.root + (8 if wire.compact else 16))[0]
                wire.data[offset + field] ^= 1
                wire.seal_root()
                with self.assertRaisesRegex(reader.SinkFormatError, message):
                    reader.open(bytes(wire.data))
            wire = Wire(data)
            wire.replace_summary(content.replace(b'"metrics"', b'"xetrics"', 1))
            with self.assertRaisesRegex(reader.SinkFormatError, "required fields"):
                reader.open(bytes(wire.data))
            # Mandatory FCS equals J, but the raw block actually produces J+1
            # or J-1. The decoder must enforce its J-sized destination.
            for changed in (content + b"x", content[:-1]):
                frame = bytearray(summary_raw_frame(changed))
                if len(content) < 256:
                    frame[5:6] = bytes((len(content),))
                elif len(content) < 65792:
                    frame[5:7] = (len(content) - 256).to_bytes(2, "little")
                else:
                    frame[5:9] = len(content).to_bytes(4, "little")
                wire = Wire(data)
                wire.replace_summary_frame(bytes(frame))
                with self.assertRaises(reader.SinkFormatError):
                    reader.open(bytes(wire.data))

    def test_column_flip_bad_directory_crc_unknown_codec_and_allocation_ceilings(self):
        _, data = fixture()
        wire = Wire(data)
        wire.data[wire.column_offset("E", 2)] ^= 1
        self.refuses(wire.data, "column CRC")
        for relative, value in ((0, 2), (1, 255), (2, 2), (3, 1), (4, 16777217), (8, 0xffffffff)):
            with self.subTest(directory_field=relative):
                wire = Wire(data)
                start = wire.block_offset("E") + 32 + 16 * 2
                if relative in (4, 8):
                    struct.pack_into("<I", wire.data, start + relative, value)
                else:
                    wire.data[start + relative] = value
                wire.seal_block("E")
                self.refuses(wire.data)
        wire = Wire(data)
        wire.data[wire.block_offset("E") + 28] ^= 1
        self.refuses(wire.data, "directory CRC")

    def test_resealed_descriptor_bad_ranges_rec0_digest_and_equity_witnesses(self):
        _, data = fixture("multiblock")
        for relative in (0, 8, 13, 16, 20, 24, 32, 64, 96, 104, 480):
            with self.subTest(descriptor_field=relative):
                wire = Wire(data)
                wire.data[wire.descriptor_offset("E") + relative] ^= 1
                wire.seal_table(0)
                self.refuses(wire.data)

    def test_block_header_flags_reserved_width_count_rec0_and_body_length(self):
        _, data = fixture()
        for relative in (0, 5, 6, 8, 12, 16, 24):
            with self.subTest(header_field=relative):
                wire = Wire(data)
                wire.data[wire.block_offset("E") + relative] ^= 1
                wire.seal_block("E")
                self.refuses(wire.data)

    def test_record_invariants_after_all_checksums_and_summaries_are_repaired(self):
        case, data = fixture("trades3")
        rows = [list(row[1:]) for row in case.rows]
        for row in rows:
            row[0] += 1
        wire = Wire(data)
        offset = wire.column_offset("T", 0)
        wire.data[offset] = 4  # DELTA first n=2, same byte length
        wire.reseal_decoded_rows("T", rows, case.head)
        self.refuses(wire.data, "T.n")

        case, data = fixture("outputs")
        rows = [list(row[1:]) for row in normalized(case.rows, case.head) if row[0] == "O"]
        rows[0][0] = 1
        wire = Wire(data)
        wire.data[wire.column_offset("O", 0)] = 2
        wire.reseal_decoded_rows("O", rows, case.head)
        self.refuses(wire.data, "O.row")

        case, data = fixture()
        rows = [list(row[1:]) for row in case.rows]
        times = [row[0] for row in rows][::-1]
        for row, time_ms in zip(rows, times):
            row[0] = time_ms
        encoding, flags, raw = encoder.encode_column(times, "i64")
        self.assertEqual((encoding, flags), (1, 0))
        wire = Wire(data)
        offset = wire.column_offset("E", 0)
        old_length = struct.unpack_from("<I", wire.data, wire.block_offset("E") + 32 + 8)[0]
        self.assertEqual(len(raw), old_length)
        wire.data[offset:offset + len(raw)] = raw
        wire.reseal_decoded_rows("E", rows, case.head)
        self.refuses(wire.data, "E time decreases")

    def test_interleave_multiplicity_and_unknown_tags_after_crc_repair(self):
        case, data = fixture()
        for tag, message in (("T", "multiplicities"), ("Z", "interleave tag")):
            wire = Wire(data)
            wire.data[wire.column_offset("I", 0)] = ord(tag)
            wire.reseal_decoded_rows("I", [(ord(tag),), (ord("E"),), (ord("E"),), (ord("E"),)], case.head)
            self.refuses(wire.data, message)

    def test_trade_side_domain_after_column_checksum_repair(self):
        case, data = fixture("trades3")
        rows = [list(row[1:]) for row in case.rows]
        rows[0][1] = "hack"
        wire = Wire(data)
        start = wire.column_offset("T", 1)
        wire.data[start + 3:start + 7] = b"hack"  # three PLAINSTR lengths precede the strings
        wire.reseal_decoded_rows("T", rows, case.head)
        self.refuses(wire.data, "invalid trade side")

    def test_canonical_v1_line_ceiling_after_valid_binary_checksums(self):
        head = header(extras=True)
        rows = [["X", 0, 0, "x" * 200000, 1.0]]
        changed = (0, 0, "\x00" * 200000, 1.0)
        for full in (False, True):
            with self.subTest(full=full):
                data = encoder.encode_bytes(head, rows, metadata("line-bound"),
                                            compress=False, force_full=full)
                wire = Wire(data)
                offset = wire.column_offset("X", 2)
                prefix = encoder.unsigned_varint(200000)
                self.assertEqual(wire.data[offset:offset + len(prefix)], prefix)
                wire.data[offset + len(prefix):offset + len(prefix) + 200000] = bytes(200000)
                wire.reseal_decoded_rows("X", [changed], head, input_rows=[["X", *changed]])
                self.refuses(wire.data, "v1 line exceeds")

    def test_resealed_wrong_counts_and_stream_hash_are_not_whole_file_success(self):
        _, data = fixture()
        wire = Wire(data)
        wire.data[wire.root + 96] ^= 1
        wire.seal_root(logical=True)
        with reader.open(bytes(wire.data)) as sink:
            self.assertFalse(sink.complete)
            self.assertEqual(sink.logical_identity_state, "UNVERIFIED")
            self.assertEqual(sink.logical_sha256.file_verification, "provisional")
            with self.assertRaises(reader.UnverifiedLogicalIdentity):
                sink.require_logical_sha256()
            with self.assertRaisesRegex(reader.SinkFormatError, "stream SHA"):
                sink.verify()
            self.assertFalse(sink.complete)
            self.assertEqual(sink.logical_identity_state, "UNVERIFIED")
        wire = Wire(data)
        struct.pack_into("<Q", wire.data, wire.root + 32, 5)
        struct.pack_into("<Q", wire.data, wire.root + 32 + 7 * 8, 5)
        wire.seal_root(logical=True)
        self.refuses(wire.data)

    def test_index_crc_leaf_proof_padding_and_locator_overlap(self):
        rows = [["E", i, float(i), 0.0] for i in range(121)]
        data = encoder.encode_bytes(header(), rows, metadata("proof"), block_records={"E": 1},
                                    compress=False, force_full=True)
        for relative in (0, 5, 6, 7, 8, 12, 16, 20, 24, 56, 88, 1080, 4092, 4607):
            with self.subTest(page_field=relative):
                wire = Wire(data)
                offset, _, _, _ = wire.locator(0)
                # Last page has one descriptor and ends at byte 4608.
                start = offset + 65536
                wire.data[start + relative] ^= 1
                if relative != 4092:
                    wire.page_crc(start)
                self.refuses(wire.data)
        wire = Wire(data)
        first_offset, _, _, _ = wire.locator(0)
        struct.pack_into("<Q", wire.data, wire.root + 512 + 7 * 64, first_offset)
        wire.seal_root()
        self.refuses(wire.data, "overlapping")

    def test_resealed_rank_duplicate_wrong_direction_null_tie_and_out_of_range(self):
        data = encoder.encode_bytes(header(), query_rows(), metadata("rank", query_rows()),
                                    compress=False, force_full=True)
        for mode in ("duplicate", "reverse", "out-of-range", "null-first", "reverse-tie"):
            with self.subTest(mode=mode):
                wire = Wire(data)
                table = 10  # pnl:asc
                offset, _, width, count = wire.locator(table)
                begin = offset + 4096
                values = [int.from_bytes(wire.data[begin + i * width:begin + (i + 1) * width], "little")
                          for i in range(count)]
                if mode == "duplicate":
                    values[1] = values[0]
                elif mode == "reverse":
                    values.reverse()
                elif mode == "out-of-range":
                    values[0] = count
                elif mode == "null-first":
                    values.insert(0, values.pop())
                else:
                    rows = [row for row in query_rows() if row[0] == "T"]
                    pair = next(i for i in range(count - 1)
                                if rows[values[i]][8] is not None and
                                rows[values[i]][8] == rows[values[i + 1]][8])
                    values[pair], values[pair + 1] = values[pair + 1], values[pair]
                for i, value in enumerate(values):
                    wire.data[begin + i * width:begin + (i + 1) * width] = value.to_bytes(width, "little")
                wire.seal_table(table)
                self.refuses(wire.data, "rank")

    def test_varint_delta_decimal_bool_utf8_null_and_trailing_body_refusals(self):
        invalid = [
            (b"\x80\x00\x01", 1, "i64", 1, 0),  # nonminimal
            (b"\x80" * 8 + b"\x01", 1, "i64", 1, 0),  # ninth byte
            (b"\xff" * 7 + b"\x10", 1, "i64", 1, 0),  # >2^53-1
            (b"\x00\x00", 1, "i64", 1, 0),  # g=0
            (b"\x01\x01", 1, "u64", 1, 0),  # negative u64
            (b"\x00\x01\x02", 1, "i64", 1, 0),  # trailing value
            (b"\x0d\x00\x01", 1, "f64?", 3, 0),  # decimal scale >12
            (b"\x02", 1, "bool", 4, 0),
            (b"\x02", 1, "f64?", 2, 1),  # unused bitmap bit
            (b"\x01", 1, "i64", 0, 1),  # bitmap on nonnullable
            (b"\x01\xff", 1, "str", 6, 0),
            (b"\x03\xed\xa0\x80", 1, "str", 6, 0),  # surrogate UTF8
            (b"\x02a", 1, "str", 6, 0),  # string overrun
            (b"\x01\x00\x01", 1, "str", 5, 0),  # invalid index
            (b"\x02\x01\x01aa\x00\x01", 2, "str", 5, 0),  # duplicate dictionary
            (b"\x02\x01\x01ab\x01\x00", 2, "str", 5, 0),  # first-seen order
            (struct.pack("<q", -(1 << 63)), 1, "i64", 0, 0),
            (struct.pack("<Q", (1 << 64) - 1), 1, "u64", 0, 0),
            (struct.pack("<d", float("nan")), 1, "f64?", 2, 0),
        ]
        for raw, n, wire, encoding, flags in invalid:
            with self.subTest(wire=wire, encoding=encoding, raw=raw[:16]):
                with self.assertRaises(reader.SinkFormatError):
                    reader._decode(raw, n, wire, encoding, flags)
        at_decimal_limit = b"\x00" + encoder.unsigned_varint((1 << 50) * 2) + b"\x01"
        with self.assertRaises(reader.SinkFormatError):
            reader._decode(at_decimal_limit, 1, "f64?", 3, 0)
        largest_decimal = b"\x00" + encoder.unsigned_varint(((1 << 50) - 1) * 2) + b"\x01"
        self.assertEqual(reader._decode(largest_decimal, 1, "f64?", 3, 0), [float((1 << 50) - 1)])
        self.assertEqual(reader._decode(encoder.unsigned_varint(1 << 52) + b"\x01", 1, "i64", 1, 0),
                         [1 << 51])
        huge = encoder.unsigned_varint((1 << 53) - 1)
        with self.assertRaises(reader.SinkFormatError):
            reader._decode(b"\x00" + huge + encoder.unsigned_varint((1 << 53) - 2), 2, "i64", 1, 0)

    def test_dictionary_expansion_and_many_tiny_columns_obey_decoded_U_budget(self):
        # Raw body is about 1 MiB, but seventeen repetitions exceed the 16 MiB
        # canonical column budget. No enormous expanded byte buffer is needed.
        size, n = 1048575, 17
        raw = b"\x01" + encoder.unsigned_varint(size) + b"x" * size + bytes(n)
        with self.assertRaisesRegex(reader.SinkFormatError, "decoded column budget"):
            reader._decode(raw, n, "str", 5, 0)

        # 20k all-null value columns have tiny encoded bodies. Their numeric
        # U minimum at 200 rows already exceeds 32 MiB, before any decode.
        head = header(outputs=True, slots=20000, constants=0)
        data = encoder.encode_bytes(head, [["O", 0, None, None, *([None] * 20000)]],
                                    metadata("wide-budget", outputs=True), compress=False, force_full=True)
        wire = Wire(data)
        desc = wire.descriptor_offset("O")
        block = wire.block_offset("O")
        struct.pack_into("<I", wire.data, desc + 16, 200)
        struct.pack_into("<I", wire.data, block + 12, 200)
        struct.pack_into("<Q", wire.data, wire.root + 32 + 2 * 8, 200)
        struct.pack_into("<Q", wire.data, wire.root + 32 + 7 * 8, 200)
        wire.seal_block("O")
        wire.seal_table(2)
        wire.seal_root(logical=True)
        self.refuses(wire.data, "decoded block budget")

    def test_zstd_window_checked_before_import_or_decoder_allocation(self):
        def oversized(tag, column, raw):
            if tag != "E" or column != 0:
                return None
            # Non-single-segment, FCS=4 bytes, window=2^25 >16MiB.
            return (b"\x28\xb5\x2f\xfd\x80\x78" + len(raw).to_bytes(4, "little") +
                    ((len(raw) << 3) | 1).to_bytes(3, "little") + raw)

        _, data = fixture(frame_factory=oversized)
        imported = []
        original = builtins.__import__

        def watch(name, *args, **kwargs):
            if name in ("pf_sink2_zstd", "compression", "zstandard"):
                imported.append(name)
                raise AssertionError("decoder imported before the window check")
            return original(name, *args, **kwargs)

        with reader.open(data) as sink:
            with mock.patch("builtins.__import__", side_effect=watch):
                with self.assertRaisesRegex(reader.SinkFormatError, "window exceeds"):
                    sink.verify()
        self.assertEqual(imported, [])

    def test_zstd_frame_structure_and_missing_library_are_explicit(self):
        raw = b"\x00\x01"
        frame = b"\x28\xb5\x2f\xfd\x20\x02\x11\x00\x00" + raw
        malformed = (frame + b"x", frame[:-1], frame[:4] + b"\x21" + frame[5:],
                     frame[:5] + b"\x03" + frame[6:], b"\x28\xb5\x2f\xfd\x00\x00",
                     frame[:6] + b"\x17\x00\x00" + raw)
        for data in malformed:
            with self.subTest(frame=data):
                with self.assertRaises(reader.SinkFormatError):
                    reader._check_zstd_frame(data, len(raw))
        original = builtins.__import__

        def missing(name, *args, **kwargs):
            if name in ("compression", "zstandard"):
                raise ImportError("deliberately absent")
            return original(name, *args, **kwargs)

        _, data = fixture()
        with mock.patch.dict(os.environ, {"PF_SINK_ZSTD_LIBRARY": ""}), \
                mock.patch("builtins.__import__", side_effect=missing):
            with self.assertRaisesRegex(reader.SinkFormatError, "needs.*zstandard"):
                reader._inflate(frame, len(raw))
            with self.assertRaisesRegex(reader.SinkFormatError, "needs.*zstandard"):
                reader.open(data)  # SUMMARY always needs a decoder, even with stored columns

    def test_generation_exact_ranges_cancellation_and_no_background_work(self):
        _, data = fixture()
        source = Source(data)
        cancelled = [False]
        with reader.open(source, abortSignal=lambda: cancelled[0]) as sink:
            cursor = sink.iter_merged()
            next(cursor)
            cancelled[0] = True
            before = len(source.reads)
            with self.assertRaises(reader.SinkCancelled):
                next(cursor)
            cursor.close()
            self.assertEqual(len(source.reads), before)
            cancelled[0] = False
            self.assertFalse(sink.complete)
            sink.verify()
            source.generation = "generation-two"
            with self.assertRaisesRegex(reader.SinkFormatError, "generation"):
                sink.summary()
            self.assertFalse(sink._complete)
        source = Source(data)
        source.after_read = lambda: setattr(source, "generation", "changed-during-read")
        with self.assertRaisesRegex(reader.SinkFormatError, "generation"):
            reader.open(source)
        source = Source(data)
        source.readRange = lambda offset, length, abortSignal: data[offset:offset + length - 1]
        with self.assertRaisesRegex(reader.SinkFormatError, "short range"):
            reader.open(source)

    def test_bad_range_data_revokes_a_previous_complete_claim(self):
        _, data = fixture()
        source = Source(data)
        with reader.open(source) as sink:
            sink.verify()
            entry = sink.descriptor("E", 0)
            wire = Wire(data)
            wire.data[wire.column_offset("E", 2)] ^= 1
            source.data = bytes(wire.data)  # adapter returns bad data under the old generation
            with self.assertRaisesRegex(reader.SinkFormatError, "column CRC"):
                sink.read_block(entry)
            self.assertFalse(sink.complete)

    def test_partial_column_sha_survives_valid_crc_and_does_not_poison_other_columns(self):
        case = next(case for case in cases() if case.name == "equity4")
        for full in (False, True):
            data = encoder.encode_bytes(case.head, case.rows, case.summary, compress=False, force_full=full)
            wire = Wire(data)
            # Change only the final delta: input's repeated last equity becomes
            # one additional step. Repair CRC, retain the original authenticated SHA.
            wire.data[wire.column_offset("E", 1) + 8] = 2
            values = [row[2] for row in case.rows]
            values[-1] = values[-2] + (values[-2] - values[-3])
            domain = b"PFS2-COL\0" + struct.pack("<BIQI", ord("E"), 1, 0, len(values))
            crc = zlib.crc32(domain + b"".join(b"\x01" + struct.pack("<d", v) for v in values))
            struct.pack_into("<I", wire.data, wire.block_offset("E") + 32 + 16 + 12, crc)
            wire.seal_block("E")
            wire.seal_root()
            with reader.open(bytes(wire.data)) as sink:
                entry = sink.descriptor("E", 0)
                good = sink.read_columns(entry, (0,))
                self.assertEqual(good.verified_columns, (0,))
                self.assertEqual(good.verification, "verified_columns")
                self.assertEqual(good.file_verification, "provisional" if full else "UNVERIFIED")
                self.assertEqual(good.columns[0], [row[1] for row in case.rows])
                with self.assertRaisesRegex(reader.SinkFormatError, "column SHA"):
                    sink.read_columns(entry, (1,))
                self.assertFalse(sink.complete)

    def test_summary_grammar_is_independently_enforced_by_encoder_and_reader(self):
        import copy
        good = metadata("grammar")
        variants = []
        for key in good:
            missing = copy.deepcopy(good)
            del missing[key]
            variants.append(json.dumps(missing, separators=(",", ":")).encode())
        for key, value in (("engine", 1), ("input", []), ("elapsed_seconds", True),
                           ("fingerprint", []), ("sink_folds", {}), ("trades", []),
                           ("report_sink", {}), ("figures", [])):
            bad = copy.deepcopy(good)
            bad[key] = value
            variants.append(json.dumps(bad, separators=(",", ":")).encode())
        raw = json.dumps(good, separators=(",", ":")).encode()
        variants.extend((b"[]", b"\xef\xbb\xbf" + raw, raw + b"\n", raw.replace(b'"metrics"', b'"summary"'),
                         raw.replace(b'"engine":"pineforge"', b'"engine":"\xff"'),
                         raw.replace(b'"engine":"pineforge"', b'"engine":"\\ud800"'),
                         raw[:-1] + b',"unsafe":9007199254740992}',
                         raw[:-1] + b',"overflow":1e309}', raw[:-1] + b',"bad":NaN}'))
        nested = 0
        for _ in range(64):
            nested = [nested]
        deep = copy.deepcopy(good)
        deep["future"] = nested
        variants.append(json.dumps(deep, separators=(",", ":")).encode())
        for content in variants:
            with self.subTest(content=content[:60]):
                with self.assertRaises(v1.SinkFormatError):
                    encoder.metadata_bytes(content, 0)
                with self.assertRaises(reader.SinkFormatError):
                    reader._metadata(content, 0)
        valid = copy.deepcopy(good)
        valid["future"] = {"z": [-0.0, "é雪😀", {"unrecognized": True}]}
        content = json.dumps(valid, separators=(",", ":")).encode()
        self.assertEqual(encoder.metadata_bytes(content, 0), content)
        self.assertEqual(v1_line(reader._metadata(content, 0)), v1_line(valid))
        # The producer's canonical left fold is also checked at sealing/verify.
        bad_folds = copy.deepcopy(good)
        bad_folds["sink_folds"]["open_at_end_pnl"] = -0.0
        with self.assertRaisesRegex(v1.SinkFormatError, "sink_folds"):
            encoder.encode_bytes(header(), [], bad_folds, compress=False)

    def test_summary_cap_and_depth_endpoints_are_real_boundary_cases(self):
        s_max = 1024 * 1024
        valid = metadata("summary-limits")
        valid["padding"] = ""
        base = json.dumps(valid, separators=(",", ":")).encode()
        valid["padding"] = "x" * (s_max - len(base))
        content = json.dumps(valid, separators=(",", ":")).encode()
        self.assertEqual(len(content), s_max)
        self.assertEqual(encoder.metadata_bytes(content, 0), content)
        for full in (False, True):
            data = encoder.encode_bytes(header(), [], content, compress=False, force_full=full)
            with reader.open(data) as sink:
                sink.verify()
                self.assertEqual(sink.summary()["padding"], valid["padding"])
        with mock.patch.object(encoder.json, "loads", side_effect=AssertionError("oversize reached JSON")):
            with self.assertRaises(encoder.SummaryLimit) as refused:
                encoder.metadata_bytes(content + b" ", 0)
        self.assertEqual(refused.exception.code, "summary_limit")
        oversized = content.replace(b'"padding":"', b'"padding":"x', 1)
        self.assertEqual(len(oversized), s_max + 1)
        for full in (False, True):
            output = io.BytesIO()
            with mock.patch.object(encoder, "compress_column", side_effect=AssertionError("oversize reached codec")):
                with self.assertRaises(encoder.SummaryLimit) as refused:
                    encoder.encode(header(), [], oversized, output, compress=False, force_full=full)
            self.assertEqual(refused.exception.code, "summary_limit")
            self.assertEqual(output.getvalue(), b"")
        value = 0
        for _ in range(63):
            value = [value]
        depth64 = metadata("depth64")
        depth64["future"] = value
        content = json.dumps(depth64, separators=(",", ":")).encode()
        self.assertEqual(encoder.metadata_bytes(content, 0), content)
        self.assertEqual(v1_line(reader._metadata(content, 0)), v1_line(depth64))

    def test_reference_writer_stored_refusal_appends_no_summary_index_root_or_success_tail(self):
        s_max, block = 1024 * 1024, 128 * 1024
        c_max = 9 + (s_max // block) * (block + 3)
        frame = mock.MagicMock()
        frame.__len__.return_value = c_max + 1
        for full in (False, True):
            output = io.BytesIO()
            with mock.patch.object(encoder._Encoder, "compact_index", side_effect=AssertionError("index after refusal")):
                with mock.patch.object(encoder._Encoder, "full_indexes", side_effect=AssertionError("index after refusal")):
                    with self.assertRaises(encoder.SummaryLimit) as refused:
                        encoder.encode(header(), [], metadata("stored-refusal"), output, force_full=full,
                                       summary_frame_factory=lambda _: frame)
            self.assertEqual(refused.exception.code, "summary_limit")
            partial = output.getvalue()
            self.assertEqual(len(partial), 16 + struct.unpack_from("<I", partial, 8)[0])
            self.assertFalse(partial.endswith(b"PFS2END\n"))
            with self.assertRaises(reader.SinkFormatError):
                reader.open(partial)


def witness_tuple(point):
    return None if point is None else (point.ordinal, point.time_ms, bits(point.mtm), bits(point.book))


def brute_series(points, which):
    finite = [point for point in points if point[which] is not None]
    minimum = min(finite, key=lambda point: point[which]) if finite else None
    maximum = max(finite, key=lambda point: point[which]) if finite else None
    selected = (points[0], minimum, maximum, points[-1])
    return len(finite), tuple(None if point is None else
                             (point[0], point[1], bits(point[2]), bits(point[3])) for point in selected)


class QueryTests(unittest.TestCase):
    def make_reader(self, *, caps=None):
        rows = query_rows()
        source = Source(encoder.encode_bytes(header(), rows, metadata("queries", rows, selected=True),
                                             block_records=caps or {"E": 7, "T": 4, "I": 11},
                                             compress=False, force_full=True))
        sink = reader.open(source)
        self.addCleanup(sink.close)
        # Reference (e): decode everything first, then independently filter/sort.
        decoded = list(sink.iter_merged())
        source.reads.clear()
        return sink, source, decoded

    def test_half_open_time_keys_null_clocks_and_duplicate_anchor_resume(self):
        sink, _, decoded = self.make_reader(caps={"E": 1, "T": 3, "I": 7})
        for tag, column, key in (("E", 0, "A"), ("T", 2, "A"), ("T", 3, "B")):
            rows = [row for row in decoded if row.tag == tag]
            for start, end in ((1700000000000, 1700000000001), (1700000060000, 1700000180000),
                               (0, 1), (1700000000000, 1700000000000), (0, (1 << 53) - 1)):
                with self.subTest(tag=tag, key=key, start=start, end=end):
                    expected = [row for row in rows if row.values[column] is not None and start <= row.values[column] < end]
                    self.assertEqual(rows_bytes(sink.iter_time_range(tag, key, start, end)), rows_bytes(expected))
                    entries = list(sink.blocks_for_time_range(tag, key, start, end))
                    nominated = {(entry.rec0, entry.n) for entry in entries}
                    for j, row in enumerate(rows):
                        if row in expected:
                            self.assertTrue(any(rec0 <= j < rec0 + n for rec0, n in nominated))
        anchor_time = 1700000000000
        expected = [row for row in decoded if row.tag == "E" and row.values[0] == anchor_time]
        self.assertEqual(len(expected), 2)
        self.assertEqual(rows_bytes(sink.iter_time_range("E", "A", anchor_time, anchor_time + 1)), rows_bytes(expected))
        self.assertEqual(rows_bytes(sink.iter_time_range("E", "A", anchor_time, anchor_time + 1,
                                                       after_ordinal=0)), rows_bytes(expected[1:]))
        case, data = fixture("outputs")
        with reader.open(data) as outputs:
            for tag, column, key in (("O", 1, "A"), ("O", 2, "B"), ("V", 3, "A"), ("V", 4, "B"), ("X", 0, "A")):
                rows = list(outputs.iter_type(tag))
                expected = [row for row in rows if row.values[column] is not None and -1 <= row.values[column] < 2]
                self.assertEqual(rows_bytes(outputs.iter_time_range(tag, key, -1, 2)), rows_bytes(expected))

    def test_inclusive_trade_slices_across_byte_and_record_boundaries(self):
        sink, _, decoded = self.make_reader()
        rows = [row for row in decoded if row.tag == "T"]
        for first, last in ((1, 1), (2, 11), (4, 8), (35, 100), (100, 101)):
            self.assertEqual(rows_bytes(sink.trade_slice(first, last)), rows_bytes(rows[first - 1:last]))
        input_rows = [trade(i + 1) for i in range(40)]
        data = encoder.encode_bytes(header(), input_rows, metadata("byte-slices", input_rows),
                                    block_bytes=4096, compress=False)
        with reader.open(data) as sink:
            all_rows = list(sink.iter_type("T"))
            self.assertEqual(rows_bytes(sink.trade_slice(13, 33)), rows_bytes(all_rows[12:33]))
            self.assertGreater(sink.tables[1].count, 1)

    def test_global_ranks_missing_last_equal_zero_ties_both_directions_and_clamp(self):
        sink, source, decoded = self.make_reader()
        rows = [row for row in decoded if row.tag == "T"]
        columns = {"n": 0, "exit": 3, "pnl": 7, "pct": 8, "ru": 9, "dd": 10}
        for key, column in columns.items():
            for direction in ("asc", "desc"):
                def compare(a, b):
                    x, y = a.values[column], b.values[column]
                    if key == "dd":
                        x = None if x is None else -abs(x) if x != 0.0 else 0.0
                        y = None if y is None else -abs(y) if y != 0.0 else 0.0
                    if x is None or y is None:
                        primary = (x is None) - (y is None)
                    else:
                        primary = (x > y) - (x < y)
                        if direction == "desc":
                            primary = -primary
                    return primary or ((a.values[0] > b.values[0]) - (a.values[0] < b.values[0]))

                expected = sorted(rows, key=cmp_to_key(compare))
                for page in (-4, 1, 2, 1000):
                    with self.subTest(key=key, direction=direction, page=page):
                        source.reads.clear()
                        actual = sink.trade_rank_page(key, direction, page=page, size=6)
                        pages = math.ceil(len(rows) / 6)
                        effective = min(max(page, 1), pages)
                        self.assertEqual((actual.total, actual.page_count, actual.effective_page),
                                         (len(rows), pages, effective))
                        self.assertEqual(rows_bytes(actual.rows), rows_bytes(expected[(effective - 1) * 6:effective * 6]))
                        block_offsets = {entry.offset for entry in sink.entries("T")}
                        block_fetches = [offset for offset, size in source.reads if size == 32 and offset in block_offsets]
                        self.assertEqual(len(block_fetches), len(set(block_fetches)), "fetch each selected block once")
        _, data = fixture("empty0")
        with reader.open(data) as empty:
            self.assertEqual(empty.trade_rank_page("pnl", "desc", page=100, size=2),
                             reader.TradePage((), 0, 1, 1))

    def check_envelope(self, sink, decoded, buckets, t0=None, t1=None):
        raw_e = [row for row in decoded if row.tag == "E"]
        finite_e = [(ordinal, row) for ordinal, row in enumerate(raw_e) if row.values[1] is not None]
        folds = sink_folds([[row.tag, *row.values] for row in decoded])
        points, begin = [], 0
        for position, (ordinal, row) in enumerate(finite_e):
            time_ms, equity, profit = row.values
            profit = 0.0 if profit is None else profit
            if position == len(finite_e) - 1 and folds["open_at_end_count"] and abs(profit) <= 1e-9:
                profit = folds["open_at_end_pnl"]
            book = equity - profit
            if not math.isfinite(book):
                book = None
            if t0 is not None and time_ms < t0:
                begin += 1
            if t0 is None or t0 <= time_ms < t1:
                points.append((ordinal, time_ms, equity, book))
        actual = sink.equity_summary(buckets, **({} if t0 is None else {"t0": t0, "t1": t1}))
        count = min(buckets, len(points))
        self.assertEqual(len(actual), count)
        for j, bucket in enumerate(actual):
            part = points[j * len(points) // count:(j + 1) * len(points) // count]
            self.assertEqual((bucket.begin, bucket.end),
                             (begin + j * len(points) // count, begin + (j + 1) * len(points) // count))
            selected = set()
            for name, which in (("mtm", 2), ("book", 3)):
                finite, witnesses = brute_series(part, which)
                series = getattr(bucket, name)
                observed = tuple(witness_tuple(getattr(series, key)) for key in ("first", "minimum", "maximum", "last"))
                self.assertEqual(observed, witnesses)
                if series.finite_count is not None:
                    self.assertEqual(series.finite_count, finite)
                else:
                    self.assertEqual((name, buckets), ("book", 1200))
                    self.assertGreater(len(part), len(bucket.points))
                selected.update(w[0] for w in witnesses if w is not None)
            self.assertEqual([point.ordinal for point in bucket.points], sorted(selected))

    def test_equal_sample_index_envelopes_null_overflow_and_earliest_equal_extreme(self):
        sink, _, decoded = self.make_reader()
        for buckets in (1, 2, 3, 5, 7, 23, 100, 1200):
            with self.subTest(buckets=buckets):
                self.check_envelope(sink, decoded, buckets)
                self.check_envelope(sink, decoded, buckets, 1700000000000, 1700000660000)
                self.check_envelope(sink, decoded, buckets, 0, 0)

    def test_aligned_summaries_fetch_no_payload_and_each_cut_block_decodes_once(self):
        rows = [["E", i, float(i % 5), float(i % 3)] for i in range(24)]
        data = encoder.encode_bytes(header(), rows, metadata("aligned"), block_records={"E": 4},
                                    compress=False, force_full=True)
        source = Source(data)
        with reader.open(source) as sink:
            decoded = list(sink.iter_type("E"))
            entries = list(sink.entries("E"))
            source.reads.clear()
            self.check_envelope(sink, decoded, 6)
            self.assertFalse(any(entry.offset <= offset < entry.offset + entry.length
                                 for entry in entries for offset, _ in source.reads))
            source.reads.clear()
            self.check_envelope(sink, decoded, 8)
            offsets = {entry.offset for entry in entries}
            fetches = [offset for offset, size in source.reads if size == 32 and offset in offsets]
            self.assertEqual(len(fetches), len(entries))
            self.assertEqual(len(fetches), len(set(fetches)))
        sink, source, decoded = self.make_reader(caps={"E": 64})
        self.check_envelope(sink, decoded, 19)
        entry = sink.descriptor("E", 0)
        self.assertEqual(source.reads.count((entry.offset, 32)), 1)


class ZstdTests(unittest.TestCase):
    def test_summary_window_at_s_max_is_admitted_in_both_profiles(self):
        zstd_required(self)
        for full in (False, True):
            case, data = fixture(force_full=full)
            content = json.dumps(case.summary, separators=(",", ":")).encode("ascii")
            raw = summary_raw_frame(content)
            hlen = 6 if len(content) < 256 else 7 if len(content) < 65792 else 9
            # Standard non-single-segment frame, window=2^20=S_MAX, FCS=J.
            frame = b"\x28\xb5\x2f\xfd\x80\x50" + len(content).to_bytes(4, "little") + raw[hlen:]
            wire = Wire(data)
            wire.replace_summary_frame(frame)
            with reader.open(bytes(wire.data)) as sink:
                sink.verify()
                self.assertEqual(sink.summary_json_bytes(), content)

    def test_summary_always_frames_at_head_level_and_bounds_raw_fallback(self):
        small = b"{}"
        raw_frame = summary_raw_frame(small)
        with mock.patch.object(encoder, "compress_column", return_value=raw_frame) as compress:
            self.assertEqual(encoder.compress_summary(small, 9), raw_frame)
            compress.assert_called_once_with(small, 9)  # no 64-byte/strict-shrink column policy
        summary = metadata("fallback")
        summary["input"]["padding"] = ""
        base = json.dumps(summary, separators=(",", ":")).encode("ascii")
        s_max, block = 1024 * 1024, 128 * 1024
        c_max = 9 + (s_max // block) * (block + 3)
        summary["input"]["padding"] = "x" * (s_max - len(base))
        raw = json.dumps(summary, separators=(",", ":")).encode("ascii")
        expected_frame = summary_raw_frame(raw)
        self.assertEqual(len(raw), s_max)
        self.assertEqual(len(expected_frame), c_max)
        expanded = mock.MagicMock()
        expanded.__len__.return_value = c_max + 1
        # Exactly the stored bound must retain the library frame. Only a
        # larger candidate triggers the fallback (9.1/12.1).
        with mock.patch.object(encoder, "compress_column", return_value=expected_frame):
            with mock.patch.object(encoder, "raw_summary_frame", side_effect=AssertionError("premature fallback")):
                self.assertEqual(encoder.compress_summary(raw, 3), expected_frame)
        for full in (False, True):
            with mock.patch.object(encoder, "compress_column", return_value=expanded) as compress:
                data = encoder.encode_bytes(header(), [], raw, compress=False, level=9, force_full=full)
                compress.assert_called_once_with(raw, 9)
            with reader.open(data) as sink:
                sink.verify()
                self.assertEqual((sink.summary_json_length, sink.summary_stored_length, sink.summary_length),
                                 (s_max, c_max, 52 + c_max))
                self.assertEqual(sink.summary_json_bytes(), raw)
                start = sink.summary_offset
                self.assertEqual(data[start + 52:start + sink.summary_length], expected_frame)
                self.assertEqual(struct.unpack_from("<I", data, start + 12)[0], zlib.crc32(raw))
                self.assertEqual(data[start + 20:start + 52], hashlib.sha256(raw).digest())

    def test_summary_raw_fallback_admits_sizes_through_s_max_and_refuses_its_successor(self):
        zstd_required(self)
        s_max, block = 1024 * 1024, 128 * 1024
        blocks = (s_max + block - 1) // block
        c_max = s_max + (4 + 1 + 4) + 3 * blocks
        # Independent route: eight payload-plus-header blocks and one header.
        stored_limit = (4 + 4 + 4 + 4 + 4 + 32) + 9 + (s_max // block) * (block + 3)
        self.assertEqual(52 + c_max, stored_limit)
        expanded = mock.MagicMock()
        expanded.__len__.return_value = c_max + 1
        for length in (1, 255, 256, 65791, 65792, block - 1, block, block + 1, s_max - 1, s_max):
            raw = b"x" * length
            with self.subTest(J=length):
                frame = encoder.raw_summary_frame(raw)
                header = 6 if length < 256 else 7 if length < 65792 else 9
                self.assertEqual(len(frame), length + header + 3 * ((length + block - 1) // block))
                self.assertEqual(frame, summary_raw_frame(raw))
                self.assertLessEqual(52 + len(frame), stored_limit)
                reader._check_zstd_frame(frame, length, window_limit=s_max, summary=True)
                with mock.patch.object(encoder, "compress_column", return_value=expanded):
                    self.assertEqual(encoder.compress_summary(raw, 3), frame)
                compressed = encoder.compress_summary(raw, 3)
                self.assertLessEqual(52 + len(compressed), stored_limit)
                self.assertEqual(reader._inflate(compressed, length, window_limit=s_max, summary=True), raw)
        with mock.patch.object(encoder, "bytearray", create=True,
                               side_effect=AssertionError("refused raw frame allocated")):
            with self.assertRaises(encoder.SummaryLimit) as refused:
                encoder.raw_summary_frame(b"x" * (s_max + 1))
        self.assertEqual(refused.exception.code, "summary_limit")
        with mock.patch.object(encoder, "compress_column", side_effect=AssertionError("oversize reached codec")):
            with self.assertRaises(encoder.SummaryLimit):
                encoder.compress_summary(b"x" * (s_max + 1), 3)

    def test_summary_recompression_preserves_decoded_identity_and_v1_report(self):
        case = next(case for case in cases() if case.name == "selected-anchor")
        raw = json.dumps(case.summary, separators=(",", ":")).encode("ascii")
        identities, stored = [], []
        for full in (False, True):
            for level, factory in ((1, None), (9, None), (3, summary_raw_frame)):
                data = encoder.encode_bytes(case.head, case.rows, raw, compress=False, force_full=full,
                                            level=level, summary_frame_factory=factory)
                with reader.open(data) as sink:
                    sink.verify()
                    self.assertEqual(sink.summary_json_bytes(), raw)
                    self.assertEqual(sink.summary_json_length, len(raw))
                    self.assertEqual(sink.summary_length, 52 + sink.summary_stored_length)
                    output = io.BytesIO()
                    result = sink.to_report_json(output)
                    self.assertEqual(output.getvalue(), case.report)
                    self.assertEqual(result["content_length"], len(case.report))
                    self.assertEqual(result["report_sha256"], hashlib.sha256(case.report).hexdigest())
                    identities.append(sink.require_logical_sha256())
                    stored.append(hashlib.sha256(data).digest())
        self.assertEqual(len(set(identities)), 1)
        self.assertGreater(len(set(stored)), 1)

    def test_size_grid_encodes_measured_compact_and_forced_full_next_to_model(self):
        zstd_required(self)
        from fractions import Fraction
        spec = importlib.util.spec_from_file_location("sink_size_report_grid", ROOT / "scripts" / "sink_size_report.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        result = module.measure_grid(n_values=(100,), fractions=(Fraction(1, 4),))
        self.assertEqual([(row["N"], row["T"], row["f"]) for row in result["grid"]],
                         [(100, t, 0.25) for t in (0, 1, 5)] +
                         [(n, t, 0.25) for _ in ("ordinary", "selected_report_view")
                          for n, t in ([(n, t) for n in (1, 2, 10, 40, 63, 64, 68, 69, 100, 127, 128)
                                        for t in range(4)] +
                                       [(n, 4) for n in (1, 63, 64, 127, 128)])])
        for row in result["grid"]:
            self.assertEqual(row["profile"], "COMPACT")
            self.assertGreater(row["actual"]["COMPACT"], 0)
            self.assertGreater(row["actual"]["FULL"], row["actual"]["COMPACT"])
            self.assertEqual(row["model"]["profile"], "COMPACT")
        self.assertTrue(module.render(result).startswith("PENDING-REAL-SUMMARY\n"))
        self.assertIn("MEASURED-FIXTURE", module.render(result))
        self.assertEqual(result["acceptance"], "PENDING")

    def test_raw_rle_multiblock_frames_and_exact_v1_export(self):
        zstd_required(self)
        case, data = fixture("equity4-zstd")
        with reader.open(data) as sink:
            sink.verify()
            self.assertEqual(rows_bytes(sink.iter_merged()), b"".join(map(v1_line, case.rows)))
            output = io.BytesIO()
            sink.to_v1(output)
            self.assertEqual(output.getvalue(), v1_file(case.head, case.rows))

    def test_optional_zstd_content_checksum_and_corruption(self):
        zstd_required(self)

        def checksum_frame(tag, column, raw):
            if tag != "E" or column != 0:
                return None
            if os.environ.get("PF_SINK_ZSTD_LIBRARY"):
                from pf_sink2_zstd import compress
                return compress(raw, 3, checksum=True)
            try:
                from compression import zstd
            except ImportError:
                import zstandard
                return zstandard.ZstdCompressor(write_content_size=True, write_checksum=True).compress(raw)
            return zstd.compress(raw, options={zstd.CompressionParameter.checksum_flag: 1,
                                               zstd.CompressionParameter.content_size_flag: 1})

        _, data = fixture(frame_factory=checksum_frame)
        with reader.open(data) as sink:
            sink.verify()
        wire = Wire(data)
        start = wire.column_offset("E", 0)
        stored = struct.unpack_from("<I", wire.data, wire.block_offset("E") + 32 + 8)[0]
        wire.data[start + stored - 1] ^= 1
        with self.assertRaisesRegex(reader.SinkFormatError, "zstd decode failed"):
            with reader.open(bytes(wire.data)) as sink:
                sink.verify()

    def test_real_zstd_and_recompression_stable_logical_identity(self):
        zstd_required(self)
        rows = [["E", i, float(i) * 0.25, float(i % 7)] for i in range(4096)]
        head, summary = header(), metadata("recompression")
        variants = [encoder.encode_bytes(head, rows, summary, level=level, compress=True) for level in (1, 3, 9)]
        variants.append(encoder.encode_bytes(head, rows, summary, block_records={"E": 17, "I": 99}, compress=False))
        identities = []
        for data in variants:
            with reader.open(data) as sink:
                sink.verify()
                identities.append(sink.logical_sha256)
                self.assertEqual(rows_bytes(sink.iter_merged()), b"".join(map(v1_line, rows)))
        self.assertEqual(len(set(identities)), 1)
        with reader.open(variants[0]) as sink:
            self.assertTrue(any(column.codec == 1 for entry in sink.entries("E") for column in sink.block_layout(entry)),
                            "real zstd row must exercise codec 1")

    def test_generated_full_vector_bundle_including_default_caps(self):
        zstd_required(self)
        from generate_sink_v2_reference_controls import generate
        with tempfile.TemporaryDirectory() as scratch:
            for case in cases(large=True):
                generate(case, Path(scratch))
                generate(case, Path(scratch), check=True)
                if case.name.startswith(("summary-max", "summary-incompressible")):
                    mask = json.loads((Path(scratch) / f"{case.name}.mask.json").read_bytes())
                    s_max, block = 1024 * 1024, 128 * 1024
                    c_max = 9 + (s_max // block) * (block + 3)
                    self.assertEqual(mask["summary"]["J"],
                                     len(json.dumps(case.summary, separators=(",", ":")).encode("ascii")))
                    self.assertEqual(mask["summary"]["admission"], "ACCEPTED")
                    if case.name.startswith("summary-incompressible"):
                        self.assertEqual(mask["summary"]["C"], c_max)
                        self.assertEqual(mask["summary"]["stored_region"], 52 + c_max)
                    else:
                        stem = Path(scratch) / f"{case.name}--summary-J{s_max + 1}-valid-json"
                        sidecar = json.loads(stem.with_suffix(".refusal.json").read_bytes())
                        self.assertEqual(sidecar["code"], "summary_limit")
                        self.assertEqual(sidecar["stage"], "open-fixed-summary-header")
                        damaged = Wire(stem.with_suffix(".pfs2").read_bytes())
                        offset = struct.unpack_from("<Q", damaged.data, damaged.root +
                                                    (8 if damaged.compact else 16))[0]
                        c, j = struct.unpack_from("<II", damaged.data, offset + 4)
                        self.assertEqual(j, s_max + 1)
                        self.assertLessEqual(c, c_max)
                        content = reader._inflate(bytes(damaged.data[offset + 52:offset + 52 + c]),
                                                  j, window_limit=s_max)
                        original = json.dumps(case.summary, separators=(",", ":")).encode("ascii")
                        self.assertEqual(content, original.replace(b'"summary_payload":"', b'"summary_payload":"x', 1))
                if case.name == "default-caps":
                    with reader.open(Path(scratch) / "default-caps.pfs2") as sink:
                        self.assertEqual([entry.n for entry in sink.entries("E")], [4096, 4096, 1808])
                        self.assertEqual([entry.n for entry in sink.entries("T")], [1024, 1024, 452])
                        self.assertEqual([entry.n for entry in sink.entries("I")], [12500])

    def test_compressed_columns_for_every_eligible_record_type(self):
        zstd_required(self)
        rows = []
        for i in range(128):
            rows.extend([
                ["E", i, float(i), 0.0],
                trade(i + 1, key=float(i % 4)),
                ["O", i, i, i + 1, 1.0, None],
                ["V", i, 0, i, i, i + 1, 0, 1, 1, -0.0, "ffffffffffffffff", "repeat"],
                ["X", i, i, "repeat", 1.0],
                ["H", "ffffffffffffffff"],
            ])
        rows.append(["K", None, -0.0])  # one value/column is below the codec threshold
        head = header(outputs=True, extras=True)
        data = encoder.encode_bytes(head, rows, metadata("zstd-types", rows, outputs=True), compress=True)
        with reader.open(data) as sink:
            sink.verify()
            self.assertEqual(rows_bytes(sink.iter_merged()), b"".join(map(v1_line, rows)))
            compressed = {tag for tag in "ETOVKXHI" for entry in sink.entries(tag)
                          if any(column.codec == 1 for column in sink.block_layout(entry))}
            self.assertEqual(compressed, set("ETOVXHI"))


class ToolTests(unittest.TestCase):
    @staticmethod
    def size_module():
        spec = importlib.util.spec_from_file_location("sink_size_report_delta3", ROOT / "scripts" / "sink_size_report.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_summary_projections_preserve_tokens_order_wrappers_and_unknowns(self):
        module = self.size_module()
        source = (b'{"engine":"pf","input":{"x":1e+01,"escaped":"\\u96ea"},'
                  b'"summary":{"x":-0,"input":{"stays":"Q"}},"future":{"a":[1,{"x":"a,b"}]},'
                  b'"outputs":{"manifest":{"z":2.00},"constants":[-0.0],"schema_version":"v1",'
                  b'"hlines":[{"price":1e-07}],"message_format":"p","manifest_sha256":"a"},'
                  b'"fingerprint":{"provenance":{"z":"\\ud83d\\ude00"}}}')
        q, m, unknown, has_m = module.summary_projections(source)
        self.assertEqual(q, (b'{"engine":"pf","summary":{"x":-0,"input":{"stays":"Q"}},'
                             b'"future":{"a":[1,{"x":"a,b"}]},'
                             b'"outputs":{"constants":[-0.0],"hlines":[{"price":1e-07}]}}'))
        self.assertEqual(m, (b'{"input":{"x":1e+01,"escaped":"\\u96ea"},'
                             b'"outputs":{"manifest":{"z":2.00},"schema_version":"v1",'
                             b'"message_format":"p","manifest_sha256":"a"},'
                             b'"fingerprint":{"provenance":{"z":"\\ud83d\\ude00"}}}'))
        self.assertEqual(unknown, ["future"])
        self.assertTrue(has_m)
        q, m, unknown, has_m = module.summary_projections(b'{"engine":"pf","provenance":{"extra":true}}')
        self.assertEqual(q, b'{"engine":"pf","provenance":{"extra":true}}')
        self.assertEqual((m, unknown, has_m), (b"{}", ["provenance"], False))

    def test_metadata_exclusion_never_erases_headers_or_comparable_frame(self):
        module = self.size_module()
        for c, cq, cm in ((1024, 512, 769), (100, 120, 200), (100, 20, 5), (1048609, 512, 1047586)):
            excluded = module.metadata_exclusion(c, cq, cm)
            self.assertGreaterEqual(excluded, 0)
            self.assertLessEqual(excluded, cm)
            self.assertLessEqual(excluded, max(0, c - cq))
            # Independently attribute only physical frame bytes. The complete
            # surrounding file remains charged, including the 52-byte header.
            whole = 947 + 52 + c + 320 + 160
            self.assertGreaterEqual(whole - excluded, 947 + 52 + min(c, cq) + 320 + 160)
        data = json.dumps(metadata("probes"), separators=(",", ":")).encode("ascii")
        with mock.patch.object(encoder, "compress_summary", side_effect=(b"q" * 20, b"m" * 30)):
            with mock.patch.object(module, "zstd_build", return_value={"id": "capture-build"}):
                probe = module.summary_probe(data, 25, 9, source_zstd_build="capture-build")
        self.assertEqual((probe["C_Q"], probe["C_M"], probe["C_meta"], probe["C_comparable"]), (20, 30, 5, 20))
        with mock.patch.object(encoder, "compress_summary", side_effect=(b"q" * 20, b"m" * 30)):
            with mock.patch.object(module, "zstd_build", return_value={"id": "different-build"}):
                probe = module.summary_probe(data, 25, 9, source_zstd_build="capture-build")
        self.assertEqual(probe["status"], "PENDING-SAME-BUILD")
        self.assertIsNone(probe["C_meta"])
        with mock.patch.object(encoder, "compress_summary", return_value=b"q" * 20) as compress:
            with mock.patch.object(module, "zstd_build", return_value={"id": "capture-build"}):
                probe = module.summary_probe(b'{"engine":"pf"}', 25, 3, source_zstd_build="capture-build")
        self.assertEqual((probe["J_M"], probe["C_M"], probe["C_meta"]), (2, 0, 0))
        compress.assert_called_once_with(b'{"engine":"pf"}', 3)

    def test_size_clauses_use_actual_bars_raw_streams_anchors_and_strict_equality(self):
        module = self.size_module()
        # Independent disjoint predicates, rather than reusing the report's
        # ordered branch. Include trades, optional records and selected anchors.
        for bars in (None, 0, 999, 1000, 100000):
            for raw_e in (0, 1, 2, 10, 40, 63, 64, 68, 69, 100, 127, 128, 999, 1000):
                for trades in (0, 1, 2, 3, 4, 250, 3314):
                    for optional in ({}, {tag: 1 for tag in "OVKXH"}):
                        predicates = {"(ii-a)": raw_e < 64 or raw_e < 128 and trades <= 3,
                                      "(i)": raw_e >= 128 or raw_e >= 64 and trades >= 4}
                        matching = [clause for clause, applies in predicates.items() if applies]
                        self.assertEqual(len(matching), 1)
                        clause = matching[0]
                        counts = {"E": raw_e, "T": trades, **optional}
                        baseline = 810 + 138 + len(str(raw_e)) + len(str(trades))
                        limit = baseline + {"(i)": -1, "(ii-a)": 3 * 1024}[clause]
                        with self.subTest(bars=bars, counts=counts):
                            # Full bytes can exceed the limit; only comparable
                            # bytes decide. All clauses must honor the exclusion.
                            row = module.size_comparison(baseline, limit + 512, 512, bars=bars, counts=counts,
                                                         profile="COMPACT", evidence="MEASURED",
                                                         anchors=min(1, raw_e))
                            self.assertEqual((row["clause"], row["integer_limit"], row["verdict"]),
                                             (clause, limit, "PASS"))
                            self.assertGreater(row["full_v2"], row["integer_limit"])
                            self.assertEqual(row["COMPARABLE_V2"], limit)
                            row = module.size_comparison(baseline, limit + 513, 512, bars=bars, counts=counts,
                                                         profile="COMPACT", evidence="MEASURED")
                            self.assertEqual(row["verdict"], "FAIL")
        for raw_e, trades, clause in ((0, 100, "(ii-a)"), (63, 0, "(ii-a)"),
                                     (64, 1, "(ii-a)"), (64, 4, "(i)"), (128, 0, "(i)")):
            row = module.size_comparison(950, 950, 0, bars=None, counts={"E": raw_e, "T": trades},
                                         profile="COMPACT", evidence="MEASURED")
            self.assertEqual(row["clause"], clause)
        row = module.size_comparison(950, 950, 0, bars=None, counts={"E": 64, "T": 0},
                                     profile="COMPACT", evidence="MEASURED")
        self.assertEqual(row["verdict"], "PASS")  # B no longer affects assignment.
        pending = module.size_comparison(950, 2503, None, bars=0, counts={},
                                        profile="COMPACT", evidence="MEASURED")
        self.assertEqual(pending["verdict"], "PENDING")
        self.assertEqual(pending["empty_complete_file"]["difference"], 1553)
        self.assertEqual(pending["empty_complete_file"]["verdict"], "PASS")

    def test_size_report_real_first_model_grid_extremes_and_pending_measurements(self):
        module = self.size_module()
        result = module.measure_grid(encode_fixtures=False)
        corners = 2 * (11 * 4 + 5) * 3
        self.assertEqual(len(result["grid"]), 7 * 3 * 3 + corners)
        self.assertEqual(len(result["extremes"]), 1 + 9 * 3 + corners)
        rendered = module.render(result)
        self.assertTrue(rendered.startswith("PENDING-REAL-SUMMARY\n"))
        self.assertEqual(next(iter(result)), "real_summary")
        self.assertEqual(result["acceptance"], "PENDING")
        for cell in result["grid"] + result["extremes"]:
            row = cell["model"]["comparison"]
            if cell.get("name") == "just-above-ceiling":
                self.assertEqual(row["verdict"], "MODEL-ONLY REFUSED(summary_limit)")
                self.assertEqual(row["size_verdict"], "MODEL-ONLY PASS")
            else:
                self.assertEqual(row["verdict"], "MODEL-ONLY PASS")
            self.assertEqual(row["C_meta"], cell["model"]["summary_model"]["C_meta"])
            self.assertEqual(module.pending_measurement(cell)["verdict"], "PENDING")
            self.assertEqual(cell["model"]["v1"], cell["model"]["v1_crosscheck"]["v1"])
        empty = result["extremes"][0]["model"]["comparison"]
        self.assertEqual((empty["v1"], empty["full_v2"], empty["COMPARABLE_V2"]), (950, 2503, 1991))
        self.assertEqual(empty["empty_complete_file"]["difference"], 1553)
        self.assertEqual(empty["empty_complete_file"]["deduction"], 0)
        self.assertIn("EMPTY COMPLETE FILE (no deduction)", rendered)
        for cell in result["extremes"]:
            row = cell["model"]["comparison"]
            if cell["name"] in ("empty", "100/0", "999/0"):
                self.assertEqual(row["complete_file"]["verdict"], "MODEL-ONLY PASS")
                self.assertEqual(row["complete_file"]["deduction"], 0)
                self.assertLessEqual(row["full_v2"], row["integer_limit"])
            if cell["name"] == "999/0":
                self.assertEqual(row["clause"], "(i)")
            if cell["name"] == "compressible-1MiB":
                c = (1024 * 1024 + 3) // 4
                self.assertEqual(row["C_meta"], c - 512)
                self.assertEqual(row["COMPARABLE_V2"], module.model_size(
                    100, 1, cell["f"])["comparison"]["COMPARABLE_V2"])
            if cell["name"] == "incompressible-1MiB":
                jm = 1024 * 1024 - 1024 + 1
                self.assertEqual(row["C_meta"], jm + 9 + 3 * math.ceil(jm / 131072))
                self.assertEqual(row["COMPARABLE_V2"], module.model_size(
                    100, 1, cell["f"])["comparison"]["COMPARABLE_V2"] + (1024 - 1 - 512))
        # A real SUMMARY does not manufacture any missing whole-file result.
        evidence = {"status": "REAL-SUMMARY", "run_id": "captured", "provenance": "input-artifact",
                    "mode": "ordinary", "J": 3000, "C": 2633 + 200, "C_meta": 200,
                    "C_comparable": 2633, "probe_status": "MEASURED"}
        result = module.measure_grid(encode_fixtures=False, real_evidence=evidence)
        self.assertTrue(module.render(result).startswith("REAL-SUMMARY\n"))
        self.assertEqual(result["acceptance"], "FAIL")
        strict = next(row for row in result["break_even"] if row["cell"] == "128/1(i)" and row["f"] == .25)
        base = module.model_size(128, 1, .25, summary_stored=0, excluded=0)["comparison"]
        expected_limit = base["v1"] - 1 - base["full_v2"]
        self.assertEqual(strict["verdict"], "MODEL-ONLY FAIL")
        self.assertEqual(strict["limit_over_J_real"], expected_limit / evidence["J"])
        self.assertEqual(result["binding_verdict"], "FAIL")

    def test_corner_grid_and_extremes_recompute_every_E_T_and_shape_boundary(self):
        from fractions import Fraction
        module = self.size_module()
        result = module.measure_grid(encode_fixtures=False)
        corners = [row for row in result["grid"] if row.get("shape")]
        extremes = [row for row in result["extremes"] if row.get("shape")]
        populations = [(n, t) for n in (1, 2, 10, 40, 63, 64, 68, 69, 100, 127, 128)
                       for t in range(4)] + [(n, 4) for n in (1, 63, 64, 127, 128)]
        expected = [(shape, n, t, float(f)) for shape in ("ordinary", "selected_report_view")
                    for n, t in populations for f in (Fraction(1, 4), Fraction(1, 2), Fraction(1))]
        for group in (corners, extremes):
            self.assertEqual([(row["shape"], row["N"], row["T"], row["f"]) for row in group], expected)
        for cell in corners + extremes:
            n, t, f = cell["N"], cell["T"], Fraction(cell["f"])
            # Separate region and payload oracle; sum once, then ceil.
            payload_e = (Fraction(1, 10) + 11 * f) * n
            payload_t, payload_i = 55 * t, Fraction(n + t, 10)
            d = 2 + int(t > 0)
            fixed = 947 + (52 + 1024) + (176 + 80 + (992 if t else 0)) + d * 96 + 320 + 160
            full = math.ceil(fixed + payload_e + payload_t + payload_i)
            comparable = math.ceil(fixed - 1024 + 512 + payload_e + payload_t + payload_i)
            # v1 uses the independent record-token byte lengths, rather than
            # calling either production model route for its expectation.
            flat = len(b'["E",1700000000000,10000.0,0.0]\n')
            active = len(b'["E",1700000000000,12345.678901234567,1.2345678901234567]\n')
            trade_text = 5 + 5 + 7 + 28 + (18 + 6 + 5 * 19) + 12 + 6 + 5 + 2 + 10 + 2
            v1_expected = math.ceil(810 + 138 + len(str(n)) + len(str(t)) +
                                    n * ((1 - f) * flat + f * active) + trade_text * t)
            row = cell["model"]["comparison"]
            self.assertEqual((row["v1"], row["full_v2"], row["COMPARABLE_V2"]),
                             (v1_expected, full, comparable))
            bars, anchors = (n, 0) if cell["shape"] == "ordinary" else (1000, 1)
            self.assertEqual((row["actual_bars"], row["raw_E"], row["anchors"], row["T"]), (bars, n, anchors, t))
            small = n < 64 or n <= 127 and t <= 3
            self.assertEqual(row["clause"], "(ii-a)" if small else "(i)")
            self.assertEqual(row["integer_limit"], v1_expected + (3 * 1024 if small else -1))
            self.assertEqual(row["comparable_minus_v1"], comparable - v1_expected)
            self.assertEqual(row["full_minus_v1"], full - v1_expected)
            self.assertEqual(row["verdict"], "MODEL-ONLY PASS")
            pending = module.pending_measurement(cell)
            self.assertEqual((pending["actual_bars"], pending["raw_E"], pending["anchors"]), (bars, n, anchors))
            self.assertEqual(pending["verdict"], "PENDING")
            self.assertEqual(cell["model"]["v2_crosscheck"]["COMPACT"], full)
            if n == 40 and t == 0 and f == Fraction(1, 4):
                self.assertGreater(comparable, v1_expected)  # still PASS(ii-a)
            if not small:
                self.assertLess(comparable, v1_expected)  # strict (i) at every f

    def test_captured_summary_and_pair_measure_actual_frames_before_grid_or_json(self):
        from fractions import Fraction
        module = self.size_module()
        rows = population_rows(100, 0)
        case = Case("capture", header("size-capture"), rows, metadata("capture", rows))
        case.summary, case.report, case.refusal = v1_report(case)
        raw = json.dumps(case.summary, separators=(",", ":")).encode("ascii")
        data = encoder.encode_bytes(case.head, rows, raw, compress=True)
        build = module.zstd_build()["id"]
        with tempfile.TemporaryDirectory() as scratch:
            first, second = Path(scratch) / "run.jsonl", Path(scratch) / "run.pfs2"
            first.write_bytes(v1_file(case.head, rows))
            second.write_bytes(data)
            evidence = module.real_summary(second, "test captured-input provenance", "ordinary", build)
            self.assertEqual((evidence["status"], evidence["probe_status"]), ("REAL-SUMMARY", "MEASURED"))
            self.assertEqual(evidence["J"], len(raw))
            self.assertEqual(evidence["C"], len(encoder.compress_summary(raw, 3)))
            self.assertTrue(evidence["frame_reproduced"])
            q = {key: case.summary[key] for key in ("engine", "summary", "diagnostics", "metrics", "sink_folds")}
            m = {key: case.summary[key] for key in ("input", "applied_inputs", "applied_overrides",
                                                   "applied_runtime", "elapsed_seconds", "fingerprint")}
            cq = len(encoder.compress_summary(json.dumps(q, separators=(",", ":")).encode("ascii"), 3))
            cm = len(encoder.compress_summary(json.dumps(m, separators=(",", ":")).encode("ascii"), 3))
            expected_exclusion = min(cm, max(0, len(encoder.compress_summary(raw, 3)) - cq))
            self.assertEqual((evidence["C_Q"], evidence["C_M"], evidence["C_meta"]), (cq, cm, expected_exclusion))
            manifest = Path(scratch) / "pairs.json"
            manifest.write_text(json.dumps([{"N": 100, "T": 0, "f": 0.25, "v1": first.name, "v2": second.name,
                                             "provenance": "same test capture", "mode": "ordinary", "anchors": 0,
                                             "source_zstd_build": build}]), encoding="utf-8")
            result = module.measure_grid(n_values=(100,), fractions=(Fraction(1, 4),), encode_fixtures=False,
                                         real_evidence=evidence, measurements=manifest)
            measured = result["grid"][0]["measured"][0]
            self.assertEqual((measured["actual_bars"], measured["raw_E"], measured["anchors"]), (100, 100, 0))
            self.assertEqual(measured["full_v2"], len(data))
            self.assertEqual(measured["COMPARABLE_V2"], len(data) - expected_exclusion)
            self.assertEqual(measured["integer_limit"], len(v1_file(case.head, rows)) + 3 * 1024)
            counted_real = evidence["C"] - expected_exclusion
            fixed = 947 + 52 + (176 + 992 + 80) + 3 * 96 + 320 + 160
            first_real = math.ceil(fixed + Fraction(285, 100) + 55 + Fraction(2, 10) + counted_real)
            baseline = math.ceil(810 + 140 + 22 + Fraction(33, 2) + 119 + 82)
            strict_real = math.ceil(fixed + Fraction(285, 100) * 64 + 55 * 4 +
                                    Fraction(68, 10) + counted_real)
            strict_v1 = math.ceil(810 + 141 + (22 + Fraction(33, 2)) * 64 + (119 + 82) * 4)
            failed = first_real > baseline + 3 * 1024 or strict_real >= strict_v1
            self.assertEqual(result["binding_checks"][0]["comparable_with_real_frame"], first_real)
            self.assertEqual(result["binding_checks"][1]["comparable_with_real_frame"], strict_real)
            self.assertEqual(result["acceptance"], "FAIL" if failed else "PENDING")
            output = io.StringIO()
            with mock.patch("sys.stdout", output):
                self.assertEqual(module.main(["--grid", "--model-only", "--real-summary", str(second),
                                              "--provenance", "captured test", "--mode", "ordinary",
                                              "--source-zstd-build", build, "--json"]), int(failed))
            payload = json.loads(output.getvalue())
            self.assertEqual(next(iter(payload)), "real_summary")
            self.assertEqual(payload["real_summary"]["status"], "REAL-SUMMARY")
            self.assertEqual(payload["grid"][0]["measured"][0]["verdict"], "PENDING")
            unknown = module.real_summary(second, "capture without writer build", "ordinary", None)
            self.assertIsNone(unknown["C_meta"])
            self.assertEqual(unknown["probe_status"], "PENDING-SAME-BUILD")
            # A legal expanding reader fixture cannot impersonate the actual
            # HEAD-level compression probe or its incompressibility verdict.
            raw_data = encoder.encode_bytes(case.head, rows, raw, compress=True,
                                            summary_frame_factory=summary_raw_frame)
            second.write_bytes(raw_data)
            mismatched = module.real_summary(second, "decode-only fixture", "ordinary", build)
            self.assertFalse(mismatched["frame_reproduced"])
            self.assertEqual(mismatched["probe_status"], "PENDING-FRAME-PARAMETERS")
            self.assertIsNone(mismatched["C_meta"])

    def test_fixed_K_binding_checks_use_real_counted_bytes_and_both_break_even_routes(self):
        from fractions import Fraction
        module = self.size_module()
        cells = ((1, 1), (64, 4), (100, 1), (128, 1), (1000, 0), (0, 0), (100, 0))
        expected = []
        for f in (Fraction(1, 4), Fraction(1, 2), Fraction(1)):
            for n, t in cells:
                if n == 0 and f != Fraction(1, 4):
                    continue
                # Independent E/T/I framing and raw printed-token byte counts.
                fixed = (947 + 52 + 480 + 272 * int(n > 0) +
                         1088 * int(t > 0) + 176 * int(n + t > 0))
                payload = (Fraction(1, 10) + 11 * f) * n + 55 * t + Fraction(n + t, 10)
                base = fixed + math.ceil(payload)
                flat = len(b'["E",1700000000000,10000.0,0.0]\n')
                active = len(b'["E",1700000000000,12345.678901234567,1.2345678901234567]\n')
                v1_bytes = math.ceil(810 + 138 + len(str(n)) + len(str(t)) +
                                     n * (flat * (1 - f) + active * f) + (119 + 82) * t)
                small = n < 64 or n < 128 and t < 4
                clause = "(ii-a)" if small else "(i)"
                limit = v1_bytes + (3 * 1024 if small else -1)
                expected.append((f"{n}/{t}{clause}", float(f), v1_bytes, base, limit - base))
        pending = module.measure_grid(encode_fixtures=False)
        table = pending["break_even"]
        self.assertEqual([(row["cell"], row["f"], row["v1"], row["base_without_frame"],
                           row["counted_frame_limit"]) for row in table], expected)
        for row in table:
            self.assertEqual(row["counted_frame_limit"], row["route_b"]["counted_frame_limit"])
            self.assertEqual(row["base_without_frame"], row["route_b"]["base_without_frame"])
            self.assertEqual(row["verdict"], "MODEL-ONLY PENDING-REAL-SUMMARY")
        first_limit, strict_limit = expected[0][4], expected[1][4]
        self.assertGreater(first_limit, strict_limit)
        for counted in (strict_limit, strict_limit + 1, first_limit, first_limit + 1):
            evidence = {"status": "REAL-SUMMARY", "probe_status": "MEASURED", "J": 5000,
                        "C": counted + 200, "C_meta": 200, "C_comparable": counted}
            result = module.measure_grid(encode_fixtures=False, real_evidence=evidence)
            checks = result["binding_checks"]
            self.assertEqual([row["cell"] for row in checks], [expected[0][0], expected[1][0]])
            self.assertEqual([row["verdict"] for row in checks],
                             ["MODEL-ONLY " + ("PASS" if counted <= limit else "FAIL")
                              for limit in (first_limit, strict_limit)])
            self.assertEqual(result["acceptance"], "FAIL" if counted > strict_limit else "PENDING")
            self.assertEqual(result["K"], pending["K"])  # Real failures never move the allowance.
            for extra, verdict in ((3 * 1024, "PASS"), (3 * 1024 + 1, "FAIL")):
                row = module.size_comparison(1000, 1000 + extra, 0, bars=1, counts={"E": 1, "T": 1},
                                             profile="COMPACT", evidence="MEASURED")
                self.assertEqual(row["verdict"], verdict)
            lines = module.render(result).splitlines()
            self.assertEqual(json.loads(lines[1]), evidence)
            self.assertEqual([json.loads(line.removeprefix("MODEL-ONLY BINDING "))["cell"]
                              for line in lines[2:4]], [expected[0][0], expected[1][0]])
            self.assertEqual(list(result)[:2], ["real_summary", "binding_checks"])
            self.assertIsNone(result["grid"][0]["measured"][0]["COMPARABLE_V2"])
            output = io.StringIO()
            with mock.patch.object(module, "real_summary", return_value=evidence), mock.patch("sys.stdout", output):
                status = module.main(["--grid", "--model-only", "--real-summary", "capture.pfs2",
                                      "--provenance", "unit-test input", "--mode", "ordinary", "--json"])
            self.assertEqual(status, int(counted > strict_limit))
            payload = json.loads(output.getvalue())
            self.assertEqual(list(payload)[:2], ["real_summary", "binding_checks"])
            self.assertEqual(payload["acceptance"], "FAIL" if counted > strict_limit else "PENDING")
            unknown = {**evidence, "probe_status": "PENDING-SAME-BUILD"}
            self.assertEqual(module.binding_checks(unknown)["binding_verdict"], "PENDING-REAL-SUMMARY")
            with self.assertRaisesRegex(module.ReportError, "C-C_meta"):
                module.binding_checks({**evidence, "C_comparable": counted + 1})

    def test_ceiling_extremes_recompute_full_comparable_and_refusal_by_two_routes(self):
        from fractions import Fraction
        module = self.size_module()
        s_max = 1024 * 1024
        rows = [row for row in module.extreme_models() if "ceiling" in row["name"]]
        self.assertEqual(len(rows), 3 * 3)

        def raw_frame_size(size):
            blocks, last = divmod(size, 128 * 1024)
            return 4 + 1 + 4 + blocks * (3 + 128 * 1024) + (3 + last if last else 0)

        stored_limit = (4 + 4 + 4 + 4 + 4 + 32) + raw_frame_size(s_max)
        for cell in rows:
            raw = cell["name"] == "ceiling-raw-fallback"
            refused = cell["name"] == "just-above-ceiling"
            j = s_max + int(refused)
            jm = j - 1024 + 1
            c = raw_frame_size(j) if raw else math.ceil(Fraction(j, 4))
            cm = raw_frame_size(jm) if raw else math.ceil(Fraction(jm, 4))
            counted = max(512, c - cm)
            excluded = c - counted
            f = Fraction(cell["f"])
            payload = (Fraction(1, 10) + 11 * f) * 100 + 55 + Fraction(101, 10)
            fixed = 947 + 52 + (176 + 992 + 80) + 3 * 96 + 320 + 160
            full = math.ceil(fixed + payload + c)
            baseline = math.ceil(810 + 142 + (22 + 10 * (1 - f) + 36 * f) * 100 + 119 + 82)
            comparison = cell["model"]["comparison"]
            summary = cell["model"]["summary_model"]
            self.assertEqual((summary["J"], summary["C"], summary["C_M"], summary["C_meta"]), (j, c, cm, excluded))
            self.assertEqual(summary["route_b"]["C_comparable"], counted)
            self.assertEqual(summary["stored_minus_limit"], c - (stored_limit - 52))
            self.assertEqual((comparison["full_v2"], comparison["COMPARABLE_V2"]), (full, full - excluded))
            self.assertEqual(comparison["v1"], baseline)
            self.assertEqual(comparison["full_minus_v1"], full - baseline)
            self.assertEqual(comparison["comparable_minus_v1"], full - excluded - baseline)
            self.assertEqual(comparison["full_minus_v1"], comparison["comparable_minus_v1"] + excluded)
            self.assertEqual(cell["model"]["v2_crosscheck"]["COMPACT"], full)
            self.assertEqual(comparison["exclusion"], "EXCLUDED_CALLER_METADATA")
            self.assertEqual(comparison["clause"], "(ii-a)")
            self.assertEqual(comparison["integer_limit"], comparison["v1"] + 3 * 1024)
            self.assertLessEqual(52 + c, stored_limit)
            if refused:
                self.assertGreater(j, s_max)
                self.assertEqual(comparison["verdict"], "MODEL-ONLY REFUSED(summary_limit)")
                self.assertEqual(comparison["size_verdict"], "MODEL-ONLY PASS")
                self.assertTrue(comparison["complete_file"]["counterfactual"])
            else:
                self.assertEqual(comparison["verdict"], "MODEL-ONLY PASS")
                if raw:
                    self.assertEqual(52 + c, stored_limit)

    def test_selected_captured_pairs_keep_B_raw_E_and_anchors_distinct(self):
        from fractions import Fraction
        module = self.size_module()
        build = module.zstd_build()["id"]
        for n in (40, 64):
            rows = population_rows(n, 0)
            summary = metadata("selected-capture", rows, selected=True)
            summary["input"]["bars"] = 1200
            summary["diagnostics"]["input_bars_processed"] = 1200
            case = Case("selected-capture", header("selected-capture"), rows, summary)
            case.summary, case.report, case.refusal = v1_report(case)
            data = encoder.encode_bytes(case.head, rows, case.summary, compress=True)
            with self.subTest(raw_e=n), tempfile.TemporaryDirectory() as scratch:
                first, second = Path(scratch) / "run.jsonl", Path(scratch) / "run.pfs2"
                first.write_bytes(v1_file(case.head, rows))
                second.write_bytes(data)
                entry = {"N": n, "T": 0, "f": 0.25, "v1": first.name, "v2": second.name,
                         "provenance": "selected test capture", "mode": "selected", "anchors": 1,
                         "source_zstd_build": build, "shape": "selected_report_view"}
                manifest = Path(scratch) / "pairs.json"
                # The same capture names a grid cell and its extreme, exactly once each.
                manifest.write_text(json.dumps([entry, {**entry, "name": f"selected_report_view/E{n}/T0"}]),
                                    encoding="utf-8")
                result = module.measure_grid(n_values=(), fractions=(Fraction(1, 4),), encode_fixtures=False,
                                             measurements=manifest)
                for group in ("grid", "extremes"):
                    cell = next(row for row in result[group]
                                if row.get("shape") == "selected_report_view" and row["N"] == n and row["f"] == 0.25)
                    measured = cell["measured"][0]
                    self.assertEqual((measured["actual_bars"], measured["raw_E"], measured["anchors"]),
                                     (1200, len(rows), 1))
                    self.assertEqual(measured["clause"], "(ii-a)")
                    self.assertEqual(measured["full_v2"], len(data))
                    self.assertEqual(measured["integer_limit"],
                                     len(v1_file(case.head, rows)) + 3 * 1024)
                    self.assertEqual(measured["COMPARABLE_V2"],
                                     len(data) - measured["summary_probe"]["C_meta"])
                for invalid in ({**entry, "anchors": None}, {**entry, "actual_bars": 999}):
                    manifest.write_text(json.dumps([invalid]), encoding="utf-8")
                    with self.assertRaises(module.ReportError):
                        module.measure_grid(n_values=(), fractions=(Fraction(1, 4),), encode_fixtures=False,
                                             measurements=manifest)

    def test_hand_column_bytes_and_stored_vector_generator(self):
        from generate_sink_v2_reference_controls import generate
        with tempfile.TemporaryDirectory() as scratch:
            for case in cases():
                if case.frame_factory:
                    continue
                generate(case, Path(scratch))
                generate(case, Path(scratch), check=True)
                mask = json.loads((Path(scratch) / f"{case.name}.mask.json").read_bytes())
                self.assertEqual([frame["tag"] for frame in mask["zstd_frames"]], ["SUMMARY"])
                self.assertEqual((Path(scratch) / f"{case.name}.report.json").read_bytes(), case.report)
                if case.refusal is None:
                    self.assertEqual(mask["report"]["report_sha256"], hashlib.sha256(case.report).hexdigest())
                    self.assertEqual(mask["report"]["content_length"], len(case.report))
                else:
                    self.assertEqual(mask["report"], {"refusal": case.refusal})
                if case.name in ("equity4", "equity4-full"):
                    s_max, block = 1024 * 1024, 128 * 1024
                    c_max = 9 + (s_max // block) * (block + 3)
                    for suffix, fault, stage in (
                            (f"summary-J{s_max + 1}", "decoded_length", "open-fixed-summary-header"),
                            (f"summary-C{c_max + 1}", "stored_length", "open-fixed-summary-header"),
                            ("summary-window", "window", "open-bounded-frame-header")):
                        sidecar = json.loads((Path(scratch) / f"{case.name}--{suffix}.refusal.json").read_bytes())
                        self.assertEqual((sidecar["code"], sidecar["first_failing_check"]["fault"], sidecar["stage"]),
                                         ("summary_limit", fault, stage))

    def test_size_report_accounts_every_actual_byte(self):
        spec = importlib.util.spec_from_file_location("sink_size_report_l21r", ROOT / "scripts" / "sink_size_report.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as scratch:
            for full in (False, True):
                case, data = fixture("trades3", force_full=full)
                first, second = Path(scratch) / "input.jsonl", Path(scratch) / "input.pfs2"
                expected = v1_file(case.head, case.rows)
                first.write_bytes(expected)
                second.write_bytes(data)
                result1 = module.measure_v1(first, v1)
                result2 = module.measure_v2(second)
                self.assertEqual(result1["file_bytes"], len(expected))
                self.assertEqual(result2["file_bytes"], len(data))
                self.assertEqual(sum(row["bytes"] for row in result2["types"].values()) +
                                 sum(result2["shared"].values()), len(data))
                rank_pages = 10 * (4096 + 3 * 4) if full else 0
                self.assertEqual(result2["types"]["T"]["rank_bytes"], rank_pages)
                self.assertEqual(result2["types"]["T"]["column_sha_bytes"], 20 * 32)
                self.assertEqual(result2["shared"]["root"], 4096 if full else 320)
                self.assertEqual(result2["profile"], "FULL" if full else "COMPACT")
                self.assertNotIn("model", result1)
                self.assertNotIn("model", result2)
                body = b"".join(expected.splitlines(keepends=True)[1:-1])
                self.assertEqual(result1["types"]["T"]["line_bytes"], len(body))

    def test_size_model_matches_independent_regrouped_section_15_1_arithmetic(self):
        from fractions import Fraction
        spec = importlib.util.spec_from_file_location("sink_size_model", ROOT / "scripts" / "sink_size_report.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        ceil = lambda n, cap: (n + cap - 1) // cap
        for n in (100, 1000, 5000, 9000, 10000, 100000, 1000000):
            for t in (0, 1, n // 20):
                e, trades, interleave = ceil(n, 4096), ceil(t, 1024), ceil(n + t, 65536)
                m2 = 947 + 52 + 1024
                a_c = m2 + 480 + 272 * e + 1088 * trades + 176 * interleave
                b = min(n, 1200)
                a_f = (m2 + 4256 + 688 * e + 1504 * trades + 592 * interleave +
                       4096 * (ceil(e, 120) + ceil(trades, 120) + ceil(interleave, 120)) +
                       40 * t + 40960 * ceil(t, 15360) + 256 * b + 4096 * ceil(b, 240))
                for fraction in (Fraction(1, 4), Fraction(1, 2), Fraction(1)):
                    model = module.model_size(n, t, fraction)
                    payload = (Fraction(1, 5) + 11 * fraction) * n + Fraction(551, 10) * t
                    self.assertEqual(model["FULL"], math.ceil(a_f + payload))
                    if n < 10980 and t < 3314 and e + trades + interleave <= 128:
                        self.assertEqual(model["COMPACT"], math.ceil(a_c + payload))
                        self.assertEqual(model["profile"], "COMPACT")
                    else:
                        self.assertIsNone(model["COMPACT"])
                        self.assertEqual(model["profile"], "FULL")
                    v1_model = (32 + 26 * fraction) * n + 201 * t + 948 + len(str(n)) + len(str(t))
                    self.assertEqual(model["v1"], math.ceil(v1_model))
                    self.assertEqual(model["v1_crosscheck"]["v1"], math.ceil(v1_model))
                    profile = model["profile"]
                    self.assertEqual(model["comparison"]["COMPARABLE_V2"], model[profile] - 512)
                    self.assertEqual(model["comparison"]["verdict"], "MODEL-ONLY PASS")
        # A broken independent route must stop reporting instead of quietly
        # choosing one total; this exercises the cross-check, not a fixed cell.
        with mock.patch.object(module, "v1_digit_count", return_value={"v1": -1}):
            with self.assertRaisesRegex(module.ReportError, "independent printed-byte count"):
                module.model_size(5000, 250, Fraction(1, 4))
        wrong = dict(module.v2_region_count(128, 1, Fraction(1, 4), 1024), COMPACT=-1)
        with mock.patch.object(module, "v2_region_count", return_value=wrong):
            with self.assertRaisesRegex(module.ReportError, "independent region count"):
                module.model_size(128, 1, Fraction(1, 4))


if __name__ == "__main__":
    if not has_zstd():
        print("FAIL: every SUM2 file needs PF_SINK_ZSTD_LIBRARY (the pinned codec), "
              "compression.zstd or zstandard",
              file=sys.stderr, flush=True)
        raise SystemExit(1)
    unittest.main()
