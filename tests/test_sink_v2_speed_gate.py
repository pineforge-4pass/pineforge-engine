#!/usr/bin/env python3
"""Synthetic statistics and a real-child fake driver for the sink speed gate.

This file is also the fake driver's SCRIPT (main(argv)) and fake codec CLI.
There are no generated programs, Python -c commands, or engine dependencies.
Its fake binary framing tests orchestration/accounting, not v2 conformance.
"""
from __future__ import annotations

import argparse
from collections import Counter
import contextlib
import copy
import hashlib
import importlib.util
import io
import itertools
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zlib


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/sink_v2_speed_gate.py"
SELF = Path(__file__).resolve()
SLEEP_SECONDS = 0.002
FINALIZE_SLEEP_SECONDS = 0.004


def import_file(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


gate = import_file(SCRIPT, "tested_sink_speed_gate")


class LateModeOptionTests(unittest.TestCase):
    def test_unset_resolves_by_mode_and_explicit_choices_remain_reachable(self):
        for mode, requested, expected in (
                ("tee", None, "structure"), ("serial", None, "full"),
                ("tee", "full", "full"), ("tee", "structure", "structure"),
                ("serial", "full", "full")):
            with self.subTest(mode=mode, requested=requested):
                args = argparse.Namespace(v2_mode=mode, v2_late_verify=requested)
                self.assertEqual(gate.resolved_late_verify(args), expected)
        self.assertEqual(gate.resolved_late_verify(argparse.Namespace()), "full")

    def test_late_mode_options_refused_before_build_inspection(self):
        for flags, message in (
                (["--v2-late-verify", "structure"], "--v2-late-verify structure requires --v2-mode tee"),
                (["--v2-compare-late-modes"], "--v2-compare-late-modes requires --v2-mode tee"),
                (["--v2-mode", "tee", "--v2-compare-late-modes", "--levels", "1"],
                 "--v2-compare-late-modes requires --size-grid-level 3 in --levels")):
            with self.subTest(flags=flags), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                diagnostic = io.StringIO()
                with patch.object(gate, "inspect_builds") as inspect, contextlib.redirect_stderr(diagnostic):
                    status = gate.main([
                        "--build-roots", *(str(root / f"absent-{n}") for n in range(4)),
                        "--ohlcv", str(root / "unused.csv"), "--output", str(root / "out"), *flags,
                    ])
                inspect.assert_not_called()
                self.assertEqual(status, 2)
                self.assertIn(message, diagnostic.getvalue())
                self.assertFalse((root / "out").exists())

    def test_late_mode_receipt_counts_expected_seed_and_grid_cells(self):
        # Inject aggregation verdicts only; these are not codec byte evidence.
        seed = {"late_mode_comparison": {"cell": "injected-seed", "equal": True}}
        cases = [{"N": 99999}, {"N": 100000}, {"N": 1000000}]
        grid = [{"case": case, "v2": {"exit_code": 0},
                 "late_mode_comparison": {"cell": f"injected-grid-{case['N']}", "equal": True}}
                for case in cases[1:]]
        with patch.object(gate, "size_grid_cases", return_value=cases):
            complete = gate.late_mode_receipt(
                [seed], grid, True, seed_cells_planned=1, skip_size_grid=False)
            self.assertIs(complete["equal"], True)
            self.assertEqual((complete["cells_compared"], complete["missing"]), (3, 0))
            self.assertEqual(complete["grid_cells_planned"], 2)
            missing = gate.late_mode_receipt(
                [seed], [grid[0], {}], True, seed_cells_planned=1, skip_size_grid=False)
            self.assertIs(missing["equal"], False)
            self.assertEqual((missing["cells_compared"], missing["missing"]), (2, 1))
            absent = gate.late_mode_receipt(
                [], [], True, seed_cells_planned=1, skip_size_grid=False)
            self.assertIs(absent["equal"], False)
            self.assertEqual(absent["missing"], 3)
            unproduced = gate.late_mode_receipt(
                [seed], grid[:1], True, seed_cells_planned=1, skip_size_grid=False)
            self.assertTrue(all(cell["equal"] is True for cell in unproduced["cells"]))
            self.assertIs(unproduced["equal"], False)
            self.assertEqual((unproduced["cells_compared"], unproduced["missing"]), (2, 1))
            self.assertEqual(unproduced["grid_cells_planned"], 2)
            skipped = gate.late_mode_receipt(
                [seed], [], True, seed_cells_planned=1, skip_size_grid=True)
            self.assertIs(skipped["equal"], True)
            self.assertEqual((skipped["cells_compared"], skipped["missing"]), (1, 0))
            self.assertEqual(skipped["grid_cells_planned"], 0)

    def test_late_mode_receipt_rejects_unequal_or_unchecked_cells(self):
        for verdict in (False, None):
            with self.subTest(verdict=verdict):
                row = {"late_mode_comparison": {"cell": "injected", "equal": verdict}}
                receipt = gate.late_mode_receipt(
                    [row], [], True, seed_cells_planned=1, skip_size_grid=True)
                self.assertIs(receipt["equal"], False)
                self.assertEqual(receipt["missing"], 0)
        self.assertIs(gate.late_mode_receipt(
            [], [], True, seed_cells_planned=0, skip_size_grid=True)["equal"], False)
        self.assertIsNone(gate.late_mode_receipt(
            [], [], False, seed_cells_planned=0, skip_size_grid=False)["equal"])

    def test_unknown_seed_build_is_recorded_as_a_failed_comparison(self):
        plan = synthetic_plan()
        seeds = {("unknown-build", plan.sizes[0], plan.strategies[0]): {"ok": True}}
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with patch.object(gate, "compare_late_modes") as compare:
                results = gate.compare_late_mode_seeds(
                    argparse.Namespace(size_grid_level=3), plan, seeds, [], output)
            compare.assert_not_called()
            self.assertEqual(len(results), 1)
            self.assertIs(results[0]["ok"], False)
            self.assertIn("unknown build", results[0]["error"])
            self.assertIn("unknown-build", results[0]["error"])
            self.assertEqual([json.loads(line) for line in
                              (output / "late-mode-seeds.jsonl").read_text().splitlines()], results)


def synthetic_plan():
    return gate.Plan(tuple(f"build-{n}" for n in range(4)), (100, 1000, 10000), ("probe",))


def injected_summary(decoded=256, stored=64, level=3, probe=None, c_q=None, c_m=0):
    """Injected evidence, never a claim about a real compressor or production JSON."""
    j_q = min(1024, decoded - 1) if c_m else decoded
    return {
        "summary_decoded_bytes": decoded, "summary_stored_bytes": stored,
        "summary_ratio": stored / decoded, "summary_expansion_bytes": stored - decoded,
        "summary_bytes": 52 + stored, "summary_zstd_frame": True, "summary_schema_validated": True,
        "summary_probe_stored_bytes": stored if probe is None else probe, "summary_probe_level": level,
        "summary_probe_zstd_build": {"version": "injected", "library": "test-only", "sha256": "0" * 64},
        "summary_probe_matches_producer": True, "level": level,
        "zstd_build": {"build": "injected independent encoder evidence", "log": "synthetic fixture"},
        "metadata_provenance": {"kind": "injected", "source_run": "synthetic independent reference"},
        "run_id": "injected-run", "J_Q": j_q, "C_Q": stored if c_q is None else c_q,
        "J_M": decoded - j_q + 1 if c_m else 2, "C_M": c_m,
        "summary_m_members": int(c_m > 0), "conservatively_counted_members": [],
    }


def injected_production_provenance():
    """Simulate the production evidence contract with explicitly injected test data."""
    return {
        "kind": "same-run production JSON", "mode": "ordinary",
        "run": {"fixture": "injected test truth"}, "metadata_path": "/injected/summary.json",
        "metadata_sha256": "a" * 64, "artifact_path": "/injected/sink.pfs2", "artifact_sha256": "b" * 64,
    }


def synthetic_rows(plan, injected_ratio=1.06):
    """Independent reference: multiplicative truth + symmetric build/run effects."""
    build_effect = (0.91, 0.98, 1.02, 1.09)
    run_effect = (0.99, 0.995, 1.0, 1.005, 1.01)
    rows = []
    for sequence, job in enumerate(gate.schedule(plan)):
        b = plan.builds.index(job.build)
        factor = injected_ratio if job.bars == max(plan.sizes) else 1.02
        wall = (job.bars / 10000) * build_effect[b] * run_effect[job.run % len(run_effect)]
        if job.arm.startswith("v2"):
            wall *= factor
        if job.warmup:
            wall *= 100  # Warm-ups must never enter the estimate.
        level = int(job.arm[4:]) if job.arm.startswith("v2-L") else 3
        trades = job.bars // 5
        type_bytes = {t: 0 for t in gate.TAGS}
        if job.arm == "v1":
            type_bytes.update(E=40 * job.bars, T=200 * trades)
        elif job.arm != "off":
            type_bytes.update(E=int((20 - level / 2) * job.bars), T=(100 - 2 * level) * trades)
        shared = 256 if job.arm != "off" else 0
        records = {tag: hashlib.sha256(f"{tag}:{job.strategy}:{job.bars}".encode()).hexdigest()
                   for tag in gate.TAGS}
        row = {
            "sequence": sequence, "kind": job.kind, "build": job.build,
            "bars": job.bars, "strategy": job.strategy, "arm": job.arm,
            "run": job.run, "warmup": job.warmup, "ok": True, "exit_code": 0,
            "wall_seconds": wall, "cpu_seconds": wall / 4, "level": level,
            "counts": {tag: job.bars if tag == "E" else trades if tag == "T" else 0 for tag in gate.TAGS},
            "record_sha256": records, "equity_sha256": records["E"],
            "legacy_trade_sha256": hashlib.sha256(f"trade:{job.strategy}:{job.bars}".encode()).hexdigest(),
            "metrics_sha256": hashlib.sha256(f"metrics:{job.strategy}:{job.bars}".encode()).hexdigest(),
            "merged_sha256": hashlib.sha256(f"merged:{job.strategy}:{job.bars}".encode()).hexdigest(),
            "type_bytes": type_bytes, "shared_bytes": shared,
            "sink_bytes": shared + sum(type_bytes.values()),
            "bytes_per_equity": type_bytes["E"] / job.bars if job.arm != "off" else None,
            "bytes_per_trade": type_bytes["T"] / trades if job.arm != "off" else None,
            "cpu_seconds_per_million_e": 0.05 + level / 100,
            "peak_rss_bytes": 64 * gate.MIB + (gate.MIB if job.arm.startswith("v2") else 0),
            "wall_scope": gate.WALL_SCOPE, "finalization_included": True,
            "script_bars": job.bars, "actual_bars": job.bars, "anchor_count": 0, "metadata_bytes": shared,
            "fraction": 0.5, "fraction_scope": "injected active fraction",
            "profile": ("COMPACT" if job.bars < 10980 and trades < 3314 else "FULL")
                       if job.arm.startswith("v2") else "V1" if job.arm == "v1" else "OFF",
            "descriptor_count": (job.bars + 4095) // 4096 + (trades + 1023) // 1024 + 1,
            "finalization_claim_bytes": gate.FINALIZATION_CLAIM,
            "extra_work_file_cap_bytes": gate.WORK_FILE_CAP,
            "peak_accounted_rss_bytes": 66 * gate.MIB + (gate.MIB if job.arm.startswith("v2") else 0),
            "peak_tmpfs_resident_bytes": 2 * gate.MIB,
            "peak_output_bytes": shared + sum(type_bytes.values()),
            "peak_scratch_bytes": 65536, "peak_work_file_bytes": 65536,
            "peak_temporary_bytes": gate.MIB, "memory_accounting_error": None,
            "rss_counter": "injected simultaneous process + resident files minus mapped duplicates",
            "rss_includes_resident_files": True,
            "file_backings": {k: {"backing": "tmpfs", "path": f"/synthetic/{k}"}
                              for k in ("scratch", "output", "temporary")},
        }
        if job.arm.startswith("v2"):
            row.update(injected_summary(level=level))
            row["metadata_provenance"] = injected_production_provenance()
        rows.append(row)
    return rows


def synthetic_memory(plan):
    artifacts, proofs = {}, []
    for build in plan.builds:
        library = hashlib.sha256(f"library:{build}".encode()).hexdigest()
        artifacts[build] = {"library_sha256": library, "strategies": {}}
        for strategy in plan.strategies:
            module = hashlib.sha256(f"module:{build}:{strategy}".encode()).hexdigest()
            artifacts[build]["strategies"][strategy] = {"module_sha256": module}
            for arm, axis in itertools.product(plan.arms[2:], ("bars", "trades")):
                proofs.append({
                    "build": build, "strategy": strategy, "arm": arm, "axis": axis,
                    "library_sha256": library, "module_sha256": module,
                    "populations": [1000, 100000, 1000000] if axis == "bars" else [10, 10000],
                    "live_bytes": [8 * gate.MIB, 8 * gate.MIB + 2048, 8 * gate.MIB + 4096]
                                  if axis == "bars" else [8 * gate.MIB, 8 * gate.MIB + 4096],
                    "file_backings": {k: "tmpfs" for k in ("scratch", "output", "temporary")},
                    "counter": "injected inclusive live measurement minus loaded OHLCV",
                    "includes_resident_files": True, "excludes_loaded_ohlcv": True,
                    "log": "synthetic independent live-allocation measurements",
                })
    return {"schema": "sink-v2-memory/v2", "rows": proofs}, artifacts


def summary_for(plan, rows, evidence=None, artifacts=None):
    if evidence is None:
        evidence, artifacts = synthetic_memory(plan)
    result = gate.summarize(plan, rows, evidence, artifacts, iterations=200, seed=9187)
    # Timing/memory tests isolate admitted files. The refusal path has its own
    # file-backed child test; it must never become an injected accepted file.
    cases = [c for c in gate.size_grid_cases() if c["shape"] != "above-ceiling-summary"]
    gate.update_size_verdict(result, rows, synthetic_size_grid(cases), expected_cases=cases)
    return result


def synthetic_size_grid(cases):
    """Independent admitted-file injections to isolate statistics/memory tests."""
    results = []
    for case in cases:
        n, t = case["N"], case["T"]
        e, trades, interleave = (n + 4095) // 4096, (t + 1023) // 1024, (n + t + 65535) // 65536
        descriptors = e + trades + interleave
        if case["shape"] == "block-boundary":
            descriptors = n + interleave
        decoded, stored, c_q, c_m = (256, 64, 64, 0)
        if case["shape"] in ("large-compressible-summary", "ceiling-compressible-summary"):
            decoded, stored, c_q, c_m = gate.MIB, gate.MIB // 4, 512, 261889
        if case["shape"] in ("incompressible-summary", "ceiling-raw-summary"):
            decoded, stored, c_q, c_m = gate.MIB, gate.MIB + 33, 512, gate.MIB - 1024 + 1 + 33
        base_bytes = 4 * gate.MIB + 512 * (n + t)
        current_bytes = 2 * gate.MIB + 64 * (n + t) + stored
        common = {
            "counts": {tag: n if tag == "E" else t if tag == "T" else 0 for tag in gate.TAGS},
            "actual_bars": case.get("actual_bars", n), "anchor_count": case.get("anchor_count", 0),
            "fraction": case["fraction"] if n else None, "fraction_scope": "injected active fraction",
            "descriptor_count": descriptors,
            "profile": "COMPACT" if n < 10980 and t < 3314 and descriptors <= 128 else "FULL",
            "wall_seconds": 1.0, "bytes_per_trade": None if not t else 100,
            "bytes_per_equity": None if not n else 8, "metadata_bytes": 256,
            "index_root_tail_bytes": 480, "fixed_floor_bytes": 480, "column_sha_total_bytes": 0,
            "peak_rss_bytes": 1024,
        }
        current = {**common, **injected_summary(decoded, stored, c_q=c_q, c_m=c_m), "sink_bytes": current_bytes}
        results.append({"case": case, "ok": True, "model": gate.case_model(case),
                        "v1": {**common, "sink_bytes": base_bytes}, "v2": current})
    return results


def raw_summary_frame(raw):
    """Legal decode-only frame with raw blocks; does not establish incompressibility."""
    frame = bytearray(b"\x28\xb5\x2f\xfd\xa0" + struct.pack("<I", len(raw)))
    for start in range(0, len(raw), 131072):
        block = raw[start:start + 131072]
        frame += ((len(block) << 3) | int(start + len(block) == len(raw))).to_bytes(3, "little") + block
    return bytes(frame)


class FakeSummaryZstd:
    """Test-only raw-frame decoder and injected compressibility; no system zstd dependency."""
    identity = {"version": "injected", "library": "fake raw-frame decoder", "sha256": "0" * 64}

    def decode(self, frame, decoded):
        gate.check_summary_frame(frame, decoded)
        pos, output = 9, bytearray()
        while True:
            header = int.from_bytes(frame[pos:pos + 3], "little")
            pos += 3
            if (header >> 1) & 3:
                raise gate.GateError("fake decoder accepts raw blocks only")
            size = header >> 3
            output += frame[pos:pos + size]
            pos += size
            if header & 1:
                break
        gate.require(pos == len(frame) and len(output) == decoded, "fake SUMMARY frame extent")
        return bytes(output)

    def compressed_size(self, raw, level):
        return max(1, len(raw) // 4)  # Injected compressible result, independent of the raw frame.


class SummaryCodecTestCase(unittest.TestCase):
    def setUp(self):
        codec = patch.object(gate, "summary_codec", return_value=FakeSummaryZstd())
        codec.start()
        self.addCleanup(codec.stop)


def injected_size_observation(n=100, trades=1, v1=5003, v2=4390, decoded=4096, stored=1024,
                              bars=None, anchors=0, probe=None, c_q=None, c_m=0):
    case = {"id": "injected-size", "N": n, "T": trades, "fraction": 0.25, "shape": "grid",
            "actual_bars": n if bars is None else bars, "anchor_count": anchors}
    result = synthetic_size_grid([case])[0]
    descriptors = (n + 4095) // 4096 + (trades + 1023) // 1024 + (n + trades + 65535) // 65536
    for arm in ("v1", "v2"):
        result[arm].update(
            counts={tag: n if tag == "E" else trades if tag == "T" else 0 for tag in gate.TAGS},
            actual_bars=n if bars is None else bars, anchor_count=anchors,
            descriptor_count=descriptors,
            profile="COMPACT" if n < 10980 and trades < 3314 and descriptors <= 128 else "FULL",
            fraction=0.25 if n else None)
    result["model"] = gate.case_model(result["case"])
    result["v1"]["sink_bytes"] = v1
    result["v2"].update(injected_summary(decoded, stored, probe=probe, c_q=c_q, c_m=c_m), sink_bytes=v2,
                        actual_bars=n if bars is None else bars, anchor_count=anchors)
    return result


def injected_production_rows():
    observation = injected_size_observation(stored=512)
    rows = []
    for sequence, (arm, source) in enumerate((("v1", observation["v1"]), ("v2-default", observation["v2"]))):
        row = copy.deepcopy(source)
        row.update(sequence=sequence, arm=arm, kind="backtest", build="injected-build",
                   bars=100, strategy="injected-production", run=0, warmup=False, ok=True)
        if arm.startswith("v2"):
            row["metadata_provenance"] = injected_production_provenance()
        rows.append(row)
    return rows


class SizeGuaranteeTests(SummaryCodecTestCase):
    def evaluate(self, observation):
        summary = {"problems": [], "speed_pass": True, "memory_pass": True}
        gate.update_size_verdict(summary, injected_production_rows(), [observation],
                                 expected_cases=[observation["case"]])
        return summary, summary["size_grid"][0]["measured"]

    def test_strict_trade_clause_and_summary_break_even(self):
        # Inject no metadata: counted C is C, so base3448/v16081 admits2632.
        for stored, verdict in ((2632, "PASS"), (2633, "FAIL"), (2634, "FAIL"), (262144, "FAIL")):
            with self.subTest(stored=stored):
                observation = injected_size_observation(n=128, v1=6081, v2=3448 + stored, stored=stored,
                                                        decoded=gate.MIB if stored == 262144 else 4096)
                summary, measured = self.evaluate(observation)
                self.assertEqual((measured["clause"], measured["verdict"]), ("(i)", verdict))
                self.assertEqual(measured["summary_max_comparable_C"], 2632)
                self.assertEqual(measured["summary_headroom_bytes"], 2632 - stored)
                self.assertEqual(summary["gate"], verdict)
        model = gate.size_model(128, 1, 0.25, stored=2633, c_q=2633, c_m=0)
        self.assertEqual(model["v2_bytes"], 6081)
        self.assertEqual(model["verdict_label"], "MODEL-ONLY FAIL")

    def test_one_3072_allowance_including_empty_and_strict_complement(self):
        for n, v1, allowance, clause in ((0, 950, 3072, "(ii-a)"),
                                         (100, 4802, 3072, "(ii-a)"),
                                         (999, 39414, -1, "(i)")):
            for extra, verdict in ((0, "PASS"), (1, "FAIL")):
                summary, measured = self.evaluate(
                    injected_size_observation(n, 0, v1, v1 + allowance + extra))
                self.assertEqual((measured["clause"], measured["verdict"]), (clause, verdict))
                self.assertEqual(measured["comparable_minus_v1_bytes"], allowance + extra)
                self.assertEqual(summary["gate"], verdict)
                if not n:
                    self.assertEqual(measured["empty_full_file_verdict"], verdict)
                    self.assertEqual(measured["empty_full_file_limit_bytes"], v1 + 3072)

    def test_n999_and_selected_anchor_use_raw_e_not_bars_for_clause(self):
        for n, bars, anchors in ((999, 999, 0), (1000, 999, 1)):
            for delta, verdict in ((-1, "PASS"), (0, "FAIL"), (1024, "FAIL")):
                summary, measured = self.evaluate(injected_size_observation(
                    n, 0, 39414, 39414 + delta, bars=bars, anchors=anchors))
                self.assertEqual((measured["clause"], measured["verdict"]), ("(i)", verdict))
                self.assertEqual(summary["gate"], verdict)
        summary, measured = self.evaluate(injected_size_observation(1000, 0, 39453, 39453))
        self.assertEqual((measured["clause"], measured["verdict"]), ("(i)", "FAIL"))
        self.assertEqual(summary["gate"], "FAIL")

    def test_small_allowances_use_comparable_bytes_and_empty_full_check_is_independent(self):
        for n, v1, allowance, clause in ((0, 950, 3072, "(ii-a)"), (100, 4802, 3072, "(ii-a)")):
            observation = injected_size_observation(
                n, 0, v1, v1 + allowance + 2000, decoded=8192, stored=3000, c_q=1000, c_m=9000)
            summary, measured = self.evaluate(observation)
            self.assertEqual(measured["C_meta"], 2000)
            self.assertEqual(measured["COMPARABLE_V2"], v1 + allowance)
            self.assertEqual((measured["clause"], measured["verdict"]), (clause, "PASS"))
            self.assertEqual(summary["gate"], "PASS")
            if not n:
                self.assertEqual(measured["empty_full_file_verdict"], "FAIL")
                self.assertEqual(measured["empty_full_file_limit_bytes"], v1 + 3072)

    def test_raw_e_allowance_ignores_bars_trades_and_optional_streams(self):
        for bars in (0, 999, 1000, 1000000):
            _, measured = self.evaluate(injected_size_observation(0, 0, bars=bars))
            self.assertEqual((measured["clause"], measured["verdict"]), ("(ii-a)", "PASS"))
        for tag in gate.TAGS:
            with self.subTest(tag=tag):
                observation = injected_size_observation(0, 0, bars=0)
                observation["v2"]["counts"][tag] = 1
                observation["v1"]["counts"][tag] = 1
                observation["v2"]["descriptor_count"] = 2
                observation["v2"]["fraction"] = 0.25
                observation["v2"]["anchor_count"] = int(tag == "E")
                _, measured = self.evaluate(observation)
                self.assertEqual((measured["clause"], measured["verdict"]), ("(ii-a)", "PASS"))
                self.assertIsNone(measured["empty_full_file_verdict"])
        observation = injected_size_observation(0, 0)
        del observation["v2"]["counts"]["H"]
        _, measured = self.evaluate(observation)
        self.assertEqual(measured["verdict"], "PENDING")

    def test_e63_64_e127_128_t3_4_report_one_clause_and_comparable_verdict(self):
        # Inject exact counted-byte boundaries, with 2000 caller bytes excluded.
        populations = ((1, 1, "(ii-a)"), (63, 3, "(ii-a)"), (63, 4, "(ii-a)"),
                       (64, 3, "(ii-a)"), (64, 4, "(i)"), (127, 3, "(ii-a)"),
                       (127, 4, "(i)"), (128, 3, "(i)"), (128, 4, "(i)"), (63, 10000, "(ii-a)"))
        for (n, trades, clause), selected, (extra, verdict) in itertools.product(
                populations, (False, True), ((0, "PASS"), (1, "FAIL"))):
            bars, anchors = (1000, 1) if selected else (n, 0)
            allowance = 3072 if clause == "(ii-a)" else -1
            with self.subTest(n=n, bars=bars, trades=trades, extra=extra):
                observation = injected_size_observation(
                    n, trades, 5000, 5000 + allowance + extra + 2000,
                    bars=bars, anchors=anchors, decoded=8192, stored=3000, c_q=1000, c_m=9000)
                summary, measured = self.evaluate(observation)
                self.assertEqual((measured["clause"], measured["verdict"]), (clause, verdict))
                self.assertEqual(measured["comparison_limit_bytes"], 5000 + allowance)
                self.assertEqual(measured["COMPARABLE_V2"], 5000 + allowance + extra)
                self.assertEqual(measured["C_meta"], 2000)
                self.assertIsNone(measured["empty_full_file_verdict"])
                self.assertEqual(summary["gate"], verdict)
                self.assertEqual([r["cell"] for r in summary["size_failures"]],
                                 ["injected-size"] if extra else [])
                summary.update(seed=1, bootstrap_iterations=200, bootstrap="injected",
                               large_bars=1000, sizes=[], level_table=[], selected_level=None)
                text = gate.render_summary(summary)
                line = next(line for line in text.splitlines() if line.startswith("SIZE injected-size "))
                self.assertIn(f"actual_bars={bars} raw_E={n} anchors={anchors} T={trades}", line)
                measured_text = line.split(" | ")[1]
                self.assertIn(f"limit_B={5000 + allowance} {clause} {verdict} ", measured_text)
                self.assertEqual(sum(measured_text.count(c) for c in ("(i)", "(ii-a)")), 1)
                if extra:
                    self.assertIn(f"SIZE COUNTEREXAMPLE injected-size FAIL{clause}", text)

    def test_selected_backtest_model_and_measured_clause_use_separate_bars_and_raw_e(self):
        for n, clause in ((63, "(ii-a)"), (64, "(ii-a)"), (127, "(ii-a)"), (128, "(i)")):
            rows = injected_production_rows()
            for row in rows:
                row["actual_bars"], row["anchor_count"] = 1000, 1
                row["counts"].update(E=n, T=0)
                row["sink_bytes"] = 5000
            summary = {"problems": [], "speed_pass": True, "memory_pass": True}
            gate.update_size_verdict(summary, rows, expected_cases=[])
            cell = summary["size_comparisons"][0]
            self.assertEqual((cell["clause"], cell["verdict"]),
                             (clause, "PASS" if n < 128 else "FAIL"))
            self.assertEqual(cell["model"]["clause"], clause)
            self.assertEqual((cell["model"]["actual_bars"], cell["model"]["raw_E"],
                              cell["model"]["anchor_count"]), (1000, n, 1))

    def test_caller_metadata_deduction_is_bounded_and_format_bytes_remain(self):
        # Inject a large M whose standalone compression exceeds C-C_Q.
        stored, c_q, c_m, format_bytes = 500000, 1000, 600000, 4000
        for extra, verdict in ((0, "PASS"), (1, "FAIL")):
            summary, measured = self.evaluate(injected_size_observation(
                n=128, v1=format_bytes + c_q + 1, v2=format_bytes + stored + extra,
                decoded=gate.MIB, stored=stored, c_q=c_q, c_m=c_m))
            self.assertEqual(measured["C_meta"], stored - c_q)  # Not c_m, C or C+52.
            self.assertEqual(measured["C_comparable"], c_q)
            self.assertEqual(measured["COMPARABLE_V2"], format_bytes + c_q + extra)
            self.assertEqual(measured["COMPARABLE_V2"] - measured["C_comparable"], format_bytes + extra)
            self.assertEqual(measured["metadata_exclusion"], "EXCLUDED_CALLER_METADATA")
            self.assertEqual((measured["clause"], measured["verdict"]), ("(i)", verdict))
            self.assertEqual(summary["gate"], verdict)
            self.assertEqual(len(summary["size_expansions"]), 1)
        # The independent M bound can be the tighter one; C<C_Q gives no deduction.
        for stored, c_q, c_m, exclusion in ((1000, 200, 300, 300), (1000, 1200, 9000, 0)):
            _, measured = self.evaluate(injected_size_observation(stored=stored, c_q=c_q, c_m=c_m))
            self.assertEqual(measured["C_meta"], exclusion)
            self.assertEqual(measured["COMPARABLE_V2"], 4390 - exclusion)
        # A large counted Q still fails even when some caller metadata is excluded.
        _, measured = self.evaluate(injected_size_observation(
            v2=20000, stored=16000, decoded=gate.MIB, c_q=14000, c_m=10000))
        self.assertEqual(measured["verdict"], "FAIL")

    def test_incompressibility_is_coverage_not_permission_to_exempt_format(self):
        values = dict(v2=1051975, decoded=gate.MIB, stored=gate.MIB + 33,
                      c_q=512, c_m=gate.MIB - 1024 + 1 + 33)
        summary, measured = self.evaluate(injected_size_observation(**values))
        self.assertEqual((measured["clause"], measured["verdict"]), ("(ii-a)", "PASS"))
        self.assertEqual(measured["C_meta"], values["c_m"])
        self.assertEqual(measured["COMPARABLE_V2"], values["v2"] - values["c_m"])
        self.assertEqual(measured["diagnostic_verdict"], "FAIL")
        self.assertEqual(measured["summary_expansion_bytes"], 33)
        self.assertEqual(summary["gate"], "PASS")
        compressible = injected_size_observation(**values, probe=262144)
        compressible["v2"]["requires_incompressible_evidence"] = True
        summary, measured = self.evaluate(compressible)
        self.assertEqual(measured["verdict"], "PASS")  # Attribution also applies when M compresses.
        self.assertTrue(measured["coverage_gaps"])
        self.assertEqual(summary["gate"], "FAIL")  # Required incompressibility is still unproved.

    def test_missing_measurements_provenance_or_probe_are_pending(self):
        for key in ("summary_decoded_bytes", "summary_stored_bytes", "summary_ratio", "metadata_provenance",
                    "zstd_build", "level", "actual_bars", "anchor_count", "summary_probe_stored_bytes",
                    "summary_probe_matches_producer", "summary_schema_validated", "sink_bytes",
                    "J_Q", "C_Q", "J_M", "C_M", "summary_m_members"):
            with self.subTest(key=key):
                observation = injected_size_observation()
                del observation["v2"][key]
                summary, measured = self.evaluate(observation)
                self.assertEqual(measured["verdict"], "PENDING")
                self.assertIsNone(measured["C_meta"])
                self.assertIsNone(measured["COMPARABLE_V2"])
                self.assertEqual(summary["gate"], "FAIL")
        summary = {"problems": []}
        gate.update_size_verdict(summary, [])
        self.assertEqual(summary["measured_size_verdict"], "PENDING")
        self.assertEqual(summary["gate"], "FAIL")
        grid = [r for r in summary["size_grid"] if r["case"]["shape"] == "grid"]
        self.assertEqual(len(grid), 63)
        self.assertTrue(all(r["measured"]["verdict"] == "PENDING" for r in grid))
        self.assertTrue(all(r["model"]["verdict"] == "PASS" for r in grid))
        selected = [r for r in summary["size_grid"] if r["case"]["shape"] == "selected_report_view"]
        self.assertEqual(len(selected), 147)
        self.assertTrue(all(r["measured"]["verdict"] == "PENDING" for r in selected))
        self.assertTrue(all(r["model"]["verdict_label"] == "MODEL-ONLY PASS" for r in selected))

    def test_round10_extreme_models_and_counted_frame_limits_are_unmeasured(self):
        expected = [
            (950, 2503, 1991), (4802, 3246, 2734), (5452, 3521, 3009), (6752, 4071, 3559),
            (5003, 4390, 3878), (5653, 4665, 4153), (6953, 5215, 4703),
            (39414, 5899, 5387), (45907, 8646, 8134), (58894, 14140, 13628),
            (39453, 5901, 5389), (45953, 8651, 8139), (58953, 14151, 13639),
            (5003, 265510, 3878), (5653, 265785, 4153), (6953, 266335, 4703),
            (5003, 1051975, 4389), (5653, 1052250, 4664), (6953, 1052800, 5214),
            (5003, 265510, 3878), (5653, 265785, 4153), (6953, 266335, 4703),
            (5003, 1051975, 4389), (5653, 1052250, 4664), (6953, 1052800, 5214),
            (5003, 265511, 3878), (5653, 265786, 4153), (6953, 266336, 4703),
        ]
        models = [gate.case_model(case) for case in gate.size_extreme_cases()
                  if case["shape"] not in ("ordinary-corner", "selected_report_view")]
        self.assertEqual([(m["v1_bytes"], m["v2_bytes"], m["COMPARABLE_V2"]) for m in models], expected)
        self.assertEqual([m["verdict_label"] for m in models],
                         ["MODEL-ONLY PASS"] * 25 + ["MODEL-ONLY REFUSED(summary_limit)"] * 3)
        self.assertEqual([m["C_meta"] for m in models],
                         [512] * 13 + [261632] * 3 + [1047586] * 3
                         + [261632] * 3 + [1047586] * 3 + [261633] * 3)
        for model in models[-9:]:
            self.assertEqual(model["summary_region_formula_bytes"], model["summary_region_term_bytes"])
            self.assertEqual(model["v2_base_formula_bytes"], model["v2_base_term_bytes"])
            self.assertEqual(model["summary_max_comparable_C"], model["summary_max_comparable_C_terms"])
            self.assertEqual(model["metadata_exclusion"], "EXCLUDED_CALLER_METADATA")
        for model in models[-6:-3]:
            self.assertEqual(model["summary_region_formula_bytes"], 1048661)
            self.assertEqual(model["summary_stored_excess_bytes"], 0)
            self.assertEqual(model["summary_decoded_excess_bytes"], 0)
            self.assertEqual(model["verdict"], "PASS")
        for model in models[-3:]:
            self.assertEqual(model["summary_region_formula_bytes"], 262197)
            self.assertLess(model["summary_stored_excess_bytes"], 0)
            self.assertEqual(model["summary_decoded_excess_bytes"], 1)
            self.assertEqual(model["counterfactual_size_verdict"], "PASS")
        self.assertEqual(models[0]["empty_full_file_verdict"], "PASS")
        for n, t, maximum in ((1, 1, 1188), (64, 4, 793),
                               (100, 1, 4709), (128, 1, 2632), (1000, 0, 34575),
                               (0, 0, 2543), (100, 0, 5652)):
            model = gate.size_model(n, t, 0.25)
            self.assertEqual(model["summary_max_comparable_C"], maximum)
            self.assertEqual(model["summary_max_comparable_ratio"], maximum / 4096)
            self.assertIn("UNMEASURED", model["metadata_scope"])

    def test_both_corner_grids_have_294_cells_and_one_round8_clause_each(self):
        cases = [c for c in gate.size_grid_cases() if c["shape"] in ("ordinary-corner", "selected_report_view")]
        populations = {(n, t) for n in (1, 2, 10, 40, 63, 64, 68, 69, 100, 127, 128) for t in range(4)}
        populations |= {(n, 4) for n in (1, 63, 64, 127, 128)}
        self.assertEqual({(c["shape"], c["N"], c["T"], c["fraction"]) for c in cases},
                         {(shape, n, t, f) for shape in ("ordinary-corner", "selected_report_view")
                          for n, t in populations for f in (0.25, 0.5, 1.0)})
        self.assertEqual(len(cases), 294)
        strict = {(128, t) for t in range(5)} | {(64, 4), (127, 4)}
        for case in cases:
            model = gate.case_model(case)
            selected = case["shape"] == "selected_report_view"
            self.assertEqual((model["actual_bars"], model["raw_E"], model["anchor_count"]),
                             (1000 if selected else case["N"], case["N"], int(selected)))
            self.assertEqual((model["clause"], model["verdict_label"]),
                             ("(i)" if (case["N"], case["T"]) in strict else "(ii-a)", "MODEL-ONLY PASS"))
            self.assertIsNone(model["empty_full_file_verdict"])
            limit = model["v1_bytes"] + (-1 if (case["N"], case["T"]) in strict else 3072)
            self.assertEqual(model["comparison_limit_bytes"], limit)
        # E40/.25 is an expansion that passes its allowance, never strict (i).
        expansion = gate.size_model(40, 0, 0.25, actual_bars=1000, anchor_count=1)
        self.assertEqual(expansion["comparable_minus_v1_bytes"], 66)
        self.assertEqual((expansion["clause"], expansion["verdict_label"]), ("(ii-a)", "MODEL-ONLY PASS"))
        summary = gate.summarize(synthetic_plan(), [])
        references = summary["size_planning_references"]
        self.assertEqual(len(references), 357)
        self.assertEqual({(r["raw_E"], r["T"], r["fraction"]) for r in references if r["anchor_count"] == 1},
                         {(n, t, f) for n, t in populations for f in (0.25, 0.5, 1.0)})
        self.assertEqual(len(gate.size_grid_cases()), 393)

    def test_k_cannot_be_changed_by_summary_or_row_input(self):
        for counted, verdict in ((512, "PASS"), (1188, "PASS"), (1189, "FAIL"), (2500, "FAIL")):
            # E1/T1's independent B0=3074 and v1=1190 cross the fixed allowance.
            observation = injected_size_observation(1, 1, 1190, 3074 + counted, stored=counted)
            observation["v2"].update(small_run_K=65536, allowance=65536, K=65536)
            observation["case"]["K"] = 65536
            summary, measured = self.evaluate(observation)
            self.assertEqual((measured["COMPARABLE_V2"], measured["comparison_limit_bytes"]),
                             (3074 + counted, 4262))
            self.assertEqual((measured["small_run_K"], measured["verdict"]), (3072, verdict))
            self.assertEqual(summary["gate"], verdict)
            self.assertEqual(summary["small_run_allowance"], {"K": 3072, "source": "FIXED spec 0.5"})
            model = gate.size_model(1, 1, 0.25, stored=counted, c_q=counted, c_m=0)
            self.assertEqual((model["comparison_limit_bytes"], model["verdict"]), (4262, verdict))
        with self.assertRaises(TypeError):
            gate.compare_size(1190, 5000, {t: int(t in "ET") for t in gate.TAGS},
                              1, 4096, 1926, 1926, 0, allowance=65536)
        with self.assertRaises(TypeError):
            gate.size_model(1, 1, 0.25, _allowance=65536)

    def test_file_backed_summary_crosses_both_binding_limits_and_report_is_real_first(self):
        # Real SUM2/raw-frame bytes, test codec, and injected unrelated whole-file
        # pairing. These are synthetic observations, not production lab evidence.
        expected = ((793, "PASS", "PASS"), (794, "PASS", "FAIL"),
                    (1188, "PASS", "FAIL"), (1189, "FAIL", "FAIL"))
        with tempfile.TemporaryDirectory() as temporary:
            sink = Path(temporary) / "binding.pfs2"
            for counted, e1_verdict, e64_verdict in expected:
                with self.subTest(counted=counted):
                    metadata = {k: v for k, v in fake_metadata(100, 1).items()
                                if k in ("engine", "summary", "metrics", "diagnostics", "sink_folds")}
                    metadata["diagnostics"]["counted_padding"] = ""
                    raw = json.dumps(metadata, separators=(",", ":")).encode()
                    metadata["diagnostics"]["counted_padding"] = "x" * (counted - 12 - len(raw))
                    fake_v2(sink, fake_records(100), 3, metadata=metadata)
                    info = gate.v2_accounting(sink)
                    rows = injected_production_rows()
                    rows[0]["sink_bytes"] = 1000000  # Keep the unrelated paired-file rule satisfied.
                    rows[1].update(info, summary_schema_validated=True, summary_probe_matches_producer=True,
                                   zstd_build={"build": "injected fake writer", "log": "test fixture"},
                                   metadata_provenance=injected_production_provenance())
                    observation = injected_size_observation()
                    summary = {"problems": [], "speed_pass": True, "memory_pass": True,
                               "seed": 1, "bootstrap_iterations": 200, "bootstrap": "injected",
                               "large_bars": 1000, "sizes": [], "level_table": [], "selected_level": None}
                    gate.update_size_verdict(summary, rows, [observation], expected_cases=[observation["case"]])
                    real = summary["real_summary"]
                    self.assertEqual((real["J"], real["C"], real["C_meta"], real["C_comparable"]),
                                     (counted - 12, counted, 0, counted))
                    binding = summary["binding_summary"]
                    self.assertEqual(binding["fixed_K"], 3072)
                    self.assertTrue(all(r["small_run_K"] == 3072 for r in binding["cells"]))
                    self.assertEqual([(r["cell"], r["fraction"], r["L"], r["L_terms"]) for r in binding["cells"]],
                                     [("E1/T1", 0.25, 1188, 1188), ("E64/T4", 0.25, 793, 793)])
                    self.assertEqual([r["verdict"] for r in binding["cells"]],
                                     ["MODEL-ONLY " + e1_verdict, "MODEL-ONLY " + e64_verdict])
                    self.assertEqual(binding["status"], e64_verdict)
                    self.assertEqual(summary["size_acceptance"], e64_verdict)
                    self.assertEqual(summary["gate"], e64_verdict)
                    self.assertEqual(summary["measured_size_verdict"], "PASS")
                    self.assertEqual(summary["model_size_verdict"], "MODEL-ONLY PASS")
                    self.assertEqual(summary["size_failures"], [])
                    self.assertEqual([(r["E"], r["fraction"], r["L"], r["L_terms"])
                                      for r in binding["other_fractions"]],
                                     [(1, 0.5, 1192, 1192), (64, 0.5, 1033, 1033),
                                      (1, 1.0, 1199, 1199), (64, 1.0, 1513, 1513)])
                    for cell in binding["cells"] + binding["other_fractions"]:
                        self.assertEqual(cell["B0"], cell["B0_terms"])
                        self.assertEqual(cell["COMPARABLE_V2"], cell["B0"] + counted)
                        self.assertEqual(cell["comparison_limit_bytes"], cell["B0"] + cell["L"])
                        self.assertEqual(cell["headroom_bytes"], cell["L"] - counted)
                    text = gate.render_summary(summary)
                    lines = text.splitlines()
                    self.assertTrue(lines[0].startswith("REAL-PRODUCTION-SUMMARY"))
                    self.assertIn(f"J={counted - 12} C={counted}", lines[0])
                    self.assertTrue(lines[3].startswith("BINDING E1/T1 f=0.25 (ii-a) MODEL-ONLY"))
                    self.assertTrue(lines[4].startswith("BINDING E64/T4 f=0.25 (i) MODEL-ONLY"))
                    self.assertLess(text.index("BINDING E64/T4"), text.index("BREAK-EVEN "))
                    self.assertLess(text.index("BINDING E64/T4"), text.index("Report sink v2 speed gate"))
                    self.assertEqual(list(json.loads(json.dumps(summary)))[:2], ["real_summary", "binding_summary"])

    def test_v1_every_case_uses_both_routes_and_disagreement_is_fatal(self):
        cases = gate.size_grid_cases()
        with patch.object(gate, "v1_model_by_terms", wraps=gate.v1_model_by_terms) as terms:
            models = [gate.case_model(case) for case in cases]
        self.assertEqual([call.args for call in terms.call_args_list],
                         [(c["N"], c["T"], c["fraction"]) for c in cases])
        for model in models:
            self.assertEqual(model["v1_formula_bytes"], model["v1_term_bytes"])
            self.assertEqual(model["v1_bytes"], model["v1_term_bytes"])
        # Independently published corrected totals; the harness has no table of them.
        corrected = [gate.size_model(5000, 250, f) for f in (0.25, 0.5, 1.0)]
        self.assertEqual([m["v1_bytes"] for m in corrected], [243705, 276205, 341205])
        independent_count = gate.v1_model_by_terms

        def corrupted_count(n, t, f):
            return independent_count(n, t, f) + 1

        with patch.object(gate, "v1_model_by_terms", side_effect=corrupted_count):
            for case in cases:
                with self.subTest(case=case["id"]):
                    with self.assertRaisesRegex(gate.GateError, "v1 arithmetic disagreement"):
                        gate.case_model(case)

    def test_binding_break_even_arithmetic_disagreement_is_fatal(self):
        independent_compare = gate.compare_size
        rows = injected_production_rows()

        def corrupted_maximum(*args, **kwargs):
            result = independent_compare(*args, **kwargs)
            result["summary_max_comparable_C"] += 1
            return result

        with patch.object(gate, "compare_size", side_effect=corrupted_maximum):
            with self.assertRaisesRegex(gate.GateError, "break-even arithmetic disagreement"):
                gate.update_size_verdict({"problems": []}, rows, expected_cases=[])

    def test_summary_projection_preserves_tokens_order_and_outputs_wrapper(self):
        raw = (
            br'{"input":{"path":"\u4e2d\/feed"},"engine":"e","summary":{"n":1e+03,"z":-0.0},'
            br'"outputs":{"manifest":{"x":"\u00e9"},"constants":{"a":2.00},"future":7,"hlines":[],"schema_version":1},'
            br'"applied_runtime":{"future":{"a":true}},"diagnostics":{"caller_flag":"M"},"extra":{"x":"\u0041"},'
            br'"fingerprint":null,"metrics":{"all":{}},"sink_folds":{"closed_count":0}}'
        )
        expected_q = (
            br'{"engine":"e","summary":{"n":1e+03,"z":-0.0},'
            br'"outputs":{"constants":{"a":2.00},"future":7,"hlines":[]},"diagnostics":{"caller_flag":"M"},'
            br'"extra":{"x":"\u0041"},"metrics":{"all":{}},"sink_folds":{"closed_count":0}}'
        )
        expected_m = (
            br'{"input":{"path":"\u4e2d\/feed"},"outputs":{"manifest":{"x":"\u00e9"},"schema_version":1},'
            br'"applied_runtime":{"future":{"a":true}},"fingerprint":null}'
        )
        q, m, members, unknown = gate.summary_projections(raw)
        self.assertEqual((q, m, members, unknown), (expected_q, expected_m, 4, ["outputs.future", "extra"]))
        escaped = br'{"\u0069nput":null,"outputs":{"schema_version":1}}'
        self.assertEqual(gate.summary_projections(escaped)[:3], (b"{}", escaped, 2))
        counted = br'{"engine":"e","outputs":{},"unknown":[1,2]}'
        self.assertEqual(gate.summary_projections(counted),
                         (br'{"engine":"e","unknown":[1,2]}', b"{}", 0, ["unknown"]))
        # Every named M member follows its parent, including additive nested data.
        fields = ("input", "applied_inputs", "applied_overrides", "applied_runtime", "elapsed_seconds",
                  "fingerprint", "report_window", "report_shape")
        raw = json.dumps({key: {"additive": "kept"} for key in fields}, separators=(",", ":")).encode()
        self.assertEqual(gate.summary_projections(raw), (b"{}", raw, len(fields), []))

    def test_real_summary_or_pending_is_first_in_text_and_json_and_models_are_labelled(self):
        summary = {
            "seed": 1, "bootstrap_iterations": 200, "bootstrap": "injected",
            "large_bars": 10000, "sizes": [], "level_table": [], "selected_level": None,
            "problems": [], "speed_pass": True, "memory_pass": True,
        }
        gate.update_size_verdict(summary, [])
        text = gate.render_summary(summary)
        self.assertTrue(text.startswith("PENDING-REAL-SUMMARY"))
        self.assertEqual(next(iter(summary)), "real_summary")
        self.assertEqual(list(summary)[:2], ["real_summary", "binding_summary"])
        self.assertEqual(summary["binding_summary"]["status"], "PENDING-REAL-SUMMARY")
        self.assertTrue(all(cell["verdict"] == "PENDING-REAL-SUMMARY"
                            for cell in summary["binding_summary"]["cells"]))
        pending_lines = text.splitlines()
        self.assertTrue(pending_lines[2].startswith("BINDING E1/T1 f=0.25 (ii-a) MODEL-ONLY"))
        self.assertTrue(pending_lines[3].startswith("BINDING E64/T4 f=0.25 (i) MODEL-ONLY"))
        self.assertEqual(summary["measured_size_verdict"], "PENDING")
        self.assertEqual(summary["model_size_verdict"], "MODEL-ONLY PASS")
        for result in summary["size_grid"]:
            self.assertEqual(result["model"]["source"], "MODEL-ONLY")
            self.assertEqual(result["model"]["verdict_label"],
                             "MODEL-ONLY REFUSED(summary_limit)" if result["case"]["shape"] == "above-ceiling-summary"
                             else "MODEL-ONLY PASS")
            self.assertEqual(result["model"]["real_summary_break_even"]["status"], "PENDING-REAL-SUMMARY")
        self.assertIn("MODEL-ONLY EMPTY complete-file check", text)
        self.assertIn("MODEL-ONLY break-even", text)

        # Different injected J/C/probes ensure the real-first ratios cannot use J4096/r.25.
        rows = injected_production_rows()
        rows[1].update(injected_summary(decoded=8192, stored=3000, c_q=1000, c_m=9000))
        rows[1]["metadata_provenance"] = injected_production_provenance()
        observation = injected_size_observation()
        gate.update_size_verdict(summary, rows, [observation], expected_cases=[observation["case"]])
        real = summary["real_summary"]
        self.assertEqual((real["J"], real["C"], real["C_meta"], real["C_comparable"]), (8192, 3000, 2000, 1000))
        self.assertEqual(real["C_comparable/J"], 1000 / 8192)
        self.assertEqual([cell["verdict"] for cell in summary["binding_summary"]["cells"]],
                         ["MODEL-ONLY PASS", "MODEL-ONLY FAIL"])
        self.assertEqual(summary["size_acceptance"], "FAIL")
        self.assertEqual(summary["small_run_allowance"]["K"], 3072)
        evaluated = summary["size_grid"][0]["model"]["real_summary_break_even"]
        self.assertEqual(evaluated["limit_ratio"], 4709 / 8192)
        self.assertEqual(evaluated["verdict"], "MODEL-ONLY PASS")
        self.assertTrue(gate.render_summary(summary).startswith("REAL-PRODUCTION-SUMMARY"))
        self.assertEqual(next(iter(json.loads(json.dumps(summary)))), "real_summary")

        for kind, provenance in (
                ("encode", injected_production_provenance()),
                ("backtest", {"kind": "synthetic population using captured production metadata"})):
            rows[1].update(kind=kind, metadata_provenance=provenance)
            self.assertEqual(gate.production_summary(rows)["status"], "PENDING-REAL-SUMMARY")

    def test_producer_binding_and_source_json_cannot_be_substituted(self):
        metadata = fake_metadata(100, 1)
        info = {**injected_summary(), "sink_folds": metadata["sink_folds"],
                "summary_metadata_sha256": gate.object_digest(metadata)}
        artifact = "a" * 64
        entry = {"artifact_sha256": artifact, "build": "injected producer", "log": "retained test evidence",
                 "zstd_library_sha256": "0" * 64}
        args = argparse.Namespace(zstd_builds={"reference": entry})
        gate.bind_summary_evidence(info, args, "reference", artifact, {"source_run": "injected"}, metadata)
        self.assertTrue(info["summary_probe_matches_producer"])
        with self.assertRaisesRegex(gate.GateError, "artifact-mismatched"):
            gate.bind_summary_evidence(info, args, "reference", "b" * 64, {}, metadata)
        other_run = copy.deepcopy(metadata)
        other_run["metrics"]["all"]["net_profit"] += 1
        with self.assertRaisesRegex(gate.GateError, "source run JSON"):
            gate.bind_summary_evidence(info, args, "reference", artifact, {}, other_run)
        args.zstd_builds["reference"] = {**entry, "zstd_library_sha256": "1" * 64}
        gate.bind_summary_evidence(info, args, "reference", artifact, {"source_run": "injected"}, metadata)
        self.assertFalse(info["summary_probe_matches_producer"])

    def test_typed_writer_refusal_is_listed_and_never_counted_as_a_size_pass_or_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stdout, stderr, sink = root / "stdout", root / "stderr", root / "invalid.pfs2"
            stdout.write_text('{"error":{"kind":"summary_limit"},"complete":0}', encoding="utf-8")
            stderr.write_text("WriterError{summary_limit}: stored SUMMARY exceeds its bound", encoding="utf-8")
            sink.write_bytes(b"\x89PFS2\r\n\x1aINVALID")
            observation = injected_size_observation()
            observation["ok"] = False
            observation["v2"]["exit_code"] = 7
            self.assertTrue(gate.capture_writer_refusal(observation["v2"], stdout, stderr, sink))
            observation["writer_refusal"] = observation["v2"]["writer_refusal"]
            summary, measured = self.evaluate(observation)
            self.assertEqual((measured["clause"], measured["verdict"]), ("(ii-a)", "REFUSED"))
            self.assertEqual(measured["refusal_kind"], "summary_limit")
            self.assertIsNone(measured["v2_bytes"])
            self.assertIsNone(measured["COMPARABLE_V2"])
            self.assertEqual([r["cell"] for r in summary["size_refusals"]], ["injected-size"])
            self.assertEqual(summary["size_failures"], [])
            self.assertEqual(summary["size_expansions"], [])
            self.assertEqual(summary["size_pending"], [])
            self.assertEqual(summary["size_files_compared"], 1)  # Only the production pair.
            self.assertEqual((summary["measured_size_verdict"], summary["size_acceptance"]), ("REFUSED", "REFUSED"))
            self.assertEqual(summary["gate"], "FAIL")
            summary.update(seed=1, bootstrap_iterations=200, bootstrap="injected", large_bars=1000,
                           sizes=[], level_table=[], selected_level=None)
            text = gate.render_summary(summary)
            self.assertTrue(text.startswith("REAL-PRODUCTION-SUMMARY"))
            self.assertIn("SIZE REFUSED injected-size summary_limit (ii-a)", text)
            self.assertNotIn("SIZE COUNTEREXAMPLE injected-size", text)
            self.assertNotIn("SIZE EXPANSION injected-size", text)
            self.assertTrue(sink.is_file())
            # The actual typed result, not a matching word or model forecast,
            # determines admission state. Successful exit cannot be a refusal.
            for status, diagnostic in ((0, "WriterError{summary_limit}"), (7, "unrelated summary_limit text"),
                                       (1, '{"error":"sink failed","code":"report_sink_failed","args":{}}'),
                                       (1, '{"error":"summary_limit","code":"other_failure","args":{}}')):
                stdout.write_text("", encoding="utf-8")
                stderr.write_text(diagnostic, encoding="utf-8")
                self.assertFalse(gate.capture_writer_refusal({"exit_code": status}, stdout, stderr, sink))
            # Finalizer -5 uses the existing run_json failure code, args and
            # exit1. The diagnostic reason distinguishes it from other sink faults.
            stdout.write_text('{"engine":"pineforge","error":"finalizer: summary_limit (-5)",'
                              '"code":"report_sink_failed","args":{}}', encoding="utf-8")
            stderr.write_text("", encoding="utf-8")
            mapped = {"exit_code": 1}
            self.assertTrue(gate.capture_writer_refusal(mapped, stdout, stderr, sink))
            self.assertEqual(mapped["writer_refusal"]["run_failure_code"], "report_sink_failed")
            mapped_observation = injected_size_observation()
            mapped_observation.update(ok=False)
            mapped_observation["v2"].update(mapped)
            mapped_summary, mapped_cell = self.evaluate(mapped_observation)
            self.assertEqual((mapped_cell["verdict"], mapped_summary["size_acceptance"]), ("REFUSED", "REFUSED"))
            stdout.write_text("", encoding="utf-8")
            stage_c = {"complete": False, "format": 2, "transcoder_version": 2,
                       "status": 4, "code": "report_sink_failed",
                       "reason": "v2_input_mismatch", "diagnostic": "summary_limit"}
            stderr.write_text("encoder diagnostic\n" + json.dumps(stage_c) + "\n", encoding="utf-8")
            self.assertTrue(gate.capture_writer_refusal({"exit_code": 4}, stdout, stderr, sink))
            stage_c["status"] = 5
            stderr.write_text(json.dumps(stage_c), encoding="utf-8")
            self.assertFalse(gate.capture_writer_refusal({"exit_code": 5}, stdout, stderr, sink))
            stdout.write_text("", encoding="utf-8")
            stderr.write_text("WriterError{summary_limit}", encoding="utf-8")
            sink.write_bytes(b"INVALID" + b"PFS2END\n")
            with self.assertRaisesRegex(gate.GateError, "success tail"):
                gate.capture_writer_refusal({"exit_code": 7}, stdout, stderr, sink)


class StatisticsTests(unittest.TestCase):
    def test_recovers_injected_truth_and_ci_contains_it(self):
        plan = synthetic_plan()
        truth = 1.06
        summary = summary_for(plan, synthetic_rows(plan, truth))
        large = next(row for row in summary["sizes"] if row["bars"] == max(plan.sizes))
        self.assertAlmostEqual(large["ratio"], truth, places=12)
        self.assertLessEqual(large["ci95"][0], truth)
        self.assertGreaterEqual(large["ci95"][1], truth)
        self.assertEqual(summary["gate"], "PASS")

    def test_twelve_percent_regression_fails_and_six_percent_passes(self):
        plan = synthetic_plan()
        for injected, expected in ((1.12, "FAIL"), (1.06, "PASS")):
            with self.subTest(injected=injected):
                summary = summary_for(plan, synthetic_rows(plan, injected))
                self.assertEqual(summary["gate"], expected)
                self.assertEqual(summary["speed_pass"], injected <= 1.10)
                self.assertIn(f"GATE {expected}", gate.render_summary(summary))

    def test_gate_boundary_and_invalid_numbers(self):
        self.assertTrue(gate.timing_passes(1.10))
        self.assertFalse(gate.timing_passes(1.100001))
        for value in (None, 0, -1, float("nan"), float("inf"), True):
            self.assertFalse(gate.timing_passes(value))

    def test_builds_have_equal_weight_and_control_is_between_builds(self):
        groups = {"a": [1.0] * 1000, "b": [10.0], "c": [20.0], "d": [30.0]}
        # Four build medians, middle two 10 and 20: the reference is arithmetic.
        self.assertEqual(gate.build_median(groups), (10.0 + 20.0) / 2)
        spread = gate.aa_spread(groups)
        self.assertEqual(spread["max_over_min"], 30.0 / 1.0)
        self.assertEqual(set(spread["build_medians_seconds"]), set(groups))

    def test_bootstrap_resamples_build_effect_and_is_reproducible(self):
        numerator = {f"b{n}": [effect * 1.04] * 30
                     for n, effect in enumerate((0.5, 0.8, 1.2, 1.5))}
        denominator = {f"b{n}": [effect] * 30
                       for n, effect in enumerate((0.5, 0.8, 1.2, 1.5))}
        first = gate.ratio_ci(numerator, denominator, 500, 1007)
        self.assertAlmostEqual(first["ratio"], 1.04)
        self.assertEqual(first, gate.ratio_ci(numerator, denominator, 500, 1007))
        # Within each build every run is constant: a run-only bootstrap would
        # collapse this interval. Independent-build variation must survive.
        self.assertLess(first["ci95"][0], 1.04)
        self.assertGreater(first["ci95"][1], 1.04)

    def test_small_and_medium_slowdowns_are_not_speed_gates(self):
        plan = synthetic_plan()
        rows = synthetic_rows(plan)
        for row in rows:
            if row["bars"] != max(plan.sizes) and row["arm"].startswith("v2"):
                row["wall_seconds"] *= 2
        summary = summary_for(plan, rows)
        self.assertTrue(summary["speed_pass"])
        self.assertEqual(summary["gate"], "PASS")

    def test_missing_empty_duplicate_and_failed_runs_fail_closed(self):
        plan = synthetic_plan()
        rows = synthetic_rows(plan)
        failed = copy.deepcopy(rows)
        failed[-1].update(ok=False, exit_code=7, error="injected child failure")
        for bad in ([], rows[:-1], rows + [rows[-1]], failed,
                    [r for r in rows if r["arm"] != "v2-default"]):
            with self.subTest(length=len(bad)):
                summary = summary_for(plan, bad)
                self.assertEqual(summary["gate"], "FAIL")
                self.assertEqual(summary["sizes"], [])  # Failed rows are never averaged.
                self.assertTrue(summary["problems"])
        missing_hash = copy.deepcopy(rows)
        del missing_hash[-1]["metrics_sha256"]
        self.assertEqual(summary_for(plan, missing_hash)["gate"], "FAIL")

    def test_trade_equity_metrics_and_full_sink_mismatches_fail(self):
        plan = synthetic_plan()
        for field in ("legacy_trade_sha256", "equity_sha256", "metrics_sha256", "merged_sha256"):
            rows = synthetic_rows(plan)
            row = next(r for r in rows if r["arm"] == "v2-default" and not r["warmup"])
            row[field] = hashlib.sha256(f"changed {field}".encode()).hexdigest()
            self.assertEqual(summary_for(plan, rows)["gate"], "FAIL")
        rows = synthetic_rows(plan)
        next(r for r in rows if r["arm"] == "v2-default")["record_sha256"]["V"] = "f" * 64
        self.assertEqual(summary_for(plan, rows)["gate"], "FAIL")

    def test_missing_memory_wrong_binary_and_growth_cannot_pass(self):
        plan = synthetic_plan()
        rows = synthetic_rows(plan)
        evidence, artifacts = synthetic_memory(plan)
        wrong_binary = copy.deepcopy(evidence)
        wrong_binary["rows"][0]["module_sha256"] = "0" * 64
        missing_axis = copy.deepcopy(evidence)
        missing_axis["rows"].pop()
        for bad in ({"schema": "missing"}, wrong_binary, missing_axis):
            summary = summary_for(plan, rows, bad, artifacts)
            self.assertEqual(summary["gate"], "FAIL")
        grown = copy.deepcopy(evidence)
        grown["rows"][0]["live_bytes"][1] += 2 * gate.MIB  # Interior peak; endpoints alone miss it.
        self.assertEqual(summary_for(plan, rows, grown, artifacts)["gate"], "FAIL")
        rss = copy.deepcopy(rows)
        for row in rss:
            if row["arm"] == "v2-default":
                row["peak_rss_bytes"] += 32 * gate.MIB
                row["peak_accounted_rss_bytes"] += 32 * gate.MIB
        self.assertEqual(summary_for(plan, rss, evidence, artifacts)["gate"], "FAIL")

    def test_level_choice_uses_size_load_and_immediate_cheaper_candidate(self):
        table = [
            {"level": 1, "total_bytes": 1000, "eligible": True},
            {"level": 2, "total_bytes": 990, "eligible": True},
            {"level": 3, "total_bytes": 980, "eligible": True},
        ]
        # Neither adjacent improvement reaches 2%; savings must not be
        # accumulated past the next cheaper candidate to justify level 3.
        self.assertEqual(gate.choose_level(table), 1)
        table[-1]["total_bytes"] = 950
        self.assertEqual(gate.choose_level(table), 3)
        table[-1]["eligible"] = False
        self.assertEqual(gate.choose_level(table), 1)
        self.assertIsNone(gate.choose_level([dict(row, eligible=False) for row in table]))
        plan = synthetic_plan()
        rows = synthetic_rows(plan)
        for row in rows:
            if row["kind"] == "encode":
                row["cpu_seconds_per_million_e"] = 100.0
        summary = summary_for(plan, rows)
        self.assertIsNone(summary["selected_level"])
        self.assertEqual(summary["gate"], "FAIL")

    def test_load_uses_observed_large_script_rate(self):
        plan = synthetic_plan()
        rows = synthetic_rows(plan)
        first = summary_for(plan, rows)
        for row in rows:
            row["script_bars"] *= 4  # E count and encoder CPU/E are unchanged.
        second = summary_for(plan, rows)
        for original, changed in zip(first["level_table"], second["level_table"]):
            self.assertAlmostEqual(changed["writer_load"], 4 * original["writer_load"])
            for before, after in zip(original["cells"], changed["cells"]):
                self.assertAlmostEqual(after["measured_large_bars_per_second"],
                                       4 * before["measured_large_bars_per_second"])
                self.assertAlmostEqual(after["writer_load"],
                                       after["encode_cpu_seconds_per_million_e"] *
                                       after["measured_large_bars_per_second"] / 1e6)

    def test_measured_candidate_memory_failure_disqualifies_that_level(self):
        plan = synthetic_plan()
        rows = synthetic_rows(plan)
        for row in rows:
            if row["arm"] == "v2-L19":
                row["peak_accounted_rss_bytes"] += 32 * gate.MIB
        summary = summary_for(plan, rows)
        candidate = next(r for r in summary["level_table"] if r["level"] == 19)
        self.assertFalse(candidate["memory_pass"])
        self.assertFalse(candidate["eligible"])
        self.assertNotEqual(summary["selected_level"], 19)
        self.assertTrue(summary["memory_pass"])  # Runtime default still satisfies its hard gate.
        self.assertEqual(summary["gate"], "PASS")
        self.assertEqual(summary["memory_gaps"], [])  # This is a measured limit, not missing evidence.

    def test_rss_gaps_unknown_backing_and_single_peak_fail(self):
        plan = synthetic_plan()
        for change in (
                {"peak_rss_bytes": None}, {"peak_accounted_rss_bytes": None},
                {"rss_includes_resident_files": False}, {"memory_accounting_error": "mincore unavailable"},
                {"peak_accounted_rss_bytes": 100 * gate.MIB},
                {"file_backings": {"scratch": {"backing": "unknown"}}}):
            rows = synthetic_rows(plan)
            row = next(r for r in rows if not r["warmup"] and r["arm"] == "v2-default")
            row.update(change)
            summary = summary_for(plan, rows)
            self.assertFalse(summary["memory_pass"], change)
            self.assertEqual(summary["gate"], "FAIL")

    def test_tmpfs_bytes_enter_incremental_gate_and_boundaries_are_inclusive(self):
        plan = synthetic_plan()
        for delta, expected in ((16 * gate.MIB, True), (16 * gate.MIB + 1, False)):
            rows = synthetic_rows(plan)
            for row in rows:
                if row["arm"].startswith("v2"):
                    row["peak_rss_bytes"] = 64 * gate.MIB  # Heap/process alone appears unchanged.
                    row["peak_tmpfs_resident_bytes"] = 2 * gate.MIB + delta
                    row["peak_accounted_rss_bytes"] = 66 * gate.MIB + delta
            self.assertEqual(summary_for(plan, rows)["memory_pass"], expected)
        evidence, artifacts = synthetic_memory(plan)
        evidence["rows"][0]["live_bytes"] = [8 * gate.MIB, 9 * gate.MIB, 8 * gate.MIB]
        self.assertTrue(summary_for(plan, synthetic_rows(plan), evidence, artifacts)["memory_pass"])
        evidence["rows"][0]["live_bytes"][1] += 1
        self.assertFalse(summary_for(plan, synthetic_rows(plan), evidence, artifacts)["memory_pass"])

    def test_one_measured_size_failure_cannot_be_hidden_by_medians(self):
        plan = synthetic_plan()
        rows = synthetic_rows(plan)
        row = next(r for r in rows if not r["warmup"] and r["arm"] == "v2-default")
        base = next(r for r in rows if (r["build"], r["bars"], r["run"], r["warmup"], r["arm"]) ==
                    (row["build"], row["bars"], row["run"], row["warmup"], "v1"))
        added = base["sink_bytes"] + 1 - row["sink_bytes"]
        row["shared_bytes"] += added
        row["sink_bytes"] += added
        row["metadata_bytes"] += added
        row.update(injected_summary(decoded=64 + added + 1, stored=64 + added))
        summary = summary_for(plan, rows)
        self.assertEqual(summary["gate"], "FAIL")
        self.assertTrue(summary["speed_pass"])
        self.assertEqual(summary["measured_size_verdict"], "FAIL")
        self.assertFalse(summary["v2_le_v1_at_every_measured_size"])
        self.assertEqual(summary["size_counterexamples"][0]["v2_bytes"] - base["sink_bytes"], 1)
        self.assertIn("SIZE COUNTEREXAMPLE", gate.render_summary(summary))

    def test_missing_finalization_scope_and_old_scratch_rows_fail(self):
        plan = synthetic_plan()
        for field in ("wall_scope", "finalization_included", "profile", "finalization_claim_bytes"):
            rows = synthetic_rows(plan)
            row = next(r for r in rows if r["arm"] == "v2-default")
            del row[field]
            self.assertEqual(summary_for(plan, rows)["gate"], "FAIL")

    def test_fewer_than_four_or_aliased_builds_refused(self):
        plan = synthetic_plan()
        for builds in (plan.builds[:3], plan.builds[:3] + (plan.builds[0],)):
            with self.assertRaisesRegex(gate.GateError, "4 distinct"):
                gate.Plan(builds, plan.sizes, plan.strategies).validate()
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stderr(io.StringIO()):
            out = Path(temporary) / "uncreated"
            self.assertEqual(gate.main([
                "--build-roots", "a", "b", "c", "--ohlcv", "unused.csv", "--output", str(out),
            ]), 2)
            self.assertFalse(out.exists())


class SchedulingAndInputTests(SummaryCodecTestCase):
    def test_interleaving_covers_every_arm_in_every_build_and_rotates(self):
        plan = synthetic_plan()
        jobs = list(gate.schedule(plan))
        keys = Counter(job.key for job in jobs)
        self.assertTrue(all(count == 1 for count in keys.values()))
        expected = len(plan.builds) * len(plan.sizes) * len(plan.strategies) * (
            len(plan.arms) + len(plan.levels)) * (plan.replicates + plan.warmups)
        self.assertEqual(len(jobs), expected)
        for build, bars in itertools.product(plan.builds, plan.sizes):
            timed = [j for j in jobs if (j.build, j.bars, j.kind) == (build, bars, "backtest")
                     and not j.warmup]
            self.assertEqual(Counter(j.arm for j in timed), Counter({a: plan.replicates for a in plan.arms}))
        for run in range(plan.replicates):
            group = [j for j in jobs if not j.warmup and j.run == run]
            first = group[:len(plan.builds)]
            self.assertEqual(len({j.build for j in first}), len(plan.builds))
            self.assertGreater(len({j.arm for j in first}), 1)
        first_by_round = [next(j.arm for j in jobs if not j.warmup and j.run == r)
                          for r in range(plan.replicates)]
        self.assertGreater(len(set(first_by_round)), 1)

    def test_reserved_options_and_abbreviations_refused(self):
        for args in (["--so=x"], ["--report-sink-format", "1"], ["--bench"], ["--rep", "1"], ["--ohl", "x"]):
            with self.assertRaises(gate.GateError):
                gate.check_driver_args(args)
        gate.check_driver_args(["--outputs", "--input-tf", "15", "--inputs", '{"length":"4"}'])

    def test_default_selection_uses_trade_count_not_directory_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subset = root / "subset.txt"
            names_and_counts = (("zz-small", 1), ("aa-large", 8), ("mm-mid", 4))
            subset.write_text("\n".join(name for name, _ in names_and_counts) + "\n", encoding="utf-8")
            for name, count in names_and_counts:
                directory = root / name
                directory.mkdir()
                # Entry/exit lines share a trade number. Count trades, not CSV rows.
                tape = "Trade #,Type\n" + "".join(f"{n},Entry\n{n},Exit\n" for n in range(1, count + 1))
                (directory / "tv_trades.csv").write_text(tape, encoding="utf-8")
            self.assertEqual(gate.default_strategies(root, subset), ("zz-small", "mm-mid", "aa-large"))

    def test_json_stream_handles_tiny_chunks_exponents_and_signed_zero(self):
        class TinyReader(io.StringIO):
            def read(self, count=-1):
                return super().read(min(count, 2))

        stream = gate.JSONStream(TinyReader("[1.25e-12,-0.0,9007199254740991]"))
        values = list(stream.array())
        self.assertEqual(values[0], 1.25e-12)
        self.assertEqual(struct.pack("<d", values[1]), struct.pack("<d", -0.0))
        self.assertEqual(values[2], 9007199254740991)
        self.assertEqual(stream.peek(), "")
        self.assertNotEqual(gate.canonical(-0.0, "f64?"), gate.canonical(0.0, "f64?"))

    def test_feed_prefixes_are_exact_and_short_large_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            feed = root / "source.csv"
            feed.write_text("timestamp,open,high,low,close,volume\n" +
                            "".join(f"{n},1,2,1,2,3\n" for n in range(6)), encoding="utf-8")
            out = root / "out"
            out.mkdir()
            paths = gate.prepare_feeds(out, (2, 4, 6), ("probe",), {}, feed)
            source_lines = feed.read_bytes().splitlines(keepends=True)
            for size in (2, 4, 6):
                self.assertEqual(paths[("probe", size)].read_bytes(), b"".join(source_lines[:size + 1]))
            with self.assertRaisesRegex(gate.GateError, "only 6 bars"):
                gate.prepare_feeds(out, (2, 4, 8), ("probe",), {}, feed)

    def test_profiles_short_pages_sha_and_rank_attribution(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "fake.pfs2"
            records = fake_records(4)
            e_payload = json.dumps([r for r in records if r[0] == "E"], separators=(",", ":")).encode()
            t_payload = json.dumps([r for r in records if r[0] == "T"], separators=(",", ":")).encode()
            for full in (False, True):
                fake_v2(path, records, 3, force_full=full)
                accounted = gate.v2_accounting(path)
                self.assertEqual(accounted["profile"], "FULL" if full else "COMPACT")
                e_index = (4096 + 512) + (4096 + 4 * 256) if full else 96
                t_index = (4096 + 512) + 10 * (4096 + 4) if full else 96
                e_bytes = 32 + 3 * (16 + 32) + len(e_payload) + e_index
                t_bytes = 32 + 20 * (16 + 32) + len(t_payload) + t_index
                self.assertEqual(accounted["type_bytes"]["E"], e_bytes)
                self.assertEqual(accounted["type_bytes"]["T"], t_bytes)
                self.assertEqual(accounted["column_sha_total_bytes"], 32 * (3 + 20 + 1))
                self.assertEqual(accounted["shared_bytes"], path.stat().st_size - e_bytes - t_bytes)
                self.assertEqual(accounted["fixed_floor_bytes"], 63432 if full else 768)
                self.assertEqual(accounted["rank_bytes"], 10 * (4096 + 4) if full else 0)
                raw_summary = json.dumps(fake_metadata(4, 1), separators=(",", ":")).encode()
                self.assertEqual(accounted["summary_decoded_bytes"], len(raw_summary))
                self.assertEqual(accounted["summary_stored_bytes"], len(raw_summary_frame(raw_summary)))
                self.assertEqual(accounted["summary_bytes"], 52 + len(raw_summary_frame(raw_summary)))
                self.assertGreater(accounted["summary_expansion_bytes"], 0)
                self.assertLess(accounted["summary_probe_stored_bytes"], len(raw_summary))
                self.assertEqual(accounted["summary_probe_level"], 3)
            with path.open("r+b") as target:
                target.truncate(path.stat().st_size - 1)
            with self.assertRaises(gate.GateError):
                gate.v2_accounting(path)

    def test_short_final_page_boundaries_and_profile_thresholds(self):
        self.assertEqual(gate.page_extent(0, 512), (0, 0, 0))
        self.assertEqual(gate.page_extent(120, 512), (1, 65536, 65536))
        self.assertEqual(gate.page_extent(121, 512), (2, 4608, 65536 + 4608))
        self.assertEqual(gate.page_extent(400000, 4), (27, 4096 + 2560, 1710592))
        for e, t, d, expected in (
                (10979, 3313, 128, "COMPACT"), (10980, 1, 5, "FULL"),
                (1200, 3314, 6, "FULL"), (100, 0, 129, "FULL")):
            self.assertEqual(gate.canonical_profile(e, t, d), expected)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.pfs2"
            fake_v2(path, fake_records(4), 3)
            with path.open("r+b") as target:
                target.seek(-160, 2)
                root = struct.unpack("<Q", target.read(8))[0]
                target.seek(root + 6)
                target.write(struct.pack("<H", 0))  # Compact length with FULL flag.
            with self.assertRaisesRegex(gate.GateError, "profile flag/length"):
                gate.v2_accounting(path)

    def test_q_m_probes_use_exact_projection_bytes_and_the_same_head_level(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "probes.pfs2"
            for metadata, has_m in ((fake_metadata(4, 1), True), (
                    {key: value for key, value in fake_metadata(4, 1).items()
                     if key in ("engine", "summary", "metrics", "diagnostics", "sink_folds")}, False)):
                fake_v2(path, fake_records(4), 7, metadata=metadata)
                raw = json.dumps(metadata, separators=(",", ":")).encode()
                q, m, _, _ = gate.summary_projections(raw)
                codec = FakeSummaryZstd()
                with patch.object(codec, "compressed_size", wraps=codec.compressed_size) as probe, \
                        patch.object(gate, "summary_codec", return_value=codec):
                    info = gate.v2_accounting(path)
                expected = [(raw, 7), (q, 7)] + ([(m, 7)] if has_m else [])
                self.assertEqual([call.args for call in probe.call_args_list], expected)
                self.assertEqual((info["J_Q"], info["J_M"]), (len(q), len(m)))
                self.assertEqual(info["C_Q"], max(1, len(q) // 4))
                self.assertEqual(info["C_M"], max(1, len(m) // 4) if has_m else 0)
                self.assertEqual(info["summary_probe_zstd_build"], codec.identity)

    def test_complete_grid_and_model_values_from_spec_ledger(self):
        cases = [c for c in gate.size_grid_cases() if c["shape"] == "grid"]
        expected = {(n, t, f) for n in (100, 1000, 5000, 9000, 10000, 100000, 1000000)
                    for t in (0, 1, n // 20) for f in (0.25, 0.5, 1.0)}
        self.assertEqual({(c["N"], c["T"], c["fraction"]) for c in cases}, expected)
        # Independent published component ledgers, including the model metadata.
        for n, t, f, fixed in ((100, 1, 0.25, 4039), (10000, 500, 0.5, 4583),
                               (100000, 5000, 0.25, 613111), (1000000, 50000, 0.5, 2770599)):
            model = gate.size_model(n, t, f)
            self.assertEqual(model["v2_bytes"], math.ceil(fixed + (0.2 + 11 * f) * n + 55.1 * t))
            self.assertEqual(model["v1_bytes"], math.ceil(948 + len(str(n)) + len(str(t)) +
                                                       (32 + 26 * f) * n + 201 * t))
        # Independent full A and comparable A' ledgers from REPORT-spec6.
        fixed = (2951, 4039, 4039, 2951, 4039, 4039, 3223, 4311, 4311,
                 3495, 4583, 4583, 3495, 4583, 4583, 360535, 407135, 613111,
                 528375, 574975, 2770599)
        comparable_fixed = (2439, 3527, 3527, 2439, 3527, 3527, 2711, 3799, 3799,
                            2983, 4071, 4071, 2983, 4071, 4071, 360023, 406623, 612599,
                            527863, 574463, 2770087)
        populations = [(n, t) for n in (100, 1000, 5000, 9000, 10000, 100000, 1000000)
                       for t in (0, 1, n // 20)]
        for (n, t), subtotal, counted_subtotal in zip(populations, fixed, comparable_fixed):
            low_v1 = math.ceil(948 + len(str(n)) + len(str(t)) + 38.5 * n + 201 * t)
            low_v2 = math.ceil(subtotal + 2.95 * n + 55.1 * t)
            low_comparable = math.ceil(counted_subtotal + 2.95 * n + 55.1 * t)
            for fraction, v1_added, v2_added in ((0.25, 0, 0), (0.5, 13 * n // 2, 11 * n // 4),
                                               (1.0, 39 * n // 2, 33 * n // 4)):
                model = gate.size_model(n, t, fraction)
                self.assertEqual((model["v1_bytes"], model["v2_bytes"]), (low_v1 + v1_added, low_v2 + v2_added))
                self.assertEqual(model["COMPARABLE_V2"], low_comparable + v2_added)
                self.assertEqual((model["J_Q"], model["C_Q"], model["J_M"], model["C_M"]), (1024, 512, 3073, 769))
                self.assertEqual((model["C_meta"], model["C_comparable"]), (512, 512))
                self.assertEqual((model["summary_decoded_bytes"], model["summary_stored_bytes"]), (4096, 1024))
                self.assertEqual(model["verdict_label"], "MODEL-ONLY PASS")

    def test_summary_caps_and_frame_header_refuse_before_decode(self):
        raw = json.dumps(fake_metadata(4, 1), separators=(",", ":")).encode()
        frame = raw_summary_frame(raw)
        gate.check_summary_frame(frame, len(raw))
        for invalid in (raw, frame[:5], frame[:4] + b"\xa1" + frame[5:],  # Dictionary flag.
                        frame[:5] + struct.pack("<I", len(raw) + 1) + frame[9:],
                        b"\x28\xb5\x2f\xfd\x80\x80" + struct.pack("<I", len(raw))):
            with self.assertRaises(gate.GateError):
                gate.check_summary_frame(invalid, len(raw))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "fake.pfs2"
            # Deliberately inconsistent extents check FIRST-failure precedence;
            # these header mutations are not 17.4's independent conformance files.
            for stored, decoded in ((1048610, len(raw)), (len(frame), 1048577)):
                fake_v2(path, fake_records(4), 3)
                with path.open("r+b") as target:
                    target.seek(-160, 2)
                    root = struct.unpack("<Q", target.read(8))[0]
                    target.seek(root + 8)
                    summary = struct.unpack("<Q", target.read(8))[0]
                    target.seek(summary + 4)
                    target.write(struct.pack("<II", stored, decoded))
                with patch.object(gate, "summary_codec") as codec:
                    with self.assertRaisesRegex(gate.SummaryLimit, "summary_limit"):
                        gate.v2_accounting(path)
                    codec.assert_not_called()
        # Header-only acceptance at the exact decoded/window ceiling; no
        # decoded buffer is allocated by these framing checks.
        gate.check_summary_frame(b"\x28\xb5\x2f\xfd\xa0" + struct.pack("<I", 1048576), 1048576)
        gate.check_summary_budget(1048576, 1048609)  # Raw fallback fits exactly, header included.
        gate.check_summary_budget(1048576, 262144)  # Compressible ceiling also fits.
        for decoded, stored in ((1048577, 8), (1024, 1048610)):
            with self.assertRaises(gate.SummaryLimit):
                gate.check_summary_budget(decoded, stored)
        # Matching FCS with separate non-single-segment windows: 1 MiB passes,
        # 2 MiB alone refuses; missing/mismatched FCS wins before the window.
        gate.check_summary_frame(b"\x28\xb5\x2f\xfd\x80\x50" + struct.pack("<I", 1024), 1024)
        with self.assertRaisesRegex(gate.SummaryLimit, "window"):
            gate.check_summary_frame(b"\x28\xb5\x2f\xfd\x80\x58" + struct.pack("<I", 1024), 1024)
        with self.assertRaisesRegex(gate.GateError, "content size differs"):
            gate.check_summary_frame(b"\x28\xb5\x2f\xfd\x80\x58" + struct.pack("<I", 1025), 1024)


class MemoryObservationTests(unittest.TestCase):
    def test_selected_actual_bars_and_anchor_come_from_window_counts(self):
        report = fake_metadata(1500, 0)
        report["report_window"] = {"counts": {"window_script_bars": 999, "anchor_points": 1}}
        row = {}
        gate.report_timings(row, report)
        self.assertEqual(row["script_bars"], 1500)
        self.assertEqual((row["actual_bars"], row["anchor_count"]), (999, 1))
        del report["report_window"]["counts"]["anchor_points"]
        with self.assertRaisesRegex(gate.GateError, "selected bars/anchor"):
            gate.report_timings({}, report)

    def test_inclusive_samples_deduplicate_mapped_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scratch = root / "scratch"
            scratch.mkdir()
            output = root / "sink.pfs2"
            output.write_bytes(b"x" * 4096)
            owned = scratch / ".pfs2-nonce"
            owned.mkdir()
            (owned / "work-00.bin").write_bytes(b"y" * 4096)
            sampler = gate.MemorySampler({"output": output, "scratch": scratch, "temporary": root / "temp"})
            sampler.paths["temporary"].mkdir()
            sampler.backings = {k: {"backing": "tmpfs"} for k in sampler.paths}
            with patch.object(gate, "resident_file_bytes", return_value=4096), \
                    patch.object(gate, "process_memory", return_value=(100 * 4096, 4096)):
                # No platform-specific fd walk is needed to test the counter arithmetic.
                with patch.object(Path, "iterdir", return_value=iter(())):
                    sampler.snapshot(12345)
            result = sampler.result(100 * 4096)
            self.assertEqual(result["peak_tmpfs_resident_bytes"], 2 * 4096)
            self.assertEqual(result["peak_accounted_rss_bytes"], 101 * 4096)
            self.assertEqual(result["peak_work_file_bytes"], 4096)
            self.assertTrue(result["rss_includes_resident_files"])
            missing = sampler.result(None)
            self.assertFalse(missing["rss_includes_resident_files"])
            self.assertIn("peak RSS unavailable", missing["memory_accounting_error"])

    def test_unknown_backing_and_no_samples_are_gaps(self):
        with tempfile.TemporaryDirectory() as temporary:
            sampler = gate.MemorySampler({"output": Path(temporary)})
            sampler.backings["output"]["backing"] = "unknown"
            result = sampler.result(64 * gate.MIB)
            self.assertIsNone(result["peak_accounted_rss_bytes"])
            self.assertFalse(result["rss_includes_resident_files"])
            self.assertIn("unknown backing", result["memory_accounting_error"])

    def test_finalization_claim_is_not_replaced_by_process_rss(self):
        report = fake_metadata(100, 1)
        row = {"peak_rss_bytes": 64 * gate.MIB}
        gate.report_timings(row, report)
        self.assertIsNone(row["finalization_peak_added_bytes"])
        self.assertIn("unmeasured", row["finalization_claim_check"])
        for amount, expected in ((gate.FINALIZATION_CLAIM, "within claim"),
                                 (gate.FINALIZATION_CLAIM + 1, "above claim, within cap"),
                                 (gate.FINALIZATION_CAP + 1, "above cap")):
            report["diagnostics"]["sink_timing"] = {"finalization_peak_added_bytes": amount}
            gate.report_timings(row, report)
            self.assertEqual(row["finalization_claim_check"], expected)


# Fake driver/codec. Valid v1 files go through the REAL v1 verifier. The fake
# v2 container has the spec's SHA/framing/short-page extents plus a private JSON test
# payload. Its fake codec is explicitly selected only by these tests.
def fake_schema():
    reader = import_file(ROOT / "docker/pf_sink.py", "fake_schema_v1")
    return [
        {"type": tag, "fields": [f[0] for f in fields],
         "types": [f[1] for f in fields], "units": [f[2] for f in fields]}
        for tag, fields in (("E", reader._E), ("T", reader._T))
    ]


def fake_records(bars):
    records = [["E", 1700000000000 + n * 60000, 10000.0 + n, -0.0] for n in range(bars)]
    records.append(["T", 1, "long", 1700000000000, 1700000000000 + (bars - 1) * 60000,
                    100.0, 101.0, 2.0, 2.0, 1.0, 3.0, -0.0, 0.0, 0, bars - 1, False,
                    1, 1, "entry", "exit", ""])
    return records


def json_line(value):
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n").encode()


def fake_v1(path, records):
    header = {"schema": "pf-report-sink", "version": 1, "run_id": "sink-speed",
              "engine": "synthetic", "records": fake_schema()}
    counts = Counter(record[0] for record in records)
    lines = [json_line(header)] + [json_line(record) for record in records]
    digest = hashlib.sha256(b"".join(lines)).hexdigest()
    trailer = {"trailer": "pf-report-sink", "version": 1,
               "counts": {tag: counts[tag] for tag in "ET"}, "sha256": digest}
    path.write_bytes(b"".join(lines) + json_line(trailer))


def fake_metadata(bars, trades):
    return {
        "engine": "synthetic", "input": {"bars": bars}, "applied_inputs": {}, "applied_overrides": {},
        "applied_runtime": {}, "elapsed_seconds": SLEEP_SECONDS,
        "summary": {"total_trades": trades}, "metrics": {"all": {"net_profit": 2.0}},
        "diagnostics": {"script_bars_processed": bars}, "fingerprint": None,
        "sink_folds": {"closed_count": trades, "open_at_end_count": 0, "open_at_end_pnl": 0.0},
    }


def fake_v2(path, records, level, force_full=False, metadata=None, block_records=None):
    header = {"schema": "pf-report-sink", "version": 2, "run_id": "sink-speed",
              "engine": "synthetic", "records": fake_schema(), "format": {"level": level}}
    raw_head = json.dumps(header, separators=(",", ":")).encode()
    blob = bytearray(b"\x89PFS2\r\n\x1a" + struct.pack("<I", len(raw_head)) + raw_head + bytes(4))
    counts = Counter(record[0] for record in records)
    blocks = Counter()
    caps = {"E": 4096, "T": 1024, "I": 65536}
    if block_records:
        caps.update({tag: int(count) for tag, count in (item.split("=") for item in block_records.split(","))})
    # The private payload is retained so the fake converter cannot reconstruct
    # an unrelated golden output when the driver writes a differing trade.
    for tag in "ETI":
        values = [r[0] for r in records] if tag == "I" else [r for r in records if r[0] == tag]
        columns = 3 if tag == "E" else 20 if tag == "T" else 1
        for rec0 in range(0, len(values), caps[tag]):
            part = values[rec0:rec0 + caps[tag]]
            payload = json.dumps(part, separators=(",", ":")).encode()
            block = bytearray(32)
            block[:4] = b"BLK2"
            block[4] = ord(tag)
            struct.pack_into("<IIQI", block, 8, columns, len(part), rec0, 48 * columns + len(payload))
            directory = bytearray(16 * columns)
            struct.pack_into("<I", directory, 8, len(payload))
            blob += block + directory + bytes(32 * columns) + payload
            blocks[tag] += 1
    data_end = len(blob)
    summary = json.dumps(metadata or fake_metadata(counts["E"], counts["T"]), separators=(",", ":")).encode()
    if len(summary) > 1024 * 1024:
        path.write_bytes(blob)  # Check J before encoding/append, as spec 10.6 requires.
        raise gate.SummaryLimit("test raw-frame writer exceeds S_MAX")
    frame = raw_summary_frame(summary)
    if 52 + len(frame) > 1048661:
        path.write_bytes(blob)  # Failed finalization leaves no root/success tail.
        raise gate.SummaryLimit("test raw-frame writer exceeds the stored SUMMARY bound")
    blob += (b"SUM2" + struct.pack("<4I", len(frame), len(summary), zlib.crc32(summary), 0)
             + hashlib.sha256(summary).digest() + frame)
    compact = not force_full and counts["E"] < 10980 and counts["T"] < 3314 and sum(blocks.values()) <= 128
    table_locations = []
    finite = sum(r[0] == "E" and r[2] is not None for r in records)
    descriptors_offset = len(blob)
    if compact:
        blob += bytes(96 * sum(blocks.values()))
    else:
        for table in range(19):
            width = 512 if table < 8 else 256 if table == 18 else 4
            entries = blocks[(gate.TAGS + "I")[table]] if table < 8 else min(1200, finite) if table == 18 else counts["T"]
            capacity = 61440 // width
            page_count = (entries + capacity - 1) // capacity
            offset = len(blob) if entries else 0
            last = 0
            for begin in range(0, entries, capacity):
                last = 4096 + min(capacity, entries - begin) * width
                page = bytearray(last)
                page[:4] = b"IDX2"
                page[4] = table
                blob += page
            table_locations.append((offset, page_count, width, entries, last, int(bool(entries))))
    root_offset = len(blob)
    root = bytearray(320 if compact else 4096)
    root[:4] = b"FTR2"
    struct.pack_into("<HH", root, 4, 2, int(compact))
    raw_counts = (counts["E"], counts["T"], 0, 0, 0, 0, 0, len(records))
    if compact:
        struct.pack_into("<QII", root, 8, data_end, 52 + len(frame), sum(blocks.values()))
        struct.pack_into("<8Q", root, 24, *raw_counts)
        struct.pack_into("<Q", root, 88, finite)
        struct.pack_into("<QQ", root, 160, descriptors_offset, 96 * sum(blocks.values()))
        struct.pack_into("<8I", root, 208, *(blocks[t] for t in gate.TAGS + "I"))
    else:
        struct.pack_into("<QQII", root, 8, data_end, data_end, 52 + len(frame), sum(blocks.values()))
        struct.pack_into("<8Q", root, 32, *raw_counts)
        struct.pack_into("<QI", root, 416, finite, 1200)
        for table, locator in enumerate(table_locations):
            struct.pack_into("<QIIQII", root, 512 + table * 64, *locator)
    blob += root
    tail = bytearray(160)
    struct.pack_into("<QI", tail, 0, root_offset, len(root))
    struct.pack_into("<Q", tail, 16, root_offset + len(root) + 160)
    tail[152:] = b"PFS2END\n"
    path.write_bytes(blob + tail)


def fake_v2_records(path):
    with path.open("rb") as source:
        source.seek(8)
        head_length = struct.unpack("<I", source.read(4))[0]
        source.seek(16 + head_length)
        populations = {"E": [], "T": [], "I": []}
        while True:
            block = source.read(32)
            if block[:4] != b"BLK2":
                break
            columns = struct.unpack_from("<I", block, 8)[0]
            body = source.read(struct.unpack_from("<I", block, 24)[0])
            populations[chr(block[4])].extend(json.loads(body[48 * columns:]))
        cursors = {t: iter(populations[t]) for t in "ET"}
        return [next(cursors[tag]) for tag in populations["I"]]


def _new_report_sink(*_args, **_kwargs):
    return True  # The harness worker must replace this callable for the off arm.


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--so", type=Path, required=True)
    parser.add_argument("--ohlcv", type=Path, required=True)
    parser.add_argument("--generated-cpp", type=Path, required=True)
    parser.add_argument("--report-sink", type=Path)
    parser.add_argument("--report-sink-format", type=int, choices=(1, 2), default=1)
    parser.add_argument("--report-sink-level", type=int, default=0)
    parser.add_argument("--spill-dir")
    parser.add_argument("--run-id")
    args = parser.parse_args(argv)
    if (args.so.parent / "fail-driver").exists():
        return 7
    with args.ohlcv.open() as source:
        bars = sum(1 for _ in source) - 1
    records = fake_records(bars)
    time.sleep(SLEEP_SECONDS)
    report = fake_metadata(bars, 1)
    if _new_report_sink() is None:
        fields = {s["type"]: s["fields"] for s in fake_schema()}
        report["trades"] = [dict(zip(fields["T"][:16], r[1:17])) for r in records if r[0] == "T"]
        report["equity_curve"] = [dict(zip(fields["E"], r[1:])) for r in records if r[0] == "E"]
    else:
        if args.report_sink is None:
            return 9  # A mislabeled off arm must not silently succeed.
        if args.report_sink_format == 2 and (args.so.parent / "mismatch-driver").exists():
            records[-1][8] += 1.0  # A differing actual PnL, not just a changed digest.
        if args.report_sink_format == 1:
            fake_v1(args.report_sink, records)
        else:
            start = time.monotonic()
            time.sleep(FINALIZE_SLEEP_SECONDS)  # Models work AFTER the last engine bar.
            report["diagnostics"]["sink_timing"] = {"finalization_seconds": time.monotonic() - start}
            fake_v2(args.report_sink, records, args.report_sink_level or 3, metadata=report)
        counts = Counter(r[0] for r in records)
        report["report_sink"] = {"version": args.report_sink_format, "counts": dict(counts)}
        if args.report_sink_format == 2:
            report["report_sink"]["scratch_flags"] = 2  # Fake driver makes no backing claim.
    print(json.dumps(report, separators=(",", ":")))
    return 0


def fake_codec(argv):
    if argv[0] == "to-v1":
        fake_v1(Path(argv[2]), fake_v2_records(Path(argv[1])))
        return 0
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=("from-v1",))
    parser.add_argument("source", type=Path)
    parser.add_argument("target", type=Path)
    parser.add_argument("--summary-json", type=Path, required=True)
    parser.add_argument("--level", type=int, required=True)
    parser.add_argument("--block-records")
    args = parser.parse_args(argv)
    with args.source.open() as source:
        values = [json.loads(line) for line in source]
    records = [row for row in values if isinstance(row, list)]
    time.sleep(SLEEP_SECONDS)
    metadata = gate.load_json(args.summary_json)
    try:
        fake_v2(args.target, records, args.level, metadata=metadata, block_records=args.block_records)
    except gate.SummaryLimit as error:
        print(json.dumps({"error": {"kind": "summary_limit", "message": str(error)}, "complete": 0}))
        print("WriterError{summary_limit}", file=sys.stderr)
        return 7
    return 0


def fake_builds(directory):
    roots = []
    for number in range(4):
        root = directory / f"build-{number}"
        library_dir = root / "build/lib"
        library_dir.mkdir(parents=True)
        (library_dir / "libpineforge.a").write_bytes(f"independent fake library {number}".encode())
        (root / "build/CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\n"
            "CMAKE_CXX_COMPILER_LAUNCHER:STRING=\nPINEFORGE_WITH_ZSTD:STRING=ON\n",
            encoding="utf-8")
        strategy = root / "corpus/validation/fake"
        strategy.mkdir(parents=True)
        (strategy / "strategy.so").write_bytes(f"independent fake module {number}".encode())
        (strategy / "generated.cpp").write_text("// identical synthetic source\n", encoding="utf-8")
        roots.append(str(root.resolve()))
    return tuple(roots)


class FakeDriverTests(SummaryCodecTestCase):
    def test_main_exits_nonzero_on_measured_clause_failures(self):
        for failing in (injected_size_observation(n=128, v1=6081, v2=6081, stored=2633),
                        injected_size_observation(0, 0, 950, 4023, stored=2544),
                        injected_size_observation(999, 0, 39414, 39414)):
            with self.subTest(clause=failing["case"]), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                builds = fake_builds(root)
                plan = gate.Plan(builds, (100, 1000, 10000), ("validation/fake",))
                rows = synthetic_rows(plan)
                feed = root / "feed.csv"
                feed.write_text("timestamp,open,high,low,close,volume\n", encoding="utf-8")
                output = root / "results"
                # Inject already passing timing/memory analysis. The actual CLI
                # must still reject the independently injected size failure.
                prepared = {
                    "seed": 1, "bootstrap_iterations": 200, "bootstrap": "injected independent analysis",
                    "large_bars": 10000, "sizes": [], "level_table": [], "selected_level": 3,
                    "problems": [], "speed_pass": True, "memory_pass": True, "gate": "PASS",
                }
                with patch.object(gate, "prepare_feeds", return_value={}), \
                        patch.object(gate, "experiment", return_value=rows), \
                        patch.object(gate, "summarize", return_value=prepared), \
                        patch.object(gate, "size_grid_cases", return_value=[failing["case"]]), \
                        patch.object(gate, "measure_size_grid", return_value=[failing]), \
                        contextlib.redirect_stdout(io.StringIO()):
                    status = gate.main([
                        "--build-roots", *builds, "--sizes", "100", "1000", "10000",
                        "--strategies", "validation/fake", "--ohlcv", str(feed),
                        "--driver", str(SELF), "--codec", str(SELF), "--output", str(output),
                        "--bootstrap-iterations", "200",
                    ])
                self.assertEqual(status, 1)
                summary = gate.load_json(output / "summary.json")
                self.assertEqual(summary["measured_size_verdict"], "FAIL")
                self.assertTrue(summary["speed_pass"])
                self.assertEqual(len(summary["size_failures"]), 1)

    def test_size_seed_outputs_and_large_summary_use_real_v1_verifier(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = root / "header.json"
            metadata = root / "meta.json"
            gate.dump_json(header, {"schema": "pf-report-sink", "version": 1,
                                    "run_id": "synthetic-grid", "engine": "synthetic", "records": fake_schema()})
            gate.dump_json(metadata, fake_metadata(4, 1))
            reader = import_file(ROOT / "docker/pf_sink.py", "seed_test_v1")
            for shape in ("grid", "outputs", "large-summary", "large-compressible-summary", "empty", "selected_report_view"):
                case = {"N": 0 if shape == "empty" else 4, "T": 0 if shape == "empty" else 1,
                        "fraction": 0.5, "shape": shape}
                if shape == "selected_report_view":
                    case.update(N=63, T=0, actual_bars=1000, anchor_count=1)
                sink, summary = root / f"{shape}.jsonl", root / f"{shape}.json"
                gate.write_size_seed(header, metadata, case, sink, summary)
                observed = gate.read_v1(sink, reader)
                self.assertEqual(observed["counts"]["E"], case["N"])
                self.assertEqual(observed["counts"]["T"], case["T"])
                self.assertEqual(gate.load_json(summary)["sink_folds"], observed["sink_folds"])
                if shape == "outputs":
                    self.assertEqual(observed["counts"]["O"], case["N"])
                    self.assertEqual(observed["counts"]["V"], case["N"])
                    self.assertEqual(observed["counts"]["K"], 1)
                if shape == "large-summary":
                    self.assertGreater(summary.stat().st_size, 65536)
                if shape == "large-compressible-summary":
                    self.assertEqual(summary.stat().st_size, 1048576)
                if shape == "selected_report_view":
                    selected_metadata = gate.load_json(summary)
                    self.assertEqual(selected_metadata["summary"]["bars_processed"], 1000)
                    self.assertEqual(selected_metadata["report_window"]["counts"],
                                     {"window_script_bars": 1000, "anchor_points": 1})
                    self.assertEqual(observed["counts"]["E"], 63)

    def test_real_child_size_grid_records_both_profiles_model_and_expansion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            seed = root / "sink.jsonl"
            fake_v1(seed, fake_records(4))
            gate.dump_json(root / "summary-meta.json", fake_metadata(4, 1))
            rows = [{"ok": True, "kind": "backtest", "arm": "v1", "warmup": True,
                     "sink_path": str(seed), "artifact_dir": str(root)}]
            cases = [
                {"id": "zero", "N": 4, "T": 0, "fraction": 0.5, "shape": "grid"},
                {"id": "one", "N": 128, "T": 1, "fraction": 0.5, "shape": "grid"},
                {"id": "full", "N": 128, "T": 0, "fraction": 1.0, "shape": "block-boundary",
                 "block_records": "E=1,T=1024,I=65536"},
                {"id": "summary", "N": 4, "T": 0, "fraction": 0.5, "shape": "large-summary"},
                {"id": "selected-63", "N": 63, "T": 0, "fraction": 0.5, "shape": "selected_report_view",
                 "actual_bars": 1000, "anchor_count": 1},
                {"id": "selected-64", "N": 64, "T": 0, "fraction": 0.5, "shape": "selected_report_view",
                 "actual_bars": 1000, "anchor_count": 1},
                {"id": "selected-128", "N": 128, "T": 3, "fraction": 0.5, "shape": "selected_report_view",
                 "actual_bars": 1000, "anchor_count": 1},
            ]
            args = argparse.Namespace(v1_reader=ROOT / "docker/pf_sink.py", codec=SELF,
                                      size_grid_level=3, keep_artifacts=False, zstd_builds={
                                          "reference": {"artifact_sha256": gate.sha256_file(SELF),
                                                        "build": "fake reference", "log": "injected",
                                                        "zstd_library_sha256": "0" * 64}})
            with patch.object(gate, "size_grid_cases", return_value=cases), \
                    contextlib.redirect_stdout(io.StringIO()):
                measured = gate.measure_size_grid(args, rows, root)
            self.assertEqual(len(measured), len(cases))
            self.assertTrue(all(r["ok"] for r in measured), [r.get("error") for r in measured])
            self.assertEqual([r["v2"]["profile"] for r in measured],
                             ["COMPACT", "COMPACT", "FULL", "COMPACT", "COMPACT", "COMPACT", "COMPACT"])
            for result, clause in zip(measured[-3:], ("(ii-a)", "(ii-a)", "(i)")):
                self.assertEqual(result["measured"]["clause"], clause)
                self.assertEqual((result["v2"]["actual_bars"], result["v2"]["anchor_count"]), (1000, 1))
            self.assertEqual(measured[-1]["measured"]["verdict"], "FAIL")  # Fake raw payload expands under (i).
            for result in measured:
                self.assertEqual(result["v1"]["counts"], result["v2"]["counts"])
                self.assertGreater(result["v1"]["wall_seconds"], 0)
                self.assertGreater(result["v2"]["wall_seconds"], 0)
                self.assertIn("v1_bytes", result["model"])
            summary = summary_for(synthetic_plan(), synthetic_rows(synthetic_plan()))
            gate.update_size_verdict(summary, [], measured, expected_cases=cases)
            self.assertEqual(summary["gate"], "FAIL")
            self.assertEqual(summary["measured_size_verdict"], "FAIL")
            self.assertIn("one", [r["cell"] for r in summary["size_failures"]])
            self.assertIn("selected-128", [r["cell"] for r in summary["size_failures"]])
            self.assertFalse(summary["v2_le_v1_at_every_measured_size"])
            self.assertIn("summary", [r["cell"] for r in summary["size_expansions"]])
            self.assertIn("MODEL-ONLY_full_v2_B", gate.render_summary(summary))

    def test_real_child_summary_refusal_is_preserved_and_later_measurements_continue(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            seed = root / "sink.jsonl"
            fake_v1(seed, fake_records(4))
            gate.dump_json(root / "summary-meta.json", fake_metadata(4, 1))
            rows = [{"ok": True, "kind": "backtest", "arm": "v1", "warmup": True,
                     "sink_path": str(seed), "artifact_dir": str(root)}]
            cases = [
                # Raw blocks at S_MAX fit exactly. Its successor refuses on J,
                # before a frame is encoded, irrespective of compressibility.
                {"id": "ceiling", "N": 100, "T": 1, "fraction": 0.25, "shape": "ceiling-compressible-summary"},
                {"id": "above", "N": 100, "T": 1, "fraction": 0.25, "shape": "above-ceiling-summary"},
                {"id": "after-refusal", "N": 4, "T": 0, "fraction": 0.5, "shape": "grid"},
            ]
            args = argparse.Namespace(v1_reader=ROOT / "docker/pf_sink.py", codec=SELF,
                                      size_grid_level=3, keep_artifacts=False, zstd_builds={
                                          "reference": {"artifact_sha256": gate.sha256_file(SELF),
                                                        "build": "fake reference", "log": "injected",
                                                        "zstd_library_sha256": "0" * 64}})
            with patch.object(gate, "size_grid_cases", return_value=cases), contextlib.redirect_stdout(io.StringIO()):
                measured = gate.measure_size_grid(args, rows, root)
            self.assertEqual(len(measured), 3)
            self.assertTrue(measured[0]["ok"], measured[0].get("error"))
            self.assertEqual((measured[0]["v2"]["summary_decoded_bytes"], measured[0]["v2"]["summary_bytes"]),
                             (1048576, 1048661))
            self.assertNotEqual(measured[0]["measured"]["verdict"], "REFUSED")
            refused = measured[1]
            self.assertFalse(refused["ok"])
            self.assertEqual(refused["v2"]["exit_code"], 7)
            self.assertEqual((refused["measured"]["clause"], refused["measured"]["verdict"]), ("(ii-a)", "REFUSED"))
            self.assertEqual(refused["v2"]["summary_decoded_bytes"], 1048577)
            self.assertEqual(refused["writer_refusal"]["receipt_complete"], 0)
            self.assertNotIn("verification", refused)
            self.assertTrue(Path(refused["v2"]["invalid_sink_path"]).is_file())
            self.assertTrue(measured[2]["ok"], measured[2].get("error"))
            summary = {"problems": [], "speed_pass": True, "memory_pass": True}
            gate.update_size_verdict(summary, injected_production_rows(), measured, expected_cases=cases)
            self.assertEqual([r["cell"] for r in summary["size_refusals"]], ["above"])
            self.assertNotIn("above", [r["cell"] for r in summary["size_failures"]])
            self.assertNotIn("above", summary["size_pending"])

    def test_hardlinked_modules_and_cache_launcher_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            builds = fake_builds(root)
            module0 = Path(builds[0]) / "corpus/validation/fake/strategy.so"
            module1 = Path(builds[1]) / "corpus/validation/fake/strategy.so"
            module1.unlink()
            os.link(module0, module1)
            with self.assertRaisesRegex(gate.GateError, "hardlinked"):
                gate.inspect_builds(builds, ("validation/fake",), {}, "build/lib/libpineforge.a")
            cache = Path(builds[0]) / "build/CMakeCache.txt"
            with cache.open("a") as target:
                target.write("CMAKE_CXX_COMPILER_LAUNCHER:STRING=ccache\n")
            with self.assertRaisesRegex(gate.GateError, "ccache off"):
                gate.inspect_builds(builds, ("validation/fake",), {}, "build/lib/libpineforge.a")

    def test_real_child_orchestration_wall_counts_bytes_and_incomplete_gate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            builds = fake_builds(root)
            feed = root / "feed.csv"
            feed.write_text("timestamp,open,high,low,close,volume\n" +
                            "".join(f"{1700000000000 + n * 60000},1,2,1,2,3\n" for n in range(6)),
                            encoding="utf-8")
            output = root / "results"
            # A reduced level set and no memory evidence MUST remain a failed
            # gate, even when every orchestration row succeeds.
            arguments = [
                "--build-roots", *builds, "--sizes", "2", "4", "6",
                "--strategies", "validation/fake", "--levels", "1",
                "--ohlcv", str(feed), "--driver", str(SELF), "--codec", str(SELF),
                "--output", str(output), "--bootstrap-iterations", "200",
                "--skip-size-grid",
            ]
            with contextlib.redirect_stdout(io.StringIO()):
                status = gate.main(arguments)
            self.assertEqual(status, 1)
            rows = [json.loads(line) for line in (output / "raw.jsonl").read_text().splitlines()]
            plan = gate.Plan(builds, (2, 4, 6), ("validation/fake",), levels=(1,))
            self.assertEqual(len(rows), len(list(gate.schedule(plan))))
            self.assertTrue(all(row["ok"] for row in rows), [r.get("error") for r in rows if not r["ok"]])
            self.assertEqual(gate.validate_rows(plan, rows), [])
            for row in rows:
                self.assertGreaterEqual(row["wall_seconds"], SLEEP_SECONDS)
                self.assertEqual(row["counts"]["T"], 1)
                self.assertEqual(row["counts"]["E"], row["bars"])
                if row["arm"] != "off":
                    self.assertEqual(row["sink_bytes"], row["shared_bytes"] + sum(row["type_bytes"].values()))
                    self.assertEqual(row["bytes_per_trade"], row["type_bytes"]["T"])
                    self.assertEqual(row["bytes_per_equity"], row["type_bytes"]["E"] / row["bars"])
                self.assertEqual(row["wall_scope"], gate.WALL_SCOPE)
                self.assertTrue(row["finalization_included"])
                self.assertIn("peak_accounted_rss_bytes", row)
                self.assertIn("memory_accounting_error", row)
                if row["kind"] == "backtest" and row["arm"].startswith("v2"):
                    self.assertGreaterEqual(row["finalization_seconds"], FINALIZE_SLEEP_SECONDS)
                    self.assertGreaterEqual(row["wall_seconds"], SLEEP_SECONDS + FINALIZE_SLEEP_SECONDS)
                    self.assertEqual(row["profile"], "COMPACT")
            summary = json.loads((output / "summary.json").read_text())
            self.assertEqual(summary["gate"], "FAIL")
            self.assertTrue(any("memory" in p for p in summary["problems"]))
            self.assertTrue(any("candidate" in p for p in summary["problems"]))
            self.assertIn("GATE FAIL / LARGE", (output / "summary.txt").read_text())
            # Reuse must leave previous evidence untouched.
            before = (output / "summary.json").read_bytes()
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(gate.main(arguments), 2)
            self.assertEqual((output / "summary.json").read_bytes(), before)
            self.assertFalse((output / "failure.json").exists())

    def test_failing_child_and_trade_mismatch_record_failed_rows(self):
        for marker in ("fail-driver", "mismatch-driver"):
            with self.subTest(marker=marker), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                builds = fake_builds(root)
                (Path(builds[0]) / "corpus/validation/fake" / marker).touch()
                artifacts = gate.inspect_builds(builds, ("validation/fake",), {}, "build/lib/libpineforge.a")
                feed = root / "feed.csv"
                feed.write_text("timestamp,open,high,low,close,volume\n1,1,2,1,2,3\n2,1,2,1,2,3\n",
                                encoding="utf-8")
                plan = gate.Plan(builds, (2, 4, 6), ("validation/fake",), levels=(1,))
                args = argparse.Namespace(
                    v1_reader=ROOT / "docker/pf_sink.py", driver=SELF, codec=SELF, keep_artifacts=False)
                output = root / "out"
                output.mkdir()
                # Exercise actual child failures without another full timing
                # campaign. The schedule's subset is deliberate in this test;
                # summary must also reject its missing planned rows.
                chosen = [
                    gate.Job("backtest", builds[0], 2, "validation/fake", arm, 0, True)
                    for arm in ("v1", "v2-default")
                ]
                with patch.object(gate, "schedule", return_value=iter(chosen)), contextlib.redirect_stdout(io.StringIO()):
                    rows = gate.experiment(args, plan, artifacts, {}, {("validation/fake", 2): feed}, output)
                self.assertTrue(any(not row["ok"] for row in rows))
                if marker == "fail-driver":
                    self.assertTrue(all(row["exit_code"] == 7 for row in rows))
                else:
                    self.assertTrue(rows[0]["ok"])
                    self.assertFalse(rows[1]["ok"])
                    self.assertIn("differs", rows[1]["error"])
                self.assertEqual(gate.summarize(plan, rows, iterations=200)["gate"], "FAIL")
                self.assertTrue(all(Path(row["artifact_dir"]).is_dir() for row in rows))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("from-v1", "to-v1"):
        raise SystemExit(fake_codec(sys.argv[1:]))
    unittest.main()
