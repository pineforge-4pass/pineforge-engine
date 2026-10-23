#!/usr/bin/env python3
"""CR-150 arena proof harness. Runtime/corpus modes are Linux LAB ONLY.

Formula tests consume hand-derived vectors. Runtime tests build generated.cpp
and arena_bound_bridge.cpp into one module against an already rebuilt archive.
The corpus path uses run_strategy.main, including its normal input/timeframe/
magnifier/window resolution. Its facts are extracted BEFORE Strategy.run.
"""

from __future__ import annotations

import argparse
import copy
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from arena_bound import (  # noqa: E402
    CHUNK_BYTES, FACTS_SCHEMA, MAX_ARENA_BYTES, MAX_U64, BoundOverflow, FactsError,
    arena_bound, combined_arena_chunks, index_peak, fold_opened_segments,
    ordered_fold_bytes, per_user_bytes, slots,
)
from arena_bound_manifest import (  # noqa: E402
    bound_probe, engine_fingerprint, plan_fingerprint,
    read_compile_facts, read_strategy_facts,
)
from arena_bound_source import inspect_pine_source  # noqa: E402

VECTORS = ROOT / "tests/data/arena_bound_vectors.json"
FIXTURE = ROOT / "tests/fixtures/arena_bound/generated.cpp"
START_MS = 1_736_121_600_000
STEP_MS = 60_000
# The owner brief's uncertified hosting default. COMPUTE owns that production
# constant; it is not defined in this engine checkout.
HOST_DEFAULT_ARENA_BYTES_FROM_BRIEF = 1 << 30


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def command_share_capacity(supplied: int, numerator: int, denominator: int) -> int:
    # Independent expectation: share constants and chunk geometry only.
    return max(CHUNK_BYTES, supplied * numerator // denominator // CHUNK_BYTES * CHUNK_BYTES)


class FormulaTests(unittest.TestCase):
    def setUp(self):
        self.vectors = json.loads(VECTORS.read_text(encoding="utf-8"))["vectors"]

    def test_command_share_one_chunk_floor(self):
        for arena_chunks, store_chunks in ((4, 1), (16, 1), (17, 1), (32, 2),
                                          (1024, 64), (16_384, 1024)):
            with self.subTest(arena_chunks=arena_chunks):
                self.assertEqual(command_share_capacity(arena_chunks * CHUNK_BYTES, 1, 16),
                                 store_chunks * CHUNK_BYTES)

    def test_combined_arena_hand_boundaries(self):
        # b is the ALREADY rounded non-command certificate, in chunks.
        # 15/16/17 straddle the floor regime; 31 -> 35 skips the dip at 32.
        for non_command, combined in (
                (0, 3), (1, 4), (12, 15), (13, 16), (14, 17),
                (15, 18), (16, 19), (17, 20), (28, 31), (29, 35),
                (30, 36), (31, 37), (39, 45), (41, 47), (42, 51),
                (1508, 1853)):
            with self.subTest(non_command=non_command):
                self.assertEqual(combined_arena_chunks(non_command), combined)
                self.assertEqual(combined - 3 * max(1, combined // 16), non_command)

    def test_combined_arena_is_the_first_sufficient_size(self):
        # Enumerate the capacity inequality, independently of the closed form.
        # Checking only n-1 would miss the dips at EVERY multiple of 16.
        previous = 0
        for non_command in range(513):
            first = next(n for n in range(non_command, 2 * non_command + 4)
                         if n - 3 * max(1, n // 16) >= non_command)
            actual = combined_arena_chunks(non_command)
            self.assertEqual(actual, first, f"non-command chunks={non_command}")
            self.assertGreaterEqual(actual, previous)
            previous = actual

    def test_logical_chunk_boundaries_keep_all_fourteen_regions(self):
        # Hand expectations include both quotient/remainder rounding and K.
        for logical, combined in (
                (0, 17), (CHUNK_BYTES - 1, 18), (CHUNK_BYTES, 18),
                (CHUNK_BYTES + 1, 19),
                (14 * CHUNK_BYTES - 1, 31), (14 * CHUNK_BYTES, 31),
                (14 * CHUNK_BYTES + 1, 35),
                (27 * CHUNK_BYTES - 1, 47), (27 * CHUNK_BYTES, 47),
                (27 * CHUNK_BYTES + 1, 51)):
            with self.subTest(logical=logical):
                with mock.patch("arena_bound.per_user_bytes", return_value={"test": logical}):
                    arena = arena_bound(self.vectors[0]["facts"])["terms"]["arena"]
                self.assertFalse(arena["unbounded"])
                self.assertEqual(arena["arenaBytes"], combined * CHUNK_BYTES)

    def test_combined_arena_range_is_checked_after_command_shares(self):
        # M=2**32-1=16*268435455+15. M-3*268435455=3489660930.
        # The next b needs M+4 chunks, although b alone still fits uint32.
        last_non_command = 3_489_660_930
        last_logical = (last_non_command - 14) * CHUNK_BYTES
        for logical, expected in ((last_logical, MAX_ARENA_BYTES),
                                  (last_logical + 1, None)):
            with self.subTest(logical=logical):
                with mock.patch("arena_bound.per_user_bytes", return_value={"test": logical}):
                    arena = arena_bound(self.vectors[0]["facts"])["terms"]["arena"]
                self.assertEqual(arena["arenaBytes"], expected)
                self.assertEqual(arena["unbounded"], expected is None)
                if expected is None:
                    self.assertIn("PLATFORM_RANGE:", arena["reason"])
                    self.assertIn("uint32 chunk", arena["reason"])
        with self.assertRaisesRegex(BoundOverflow, "addition exceeds uint64"):
            combined_arena_chunks(MAX_U64)
        with mock.patch("arena_bound.per_user_bytes", return_value={"test": MAX_U64}):
            arena = arena_bound(self.vectors[0]["facts"])["terms"]["arena"]
        self.assertIsNone(arena["arenaBytes"])
        self.assertIn("PLATFORM_RANGE: multiplication exceeds uint64", arena["reason"])

    def test_million_point_half_size_remains_a_negative_witness(self):
        vector = next(row for row in self.vectors
                      if row["name"] == "million-point-negative-control-plan")
        certified = arena_bound(vector["facts"])["terms"]["arena"]["arenaBytes"]
        # B=24*C+1222; b=25+14=39; q=2; n=39+6=45.
        self.assertEqual(certified, 47_185_920)
        self.assertEqual(certified % CHUNK_BYTES, 0)
        self.assertEqual(certified - 3 * command_share_capacity(certified, 1, 16),
                         39 * CHUNK_BYTES)
        half = certified // 2
        supplied = half - half % CHUNK_BYTES
        self.assertEqual(supplied, 22 * CHUNK_BYTES)
        self.assertLess(supplied, 25_165_824)  # Actual 24*C curve from the lab brief.

    def test_runtime_fixture_certificates_hand_derived(self):
        # Current fixture_facts, independently summed by hand. Columns are
        # case, bars, logical B, old non-command chunks, combined chunks.
        # The command capacity expectation uses only arena/share geometry.
        for which, bars, logical, non_command, combined in (
                (1, 32, 1_898_366_958, 1825, 2245),
                (2, 512, 1_111_429_870, 1074, 1320),
                (3, 32, 202_251_294, 207, 252),
                (4, 64, 967_682_798, 937, 1150),
                (5, 32, 1_898_366_958, 1825, 2245),
                (6, 512, 4_207_814, 19, 22),
                (7, 384, 631_186_438, 616, 757),
                (0, 1_048_576, 25_167_046, 39, 45)):
            with self.subTest(which=which):
                facts = {
                    "schema": FACTS_SCHEMA,
                    "data": {"scriptBars": bars, "selected": int(which == 7),
                             "windowBars": 256 if which == 7 else bars,
                             "source": "hand runtime fixture plan"},
                    "strategy": fixture_facts(which, bars),
                }
                arena = arena_bound(facts)["terms"]["arena"]
                self.assertFalse(arena["unbounded"])
                self.assertEqual(sum(per_user_bytes(arena["inputs"]).values()), logical)
                self.assertEqual(arena["arenaBytes"], combined * CHUNK_BYTES)
                self.assertEqual(
                    arena["arenaBytes"]
                    - 3 * command_share_capacity(arena["arenaBytes"], 1, 16),
                    non_command * CHUNK_BYTES)

    def test_hand_vectors_and_exact_a5_wider_shape(self):
        for vector in self.vectors:
            with self.subTest(vector=vector["name"]):
                actual = arena_bound(vector["facts"])
                self.assertEqual(actual, vector["expected"])
                arena = actual["terms"]["arena"]
                self.assertEqual(set(arena["inputs"]), set("TDQLRE"))
                self.assertEqual(set(actual),
                                 {"terms", "unbounded", "unboundedTerms", "reason"})
                self.assertEqual(set(actual["terms"]),
                                 {"arena", "ohlcv", "finalizerStream", "childPrivate"})
                self.assertNotIn("childPrivate", actual["unboundedTerms"])
                self.assertNotIn("childPrivate", actual["reason"])
                self.assertEqual(arena["bytes"], arena["arenaBytes"])
                if not arena["unbounded"]:
                    self.assertEqual(sum(per_user_bytes(arena["inputs"]).values()),
                                     vector["logicalBytes"])
                    non_command = ((vector["logicalBytes"] // CHUNK_BYTES
                                    + int(vector["logicalBytes"] % CHUNK_BYTES != 0) + 14)
                                   * CHUNK_BYTES)
                    self.assertEqual(
                        arena["arenaBytes"]
                        - 3 * command_share_capacity(arena["arenaBytes"], 1, 16),
                        non_command)

    def test_rehash_startup_boundary_and_growth(self):
        # Independent index sizes: 64 slots through key 32, then new 128 +
        # old 64 slots at key 33. Key 65 needs new 256 + old 128 slots.
        for keys, count, peak in ((0, 0, 0), (1, 64, 1024), (32, 64, 1024),
                                  (33, 128, 3072), (64, 128, 3072),
                                  (65, 256, 6144)):
            self.assertEqual(slots(keys), count)
            self.assertEqual(index_peak(keys), peak)

    def test_current_store_inventory_and_fold_record_derivation(self):
        inputs = {name: {"value": value}
                  for name, value in zip("TDQLRE", (2, 2, 4, 2, 2, 2))}
        peaks = per_user_bytes(inputs)
        self.assertEqual(set(peaks), {
            "equity", "positionTail", "cohorts", "exitRank",
            "erasedData", "erasedIndex", "consumedData", "consumedIndex",
            "orderedFold", "successorRoots", "openings",
        })
        self.assertEqual(len(peaks), 11)  # Ten contributions and one explicit exclusion.
        self.assertNotIn("sequenceData", peaks)
        self.assertNotIn("sequenceIndex", peaks)
        self.assertEqual(peaks["openings"], 0)  # Stock source owns vectors.
        # Four ever-keys: <=4 leaves, <=3 binary branches, <=4 saved nodes.
        self.assertEqual(peaks["orderedFold"], 136 * 4 + 2088 * 3 + 2096 * 4)
        self.assertEqual(sum(peaks.values()), 18146)
        self.assertEqual(ordered_fold_bytes(0, 4097), 0)
        self.assertEqual(ordered_fold_bytes(1, 0), 2232)
        self.assertEqual(ordered_fold_bytes(16, 0), 67032)
        self.assertEqual(ordered_fold_bytes(17, 0), 71352)
        # With L=0 the joined key is one byte; <=18 branch snapshots
        # plus the final leaf. This pins 2L+1 and the extra saved leaf.
        self.assertEqual(ordered_fold_bytes(32, 0), 108904)
        # Distinct-ever membership, not live count, feeds Q. Erase does not
        # reduce any of the three physical high-water marks.
        self.assertGreater(ordered_fold_bytes(33, 64), ordered_fold_bytes(32, 64))

    def test_fold_segment_rule_at_each_record_size(self):
        for record, at_64 in ((136, 3), (2088, 64), (2096, 64)):
            per_segment = 4095 // record
            self.assertEqual(fold_opened_segments(0, record, 4095), 0)
            self.assertEqual(fold_opened_segments(1, record, 4095), 1)
            self.assertEqual(fold_opened_segments(per_segment, record, 4095), 1)
            self.assertEqual(fold_opened_segments(per_segment + 1, record, 4095), 2)
            self.assertEqual(fold_opened_segments(64, record, 4095), at_64)
        with self.assertRaises(FactsError):
            fold_opened_segments(1, 2096, 2095)

    def test_no_float_bool_negative_missing_or_guessed_bounds(self):
        seed = self.vectors[1]["facts"]
        for invalid in (True, -1, 1.0, "100"):
            facts = copy.deepcopy(seed)
            facts["strategy"]["fills"]["perBar"] = invalid
            with self.assertRaises(FactsError):
                arena_bound(facts)
        facts = copy.deepcopy(seed)
        del facts["strategy"]["successors"]["perBar"]
        missing = arena_bound(facts)["terms"]["arena"]
        self.assertTrue(missing["unbounded"])
        self.assertIsNone(missing["arenaBytes"])
        self.assertIn("successors.perBar", missing["reason"])
        facts = copy.deepcopy(seed)
        facts["strategy"]["ids"]["literals"] = ["é"]
        facts["strategy"]["ids"]["maxBytes"] = 1
        with self.assertRaises(FactsError):
            arena_bound(facts)
        facts = copy.deepcopy(seed)
        facts["data"]["windowBars"] = facts["data"]["scriptBars"] + 1
        with self.assertRaises(FactsError):
            arena_bound(facts)

    def test_overflow_does_not_clamp_or_size_to_a_finite_guess(self):
        facts = copy.deepcopy(self.vectors[1]["facts"])
        facts["strategy"]["fills"]["perBar"] = MAX_U64
        result = arena_bound(facts)["terms"]["arena"]
        self.assertTrue(result["unbounded"])
        self.assertIsNone(result["arenaBytes"])
        self.assertTrue(result["reason"].startswith("PLATFORM_RANGE:"))
        # Below u64, beyond the actual arena's uint32 chunk representation.
        facts = copy.deepcopy(self.vectors[1]["facts"])
        facts["strategy"]["fills"]["fixed"] = 1 << 50
        result = arena_bound(facts)["terms"]["arena"]
        self.assertTrue(result["unbounded"])
        self.assertIsNone(result["arenaBytes"])
        self.assertIn("uint32 chunk", result["reason"])

    def test_monotone_growth_and_selected_curve(self):
        facts = copy.deepcopy(self.vectors[1]["facts"])
        before = arena_bound(facts)["terms"]["arena"]
        for group, key in (("fills", "fixed"), ("ids", "fixedFresh"),
                           ("ids", "maxBytes"), ("pairKeys", "fixed"),
                           ("successors", "fixed")):
            larger = copy.deepcopy(facts)
            larger["strategy"][group][key] += 100_000
            after = arena_bound(larger)["terms"]["arena"]
            self.assertFalse(after["unbounded"])
            self.assertGreaterEqual(after["arenaBytes"], before["arenaBytes"])
        self.assertEqual(before["inputs"]["E"]["value"], 2001)
        facts["data"]["selected"] = 0
        self.assertEqual(arena_bound(facts)["terms"]["arena"]["inputs"]["E"]["value"], 10000)

    def test_missing_and_stale_sidecar_fail_before_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            probe = Path(temporary)
            result, verdict = bound_probe(
                probe, {"scriptBars": 1, "selected": 0, "windowBars": 1,
                        "source": "test input plan"})
            self.assertEqual(verdict, "UNCLASSIFIED")
            self.assertTrue(result["unbounded"])
            self.assertIsNone(result["terms"]["arena"]["arenaBytes"])
            (probe / "generated.cpp").write_text("source A", encoding="utf-8")
            write_sidecar(probe, self.vectors[1]["facts"]["strategy"])
            (probe / "generated.cpp").write_text("source B", encoding="utf-8")
            with self.assertRaises(FactsError):
                read_strategy_facts(probe)

    def test_exact_plan_binding_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            probe = Path(temporary)
            (probe / "generated.cpp").write_text("bound input", encoding="utf-8")
            write_sidecar(probe, self.vectors[1]["facts"]["strategy"],
                          scope={"kind": "plan", "sha256": "0" * 64})
            data = {"scriptBars": 1, "selected": 0, "windowBars": 1,
                    "source": "pre-start plan", "planSha256": "1" * 64}
            wrong, verdict = bound_probe(probe, data)
            self.assertEqual(verdict, "UNCLASSIFIED")
            self.assertTrue(wrong["terms"]["arena"]["unbounded"])
            data["planSha256"] = "0" * 64
            right, verdict = bound_probe(probe, data)
            self.assertEqual(verdict, "BOUNDED")
            self.assertFalse(right["terms"]["arena"]["unbounded"])

    def test_census_enumerates_the_pinned_population_once(self):
        baseline = (ROOT / "scripts/corpus_parity_baseline.txt").read_text()
        pinned = set(re.findall(r"validation/([^/]+)/engine_trades\.csv", baseline))
        census = (ROOT / "docs/design/arena-bound-corpus-census.md").read_text()
        table = census.split("## Every known probe", 1)[1]
        listed = re.findall(r"^\| ([a-z0-9][a-z0-9-]*) \|", table, re.MULTILINE)
        self.assertEqual(len(listed), len(set(listed)))
        self.assertEqual(set(listed), pinned)

    def test_source_detector_distinguishes_infinite_from_unknown(self):
        data = {"scriptBars": 2, "selected": 0, "windowBars": 2, "source": "input plan"}
        with tempfile.TemporaryDirectory() as temporary:
            probe = Path(temporary)
            for filename, expected in (("unbounded_ids.pine", "UNBOUNDED"),
                                       ("unknown_trip_count.pine", "UNCLASSIFIED")):
                source = ROOT / "tests/fixtures/arena_bound" / filename
                shutil.copyfile(source, probe / "strategy.pine")
                bound, verdict = bound_probe(probe, data)
                self.assertEqual(verdict, expected)
                self.assertTrue(bound["unbounded"])
                self.assertIsNone(bound["terms"]["arena"]["arenaBytes"])
                inventory = inspect_pine_source(source.read_text(), filename)
                self.assertTrue(inventory["orderCallSites"])
                self.assertTrue(inventory["computedIdArguments"])
                self.assertTrue(inventory["loops"])
        inventory = inspect_pine_source(
            '// strategy.entry("comment", strategy.long)\n'
            'f() =>\n    strategy.entry("literal", strategy.long)\n'
            'for i = 0 to 7\n    f()\n', "inventory test")
        self.assertEqual(len(inventory["orderCallSites"]), 1)
        self.assertEqual(inventory["literalIds"], ["literal"])
        self.assertEqual(inventory["loops"][0]["constantTrips"], 8)
        # One syntactic site invoked eight times is NOT a one-order bound.
        self.assertEqual(inventory["controlFlow"]["status"], "unknown")
        recursion = inspect_pine_source("first() => second()\nsecond() => first()\n",
                                       "recursive source")
        self.assertEqual(recursion["recursiveFunctions"], ["first", "second"])

    def test_rss_measurement_does_not_invent_an_as_bound(self):
        vector = next(row for row in self.vectors
                      if row["name"] == "all-terms-and-separate-feed-pages")
        facts = copy.deepcopy(vector["facts"])
        facts["childPrivate"] = {
            "rssBytes": 46_200_000, "asBytes": None, "measured": True,
            "rssCertified": False, "asCertified": False,
            "source": "pre-flat RSS observation; not an applicable bound",
        }
        result = arena_bound(facts)
        term = result["terms"]["childPrivate"]
        self.assertEqual(term["rssBytes"], 46_200_000)
        self.assertIsNone(term["asBytes"])
        self.assertTrue(term["measured"])
        self.assertTrue(term["unbounded"])
        self.assertFalse(result["unbounded"])
        self.assertEqual(result["unboundedTerms"], [])
        self.assertEqual(result["reason"], "")

    def test_private_proof_flags_and_overflow_are_informational(self):
        vector = next(row for row in self.vectors
                      if row["name"] == "all-terms-and-separate-feed-pages")
        # This vector has finite arena/OHLCV/stream and no private proof.
        missing = arena_bound(vector["facts"])
        self.assertTrue(missing["terms"]["childPrivate"]["unbounded"])
        self.assertIsNone(missing["terms"]["childPrivate"]["rssBytes"])
        self.assertIsNone(missing["terms"]["childPrivate"]["asBytes"])
        self.assertFalse(missing["terms"]["childPrivate"]["measured"])
        self.assertFalse(missing["unbounded"])
        self.assertEqual(missing["unboundedTerms"], [])
        self.assertEqual(missing["reason"], "")
        toy = {"rssBytes": 4096, "asBytes": 8192, "measured": False,
               "rssCertified": True, "asCertified": True,
               "source": "hand toy private-memory proof; not an engine measurement"}
        for changes, private_unbounded in (
                ({}, False),
                ({"rssCertified": False}, True),
                ({"asCertified": False}, True),
                ({"rssBytes": None}, True),
                ({"asBytes": None}, True),
                ({"asBytes": MAX_U64 + 1}, True)):
            with self.subTest(changes=changes):
                facts = copy.deepcopy(vector["facts"])
                facts["childPrivate"] = {**toy, **changes}
                result = arena_bound(facts)
                self.assertEqual(result["terms"]["childPrivate"]["unbounded"],
                                 private_unbounded)
                self.assertFalse(result["unbounded"])
                self.assertEqual(result["unboundedTerms"], [])
                self.assertEqual(result["reason"], "")

    def test_each_sizing_term_still_controls_the_top_level_verdict(self):
        vector = next(row for row in self.vectors
                      if row["name"] == "all-terms-and-separate-feed-pages")
        for term in ("arena", "ohlcv", "finalizerStream"):
            with self.subTest(term=term):
                facts = copy.deepcopy(vector["facts"])
                if term == "arena":
                    # T/E/L remain certified, so the stream stays finite.
                    del facts["strategy"]["successors"]["perBar"]
                else:
                    del facts["ohlcv" if term == "ohlcv" else "stream"]
                result = arena_bound(facts)
                self.assertTrue(result["terms"]["childPrivate"]["unbounded"])
                self.assertTrue(result["unbounded"])
                self.assertEqual(result["unboundedTerms"], [term])
                self.assertEqual(result["reason"], "no certified finite bound for: " + term)

    def test_stream_line_ceiling_and_missing_count_are_different(self):
        vector = next(row for row in self.vectors
                      if row["name"] == "all-terms-and-separate-feed-pages")
        facts = copy.deepcopy(vector["facts"])
        del facts["stream"]["commentBytes"]
        capped = arena_bound(facts)["terms"]["finalizerStream"]
        self.assertFalse(capped["unbounded"])
        self.assertEqual(capped["derivation"]["maxLineBytes"]["T"], 1_048_576)
        del facts["stream"]["eventsPerBar"]
        unknown = arena_bound(facts)
        self.assertTrue(unknown["terms"]["finalizerStream"]["unbounded"])
        self.assertIn("finalizerStream", unknown["unboundedTerms"])
        self.assertFalse(unknown["terms"]["arena"]["unbounded"])


def fingerprint(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def write_sidecar(probe: Path, strategy: dict, *, scope=None, stream=None) -> None:
    """Fixture's independently reviewed static proof, never a run's output."""
    document = {
        "schema": FACTS_SCHEMA,
        "producer": "CR150-L22u reviewed generated-code fixture",
        "engineSha256": engine_fingerprint(),
        "scope": scope if scope is not None else {"kind": "all-data"},
        "sha256": {name: fingerprint(probe / name)
                   for name in ("generated.cpp", "strategy.pine", "inputs.json")},
        "strategy": strategy,
    }
    if stream is not None:
        document["stream"] = stream
    (probe / "arena-facts.json").write_text(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def compile_module(build: Path, generated: Path, destination: Path) -> Path:
    """Use this build's compiler/options; do not rebuild or mutate the archive."""
    archive = build / "lib/libpineforge.a"
    check(archive.is_file(), f"missing rebuilt archive: {archive}")
    newest = max(path.stat().st_mtime_ns
                 for directory in (ROOT / "src", ROOT / "include")
                 for path in directory.rglob("*") if path.is_file())
    newest = max(newest, (ROOT / "CMakeLists.txt").stat().st_mtime_ns)
    check(archive.stat().st_mtime_ns > newest,
          "stale libpineforge.a; perform a FULL all-target build before this test")
    commands_path = build / "compile_commands.json"
    check(commands_path.is_file(), "configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
    commands = json.loads(commands_path.read_text())
    record = next(row for row in commands
                  if Path(row["file"]).resolve() == ROOT / "src/c_abi.cpp"
                  and "pineforge.dir" in row.get("command", " ".join(row.get("arguments", []))))
    original = record.get("arguments") or shlex.split(record["command"])
    command = []
    skip = False
    source = Path(record["file"]).resolve()
    for word in original:
        if skip:
            skip = False
            continue
        if word in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif word in ("-c", "-MD", "-MMD", "-MP", "-fvisibility=hidden",
                      "-fvisibility-inlines-hidden"):
            continue
        elif Path(word).is_absolute() and Path(word).resolve() == source:
            continue
        elif word == record["file"]:
            continue
        else:
            command.append(word)
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Whole archive is the corpus/tutorial's link rule: ctypes dlsym alone
    # would not pull the C ABI object out of an ordinary static archive.
    command += [
        "-shared", "-fPIC", "-Wno-unused-parameter",
        str(generated), str(ROOT / "tests/arena_bound_bridge.cpp"),
        "-Wl,--whole-archive", str(archive), "-Wl,--no-whole-archive",
        "-Wl,--wrap=pwrite", "-Wl,--wrap=write", "-Wl,--wrap=ftruncate",
        "-Wl,--wrap=mkstemp", "-Wl,--wrap=mkostemp",
        "-Wl,--wrap=open", "-Wl,--wrap=openat",
        "-pthread", "-o", str(destination),
    ]
    completed = subprocess.run(command, cwd=record["directory"],
                               text=True, capture_output=True, check=False)
    (destination.parent / (destination.stem + "-compile.log")).write_text(
        shlex.join(command) + "\n" + completed.stdout + completed.stderr)
    check(completed.returncode == 0,
          f"driver compile failed; see {destination.parent}/{destination.stem}-compile.log")
    return destination


def data_before_run(runner, bars_csv: Path, kwargs: dict,
                    selected_window: tuple[int, int] | None) -> dict:
    """Count only the loader's input, before strategy_create or run_backtest."""
    loaded = kwargs.get("preloaded_bars")
    if loaded is None:
        bars, count, _ = runner._load_bars(
            bars_csv, ohlcv_start_ms=kwargs.get("ohlcv_start_ms"),
            ohlcv_end_ms=kwargs.get("ohlcv_end_ms"))
    else:
        bars, count = loaded
    tail = kwargs.get("ohlcv_tail_bar")
    # Charging one extra bar even if the loader will not append it is safe.
    script_bound = count + int(tail is not None)
    input_tf = str(kwargs.get("input_tf") or "")
    script_tf = str(kwargs.get("script_tf") or input_tf)
    if script_tf and script_tf != input_tf:
        input_seconds = runner._tf_seconds(input_tf)
        if input_seconds == 0 and count >= 2:
            input_seconds = (int(bars[1].timestamp) - int(bars[0].timestamp)) // 1000
        script_seconds = runner._tf_seconds(script_tf)
        # A script bucket contains an input bar; no empty bucket emits a
        # script calculation on this historical route. Month buckets group
        # intraday inputs too. Unsupported relationships need the real planner.
        groups_input = (input_seconds > 0 and script_seconds >= input_seconds)
        groups_input |= (script_seconds == -1 and 0 < input_seconds <= 86400)
        check(groups_input, "UNCERTIFIED: cannot prove scriptBars <= loaded bars; "
              "supply a data planner proof, never assume S=N for this timeframe")
    window_count = script_bound
    if selected_window is not None:
        # The fixture's same-timeframe tape has exact pre-start labels.
        check(input_tf == script_tf == "1" and tail is None,
              "selected fixture needs its exact one-minute input plan")
        start, end = selected_window
        window_count = sum(start <= int(bars[i].timestamp) < end for i in range(count))
    settings = {key: value for key, value in kwargs.items()
                if key not in ("preloaded_bars", "on_report")}
    settings["selectedWindow"] = selected_window
    settings["effectiveInputTf"] = input_tf
    settings["effectiveScriptTf"] = script_tf
    settings["inputCount"] = count
    with memoryview(bars).cast("B") as view:
        plan_sha256 = plan_fingerprint(view, settings)
    return {
        "scriptBars": script_bound,
        "selected": int(selected_window is not None),
        "windowBars": window_count,
        "planSha256": plan_sha256,
        "source": (f"pre-start _load_bars({bars_csv}); input_tf={input_tf!r}; "
                   f"script_tf={script_tf!r}; selected labels counted before run"),
    }


class SelectedWindow(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("version", ctypes.c_uint32),
                ("start_ms", ctypes.c_int64), ("end_ms", ctypes.c_int64)]


def measured_class(runner, probe: Path, module: Path, arena_directory: Path,
                   measurements: list, selected_window=None, half=False,
                   command_first=False, observe_commands=False, supplied_bytes=None,
                   uncertified_default=False):
    base = runner.Strategy
    watch_commands = command_first or observe_commands

    class MeasuredStrategy(base):
        def __init__(self, _unused):
            super().__init__(module)
            lib = self.lib
            lib.arena_bound_test_attach.argtypes = [ctypes.c_int, ctypes.c_uint64]
            lib.arena_bound_test_attach.restype = ctypes.c_int
            lib.arena_bound_test_detach.argtypes = []
            lib.arena_bound_test_detach.restype = ctypes.c_int
            lib.arena_bound_test_error.argtypes = []
            lib.arena_bound_test_error.restype = ctypes.c_char_p
            lib.arena_bound_test_creation_probe.argtypes = []
            lib.arena_bound_test_creation_probe.restype = ctypes.c_int
            for name in ("high_water", "bytes_used", "legacy_writes", "legacy_creations"):
                function = getattr(lib, "arena_bound_test_" + name)
                function.argtypes = []
                function.restype = ctypes.c_uint64
            if watch_commands:
                lib.arena_bound_test_watch_commands.argtypes = [ctypes.c_uint64]
                lib.arena_bound_test_watch_commands.restype = ctypes.c_int
                for name in ("failures", "needed", "failed_extent", "peak_extent", "capacity",
                             "share_numerator", "share_denominator", "before_used", "before_total",
                             "before_supplied",
                             "failed_before_used", "failed_before_high_water", "failed_high_water",
                             "failed_supplied", "failed_used", "failed_free"):
                    function = getattr(lib, "arena_bound_test_command_" + name)
                    function.argtypes = []
                    function.restype = ctypes.c_uint64
            self.fixture_counts = {}
            self.fixture_trades = 0
            original_create = lib.strategy_create
            if selected_window is not None:
                lib.strategy_set_selected_window_v1.argtypes = [
                    ctypes.c_void_p, ctypes.POINTER(SelectedWindow)]
                lib.strategy_set_selected_window_v1.restype = ctypes.c_int

                def create_selected(params):
                    state = original_create(params)
                    config = SelectedWindow(ctypes.sizeof(SelectedWindow), 1,
                                            *selected_window)
                    check(lib.strategy_set_selected_window_v1(state, ctypes.byref(config)) == 0,
                          "selected-window configuration refused")
                    return state

                lib.strategy_create = create_selected
            original_free = lib.strategy_free

            def observe_then_free(state):
                try:
                    for name in ("entries", "exits", "roots", "peak_lots"):
                        function = getattr(lib, "arena_bound_fixture_" + name, None)
                        if function is not None:
                            function.argtypes = [ctypes.c_void_p]
                            function.restype = ctypes.c_uint64
                            self.fixture_counts[name] = int(function(state))
                finally:
                    original_free(state)

            lib.strategy_free = observe_then_free

        def run(self, bars_csv, params=None, **kwargs):
            if kwargs.get("preloaded_bars") is None:
                bars, count, _ = runner._load_bars(
                    bars_csv, ohlcv_start_ms=kwargs.get("ohlcv_start_ms"),
                    ohlcv_end_ms=kwargs.get("ohlcv_end_ms"))
                # The execution receives exactly the immutable array counted
                # and fingerprinted here; no second CSV read can change it.
                kwargs["preloaded_bars"] = (bars, count)
            data = data_before_run(runner, bars_csv, kwargs, selected_window)
            inputs_file = probe / "inputs.json"
            if inputs_file.is_file():
                check((params or {}) == json.loads(inputs_file.read_text()),
                      "runtime parameters differ from the fingerprinted inputs.json")
            all_terms, verdict = bound_probe(probe, data)
            bound = all_terms["terms"]["arena"]
            if uncertified_default:
                check(bound["unbounded"] and supplied_bytes is None and not half,
                      "default-arena witness must really lack a certified arena bound")
            else:
                check(verdict == "BOUNDED" and not bound["unbounded"],
                      f"{probe.name}: {bound.get('reason', verdict)}")
            certified = bound["arenaBytes"]
            supplied = (HOST_DEFAULT_ARENA_BYTES_FROM_BRIEF if uncertified_default
                        else supplied_bytes if supplied_bytes is not None
                        else certified // 2 if half else certified)
            # Whole-chunk undersizing avoids an unallocatable fractional tail.
            supplied -= supplied % CHUNK_BYTES
            arena_directory.mkdir(parents=True, exist_ok=True)
            fd, backing = tempfile.mkstemp(prefix="arena-", dir=arena_directory)
            attached = False
            try:
                # Wrapper-side reservation happens before strategy_create.
                os.posix_fallocate(fd, 0, supplied)
                os.unlink(backing)
                check(self.lib.arena_bound_test_attach(fd, supplied) == 0,
                      f"arena attach: {self.lib.arena_bound_test_error()!r}")
                attached = True
                if watch_commands:
                    # The 32-bar tape's complete curve is an independent
                    # report first-write allowance (two doubles per row).
                    # The failed command observer and store identity below
                    # additionally reject exhaustion in any auxiliary user.
                    curve_bytes = data["windowBars"] * 2 * ctypes.sizeof(ctypes.c_double)
                    report_first_bytes = ((curve_bytes + CHUNK_BYTES - 1) // CHUNK_BYTES
                                          * CHUNK_BYTES)
                    check(self.lib.arena_bound_test_watch_commands(report_first_bytes) == 0,
                          f"command observation setup: {self.lib.arena_bound_test_error()!r}")
                    before_total = int(self.lib.arena_bound_test_command_before_total())
                    before_used = int(self.lib.arena_bound_test_command_before_used())
                    stats_supplied = int(self.lib.arena_bound_test_command_before_supplied())
                    numerator = int(self.lib.arena_bound_test_command_share_numerator())
                    denominator = int(self.lib.arena_bound_test_command_share_denominator())
                    capacity = command_share_capacity(stats_supplied, numerator, denominator)
                    check(stats_supplied == supplied,
                          "attached arena stats differ from the host-supplied arena size")
                    check(before_total - before_used >= report_first_bytes,
                          "report first-write allowance is absent before the command witness")
                    check(self.lib.arena_bound_test_command_capacity() == capacity,
                          "opened command store share differs from supplied arena stats")
                check(self.lib.arena_bound_test_creation_probe() == 0,
                      "creation guard must refuse and count empty mkstemp/open/openat files")
                print(f"arena_bound: begin probe={probe.name} params={params!r} "
                      f"half={half} supplied={supplied} certified={certified}",
                      file=sys.stderr, flush=True)
                try:
                    report = super().run(bars_csv, params=params, **kwargs)
                except RuntimeError as error:
                    print(f"arena_bound: failed probe={probe.name} params={params!r} "
                          f"half={half} code={getattr(error, 'run_failure_code', None)!r} "
                          f"args={getattr(error, 'run_failure_args', None)!r} "
                          f"fixture_counts={self.fixture_counts!r} "
                          f"high_water={int(self.lib.arena_bound_test_high_water())} "
                          f"bytes_used={int(self.lib.arena_bound_test_bytes_used())} "
                          f"legacy_writes={int(self.lib.arena_bound_test_legacy_writes())} "
                          f"legacy_creations={int(self.lib.arena_bound_test_legacy_creations())} "
                          f"error={error!r}",
                          file=sys.stderr, flush=True)
                    if command_first:
                        check(getattr(error, "run_failure_code", None) == "report_sink_failed",
                              f"command arena failure lost its typed code: {error!r}")
                        check("scratch arena exhausted" in str(error),
                              "command witness must fail at arena exhaustion")
                        raw_args = getattr(error, "run_failure_args", None)
                        check(isinstance(raw_args, str),
                              f"command arena failure has no canonical JSON arguments: {raw_args!r}")
                        args = json.loads(raw_args)
                        check(isinstance(args, dict) and args.get("reason") == "arena_exhausted",
                              f"command arena failure lost its typed arguments: {args!r}")
                        check(args.get("store") == "command",
                              f"a report user exhausted before the command store: {args!r}")
                        check(self.lib.arena_bound_test_command_failures() > 0,
                              "no command backing returned io_error")
                        extent = int(self.lib.arena_bound_test_command_failed_extent())
                        store_need = ((extent + CHUNK_BYTES - 1) // CHUNK_BYTES * CHUNK_BYTES)
                        needed = min((1 << 63) - 1,
                                     (store_need * denominator + numerator - 1) // numerator)
                        check(self.lib.arena_bound_test_command_needed() == needed,
                              "bridge demand differs from independent extent/share arithmetic")
                        snapshot = {
                            "high_water_bytes": int(
                                self.lib.arena_bound_test_command_failed_high_water()),
                            "arena_bytes": int(self.lib.arena_bound_test_command_failed_supplied()),
                            "claimed_bytes": int(self.lib.arena_bound_test_command_failed_used()),
                            "needed_bytes_lower_bound": needed,
                        }
                        for name, expected in snapshot.items():
                            check(type(args.get(name)) is int and args[name] == expected,
                                  f"{name}: typed argument {args.get(name)!r} != "
                                  f"post-failure arena snapshot {expected}")
                        check(store_need > capacity
                              and self.lib.arena_bound_test_command_failed_free() > 0,
                              "a command store must exhaust its share before the arena fills")
                        check(snapshot["claimed_bytes"]
                              == self.lib.arena_bound_test_command_failed_before_used()
                              and snapshot["high_water_bytes"]
                              == self.lib.arena_bound_test_command_failed_before_high_water(),
                              "share refusal changed the arena's claims or high-water")
                        check(self.lib.arena_bound_test_legacy_writes() == 0,
                              "command exhaustion touched legacy private scratch")
                        measurements.append({
                            "probe": probe.name, "negative": True, "commandFirst": True,
                            "arenaBytes": certified, "supplied": supplied,
                            "beforeUsed": before_used, "beforeTotal": before_total,
                            "snapshot": snapshot, "legacyWrites": 0, "capacity": capacity,
                            "requestedExtent": extent,
                        })
                        return None
                    if not half:
                        raise
                    check(getattr(error, "run_failure_code", None) == "report_sink_failed",
                          f"undersized arena failed with the wrong type: {error!r}")
                    check("scratch arena exhausted" in str(error),
                          "undersizing must fail at arena capacity, not another I/O fault")
                    measurements.append({"probe": probe.name, "negative": True,
                                         "arenaBytes": certified, "supplied": supplied})
                    return None
                check(not half, "bound/2 unexpectedly completed; negative witness is ineffective")
                check(not command_first,
                      "command-share witness unexpectedly completed")
                actual = int(self.lib.arena_bound_test_high_water())
                used = int(self.lib.arena_bound_test_bytes_used())
                check(self.lib.arena_bound_test_legacy_writes() == 0,
                      "a physical scratch user bypassed ScratchFile/arena")
                check(self.lib.arena_bound_test_legacy_creations() == 0,
                      "a scratch user attempted file creation while an arena was attached")
                check(0 <= used <= actual <= supplied
                      and (certified is None or actual <= certified),
                      f"{probe.name}: actual={actual}, used={used}, bound={certified}")
                stream = all_terms["terms"]["finalizerStream"]
                if not stream["unbounded"]:
                    stream_actual = Path(report.sink.path).stat().st_size
                    check(stream_actual <= stream["bytes"],
                          f"{probe.name}: sink stream exceeds its pre-start U_bound")
                # A curve large enough to spill is an actual-path witness.
                if data["windowBars"] > 6000:
                    check(actual >= CHUNK_BYTES,
                          "large curve used no arena; a legacy path was measured")
                self.fixture_trades = int(report.get("total_trades", 0))
                ceiling = supplied if certified is None else certified
                row = {"probe": probe.name, "actual": actual, "arenaBytes": ceiling,
                       "inputs": bound["inputs"],
                       "unboundedTerms": all_terms["unboundedTerms"],
                       "childPrivate": all_terms["terms"]["childPrivate"]}
                if watch_commands:
                    row.update(commandPeakExtent=int(
                        self.lib.arena_bound_test_command_peak_extent()),
                        commandCapacity=capacity, shareNumerator=numerator,
                        shareDenominator=denominator, supplied=stats_supplied,
                        uncertified=uncertified_default)
                measurements.append(row)
                print(f"arena_bound: probe={probe.name} actual={actual} bound={ceiling} "
                      f"actual/bound={actual}/{ceiling} "
                      f"ratio={actual / ceiling:.9f}", flush=True)
                return report
            finally:
                if attached:
                    check(self.lib.arena_bound_test_detach() == 0,
                          f"arena detach: {self.lib.arena_bound_test_error()!r}")
                os.close(fd)
                Path(backing).unlink(missing_ok=True)

    return MeasuredStrategy


def fixture_facts(which: int, bars: int) -> dict:
    """Reviewed static schedule proof; see the derivation in arena-bound.md.

    These are bounds on code paths/finite tape, not observed counts. The full
    source closure, including synthetic IDs, is deliberately overcounted.
    """
    source = "tests/fixtures/arena_bound/generated.cpp static schedule and bounded tape"
    control = {"status": "bounded", "source": source}
    if which in (0, 6):
        return {
            "controlFlow": control,
            "fills": {"perBar": 0, "fixed": 0, "rowsPerFill": 0,
                      "fixedRows": 0, "source": source},
            "ids": {"literals": [], "freshPerBar": 0, "fixedFresh": 0,
                    "maxBytes": 0, "source": source},
            "pairKeys": {"mode": "opportunities", "perBar": 0, "fixed": 0,
                         "source": source},
            "successors": {"perBar": 512 if which == 6 else 0, "fixed": 0,
                           "source": source},
        }
    width = 8 if which in (1, 5) else (4 if which == 4 else 1)
    # Each of three partial brackets has at most three legs per opening.
    # Charge each of the four modeled OHLC points a full set, plus market
    # opens/closes. There is no COOF; even ALL historical entry attempts have
    # notional <= 102*16*width*bars on the bounded tape, far below 1e12.
    fills = 4 * (width + 9 * width * width + 1)
    # Total entry calls is a universal lot bound even for same-bar admission
    # quirks; unlike pyramiding alone it is safe for this fixture schedule.
    lots = width * bars
    return {
        "controlFlow": control,
        "fills": {"perBar": fills, "fixed": 0, "rowsPerFill": lots,
                  "fixedRows": lots, "source": source},
        "ids": {"literals": ["__close__", "__pine_close_all", "__margin_call__",
                             "__intraday_loss__", "__intraday_cap_close__"],
                "freshPerBar": 8 * width, "fixedFresh": 8,
                "maxBytes": 4160 if which == 3 else 64, "source": source},
        "pairKeys": {"mode": "opportunities", "perBar": 16 * width * width,
                     "fixed": 0, "source": source},
        "successors": {"perBar": 4 * fills, "fixed": 0, "source": source},
    }


def command_share_runs(runner, probe: Path, shared: Path, arena_directory: Path,
                       measurements: list, bars, count: int) -> None:
    """Same input at two smaller placements and an independent default arena."""
    params = {"case": 1}
    kwargs = {"preloaded_bars": (bars, count), "input_tf": "1", "script_tf": "1"}
    # No sidecar: this really exercises the uncertified/default path. It is a
    # separate strategy/run, not a second view of the eventual 2x report.
    default_probe = probe.parent / "command-share-uncertified"
    default_probe.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(FIXTURE, default_probe / "generated.cpp")
    (default_probe / "inputs.json").write_text(json.dumps(params) + "\n")
    reference_class = measured_class(
        runner, default_probe, shared, arena_directory, measurements,
        observe_commands=True, uncertified_default=True)
    with mock.patch.dict(os.environ, {runner.REPORT_SINK_ENV: "1"}):
        reference = reference_class(shared).run(default_probe / "unused.csv",
                                                params=params, **kwargs)
    try:
        reference_bytes = Path(reference.sink.path).read_bytes()  # Entire canonical report/trailer.
        reference_row = measurements[-1]
        check(reference_row["uncertified"]
              and reference_row["supplied"] == HOST_DEFAULT_ARENA_BYTES_FROM_BRIEF,
              "uncertified run did not receive the stated hosting default")
        numerator, denominator = (reference_row["shareNumerator"],
                                  reference_row["shareDenominator"])
        expected_capacity = command_share_capacity(reference_row["supplied"], numerator, denominator)
        check((numerator, denominator) == (1, 16)
              and expected_capacity == 64 * CHUNK_BYTES
              and reference_row["commandCapacity"] == expected_capacity,
              "independent 1/16 arithmetic must give the 1 GiB default a 64 MiB command capacity")
        extent = reference_row["commandPeakExtent"]
        check(extent > 0 and reference.sink.counts.get("T", 0) > 0,
              "independent reference executed neither command pages nor full trade rows")
    finally:
        reference.close()

    store_need = ((extent + CHUNK_BYTES - 1) // CHUNK_BYTES * CHUNK_BYTES)
    arena_need = (store_need * denominator + numerator - 1) // numerator
    floor_case = store_need == CHUNK_BYTES
    if floor_case:
        # One chunk cannot cross the floored share. Replay successfully with
        # room for the complete engine's other physical users; the C++ floor
        # witnesses separately fix their arena at exactly four chunks.
        one_need = max(4 * CHUNK_BYTES, reference_row["actual"])
    else:
        one_need = (max(arena_need, reference_row["actual"]) + 1) // 2
    one_x = (one_need + CHUNK_BYTES - 1) // CHUNK_BYTES * CHUNK_BYTES
    one_capacity = command_share_capacity(one_x, numerator, denominator)
    two_capacity = command_share_capacity(2 * one_x, numerator, denominator)
    check((store_need <= one_capacity if floor_case else one_capacity < store_need <= two_capacity),
          "fixture must fit the floor or isolate share exhaustion between 1x and 2x")
    check(2 * one_x < reference_row["supplied"],
          "independent reference arena must be larger than the 2x arena")

    first_class = measured_class(
        runner, probe, shared, arena_directory, measurements,
        command_first=not floor_case, observe_commands=floor_case, supplied_bytes=one_x)
    with mock.patch.dict(os.environ, {runner.REPORT_SINK_ENV: "1"}):
        first = first_class(shared).run(probe / "unused.csv", params=params, **kwargs)
    if floor_case:
        try:
            check(measurements[-1]["commandCapacity"] == one_capacity
                  and measurements[-1]["commandPeakExtent"] == extent,
                  "1x floor replay changed the input's command-store extent")
            check(Path(first.sink.path).read_bytes() == reference_bytes,
                  "1x floor replay's full report differs from the independent default-arena report")
        finally:
            first.close()
    else:
        check(measurements[-1].get("commandFirst") is True,
              "1x run did not produce the typed command-share witness")

    control_class = measured_class(
        runner, probe, shared, arena_directory, measurements,
        observe_commands=True, supplied_bytes=2 * one_x)
    with mock.patch.dict(os.environ, {runner.REPORT_SINK_ENV: "1"}):
        control = control_class(shared).run(probe / "unused.csv", params=params, **kwargs)
    try:
        check(measurements[-1]["commandCapacity"] == two_capacity
              and measurements[-1]["commandPeakExtent"] == extent,
              "2x replay changed the input's command-store extent")
        check(Path(control.sink.path).read_bytes() == reference_bytes,
              "2x full report differs byte-for-byte from the independent large-arena report")
    finally:
        control.close()


def fixture_run(build: Path, work: Path, arena_directory: Path) -> list:
    import run_strategy as runner

    shared = compile_module(build, FIXTURE, work / "fixture/strategy.so")
    rows = []
    # Small bar counts keep the deliberately coarse all-opening lot bound
    # affordable on disk. Many IDs and root rows exceed fixed RAM tails.
    scenarios = (
        ("orders-every-bar", 1, 32),
        ("many-computed-ids", 2, 512),
        ("long-id-components", 3, 32),
        ("partial-exit-ladders", 4, 64),
        ("pyramiding-at-limit", 5, 32),
        ("successor-replacements", 6, 512),
        # Feed 128 pre-roll + 256 window bars, all before the exclusive end.
        # The selected-window descriptor does not trim the primary feed.
        ("selected-window", 7, 384),
        ("large-curve", 0, 1_048_576),
    )
    for name, which, count in scenarios:
        probe = work / name
        probe.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(FIXTURE, probe / "generated.cpp")
        (probe / "inputs.json").write_text(json.dumps({"case": which}) + "\n")
        # Input generation is entirely independent of bound arithmetic.
        bars = (runner.BarC * count)()
        for i in range(count):
            bars[i] = runner.BarC(open=100.0, high=102.0, low=99.0, close=100.0,
                                  volume=1000.0, timestamp=START_MS + i * STEP_MS)
        window = ((START_MS + 128 * STEP_MS, START_MS + 384 * STEP_MS)
                  if which == 7 else None)
        run_kwargs = {"preloaded_bars": (bars, count), "input_tf": "1", "script_tf": "1"}
        data = data_before_run(runner, probe / "unused.csv", run_kwargs, window)
        facts = fixture_facts(which, count)
        write_sidecar(probe, facts,
                      scope={"kind": "plan", "sha256": data["planSha256"]},
                      stream={"source": "fixture has no output/trace/hash declarations",
                              "openAtEndRows": facts["fills"]["rowsPerFill"],
                              "outputs": 0, "trace": 0, "hashes": 0,
                              "commentBytes": 64})
        if which == 1:
            command_share_runs(runner, probe, shared, arena_directory, rows, bars, count)
        cls = measured_class(runner, probe, shared, arena_directory, rows, window)
        strategy = cls(shared)
        with mock.patch.dict(os.environ, {runner.REPORT_SINK_ENV: "1"}):
            report = strategy.run(probe / "unused.csv", params={"case": which},
                                  preloaded_bars=(bars, count), input_tf="1", script_tf="1")
        if which not in (0, 6):
            check(strategy.fixture_counts.get("entries", 0) > 0
                  and strategy.fixture_counts.get("exits", 0) > 0,
                  f"{name}: adversarial source commands never ran")
            # Read real rows so a no-fill fixture cannot count as trade proof.
            closed = [trade for trade in report["trades"] if not trade.get("open_at_end")]
            check(len(closed) > 0, f"{name}: no real closed/partial trade rows")
            if which == 4:
                check(len(closed) > strategy.fixture_counts["entries"],
                      "partial-exit witness produced no extra closed slices")
        if which == 6:
            check(strategy.fixture_counts.get("roots") == count * 512,
                  "root witness did not issue its statically counted replacement chain")
        if which == 5:
            check(strategy.fixture_counts.get("peak_lots") == 8,
                  "pyramiding witness did not reach its eight-lot declaration limit")
        if which == 1:
            check(strategy.fixture_counts["entries"] == 8 * count,
                  "max-orders witness did not submit eight entries every script bar")
        report.close()
        if which == 0:
            check(rows[-1]["actual"] >= 23 * CHUNK_BYTES,
                  "million-point fixture did not materialize its actual curve")
            cls = measured_class(runner, probe, shared, arena_directory, rows, half=True)
            with mock.patch.dict(os.environ, {runner.REPORT_SINK_ENV: "1"}):
                cls(shared).run(probe / "unused.csv", params={"case": 0},
                                preloaded_bars=(bars, count), input_tf="1", script_tf="1")
    return rows


def corpus_census(corpus: Path) -> tuple[list[Path], list[dict]]:
    baseline = ROOT / "scripts/corpus_parity_baseline.txt"
    names = [line.split()[1].removesuffix("/engine_trades.csv")
             for line in baseline.read_text().splitlines()
             if re.match(r"^[0-9a-f]{64}  validation/", line)]
    check(len(names) == len(set(names)) and names, "empty/duplicate parity population")
    # Mirror run_corpus.sh's source-marked validation directories, and also
    # retain every pinned missing probe so absent input cannot shrink a sweep.
    discovered = {str(path.relative_to(corpus)) for path in (corpus / "validation").glob("*")
                  if path.is_dir() and ((path / "strategy.pine").is_file()
                                        or (path / "generated.cpp").is_file())}
    names = sorted(set(names) | discovered)
    entries = []
    for name in names:
        probe = corpus / name
        try:
            compiled, verdict = read_compile_facts(probe)
            strategy = compiled["strategy"]
            reason = strategy.get("controlFlow", {}).get("reason", "")
            # This counts which proofs exist, not request byte sizes. Real
            # request dimensions are resolved by the runner before every run.
            result = arena_bound({
                **compiled, "schema": FACTS_SCHEMA,
                "data": {"scriptBars": 1, "selected": 0, "windowBars": 1,
                         "source": "census proof-availability validation"},
            })
            unbounded_terms = result["unboundedTerms"]
            private_unbounded = result["terms"]["childPrivate"]["unbounded"]
            if verdict == "BOUNDED" and result["terms"]["arena"]["unbounded"]:
                verdict = "UNCLASSIFIED"
                reason = result["terms"]["arena"].get("reason", "missing arena proof")
        except (OSError, ValueError) as error:
            verdict, reason = "UNCLASSIFIED", str(error)
            unbounded_terms = ["arena", "ohlcv", "finalizerStream"]
            private_unbounded = True
        entries.append({"probe": name, "classification": verdict, "reason": reason,
                        "unboundedTerms": unbounded_terms,
                        "childPrivateUnbounded": private_unbounded})
        print(f"arena_census: {name} {verdict} {reason}", flush=True)
    counts = {kind: sum(row["classification"] == kind for row in entries)
              for kind in ("BOUNDED", "UNBOUNDED", "UNCLASSIFIED")}
    print("arena_census_counts: " + json.dumps(counts, sort_keys=True), flush=True)
    print("arena_unbounded_probes: " + json.dumps(
        [row["probe"] for row in entries if row["classification"] == "UNBOUNDED"]),
        flush=True)
    print("arena_census_unbounded_term_counts: " + json.dumps({
        term: sum(term in row["unboundedTerms"] for row in entries)
        for term in ("arena", "ohlcv", "finalizerStream")
    }, sort_keys=True), flush=True)
    print("arena_census_child_private_unbounded_count: " +
          str(sum(row["childPrivateUnbounded"] for row in entries)) +
          " (informational)", flush=True)
    return [corpus / row["probe"] for row in entries
            if row["classification"] == "BOUNDED"], entries


def corpus_run(build: Path, corpus: Path, work: Path, arena_directory: Path,
               census_result=None) -> list:
    probes, census = census_result if census_result is not None else corpus_census(corpus)
    (work / "census.json").write_text(json.dumps(census, indent=2) + "\n")
    unclassified = [row["probe"] for row in census if row["classification"] == "UNCLASSIFIED"]
    # Run every classifiable probe even if the eventual coverage verdict is
    # incomplete. No missing sidecar is a successful skip.
    # This is a bound-proof guard, not production admission/routing. COMPUTE
    # uses its default attempt-0 arena and escalation when facts are missing.
    # A guessed arena here could not prove actual <= certified bound.
    check(probes, "RETURN TO CORE-TOP: no corpus probe has a certified FACTS record")
    expected_pin = subprocess.check_output(
        ["git", "rev-parse", "HEAD:corpus"], cwd=ROOT, text=True).strip()
    actual_pin = subprocess.check_output(
        ["git", "-C", str(corpus), "rev-parse", "HEAD"], text=True).strip()
    check(actual_pin == expected_pin, f"corpus pin mismatch: {actual_pin} != {expected_pin}")
    # Exactly once, before any consumers. No run_corpus.sh overlaps this test.
    subprocess.run([sys.executable, "-B", str(ROOT / "scripts/derive_corpus_feeds.py")],
                   cwd=ROOT, check=True)
    rows = []
    for probe in probes:
        module = compile_module(build, probe / "generated.cpp",
                                work / probe.name / "strategy.so")
        result_path = work / probe.name / "measurements.json"
        command = [sys.executable, "-B", str(Path(__file__).resolve()),
                   "--probe", str(probe), "--module", str(module),
                   "--arena-dir", str(arena_directory), "--result", str(result_path)]
        with (work / probe.name / "run.log").open("w") as log:
            completed = subprocess.run(command, cwd=ROOT, text=True, stdout=log,
                                       stderr=subprocess.STDOUT, check=False)
        check(completed.returncode == 0, f"{probe.name}: see {work / probe.name / 'run.log'}")
        observed = json.loads(result_path.read_text())
        check(observed, f"{probe.name}: canonical runner executed no measured run")
        rows.extend(observed)
        for row in observed:
            print(f"arena_bound: probe={probe.name} actual={row['actual']} "
                  f"bound={row['arenaBytes']} "
                  f"ratio={row['actual'] / row['arenaBytes']:.9f}", flush=True)
    by_probe = {}
    for row in rows:
        by_probe.setdefault(row["probe"], set()).update(row["unboundedTerms"])
    print("arena_corpus_actual_plan_unbounded_term_counts: " + json.dumps({
        term: sum(term in flags for flags in by_probe.values())
        for term in ("arena", "ohlcv", "finalizerStream")
    }, sort_keys=True), flush=True)
    print("arena_corpus_actual_plan_child_private_unbounded_count: " +
          str(len({row["probe"] for row in rows if row["childPrivate"]["unbounded"]})) +
          " (informational)", flush=True)
    check(not unclassified,
          "RETURN TO CORE-TOP: incomplete corpus certification: " + ", ".join(unclassified))
    return rows


def canonical_probe(args) -> int:
    import run_strategy as runner

    rows = []
    cls = measured_class(runner, args.probe, args.module, args.arena_dir, rows)
    # Reuse the actual corpus CLI path. This preserves range-end, special
    # per-probe feeds and declared magnifier behavior without reconstructing it.
    output = args.result.with_suffix(".trades.csv")
    argv = [str(ROOT / "scripts/run_strategy.py"), str(args.probe), "-o", str(output)]
    with mock.patch.object(runner, "Strategy", cls), \
            mock.patch.object(runner, "find_strategy_lib", return_value=args.module), \
            mock.patch.object(sys, "argv", argv), \
            mock.patch.dict(os.environ, {runner.REPORT_SINK_ENV: "1"}):
        result = runner.main()
    args.result.write_text(json.dumps(rows, indent=2) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--formulas-only", action="store_true")
    mode.add_argument("--adversarial-only", action="store_true")
    mode.add_argument("--census-only", action="store_true")
    mode.add_argument("--probe", type=Path)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--corpus", type=Path, default=ROOT / "corpus")
    parser.add_argument("--arena-dir", type=Path)
    parser.add_argument("--module", type=Path)
    parser.add_argument("--result", type=Path)
    args = parser.parse_args()
    if args.formulas_only:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(FormulaTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if args.census_only:
        _, census = corpus_census(args.corpus.resolve())
        return int(any(row["classification"] == "UNCLASSIFIED" for row in census))
    if sys.platform != "linux":
        print("SKIP: arena runtime/corpus proof is Linux lab-only; never run on the Mac")
        return 77
    build = args.build_dir.resolve()
    work = build / "arena-bound"
    work.mkdir(parents=True, exist_ok=True)
    args.arena_dir = (args.arena_dir or work / "backings").resolve()
    if args.probe is not None:
        check(args.module is not None and args.result is not None, "--probe needs --module/--result")
        return canonical_probe(args)
    census_result = None
    if not args.adversarial_only:
        # Always deliver the count/list, even if a later missing scratch port
        # makes the first real fixture fail.
        census_result = corpus_census(args.corpus.resolve())
        (work / "census.json").write_text(json.dumps(census_result[1], indent=2) + "\n")
    rows = fixture_run(build, work, args.arena_dir)
    (work / "fixture-measurements.json").write_text(json.dumps(rows, indent=2) + "\n")
    if not args.adversarial_only:
        rows.extend(corpus_run(build, args.corpus.resolve(), work, args.arena_dir,
                               census_result))
    (work / "measurements.json").write_text(json.dumps(rows, indent=2) + "\n")
    measured = [row for row in rows if "actual" in row]
    check(measured, "zero runtime measurements")
    worst = min(measured, key=lambda row: row["actual"] / row["arenaBytes"])
    print("arena_bound_worst_conservatism: " + json.dumps(worst, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, FactsError, OSError, subprocess.SubprocessError) as error:
        print(f"test_arena_bound: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
