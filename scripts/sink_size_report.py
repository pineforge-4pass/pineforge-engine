#!/usr/bin/env python3
"""Measure full/comparable v1/v2 bytes under sink-v2 round 10.

python3 -I scripts/sink_size_report.py INPUT [--v2 MATCHING.pfs2] [--json]
python3 -I scripts/sink_size_report.py --grid --real-summary RUN.pfs2 \
    --provenance CAPTURE_ID --mode ordinary --source-zstd-build BUILD_ID

v1: strict receipt verification, actual line/token bytes and a whole-column
zlib-9 text proxy. v2: full verification, actual stored/raw columns, data
framing/SHA tables, actual short index pages or compact descriptors, ranks
charged to T and ID18 charged to E. Only SUMMARY-frame caller metadata
is deductible. A real production SUMMARY comes FIRST; absent evidence stays
PENDING. Synthetic grid encodings are labelled MEASURED-FIXTURE, and every
planning verdict is MODEL-ONLY. --measurements accepts captured real-run pairs
for the grid/extremes; --model-only leaves missing whole files PENDING.
"""
from __future__ import annotations

import argparse
import collections
from fractions import Fraction
import hashlib
import importlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from typing import Any
import zlib

REPO = Path(__file__).resolve().parents[1]
DEFAULT_PF_SINK = REPO / "docker" / "pf_sink.py"


class ReportError(ValueError):
    """Invalid sink, source changed during measurement, or accounting error."""


