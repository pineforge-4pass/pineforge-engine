"""Generate canonical stage-C vectors with stage A and the REAL transcoder.

No reference encoder supplies a release vector. --check reruns both tools,
verifies every inverse and compares every artifact byte. Run only on the lab.
Reader-only noncanonical layouts/faults live in the separate reference-control
generator and are not promoted as stage-C files.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "docker"))
import pf_sink
import pf_sink2
import selected_window_report as swr
from sink_v2_cases import (BUNDLE, header, metadata, normalized, population_rows,
                           report_trade, sink_folds, v1_file, v1_line, Case, v1_report)


def compact(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")


def producer_metadata(name, rows=(), *, outputs=False, selected=False):
    """Stage-C captures use v1 producer fields; additive controls stay separate."""
    seed = metadata(name, rows, outputs=outputs)
    seed["input"] = {"ohlcv": str(Path("fixtures") / (name + ".csv"))}
    if selected:
        times = [row[1] for row in rows if row[0] == "E"]
        # The first E is the anchor, followed by the selected script bars.
        bars = len(times) - 1
        start, end = times[0], times[-1] + 60000
        first, last = (times[1], times[-1]) if bars else (None, None)
        request = swr.WindowRequest(start, end, 0, start, "1", "1", 60)
        plan = {key: bars for key in ("supplied_input_bars", "supplied_script_bars",
                                     "fed_input_bars", "fed_script_bars",
                                     "window_input_bars", "window_script_bars")}
        plan.update({key: 0 for key in ("available_script_bars", "used_script_bars",
                                       "trimmed_script_bars", "trim_index", "preroll_input_bars")})
        plan.update({key: value for prefix in ("window", "fed", "supplied")
                     for key, value in ((prefix + "_first_data_ms", first),
                                        (prefix + "_last_data_ms", last))})
        plan.update(preroll_first_bar_ms=None, preroll_last_bar_ms=None,
                    trim_start_ms=start, shortfall=False)
        source_hash = hashlib.sha256(compact([row for row in rows if row[0] == "E"])).hexdigest()
        feed = {key: source_hash for key in ("source_bytes_sha256", "source_values_sha256",
                                             "evaluated_source_values_sha256")}
        feed["input_tf_seconds"] = request.input_tf_seconds
        window = swr.build_report_window(request, plan, applied=True, feed=feed,
                                         calendar={"timezone": "UTC", "session": None})
        seed["input"].update(bars=bars, first_ts=first, last_ts=last)
        seed["diagnostics"].update(input_bars_processed=bars, script_bars_processed=bars)
        seed["applied_runtime"]["report_window"] = window
        seed["report_window"] = window
        seed["report_shape"] = swr.report_shape_full(len(times), sum(row[0] == "T" for row in rows))
        seed["fingerprint"] = {"version": swr.FINGERPRINT_VERSION,
                               "provenance": {"schema_version": swr.FINGERPRINT_VERSION,
                                              "runtime": {"report_window": window}}}
        seed["applied_inputs"]["label"] = "完整"
        seed["diagnostics"]["timing"] = {"samples_ns": [123456789, 234567890]}
        seed["diagnostics"]["throughput"] = {"items_processed": bars,
                                            "samples_ns": [123456789, 234567890],
                                            "magnifier_mode": "none"}
        seed["diagnostics"]["phase_timing"] = {"version": 1, "execution_ms": -0.0,
                                              "result_assembly_ms": 1e-7, "results_digest_ms": None}
    return seed


def producer_output_manifest():
    """A recorded manifest with unique identities and one entry per slot."""
    manifest = {
        "message_format": "pineforge/v1",
        "outputs": [{"index": 0, "id": 'alert-é雪😀\x00"\\', "kind": "alert", "freq": "all"},
                    {"index": 1, "id": "plot-雪", "kind": "plot"},
                    {"index": 2, "id": "line", "kind": "hline", "price": {"constant": 1}}],
        "series": [{"slot": 0, "output": "color", "encoding": "rgba-u32"},
                   {"slot": 1, "output": "float"}],
        "constants": [{"index": 0, "encoding": "rgba-u32"}, {"index": 1}],
    }
    return json.dumps(manifest, ensure_ascii=False, indent=1).encode("utf-8")


def inputs():
    """Input-derived cases. No output bytes, checksum pins or expected sizes."""
    for name in ("empty0", "equity4", "trades3", "multiblock", "equity4-zstd"):
        rows = [json.loads(line) for line in (BUNDLE / (name + ".records.jsonl")).read_bytes().splitlines()
                if line]
        yield name, header("vec-" + name), rows, producer_metadata(name, rows)
    for name, e, t in (("compact-profile", 1000, 50), ("full-profile", 100000, 5000),
                       ("boundary-e10979", 10979, 1), ("boundary-e10980", 10980, 1),
                       ("boundary-t3313", 1200, 3313), ("boundary-t3314", 1200, 3314),
                       ("multiblock-default", 10000, 2500), ("zstd-real", 4096, 0)):
        rows = population_rows(e, t)
        yield name, header("vec-" + name), rows, producer_metadata(name, rows)
    one = [report_trade(1)]
    yield "one-trade", header("vec-one-trade"), one, producer_metadata("one-trade", one)
    null_trade = report_trade(1)
    null_trade[5:13] = [None] * 8
    yield "all-null-trade", header("vec-all-null-trade"), [null_trade], producer_metadata("all-null-trade", [null_trade])
    rows = [report_trade(i + 1, key=float(i % 4)) for i in range(41)]
    for row in rows[::6]:
        row[8] = None
    yield "nullable-ranks", header("vec-nullable-ranks"), rows, producer_metadata("nullable-ranks", rows)
    safe = (1 << 53) - 1
    rows = [["E", -safe, -0.0, 0.0], ["E", safe, 1.0, -0.0], report_trade(1)]
    rows[-1][16] = safe
    yield "integer-boundaries", header("vec-integer-boundaries"), rows, producer_metadata("integer-boundaries", rows)
    rows = [["E", i, v, 0.0] for i, v in enumerate((5e-324, -5e-324, -0.0, 1e-7, 1e16, 1e-12))]
    yield "float-boundaries", header("vec-float-boundaries"), rows, producer_metadata("float-boundaries", rows)
    rows = [report_trade(i + 1, key=value) for i, value in enumerate((1e16, 1.0, -1e16, -0.0))]
    for row in rows:
        row[15] = True
    yield "fold-order", header("vec-fold-order"), rows, producer_metadata("fold-order", rows)
    rows = [["E", 0, 1.0, 0.0], report_trade(1)]
    rows[-1][-1] = 'é雪😀\x00\b\f\n\r\t\x7f"\\'
    yield "utf8-controls", header("vec-utf8-controls"), rows, producer_metadata("utf8-controls", rows)
    rows = [["K", None, -0.0]]
    yield "outputs-empty", header("vec-outputs-empty", outputs=True), rows, producer_metadata("outputs-empty", rows, outputs=True)
    rows = [["E", 0, 1.0, 0.0], ["O", 0, None, None, -0.0, None],
            ["V", 0, 0, 0, None, None, 0, 1, 1, None, "ffffffffffffffff", "雪\x00"],
            ["K", None, -0.0], ["X", 0, 0, "trace\x00", None], ["H", "8000000000000000"],
            report_trade(1)]
    yield "outputs", header("vec-outputs", outputs=True, extras=True), rows, producer_metadata("outputs", rows, outputs=True)
    rows = [["E", 0, 1.0, 0.0], ["E", 0, 2.0, -0.0], report_trade(1)]
    yield "selected-anchor", header("vec-selected-anchor"), rows, producer_metadata("selected-anchor", rows, selected=True)
    for name, entropy in (("summary-max", False), ("summary-entropy-candidate", True)):
        seed = producer_metadata(name)
        seed["applied_inputs"]["summary_payload"] = ""
        yield name, header("vec-" + name), [], seed


def checked(command, **kwargs):
    result = subprocess.run(command, capture_output=True, check=False, **kwargs)
    if result.returncode:
        raise ValueError(f"{command[0]} exited {result.returncode}: {result.stderr.decode(errors='replace')}")
    return result


def generate(name, head, rows, summary, args, scratch):
    head["engine"] = args.version["engine"]
    summary, oracle_report, oracle_refusal = v1_report(
        Case(name, head, rows, summary), manifest_factory=producer_output_manifest)
    if name in ("summary-max", "summary-entropy-candidate"):
        length = 1048576 - len(compact(summary))
        summary["applied_inputs"]["summary_payload"] = (
            base64.b85encode(hashlib.shake_256(b"sink-v2-stage-c-entropy").digest(length)).decode("ascii")[:length]
            if name == "summary-entropy-candidate" else "x" * length)
        summary, oracle_report, oracle_refusal = v1_report(
            Case(name, head, rows, summary), manifest_factory=producer_output_manifest)
        if len(compact(summary)) != 1048576:
            raise ValueError("SUMMARY input construction missed the decoded ceiling")
    original = v1_file(head, rows)
    source, verified = scratch / "source.v1", scratch / "verified.v1"
    stdout, output = scratch / "stdout.json", scratch / "out.pfs2"
    source.write_bytes(original)
    # sink_folds is end-written v2 metadata, absent from the v1 stdout capture.
    # The transcoder derives it independently from the verified input stream.
    stdout.write_bytes(compact({key: value for key, value in summary.items() if key != "sink_folds"}) + b"\n")
    with source.open("rb") as stream:
        result = checked([sys.executable, "-I", "-B", str(args.stage_a), "--in-fd", str(stream.fileno()),
                          "--out", str(verified), "--spool-dir", str(scratch),
                          "--max-bytes", str(len(original))], pass_fds=(stream.fileno(),))
    stage_receipt = json.loads(result.stdout)
    if not stage_receipt["complete"] or verified.read_bytes() != original:
        raise ValueError(name + ": stage-A verification/publication disagrees")
    receipt_path = scratch / "stage-a.json"
    receipt_path.write_bytes(result.stdout)
    # A safe, deliberately generous test cap. Production admission uses the
    # bound calculator and its own image reserve; this is not a size verdict.
    command = [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_encode_v2.py"),
               "--transcoder", str(args.transcoder), "--verified-input", str(verified),
               "--stage-a-receipt", str(receipt_path), "--summary", str(stdout),
               "--out", str(output), "--spool-dir", str(scratch),
               "--spool-max-bytes", str((1 << 53) - 1), "--spool-max-files", "1"]
    result = checked(command)
    receipt = json.loads(result.stdout)
    data = output.read_bytes()
    if not receipt["complete"] or receipt["sink_bytes"] != len(data):
        raise ValueError(name + ": incomplete/wrong-size receipt")
    with pf_sink2.open(data) as reader:
        reader.verify()
        actual_rows = [[row.tag, *row.values] for row in reader.iter_merged()]
        if b"".join(map(v1_line, actual_rows)) != b"".join(map(v1_line, normalized(rows, head))):
            raise ValueError(name + ": independent reader changed a field")
        inverse = io.BytesIO()
        reader.to_v1(inverse)
        if inverse.getvalue() != original or reader.logical_sha256 != receipt["sha256"]:
            raise ValueError(name + ": inverse/logical identity differs")
        if reader.summary()["sink_folds"] != sink_folds(normalized(rows, head)):
            raise ValueError(name + ": fold receipt differs")
        profile = reader.profile
        replay = subprocess.run(
            [sys.executable, "-I", "-B", str(ROOT / "docker/pineforge_sink_to_json.py"),
             "--in", str(verified), "--stdout-json", str(stdout), "--out", "-",
             "--max-bytes", str((1 << 53) - 1)], capture_output=True, check=False)
        expected_report = oracle_report
        actual_report = io.BytesIO()
        if oracle_refusal is None:
            if replay.returncode or replay.stdout != oracle_report:
                raise ValueError(name + ": retained-v1 replay disagrees with the existing v1 producer: "
                                 + replay.stderr.decode(errors="replace"))
            reader.to_report_json(actual_report)
            if actual_report.getvalue() != expected_report:
                raise ValueError(name + ": report bytes differ from retained-v1 replay")
        else:
            try:
                reader.to_report_json(actual_report)
            except pf_sink2.ReportResultInvalid as error:
                if {"code": error.code, "args": error.code_args} != oracle_refusal or actual_report.getvalue():
                    raise ValueError(name + ": report refusal differs from v1 replay") from error
            else:
                raise ValueError(name + ": v2 report accepted a refused v1 result")
    artifacts = {
        name + ".v1.jsonl": original,
        name + ".pfs2": data,
        name + ".v2.hex": ("\n".join(data[i:i + 32].hex(" ") for i in range(0, len(data), 32)) + "\n").encode(),
        name + ".records.jsonl": b"".join(map(v1_line, normalized(rows, head))),
        name + ".summary.json": compact(summary),
        name + ".report.json": expected_report,
        name + ".stage-a.json": compact(stage_receipt) + b"\n",
        name + ".receipt.json": compact(receipt) + b"\n",
        name + ".mask.json": compact({"status": "GENERATED", "producer": "pf_sink_transcode_v2",
                                      "profile": profile, "v2_sha256": hashlib.sha256(data).hexdigest(),
                                      "v2_size": len(data), "logical_sha256": receipt["sha256"]}) + b"\n",
    }
    return artifacts, {"name": name, "profile": profile, "bytes": len(data),
                       "rows": len(rows), "sha256": hashlib.sha256(data).hexdigest()}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transcoder", type=Path, required=True)
    parser.add_argument("--stage-a", type=Path, default=ROOT / "docker/pineforge_sink_finalize.py")
    parser.add_argument("--output-dir", type=Path, default=BUNDLE)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--force-bundle", action="store_true",
                        help="allow generation into the committed input bundle; --check never writes it")
    args = parser.parse_args(argv)
    try:
        args.output_dir = args.output_dir.resolve()
        if args.output_dir == BUNDLE.resolve() and not args.check and not args.force_bundle:
            raise ValueError("refusing to overwrite the committed vector input bundle; "
                             "use --force-bundle to regenerate it")
        args.transcoder, args.stage_a = args.transcoder.resolve(), args.stage_a.resolve()
        args.version = json.loads(checked([str(args.transcoder), "--version"]).stdout)
        artifacts, manifest = {}, []
        with tempfile.TemporaryDirectory(prefix="sink-v2-generator-") as temporary:
            for index, (name, head, rows, summary) in enumerate(inputs()):
                scratch = Path(temporary) / str(index)
                scratch.mkdir(mode=0o700)  # the transcoder refuses a group/world-writable spool dir (umask 0002 hosts)
                produced, evidence = generate(name, head, rows, summary, args, scratch)
                artifacts.update(produced)
                manifest.append(evidence)
                print(f"{name}: GENERATED AND VERIFIED WITH STAGE A + REAL TRANSCODER", flush=True)
        generation = {"schema": "sink-v2-stage-c-vectors/v1", "producer": "pf_sink_transcode_v2",
                      "version": args.version, "vector_count": len(manifest), "vectors": manifest,
                      "transcoder_sha256": hashlib.sha256(args.transcoder.read_bytes()).hexdigest(),
                      "stage_a_sha256": hashlib.sha256(args.stage_a.read_bytes()).hexdigest()}
        artifacts["GENERATION.json"] = compact(generation) + b"\n"
        artifacts["README.md"] = (  # test_report_sink_reader_vectors reads this file; only --force-bundle can put it over the committed one
            "# Generated stage-C vectors\n\n"
            "Generated only by tests/generate_sink_v2_vectors.py using stage A and pf_sink_transcode_v2.\n"
            "Every inverse and field was compared with its input. GENERATION.json binds the tool and count.\n\n"
            "| Vector | Profile | Bytes |\n|---|---|---:|\n" +
            "".join(f"| {v['name']} | {v['profile']} | {v['bytes']} |\n" for v in manifest)).encode()
        if not args.check:
            args.output_dir.mkdir(parents=True, exist_ok=True)
        for name, content in artifacts.items():
            path = args.output_dir / name
            if args.check:
                if not path.is_file() or path.read_bytes() != content:
                    raise ValueError(str(path) + ": missing/different generated artifact")
            else:
                path.write_bytes(content)
        print(f"stage-C vectors: count={len(manifest)} artifacts={len(artifacts)} "
              f"{'MATCH' if args.check else 'GENERATED'}")
    except (OSError, ValueError, pf_sink.SinkFormatError) as error:
        print(f"stage-C vector generation: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