GRID_N = (100, 1000, 5000, 9000, 10000, 100000, 1000000)
CORNER_E = (1, 2, 10, 40, 63, 64, 68, 69, 100, 127, 128)
T4_E = (1, 63, 64, 127, 128)
FRACTIONS = (Fraction(1, 4), Fraction(1, 2), Fraction(1))
SPEC_SMALL_K = 3 * 1024  # Fixed by 0.5 for both model and measured verdicts.
SUMMARY_CEILING = 1024 * 1024  # S_MAX (3.7); blocks retain their 32 MiB bound.
SUMMARY_STORED_LIMIT = 52 + 9 + SUMMARY_CEILING + 3 * ((SUMMARY_CEILING + 131071) // 131072)
METADATA_MEMBERS = frozenset(("input", "applied_inputs", "applied_overrides", "applied_runtime",
                              "elapsed_seconds", "fingerprint", "report_window", "report_shape"))
COMPARABLE_MEMBERS = frozenset(("engine", "summary", "metrics", "diagnostics", "sink_folds", "outputs"))
OUTPUT_METADATA_MEMBERS = frozenset(("schema_version", "message_format", "manifest_sha256", "manifest"))


def load_reader_module(path: Path):
    spec = importlib.util.spec_from_file_location("pf_sink", path)
    if spec is None or spec.loader is None:
        raise ReportError(f"cannot import v1 reader {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["pf_sink"] = module
    spec.loader.exec_module(module)
    return module


def v2_module():
    # Resolve siblings by this script's repository, also under Python -I.
    # Never import a module from an unrelated working directory.
    docker = str(REPO / "docker")
    if docker not in sys.path:
        sys.path.insert(0, docker)
    return importlib.import_module("pf_sink2")


def zstd_build() -> dict[str, str]:
    if os.environ.get("PF_SINK_ZSTD_LIBRARY"):
        v2_module()
        try:
            from pf_sink2_zstd import library
            lib = library()
            identity = lib.pineforge_sink_zstd_reference_build().decode("ascii")
        except (ImportError, OSError, ValueError, AttributeError) as error:
            raise ReportError(f"SUMMARY probes cannot load PF_SINK_ZSTD_LIBRARY: {error}") from error
        number = lib.ZSTD_versionNumber()
        version = f"{number // 10000}.{number // 100 % 100}.{number % 100}"
        return {"id": identity, "version": version, "package": "vendored",
                "backend": "pinned-conformance"}
    try:
        from compression import zstd
    except ImportError:
        try:
            import zstandard
        except ImportError as error:
            raise ReportError("SUMMARY probes need PF_SINK_ZSTD_LIBRARY (the pinned conformance codec), "
                              "compression.zstd or zstandard") from error
        version = ".".join(map(str, zstandard.ZSTD_VERSION))
        backend = str(zstandard.backend)
        return {"id": f"zstandard-{zstandard.__version__}/{backend}/libzstd-{version}", "version": version,
                "package": zstandard.__version__, "backend": backend}
    version = str(zstd.zstd_version)
    return {"id": f"compression.zstd-{sys.version.split()[0]}/libzstd-{version}", "version": version,
            "package": sys.version.split()[0], "backend": "stdlib"}


def object_tokens(data: bytes) -> list[tuple[str, bytes, bytes]]:
    """Slice already-validated compact JSON members, never reserialize values."""
    if not data.startswith(b"{") or not data.endswith(b"}"):
        raise ReportError("projection needs a validated JSON object")
    if data == b"{}":
        return []
    starts = [1]
    depth = 0
    quoted = escaped = False
    for pos in range(1, len(data) - 1):
        byte = data[pos]
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (91, 123):
            depth += 1
        elif byte in (93, 125):
            depth -= 1
        elif byte == 44 and depth == 0:
            starts.append(pos + 1)
    result = []
    ends = [start - 1 for start in starts[1:]] + [len(data) - 1]
    for start, end in zip(starts, ends):
        pos, escaped = start + 1, False
        if data[start] != 34:
            raise ReportError("projection member key is not a string")
        while pos < end:
            byte = data[pos]
            if not escaped and byte == 34:
                break
            escaped = not escaped and byte == 92
            pos += 1
        if data[pos + 1:pos + 2] != b":":
            raise ReportError("projection member separator")
        result.append((json.loads(data[start:pos + 1]), data[start:pos + 2], data[pos + 2:end]))
    return result


def summary_projections(data: bytes) -> tuple[bytes, bytes, list[str], bool]:
    """0.5: retain order, key/value tokens, braces and compact comma separators."""
    q, m, conservative = [], [], []
    for key, prefix, value in object_tokens(data):
        if key == "outputs":
            oq, om = [], []
            for child, child_prefix, child_value in object_tokens(value):
                (om if child in OUTPUT_METADATA_MEMBERS else oq).append(child_prefix + child_value)
            if oq:
                q.append(prefix + b"{" + b",".join(oq) + b"}")
            if om:
                m.append(prefix + b"{" + b",".join(om) + b"}")
        elif key in METADATA_MEMBERS:
            m.append(prefix + value)
        else:
            q.append(prefix + value)
            if key not in COMPARABLE_MEMBERS:
                conservative.append(key)
    return b"{" + b",".join(q) + b"}", b"{" + b",".join(m) + b"}", conservative, bool(m)


def metadata_exclusion(c: int, cq: int, cm: int) -> int:
    if min(c, cq, cm) < 0:
        raise ReportError("negative SUMMARY probe length")
    return min(cm, max(0, c - cq))


def summary_probe(data: bytes, stored: int, level: int, *,
                  source_zstd_build: str | None = None, full_frame: bytes | None = None) -> dict[str, Any]:
    v2_module()
    reference = importlib.import_module("pf_sink2_reference")
    build = zstd_build()
    q, m, conservative, has_m = summary_projections(data)
    cq = len(reference.compress_summary(q, level))
    cm = len(reference.compress_summary(m, level)) if has_m else 0
    # A file does not carry a zstd build ID. Require capture provenance for the
    # writer's build; a coincidentally equal frame length is not that evidence.
    same_build = source_zstd_build == build["id"]
    reencoded = reference.compress_summary(data, level) if full_frame is not None else None
    reproduced = reencoded == full_frame if full_frame is not None else None
    compatible = same_build and reproduced is not False
    excluded = metadata_exclusion(stored, cq, cm) if compatible else None
    comparable = stored - excluded if excluded is not None else None
    return {"status": ("MEASURED" if compatible else "PENDING-SAME-BUILD" if not same_build
                       else "PENDING-FRAME-PARAMETERS"),
            "J": len(data), "C": stored, "C/J": stored / len(data), "HEAD_level": level,
            "zstd_build": build, "source_zstd_build": source_zstd_build or "PENDING",
            "C_reencoded": len(reencoded) if reencoded is not None else None, "frame_reproduced": reproduced,
            "J_Q": len(q), "C_Q": cq, "J_M": len(m), "C_M": cm,
            "C_meta": excluded, "C_comparable": comparable,
            "C_comparable/J": comparable / len(data) if comparable is not None else None,
            "conservatively_counted": conservative,
            "deductible_region": "SUMMARY frame only; all 52 header bytes and format overhead counted"}


def size_comparison(v1_bytes: int | None, full_bytes: int | None, excluded: int | None, *,
                    bars: int | None, counts: dict[str, int], profile: str,
                    evidence: str, anchors: int | None = None) -> dict[str, Any]:
    raw_e, trades = counts.get("E", 0), counts.get("T", 0)
    # 0.5 assigns exactly ONE size clause, before considering metadata (iii).
    # Raw E includes selected anchors. B and optional streams do not assign
    # a clause. The complement is E>=128 OR (E>=64 AND T>=4).
    small = (raw_e < 128 and trades < 4) or raw_e < 64
    clause, allowance = ("(ii-a)", SPEC_SMALL_K) if small else ("(i)", -1)
    limit = v1_bytes + allowance if v1_bytes is not None and allowance is not None else None
    comparable = full_bytes - excluded if full_bytes is not None and excluded is not None else None
    verdict = ("PENDING" if comparable is None or limit is None else
               "PASS" if comparable <= limit else "FAIL")
    result = {"evidence": evidence, "actual_bars": bars, "raw_E": raw_e,
              "T": trades, "anchors": anchors, "profile": profile, "v1": v1_bytes,
              "full_v2": full_bytes, "C_meta": excluded, "COMPARABLE_V2": comparable,
              "exclusion": "EXCLUDED_CALLER_METADATA" if excluded else "NONE" if excluded == 0 else "PENDING",
              "clause": clause, "integer_limit": limit,
              "comparable_minus_v1": comparable - v1_bytes if comparable is not None and v1_bytes is not None else None,
              "full_minus_v1": full_bytes - v1_bytes if full_bytes is not None and v1_bytes is not None else None,
              "verdict": evidence + " " + verdict if evidence == "MODEL-ONLY" else verdict}
    result["K"] = SPEC_SMALL_K if small else None
    result["K_status"] = "FIXED (0.5); MODEL AND MEASURED" if small else "N/A"
    result["complete_file"] = {
        "allowance": allowance, "deduction": 0, "full_v2": full_bytes, "limit": limit,
        "difference": result["full_minus_v1"],
        "verdict": ("PENDING" if full_bytes is None or limit is None else
                    ("MODEL-ONLY " if evidence == "MODEL-ONLY" else "") +
                    ("PASS" if full_bytes <= limit else "FAIL"))}
    if all(counts.get(tag, 0) == 0 for tag in "ETOVKXH"):
        result["empty_complete_file"] = result["complete_file"]
    return result


class StreamingZlib:
    def __init__(self):
        self.compressor = zlib.compressobj(9)
        self.pending = bytearray()
        self.raw = 0
        self.packed = 0

    def add(self, token: bytes):
        self.raw += len(token)
        self.pending.extend(token)
        if len(self.pending) >= 65536:
            self.packed += len(self.compressor.compress(self.pending))
            self.pending.clear()

    def finish(self):
        self.packed += len(self.compressor.compress(self.pending))
        self.pending.clear()
        self.packed += len(self.compressor.flush())
        return self.packed


def array_tokens(line: bytes) -> list[bytes]:
    """Slice actual scalar lexemes, respecting escaped commas/quotes.

    The strict v1 reader has already validated the flat record. Reformatting
    strings would give false byte counts for alternate valid JSON escapes.
    """
    if not line.startswith(b"[") or not line.endswith(b"]\n"):
        raise ReportError("internal: raw line differs from decoded record")
    quoted = escaped = False
    start = 1
    result = []
    for pos in range(1, len(line) - 2):
        byte = line[pos]
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte == 44:
            result.append(line[start:pos])
            start = pos + 1
    result.append(line[start:-2])
    return result


def identity(path: Path):
    stat = path.stat()
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns


def measure_v1(path: Path, pf_sink: Any = None) -> dict[str, Any]:
    pf_sink = pf_sink or load_reader_module(DEFAULT_PF_SINK)
    before = identity(path)
    with path.open("rb") as raw, pf_sink.SinkReader(path) as sink:
        header_line = raw.readline(pf_sink.MAX_LINE_BYTES + 1)
        head = sink.header
        types = {d["type"]: {"count": 0, "line_bytes": 0, "columns": [
            {"field": field, "type": wire, "nulls": 0}
            for field, wire in zip(d["fields"], d["types"])]} for d in head["records"]}
        texts = {tag: [StreamingZlib() for _ in info["columns"]] for tag, info in types.items()}
        for record in sink:
            line = raw.readline(pf_sink.MAX_LINE_BYTES + 1)
            tokens = array_tokens(line)
            if json.loads(tokens[0]) != record.tag or len(tokens) != len(record.values) + 1:
                raise ReportError("internal: raw line differs from reader")
            info = types[record.tag]
            info["count"] += 1
            info["line_bytes"] += len(line)
            for j, (value, token) in enumerate(zip(record.values, tokens[1:])):
                info["columns"][j]["nulls"] += int(value is None)
                texts[record.tag][j].add(token + b"\n")
        trailer_line = raw.readline(pf_sink.MAX_LINE_BYTES + 1)
        if not sink.complete or not trailer_line or raw.read(1):
            raise ReportError("internal: v1 receipt/raw file disagreement")
        trailer = sink.trailer
        counts = sink.counts
    if before != identity(path):
        raise ReportError("v1 source changed during measurement")
    file_bytes = before[2]
    accounted = len(header_line) + len(trailer_line) + sum(info["line_bytes"] for info in types.values())
    if accounted != file_bytes or any(info["count"] != counts[tag] for tag, info in types.items()):
        raise ReportError("internal: v1 accounting/count mismatch")
    for tag, info in types.items():
        info["bytes"] = info["line_bytes"]
        info["bytes_per_record"] = info["bytes"] / info["count"] if info["count"] else None
        for j, column in enumerate(info["columns"]):
            column["text_bytes"] = texts[tag][j].raw
            column["zlib9_bytes"] = texts[tag][j].finish()
        info["column_zlib9_sum"] = sum(column["zlib9_bytes"] for column in info["columns"])
    return {"path": str(path), "version": 1, "file_bytes": file_bytes,
            "header": {"run_id": head["run_id"], "engine": head["engine"]},
            "v1_sha256": trailer["sha256"], "types": types,
            "shared": {"head": len(header_line), "trailer": len(trailer_line)}}


def measure_v2(path: Path, *, content_digest: bool = False,
               source_zstd_build: str | None = None) -> dict[str, Any]:
    pf_sink2 = v2_module()
    layout = importlib.import_module("pf_sink2_layout")
    with pf_sink2.open(path) as sink:
        sink.verify()
        head = sink.head()
        types = {}
        shared = {"head": sink.data_start, "summary": sink.summary_length,
                  "I_data": 0, "I_index": sink.tables[7].byte_length,
                  "root": sink.root_length, "tail": layout.TAIL_SIZE}
        for tag in (*sink.counts(), "I"):
            table = sink.tables[layout.TAGS.index(tag)]
            descriptor = next((d for d in head["records"] if d["type"] == tag), None)
            fields = ["tag"] if tag == "I" else descriptor["fields"]
            wires = ["order"] if tag == "I" else descriptor["types"]
            columns = [{"field": field, "type": wire, "raw_bytes": 0, "stored_bytes": 0,
                        "encodings": collections.Counter(), "codecs": collections.Counter()}
                       for field, wire in zip(fields, wires)]
            data_bytes = framing_bytes = sha_bytes = records = blocks = 0
            for entry in sink.entries(tag):
                blocks += 1
                records += entry.n
                data_bytes += entry.length
                framing_bytes += layout.BLOCK_HEADER + len(columns) * layout.COLUMN_FRAMING
                sha_bytes += len(columns) * layout.COLUMN_SHA_SIZE
                for column, meta in zip(columns, sink.block_layout(entry)):
                    column["raw_bytes"] += meta.raw_len
                    column["stored_bytes"] += meta.stored_len
                    column["encodings"][str(meta.encoding)] += 1
                    column["codecs"][str(meta.codec)] += 1
            if tag == "I":
                shared["I_data"] = data_bytes
                continue
            index_bytes = table.byte_length
            rank_bytes = sum(table.byte_length for table in sink.tables[8:18]) if tag == "T" else 0
            figure_bytes = sink.tables[18].byte_length if tag == "E" else 0
            total = data_bytes + index_bytes + rank_bytes + figure_bytes
            types[tag] = {"count": records, "blocks": blocks, "data_bytes": data_bytes,
                          "framing_bytes": framing_bytes, "column_sha_bytes": sha_bytes,
                          "descriptor_bytes": index_bytes, "rank_bytes": rank_bytes, "figure_bytes": figure_bytes,
                          "equity_witness_bytes_within_descriptors": blocks * 2 * layout.SERIES_SIZE
                          if tag == "E" and not sink.compact else 0,
                          "bytes": total, "bytes_per_record": total / records if records else None, "columns": columns}
        accounted = sum(info["bytes"] for info in types.values()) + sum(shared.values())
        if accounted != sink.size:
            raise ReportError(f"internal: v2 bytes add to {accounted}, file has {sink.size}")
        result = {"path": str(path), "version": 2, "profile": sink.profile, "file_bytes": sink.size,
                  "header": {"run_id": head["run_id"], "engine": head["engine"], "level": head["format"]["level"]},
                  "logical_sha256": sink.require_logical_sha256(), "types": types, "shared": shared,
                  "summary_probe": summary_probe(sink.summary_json_bytes(), sink.summary_stored_length,
                                                 head["format"]["level"], source_zstd_build=source_zstd_build,
                                                 full_frame=sink.summary_frame_bytes()),
                  "actual_bars": head["summary"]["diagnostics"].get("input_bars_processed")}
        if type(result["actual_bars"]) is not int or result["actual_bars"] < 0:
            result["actual_bars"] = None
        if content_digest:
            result["v1_sha256"] = sink.content_sha256()
        return result


def real_summary(path: Path, provenance: str, mode: str, source_zstd_build: str | None) -> dict[str, Any]:
    with v2_module().open(path) as sink:
        sink.verify()
        meta = sink.summary()
        # A framing-only JSON is not a production report measurement.
        required = importlib.import_module("pf_sink2_layout").REPORT_SUMMARY_FIELDS
        if not set(required) <= meta["summary"].keys() or not meta["input"] or not meta["diagnostics"]:
            raise ReportError("real SUMMARY requires the complete captured run_json metadata")
        selected, outputs = "report_window" in meta, "outputs" in meta
        actual_mode = ("selected" if selected else "ordinary") + ("-outputs" if outputs else "")
        if mode != actual_mode:
            raise ReportError("declared production mode differs from SUMMARY")
        probes = summary_probe(sink.summary_json_bytes(), sink.summary_stored_length,
                               sink.head()["format"]["level"], source_zstd_build=source_zstd_build,
                               full_frame=sink.summary_frame_bytes())
        probe_status = probes.pop("status")
        return {"status": "REAL-SUMMARY", "run_id": sink.head()["run_id"],
                "artifact": str(path), "provenance": provenance, "mode": mode,
                "probe_status": probe_status, **probes}


def render_file(result: dict[str, Any]) -> str:
    profile = " " + result["profile"] if result["version"] == 2 else ""
    lines = [f"sink_size_report: {result['path']} (v{result['version']}{profile})",
             f"file bytes: {result['file_bytes']:,}", f"header: {json.dumps(result['header'], ensure_ascii=False)}",
             "type       records          bytes     bytes/record"]
    for tag, info in result["types"].items():
        per = "-" if info["bytes_per_record"] is None else f"{info['bytes_per_record']:,.3f}"
        lines.append(f"{tag:<5}{info['count']:>13,}{info['bytes']:>15,}{per:>17}")
        if result["version"] == 2:
            lines.append(f"  data={info['data_bytes']:,} descriptors/pages={info['descriptor_bytes']:,} "
                         f"ranks/pages={info['rank_bytes']:,} figure/pages={info['figure_bytes']:,} "
                         f"column-SHA={info['column_sha_bytes']:,} (actual short lengths)")
        for column in info["columns"]:
            if result["version"] == 1:
                lines.append(f"  {column['field']}: actual token+LF bytes={column['text_bytes']:,}, "
                             f"whole-column text zlib9 proxy={column['zlib9_bytes']:,}, nulls={column['nulls']:,}")
            else:
                lines.append(f"  {column['field']}: raw={column['raw_bytes']:,}, stored={column['stored_bytes']:,}, "
                             f"encodings={dict(column['encodings'])}, codecs={dict(column['codecs'])}")
    lines.append("shared bytes: " + json.dumps(result["shared"], separators=(",", ":")))
    digest = result.get("logical_sha256", result.get("v1_sha256"))
    lines.append(("logical_sha256" if result["version"] == 2 else "v1_sha256") + ": " + digest)
    return "\n".join(lines) + "\n"


def render_comparison(label: str, row: dict[str, Any]) -> str:
    show = lambda value: "PENDING" if value is None else str(value)
    signed = lambda value: "PENDING" if value is None else f"{value:+d}"
    return (f"{row['evidence']} {label} B={show(row['actual_bars'])} N={row['raw_E']} "
            f"raw_E={row['raw_E']} T={row['T']} "
            f"anchors={show(row['anchors'])} profile={row['profile']} "
            f"v1={show(row['v1'])} full_v2={show(row['full_v2'])} comparable={show(row['COMPARABLE_V2'])} "
            f"C_meta={show(row['C_meta'])} {row['exclusion']} clause={row['clause']} "
            f"limit={show(row['integer_limit'])} comparable-v1={signed(row['comparable_minus_v1'])} "
            f"full-v1={signed(row['full_minus_v1'])} "
            f"admission={row.get('admission', 'PENDING' if row['full_v2'] is None else 'ADMITTED')} "
            f"{row['verdict']}" +
            (f" counterfactual-size={row['size_verdict']}" if "size_verdict" in row else ""))


def render(result: dict[str, Any]) -> str:
    evidence = result.get("real_summary", {"status": "PENDING-REAL-SUMMARY"})
    # This must be the first output, including single-file/JSON report modes.
    lines = [evidence["status"]]
    if evidence["status"] == "REAL-SUMMARY":
        lines.append(json.dumps(evidence, ensure_ascii=True, separators=(",", ":")))
    # 0.5/15.1: these two evaluated inequalities immediately follow the real
    # J/C/projection evidence, before any grid cell or other model diagnostic.
    for row in result.get("binding_checks", []):
        lines.append("MODEL-ONLY BINDING " + json.dumps(row, separators=(",", ":")))
    if "binding_verdict" in result:
        lines.append("Binding checks: " + result["binding_verdict"])
        lines.append(f"K={SPEC_SMALL_K} FIXED (0.5); MODEL AND MEASURED; never re-derived.")
    if "grid" in result:
        lines.extend(("Section 15.1; all planning verdicts MODEL-ONLY; absent captured files PENDING.",
                      "Model assumptions: H2=947 J=4096 C=1024 J_Q=1024 C_Q=512 J_M=3073 C_M=769 C_meta=512.",
                      "SUMMARY is always one zstd frame. Only its C-byte payload is deductible.",
                      "0.5: (raw_E<128 AND T<4) OR raw_E<64 -> (ii-a); every other run -> (i).",
                      "COMPARABLE_V2 <= v1+K under (ii-a), strictly < v1 under (i). K is fixed at 3072.",
                      f"SUMMARY: J/window <= S_MAX={SUMMARY_CEILING} AND 52+C <= {SUMMARY_STORED_LIMIT}; "
                      "excess is summary_limit. Depth <=64.",
                      "MODEL-ONLY: empty, N100/T0 and N999/T0 also pass on full bytes; see complete-file diagnostics."))
        for row in result["break_even"]:
            lines.append("MODEL-ONLY break-even " + json.dumps(row, separators=(",", ":")))
        for row in result["grid"] + result["extremes"]:
            label = row.get("name", f"N={row['N']},T={row['T']}") + f",f={row['f']}"
            if row.get("shape"):
                label = row["shape"] + "," + label
            lines.append(render_comparison(label, row["model"]["comparison"]))
            lines.append("MODEL-ONLY independent v1 byte count " + label + " " +
                         json.dumps(row["model"]["v1_crosscheck"], separators=(",", ":")))
            lines.append("MODEL-ONLY independent v2 region count " + label + " " +
                         json.dumps(row["model"]["v2_crosscheck"], separators=(",", ":")))
            if "name" in row:
                lines.append("MODEL-ONLY SUMMARY " + label + " " +
                             json.dumps(row["model"]["summary_model"], separators=(",", ":")))
            measured = row.get("measured") or [pending_measurement(row)]
            lines.extend(render_comparison(label, item) for item in measured)
            lines.extend(render_comparison(label, item) for item in row.get("fixture_measurements", []))
            for item in measured + row.get("fixture_measurements", []):
                if "summary_probe" in item:
                    lines.append(item["evidence"] + " SUMMARY " + label + " " +
                                 json.dumps(item["summary_probe"], separators=(",", ":")))
            for name, item in (("MODEL-ONLY", row["model"]["comparison"]), *(
                    ("MEASURED", value) for value in measured)):
                if "empty_complete_file" in item:
                    lines.append(name + " EMPTY COMPLETE FILE (no deduction): " +
                                 json.dumps(item["empty_complete_file"], separators=(",", ":")))
                elif "complete_file" in item:
                    lines.append(name + " COMPLETE FILE (no deduction) " + label + ": " +
                                 json.dumps(item["complete_file"], separators=(",", ":")))
        lines.append("Overall production size acceptance: " + result["acceptance"])
    elif "v1" in result:
        lines.extend((render_file(result["v1"]).rstrip(), render_file(result["v2"]).rstrip(),
                      render_comparison("captured-pair", result["comparison"])))
        lines.append("SUMMARY probes: " + json.dumps(result["v2"]["summary_probe"], separators=(",", ":")))
        if "empty_complete_file" in result["comparison"]:
            lines.append("MEASURED EMPTY COMPLETE FILE (no deduction): " +
                         json.dumps(result["comparison"]["empty_complete_file"], separators=(",", ":")))
    else:
        lines.append(render_file(result).rstrip())
        if "summary_probe" in result:
            lines.append("SUMMARY probes: " + json.dumps(result["summary_probe"], separators=(",", ":")))
        lines.append("Whole-file comparison: PENDING (a matching v1/v2 pair is required)")
    return "\n".join(lines) + "\n"


def v1_digit_count(n: int, t: int, fraction: Fraction) -> dict[str, Any]:
    """15.1 independent route: count printed pieces, not the blended rate/M1."""
    fraction = Fraction(fraction)
    # E: prefix/tag/separators/time/LF = 22; flat numeric text 7+3,
    # in-position numeric text 18+18. T: float text 18+6+5*19,
    # remaining pieces 5+5+7+28+12+6+5+2+10+2. No model totals table.
    e_fixed = (5 + 13 + 1 + 1 + 2) * n
    flat = (7 + 3) * (1 - fraction) * n
    positioned = (18 + 18) * fraction * n
    t_float = (18 + 6 + 5 * 19) * t
    t_other = (5 + 5 + 7 + 28 + 12 + 6 + 5 + 2 + 10 + 2) * t
    trailer = 138 + len(str(n)) + len(str(t))
    total = 810 + trailer + e_fixed + flat + positioned + t_float + t_other
    return {"header": 810, "trailer": trailer, "E_fixed": e_fixed,
            "E_numeric": str(flat + positioned), "T_float": t_float, "T_other": t_other,
            "v1": -(-total.numerator // total.denominator)}


def v2_region_count(n: int, t: int, fraction: Fraction, c: int) -> dict[str, Any]:
    """Second route: separate E/T/I payloads, headers/SHA, and physical pages."""
    populations = ((n + 4095) // 4096, (t + 1023) // 1024, (n + t + 65535) // 65536)
    d = sum(populations)
    block_headers = 32 * d
    directories_and_sha = sum(count * columns * (16 + 32)
                               for count, columns in zip(populations, (3, 20, 1)))
    payload = (Fraction(1, 10) + 11 * fraction) * n + 55 * t + Fraction(n + t, 10)

    def physical_table(count: int, width: int) -> int:
        full, last = divmod(count, 61440 // width)
        return full * 65536 + (4096 + last * width if last else 0)

    descriptors = sum(physical_table(count, 512) for count in populations)
    ranks = sum(physical_table(t, 4) for _ in range(10))
    figure = physical_table(min(n, 1200), 256)
    shared = 947 + (4 + 4 + 4 + 4 + 4 + 32) + c
    data = block_headers + directories_and_sha + payload
    compact = shared + data + d * 96 + 320 + 160
    full = shared + data + descriptors + ranks + figure + 4096 + 160
    rounded = lambda value: -(-value.numerator // value.denominator)
    return {"block_headers": block_headers, "directories_and_sha": directories_and_sha,
            "payload_E_T_I": str(payload), "descriptors_FULL": descriptors, "ranks": ranks,
            "figure": figure, "COMPACT": rounded(compact), "FULL": rounded(full)}


def model_size(n: int, t: int, fraction: Fraction, *, summary_stored: int = 1024,
               summary_json: int = 4096, excluded: int = 512, actual_bars: int | None = None,
               anchors: int = 0) -> dict[str, Any]:
    """Literal 15.1 arithmetic, kept separate from every measured byte count."""
    fraction = Fraction(fraction)
    ceil = lambda value, divisor: (value + divisor - 1) // divisor
    e, trades, interleave = ceil(n, 4096), ceil(t, 1024), ceil(n + t, 65536)
    blocks = e + trades + interleave
    compact = n < 10980 and t < 3314 and blocks <= 128
    pages = lambda count, width: count * width + 4096 * ceil(count, 61440 // width) if count else 0
    metadata1, metadata2 = 948 + len(str(n)) + len(str(t)), 947 + 52 + summary_stored
    framing = 176 * e + 992 * trades + 80 * interleave
    payload = (Fraction(1, 5) + 11 * fraction) * n + Fraction(551, 10) * t
    size1 = (32 + 26 * fraction) * n + 201 * t + metadata1
    size_c = metadata2 + payload + framing + 96 * blocks + 480
    size_f = (metadata2 + payload + framing + sum(pages(count, 512) for count in (e, trades, interleave)) +
              10 * pages(t, 4) + pages(min(n, 1200), 256) + 4256)
    rounded = lambda value: -(-value.numerator // value.denominator)
    profile = "COMPACT" if compact else "FULL"
    crosscheck = v1_digit_count(n, t, fraction)
    if rounded(size1) != crosscheck["v1"]:
        raise ReportError("MODEL-ONLY v1 formula disagrees with independent printed-byte count")
    regions = v2_region_count(n, t, fraction, summary_stored)
    if rounded(size_c) != regions["COMPACT"] or rounded(size_f) != regions["FULL"]:
        raise ReportError("MODEL-ONLY v2 formula disagrees with independent region count")
    result = {"v1": rounded(size1), "v1_crosscheck": crosscheck,
              "v2_crosscheck": regions,
              "COMPACT": rounded(size_c) if compact else None,
              "FULL": rounded(size_f), "profile": profile,
              "summary_model": {"J": summary_json, "C": summary_stored, "J_Q": 1024, "C_Q": 512,
                                "J_M": 3073, "C_M": 769, "C_meta": excluded}}
    result["comparison"] = size_comparison(result["v1"], result[profile], excluded,
                                           bars=n if actual_bars is None else actual_bars,
                                           counts={"E": n, "T": t}, profile=profile,
                                           evidence="MODEL-ONLY", anchors=anchors)
    refused = summary_json > SUMMARY_CEILING or 52 + summary_stored > SUMMARY_STORED_LIMIT
    result["comparison"]["admission"] = "REFUSED(summary_limit)" if refused else "MODEL-ONLY FITS"
    if refused:
        result["comparison"]["size_verdict"] = result["comparison"]["verdict"]
        result["comparison"]["verdict"] = "MODEL-ONLY REFUSED(summary_limit)"
        result["comparison"]["complete_file"]["counterfactual"] = True
    return result


def corner_models(fractions=FRACTIONS, *, extreme: bool = False) -> list[dict[str, Any]]:
    """15.1: 147 cells per ordinary/selected shape; also required extremes."""
    rows = []
    cells = [(n, t) for n in CORNER_E for t in range(4)] + [(n, 4) for n in T4_E]
    for shape in ("ordinary", "selected_report_view"):
        for n, t in cells:
            bars, anchors = (n, 0) if shape == "ordinary" else (1000, 1)
            for fraction in fractions:
                model = model_size(n, t, fraction, actual_bars=bars, anchors=anchors)
                row = {"shape": shape, "B": bars, "N": n, "raw_E": n,
                       "anchors": anchors, "T": t, "f": float(fraction), "profile": model["profile"],
                       "actual": {"v1": None, "COMPACT": None, "FULL": None}, "model": model}
                if extreme:
                    row["name"] = f"{shape}/E{n}/T{t}"
                rows.append(row)
    return rows


def extreme_models() -> list[dict[str, Any]]:
    rows = [{"name": "empty", "N": 0, "T": 0, "f": None, "model": model_size(0, 0, Fraction(0))}]
    for name, n, t in (("100/0", 100, 0), ("100/1", 100, 1), ("999/0", 999, 0),
                       ("1000/0", 1000, 0), ("compressible-1MiB", 100, 1),
                       ("incompressible-1MiB", 100, 1), ("ceiling-compressible", 100, 1),
                       ("ceiling-raw-fallback", 100, 1), ("just-above-ceiling", 100, 1)):
        for fraction in FRACTIONS:
            large = "1MiB" in name or "ceiling" in name
            summary = (large_summary_model(SUMMARY_CEILING + int(name == "just-above-ceiling"),
                                            raw=name in ("incompressible-1MiB", "ceiling-raw-fallback"))
                       if large else {"J": 4096, "C": 1024, "C_meta": 512})
            model = model_size(n, t, fraction, summary_stored=summary["C"],
                               summary_json=summary["J"], excluded=summary["C_meta"])
            model["summary_model"].update(summary)
            rows.append({"name": name, "N": n, "T": t, "f": float(fraction), "model": model})
    return rows + corner_models(extreme=True)


def large_summary_model(j: int, *, raw: bool) -> dict[str, Any]:
    """Two independent frame/projection counts; these are never measurements."""
    jq, cq = 1024, 512
    jm = j - jq + 1  # Compact Q/M objects share one comma/braces adjustment.
    if raw:
        c = j + 9 + 3 * ((j + 131071) // 131072)
        cm = jm + 9 + 3 * ((jm + 131071) // 131072)

        def framed_pieces(size: int) -> int:
            full, last = divmod(size, 131072)
            return 9 + full * (131072 + 3) + (last + 3 if last else 0)

        other_c, other_cm = framed_pieces(j), framed_pieces(jm)
    else:
        c, cm = (j + 3) // 4, (jm + 3) // 4
        # Count complete groups plus a residual group independently, including
        # the S_MAX+1 counterfactual (not divisible by four).
        groups, residual = divmod(j, 4)
        removed, retained = divmod(jq - 1, 4)
        other_c = groups + int(residual > 0)
        other_cm = groups - removed - ((retained - residual) // 4)
    excluded = metadata_exclusion(c, cq, cm)
    counted_b = max(cq, other_c - other_cm)
    excluded_b = other_c - counted_b
    if (c, cm, excluded) != (other_c, other_cm, excluded_b):
        raise ReportError("MODEL-ONLY SUMMARY projection/frame routes disagree")
    return {"J": j, "C": c, "J_Q": jq, "C_Q": cq, "J_M": jm, "C_M": cm, "C_meta": excluded,
            "C_comparable": c - excluded, "stored_region": 52 + c,
            "stored_minus_limit": 52 + c - SUMMARY_STORED_LIMIT, "variant": "raw-fallback" if raw else "compressible",
            "route_b": {"C": other_c, "C_M": other_cm, "C_meta": excluded_b,
                        "C_comparable": counted_b,
                        "stored_minus_limit": other_c - (SUMMARY_STORED_LIMIT - 52)}}


def pending_measurement(row: dict[str, Any]) -> dict[str, Any]:
    return size_comparison(None, None, None, bars=row.get("B", row["N"]),
                           counts={"E": row["N"], "T": row["T"]},
                           profile=row["model"]["profile"], evidence="MEASURED",
                           anchors=row.get("anchors", 0))


def break_even(evidence: dict[str, Any]) -> list[dict[str, Any]]:
    """15.1 table, two byte-count routes; limits never set or adjust K."""
    j, c = evidence.get("J"), None
    if evidence.get("status") == "REAL-SUMMARY" and evidence.get("probe_status") == "MEASURED":
        stored, excluded = evidence.get("C"), evidence.get("C_meta")
        if stored is not None and excluded is not None:
            if not 0 <= excluded <= stored or evidence.get("C_comparable") != stored - excluded:
                raise ReportError("real SUMMARY counted frame must equal C-C_meta")
            c = stored - excluded
    rows = []
    cells = ((1, 1), (64, 4), (100, 1), (128, 1), (1000, 0), (0, 0), (100, 0))
    for f in FRACTIONS:
        for n, t in cells:
            if n == 0 and f != FRACTIONS[0]:
                continue
            # Route A: the complete model at C=0, subtracted from the integer
            # size limit. model_size also checks v1/v2 via independent regions.
            model = model_size(n, t, f, summary_stored=0, excluded=0)
            base = model["comparison"]
            limit = base["integer_limit"] - base["full_v2"]
            # Route B (15.1): start with the counted-512 excess, then spend
            # the remaining fixed allowance, or enforce the strict one-byte
            # saving under (i). No KiB rounding or worst-cell selection.
            reference = model_size(n, t, f)
            counted = reference["v2_crosscheck"][reference["profile"]] - reference["summary_model"]["C_meta"]
            excess = counted - model["v1_crosscheck"]["v1"]
            other_limit = 512 + (SPEC_SMALL_K if base["clause"] == "(ii-a)" else -1) - excess
            if limit != other_limit:
                raise ReportError("MODEL-ONLY break-even byte-count routes disagree")
            evaluation = "PENDING-REAL-SUMMARY" if c is None else "PASS" if c <= limit else "FAIL"
            row = {"evidence": "MODEL-ONLY", "cell": f"{n}/{t}{base['clause']}",
                   "f": float(f), "clause": base["clause"],
                   "binding": (n, t) in cells[:2] and f == FRACTIONS[0],
                   "v1": base["v1"], "integer_limit": base["integer_limit"],
                   "base_without_frame": base["full_v2"], "counted_frame_limit": limit,
                   "route_b": {"base_without_frame": model["v2_crosscheck"][model["profile"]],
                               "excess_at_counted_512": excess, "counted_frame_limit": other_limit},
                   "J_real": j, "C_comparable_real": c,
                   "comparable_with_real_frame": base["full_v2"] + c if c is not None else None,
                   "ratio_real": c / j if c is not None and j else None,
                   "limit_over_J_real": limit / j if j else None,
                   "verdict": "MODEL-ONLY " + evaluation}
            if n == t == 0:
                full_c = evidence.get("C") if evidence.get("status") == "REAL-SUMMARY" else None
                row["full_frame_diagnostic"] = {
                    "C_real": full_c, "frame_limit": limit,
                    "verdict": "MODEL-ONLY " + ("PENDING-REAL-SUMMARY" if full_c is None else
                                              "PASS" if full_c <= limit else "FAIL")}
            rows.append(row)
    return rows


def binding_checks(evidence: dict[str, Any]) -> dict[str, Any]:
    checks = break_even(evidence)[:2]
    verdict = ("FAIL" if any(row["verdict"] == "MODEL-ONLY FAIL" for row in checks) else
               "PENDING-REAL-SUMMARY" if any(row["verdict"] == "MODEL-ONLY PENDING-REAL-SUMMARY" for row in checks)
               else "MODEL-ONLY PASS")
    return {"binding_checks": checks, "binding_verdict": verdict,
            "K": SPEC_SMALL_K, "K_status": "FIXED (0.5); MODEL AND MEASURED"}


def attach_measurements(result: dict[str, Any], manifest: Path) -> None:
    """Read complete captured pairs, not byte counts asserted by a caller.

    JSON list entries: N,T,f, v1,v2 (paths relative to manifest), provenance,
    mode, source_zstd_build, anchors; extreme entries also name the extreme.
    Corner grid entries use shape=ordinary or selected_report_view; extreme
    names identify that shape. Selected rows need actual_bars>=1000 and an
    explicit anchor count.
    actual_bars is optional when diagnostics.input_bars_processed is present.
    """
    entries = json.loads(manifest.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise ReportError("measurements manifest must be a list of captured pairs")
    for entry in entries:
        if (type(entry) is not dict or any(key not in entry for key in ("N", "T", "f", "v1", "v2")) or
                any(type(entry[key]) is not int or entry[key] < 0 for key in ("N", "T")) or
                any(type(entry[key]) is not str for key in ("v1", "v2"))):
            raise ReportError("captured pair needs nonnegative integer N/T, f, and v1/v2 artifact paths")
        if not entry.get("provenance") or not entry.get("mode"):
            raise ReportError("captured pair needs provenance and mode")
        matches = [row for row in result["grid"] + result["extremes"]
                   if (row["N"], row["T"], row["f"], row.get("name"), row.get("shape")) ==
                   (entry["N"], entry["T"], entry["f"], entry.get("name"),
                    entry.get("shape", "selected_report_view" if str(entry.get("name", "")).startswith(
                        "selected_report_view/") else "ordinary" if str(entry.get("name", "")).startswith(
                        "ordinary/") else None))]
        if len(matches) != 1:
            raise ReportError("captured pair does not name exactly one grid/extreme cell")
        first = measure_v1(manifest.parent / entry["v1"])
        second = measure_v2(manifest.parent / entry["v2"], content_digest=True,
                            source_zstd_build=entry.get("source_zstd_build"))
        if first["v1_sha256"] != second["v1_sha256"]:
            raise ReportError("captured pair has different v1 content")
        counts = {tag: info["count"] for tag, info in second["types"].items()}
        if (counts["E"], counts["T"]) != (entry["N"], entry["T"]):
            raise ReportError("captured pair populations differ from its cell")
        actual_bars = entry.get("actual_bars", second["actual_bars"])
        if second["actual_bars"] is not None and actual_bars != second["actual_bars"]:
            raise ReportError("actual bars differ from captured diagnostics")
        if actual_bars is not None and (type(actual_bars) is not int or actual_bars < 0):
            raise ReportError("actual bars must be a nonnegative integer")
        anchors = entry.get("anchors")
        if anchors is not None and (type(anchors) is not int or not 0 <= anchors <= counts["E"]):
            raise ReportError("anchors must be a nonnegative raw-E subcount")
        if matches[0].get("shape") == "selected_report_view" and (
                actual_bars is None or actual_bars < 1000 or anchors is None or
                entry["mode"] not in ("selected", "selected-outputs")):
            raise ReportError("selected_report_view needs B>=1000, raw E, anchors and selected mode")
        if matches[0].get("shape") == "ordinary" and (
                actual_bars != counts["E"] or entry["mode"] not in ("ordinary", "ordinary-outputs") or
                anchors not in (None, 0)):
            raise ReportError("ordinary corner needs B=raw E, no anchors and ordinary mode")
        evidence = real_summary(manifest.parent / entry["v2"], entry["provenance"],
                                entry["mode"], entry.get("source_zstd_build"))
        name = entry.get("name")
        if name in ("compressible-1MiB", "incompressible-1MiB") and evidence["J"] != 1024 * 1024:
            raise ReportError("retained large-metadata cell needs exactly 1 MiB decoded SUMMARY")
        if name in ("ceiling-compressible", "ceiling-raw-fallback") and evidence["J"] != SUMMARY_CEILING:
            raise ReportError("ceiling cell needs J=S_MAX decoded SUMMARY")
        if name == "just-above-ceiling":
            raise ReportError("just-above cell requires writer-refusal evidence, not an accepted sink")
        if name == "ceiling-raw-fallback":
            with v2_module().open(manifest.parent / entry["v2"]) as sink:
                frame = sink.summary_frame_bytes()
                expected = importlib.import_module("pf_sink2_reference").raw_summary_frame(sink.summary_json_bytes())
            if frame != expected:
                raise ReportError("raw-fallback ceiling cell needs the exact 9.1 frame")
        if name == "incompressible-1MiB" and (
                evidence["C_reencoded"] is None or evidence["C_reencoded"] < evidence["J"]):
            raise ReportError("incompressible claim shrinks at the captured HEAD level")
        measured = size_comparison(first["file_bytes"], second["file_bytes"], evidence["C_meta"],
                                   bars=actual_bars, counts=counts, profile=second["profile"],
                                   evidence="MEASURED", anchors=entry.get("anchors"))
        measured.update(provenance=entry["provenance"], mode=entry["mode"], summary_probe=evidence)
        matches[0].setdefault("measured", []).append(measured)


def measure_grid(n_values=GRID_N, fractions=FRACTIONS, *, encode_fixtures: bool = True,
                 real_evidence: dict[str, Any] | None = None,
                 measurements: Path | None = None) -> dict[str, Any]:
    """Models plus optional actual synthetic files and captured production pairs."""
    evidence = real_evidence or {"status": "PENDING-REAL-SUMMARY"}
    checks = binding_checks(evidence)
    rows = [{"N": n, "T": t, "f": float(fraction), "profile": model_size(n, t, fraction)["profile"],
             "actual": {"v1": None, "COMPACT": None, "FULL": None}, "model": model_size(n, t, fraction)}
            for n in n_values for t in (0, 1, n // 20) for fraction in fractions]
    rows.extend(corner_models(fractions))
    result = {"real_summary": evidence, **checks, "grid": rows, "extremes": extreme_models(),
              "break_even": break_even(evidence), "acceptance": "PENDING"}
    if encode_fixtures:
        measure_fixture_grid(rows)
    if measurements is not None:
        attach_measurements(result, measurements)
    for row in rows + result["extremes"]:
        row.setdefault("measured", [pending_measurement(row)])
    captured = [item for row in rows + result["extremes"] for item in row.get("measured", [])]
    if result["binding_verdict"] == "FAIL" or any(item["verdict"] == "FAIL" for item in captured):
        result["acceptance"] = "FAIL"
    elif (evidence.get("probe_status") == "MEASURED" and
          all(row.get("measured") and all(item["verdict"] == "PASS" for item in row["measured"])
              for row in rows + result["extremes"])):
        # This finite population cannot prove the specification's EVERY-run
        # guarantee, including arbitrary O/V/K/X/H shapes.
        result["acceptance"] = "MEASURED-GRID-PASS; EVERY-RUN GUARANTEE PENDING"
    return result


def measure_fixture_grid(rows: list[dict[str, Any]]) -> None:
    """Keep the original streaming file experiment, explicitly synthetic."""
    v2_module()
    tests = str(REPO / "tests")
    if tests not in sys.path:
        sys.path.insert(0, tests)
    reference = importlib.import_module("pf_sink2_reference")
    fixtures = importlib.import_module("sink_v2_cases")
    head = fixtures.header("vec-equity4")
    build_id = zstd_build()["id"]
    with tempfile.TemporaryDirectory() as scratch:
        first, second = Path(scratch) / "input.jsonl", Path(scratch) / "output.pfs2"
        for cell in rows:
            n, t, fraction = cell["N"], cell["T"], cell["f"]
            # A small grammar fixture, not the 4096-byte production MODEL and
            # never evidence that any captured run has this metadata shape.
            summary = {"engine": "pineforge", "input": {}, "applied_inputs": {},
                       "applied_overrides": {}, "applied_runtime": {}, "elapsed_seconds": 0.0,
                       "summary": {}, "metrics": {},
                       "diagnostics": {"input_bars_processed": cell.get("B", n)}, "fingerprint": None,
                       "sink_folds": {"closed_count": t, "open_at_end_count": 0, "open_at_end_pnl": 0.0}}
            digest = hashlib.sha256()
            with first.open("wb") as stream:
                line = fixtures.v1_line(head)
                stream.write(line)
                digest.update(line)
                for row in fixtures.iter_population_rows(n, t, fraction):
                    if row[0] == "T":
                        row[15] = False
                    line = fixtures.v1_line(row)
                    stream.write(line)
                    digest.update(line)
                stream.write(fixtures.v1_line({"trailer": "pf-report-sink", "version": 1,
                                              "counts": {"E": n, "T": t}, "sha256": digest.hexdigest()}))
            actual1 = measure_v1(first)["file_bytes"]
            reference.from_v1(first, second, summary, compress=True)
            canonical = measure_v2(second, content_digest=True, source_zstd_build=build_id)
            if canonical["v1_sha256"] != digest.hexdigest():
                raise ReportError("grid input/converted v1 identity differs")
            variants = [canonical]
            actual_c = canonical["file_bytes"] if canonical["profile"] == "COMPACT" else None
            actual_f = canonical["file_bytes"]
            if actual_c is not None:
                reference.from_v1(first, second, summary, compress=True, force_full=True)
                full = measure_v2(second, content_digest=True, source_zstd_build=build_id)
                if full["v1_sha256"] != digest.hexdigest() or full["logical_sha256"] != canonical["logical_sha256"]:
                    raise ReportError("grid profile populations/logical identities differ")
                actual_f = full["file_bytes"]
                variants.append(full)
            cell["actual"] = {"v1": actual1, "COMPACT": actual_c, "FULL": actual_f}
            cell["fixture_measurements"] = []
            for variant in variants:
                item = size_comparison(actual1, variant["file_bytes"], variant["summary_probe"]["C_meta"],
                                       bars=cell.get("B", n), counts={"E": n, "T": t}, profile=variant["profile"],
                                       evidence="MEASURED-FIXTURE", anchors=cell.get("anchors", 0))
                item["summary_probe"] = variant["summary_probe"]
                cell["fixture_measurements"].append(item)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, nargs="?", help="v1 or v2 sink (detected by content)")
    parser.add_argument("--grid", action="store_true",
                        help="measure 63 original plus 294 ordinary/selected corner cells in both legal profiles")
    parser.add_argument("--model-only", action="store_true", help="leave whole-file grid measurements PENDING")
    parser.add_argument("--measurements", type=Path, help="manifest of captured production v1/v2 pairs")
    parser.add_argument("--real-summary", type=Path, help="production pfs2 artifact to print FIRST")
    parser.add_argument("--provenance", help="captured-run/artifact identity for --real-summary")
    parser.add_argument("--mode", choices=("ordinary", "selected", "ordinary-outputs", "selected-outputs"))
    parser.add_argument("--source-zstd-build", help="writer build ID from capture logs; must match local probe build ID")
    parser.add_argument("--v2", type=Path, help="also measure a matching v2 file and compare v1-export identity")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--pf-sink", type=Path, default=DEFAULT_PF_SINK)
    parser.add_argument("--no-model", action="store_true", help="compatibility flag for single-file measurement")
    args = parser.parse_args(argv)
    if args.grid and (args.path is not None or args.v2 is not None):
        parser.error("--grid cannot be combined with an input file")
    if not args.grid and args.path is None:
        parser.error("an input file or --grid is required")
    if args.real_summary and (not args.provenance or not args.mode):
        parser.error("--real-summary requires --provenance and --mode")
    if (args.model_only or args.measurements) and not args.grid:
        parser.error("--model-only and --measurements require --grid")
    try:
        evidence = (real_summary(args.real_summary, args.provenance, args.mode, args.source_zstd_build)
                    if args.real_summary else {"status": "PENDING-REAL-SUMMARY"})
        first = b""
        if args.grid:
            result = measure_grid(encode_fixtures=not args.model_only, real_evidence=evidence,
                                  measurements=args.measurements)
        else:
            with args.path.open("rb") as source:
                first = source.read(8)
        if args.grid:
            pass
        elif first == b"\x89PFS2\r\n\x1a":
            if args.v2:
                raise ReportError("--v2 comparison requires a v1 positional input")
            result = measure_v2(args.path, source_zstd_build=args.source_zstd_build)
        else:
            result = measure_v1(args.path, load_reader_module(args.pf_sink))
            if args.v2:
                measured = measure_v2(args.v2, content_digest=True, source_zstd_build=args.source_zstd_build)
                if result["v1_sha256"] != measured["v1_sha256"]:
                    raise ReportError("comparison files have different v1-export identities")
                result = {"v1": result, "v2": measured,
                          "same_v1_sha256": True,
                          "ratio_v1_over_v2": result["file_bytes"] / measured["file_bytes"],
                          "comparison": size_comparison(
                              result["file_bytes"], measured["file_bytes"], measured["summary_probe"]["C_meta"],
                              bars=measured["actual_bars"],
                              counts={tag: info["count"] for tag, info in measured["types"].items()},
                              profile=measured["profile"], evidence="MEASURED")}
        # Insertion order is the JSON output contract too.
        result = {"real_summary": evidence, **binding_checks(evidence), **result}
    except (ValueError, OSError) as error:
        print(f"sink_size_report: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, indent=2) if args.json else render(result), end="\n" if args.json else "")
    return 1 if result["binding_verdict"] == "FAIL" or result.get("acceptance") == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
