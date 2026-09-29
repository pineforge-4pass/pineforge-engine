#!/usr/bin/env python3
"""Every number in benchmarks/README.md's headline traces to a committed source.

The README's "## Headline" section, up to "### Where the non-excellent rows
come from", prints the benchmark's figures. Each claim below matches one
phrase of it and derives every number the phrase captures from its sources:
files committed in this repository (the raw timing files under results/raw/,
the reports under results/, uv.lock) or a file at a pinned commit. The check
fails when

- a number in the section is captured by no claim: it has no source;
- a claim matches nothing: its phrase changed;
- a source is not committed, a pinned commit or its file is missing, or a
  file under results/raw/ differs from the sha256 manifest.json records;
- a captured number disagrees with the value derived from its sources, at the
  precision the README prints it with.

Quantile labels (Q1, p95) and "sha256" name a statistic or a hash, not a
number, and "one" is as often a pronoun: none of them must be captured, though
a claim may capture and check them.

    python3 benchmarks/check_provenance.py              # the working tree
    python3 benchmarks/check_provenance.py --rev REV    # a commit's README and sources
    python3 benchmarks/check_provenance.py --readme F   # another README text
    python3 benchmarks/check_provenance.py --list       # each number's sources
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "benchmarks" / "speed"))
from aggregate import quantile  # noqa: E402  (speed.md's own quantile)

README = "benchmarks/README.md"
RESULTS = "benchmarks/results"
SPEED = f"{RESULTS}/speed.md"
SUMMARY = f"{RESULTS}/summary.md"
SELECTION = f"{RESULTS}/selection.json"
RAW = f"{RESULTS}/raw"
MANIFEST = f"{RAW}/manifest.json"
NOW = f"{RAW}/2026-09-29-35db01c8"  # this refresh's measuring window
SWEEP = (f"{NOW}/pf_speed_a.json", f"{NOW}/pf_speed_b.json")  # slots 001-100, 101-201
PC_AB = ("pc-ab-v6103-1", "pc-ab-v6102-1", "pc-ab-v6102-2", "pc-ab-v6103-2")  # new, old, old, new
BENCH3, BENCH2, JUNE = (f"{RAW}/2026-09-22-063e4460", f"{RAW}/2026-09-22-e9ad37dd",
                        f"{RAW}/2026-06-11-94596bf")
PREV_TABLE = "35db01c8"  # the commit whose README carries the 2026-09-22 table
JUNE_TABLE = "933fe583"  # the commit whose README carries the 2026-06-11 table
SECTION = re.compile(r"^## Headline\n(.*?)^### Where the non-excellent rows come from",
                     re.M | re.S)
WORDS = {"one": 1, "two": 2, "three": 3, "four": 4, "five": 5,
         "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10}
NUMBER = re.compile(r"\w*\d\w*(?:[.,]\w+)*|\b(?:%s)\b" % "|".join(WORDS), re.I)
LABEL = re.compile(r"[pPqQ]\d{1,2}|sha\d+|one", re.I)
TALLY = ("graded", "emitted", "tv", "excellent", "strong", "moderate", "weak", "minimal",
         "anomaly", "engine_only", "n/a")
UNIT_MS = {"ns": 1e-6, "us": 1e-3, "ms": 1.0, "s": 1e3}


class Unsourced(Exception):
    """A source is not committed, or does not hold what the claim reads."""


class Mismatch(Exception):
    """The sources contradict the README beyond one number."""


class Tree:
    """The repository's files: the working tree (tracked files only) or commit ``rev``."""

    def __init__(self, rev: str | None = None):
        self.rev = rev
        self.reads: list[str] = []
        self._read: dict[str, bytes] = {}

    def _git(self, *args: str, text: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=text)

    def bytes(self, path: str) -> bytes:
        self.reads.append(path)
        if path not in self._read:
            if self.rev:
                res = self._git("show", f"{self.rev}:{path}", text=False)
                if res.returncode:
                    raise Unsourced(f"{path} is not committed at {self.rev}")
                self._read[path] = res.stdout
            elif self._git("ls-files", "--error-unmatch", path).returncode or not (REPO / path).is_file():
                raise Unsourced(f"{path} is not committed")
            else:
                self._read[path] = (REPO / path).read_bytes()
        return self._read[path]

    def text(self, path: str) -> str:
        return self.bytes(path).decode("utf-8")

    def json(self, path: str):
        return json.loads(self.bytes(path))

    def files(self, directory: str) -> list[str]:
        res = (self._git("ls-tree", "-r", "--name-only", self.rev, directory) if self.rev
               else self._git("ls-files", directory))
        return res.stdout.split()

    def pinned(self, commit: str, path: str) -> str:
        self.reads.append(f"{commit}:{path}")
        res = self._git("show", f"{commit}:{path}")
        if res.returncode:
            raise Unsourced(f"pinned {commit}:{path} is not in this clone")
        return res.stdout

    def is_commit(self, sha: str) -> bool:
        return self._git("cat-file", "-e", f"{sha}^{{commit}}").returncode == 0



@dataclass(frozen=True)
class Num:
    """A derived value; the README may round it to the digits it prints."""
    value: float
    slack: float = 0.0  # rounding the source itself already carries
    scale: float = 1.0  # the printed unit: 1e3 for "603k", 1e6 for "0.64 M"


@dataclass(frozen=True)
class Prefix:
    """A full sha the README abbreviates."""
    full: str


@dataclass(frozen=True)
class Commit:
    """A sha that must name a commit of this repository."""
    tree: Tree


def printed(text: str) -> tuple[float, int]:
    """The README's number and the decimals it prints."""
    if text.lower() in WORDS:
        return float(WORDS[text.lower()]), 0
    plain = text.replace(",", "")
    return float(plain), len(plain.split(".")[1]) if "." in plain else 0


def agrees(text: str, expected) -> bool:
    if isinstance(expected, Commit):
        return expected.tree.is_commit(text)
    if isinstance(expected, Prefix):
        return len(text) >= 7 and expected.full.startswith(text)
    if isinstance(expected, str):
        return text == expected
    value, decimals = printed(text)
    if isinstance(expected, Num):
        return (abs(value - expected.value / expected.scale)
                <= 0.5 * 10 ** -decimals + expected.slack / expected.scale + 1e-9)
    return decimals == 0 and value == expected


def shown(expected) -> str:
    if isinstance(expected, Num):
        return f"{expected.value / expected.scale:.6g}"
    if isinstance(expected, Commit):
        return "a commit of this repository"
    if isinstance(expected, Prefix):
        return f"a prefix of {expected.full}"
    return repr(expected)


def field(pattern: str, text: str, source: str) -> str:
    m = re.search(pattern, text, re.M)
    if not m:
        raise Unsourced(f"{source} has no /{pattern}/")
    return m.group(1)


# ---------------------------------------------------------------------------
# What the sources hold
# ---------------------------------------------------------------------------

RANK = {"n/a": -1, "minimal": 0, "weak": 1, "moderate": 2, "strong": 3, "excellent": 4}


def tallies(tree: Tree) -> dict[tuple[str, str], dict[str, int]]:
    """summary.md's tallies: (scope, engine) -> column -> count."""
    out = {}
    for line in tree.text(SUMMARY).splitlines():
        m = re.fullmatch(r"\| (corpus|closed|all) \(\d+\) \| (\w+) \| (.+) \|", line)
        if m:
            out[(m[1], m[2])] = dict(zip(TALLY, (int(c.replace(",", ""))
                                                 for c in m[3].split(" | "))))
    return out


def labels_of(text: str) -> dict[int, dict[str, str]]:
    """A summary.md's per-strategy cells: slot number -> engine -> cell text
    (its tier is ``tier(cell)``). The slot number is the key: a closed slot's
    name differs between refreshes (NNN-closed since this one)."""
    out = {}
    for line in text.splitlines():
        m = re.fullmatch(r"\| (\d{3})-\S+ \| (?:corpus|closed) \| [^|]+ \| \d+ \| "
                         r"([^|]+) \| ([^|]+) \| [^|]+ \|", line)
        if m:
            out[int(m[1])] = {"PineForge": m[2].strip(), "PyneCore": m[3].strip()}
    return out


def tier(cell: str) -> str:
    return cell.split()[1]


def slot_labels(tree: Tree) -> dict[int, dict[str, str]]:
    """summary.md's per-strategy tiers: slot number -> engine -> label."""
    return {k: {e: tier(c) for e, c in v.items()} for k, v in labels_of(tree.text(SUMMARY)).items()}


def gbench(tree: Tree, path: str, variant: str) -> dict[str, dict]:
    """A Google Benchmark file's ``<slug>/throughput/<variant>`` entries by slug."""
    return {b["name"].split("/")[0]: b for b in tree.json(path)["benchmarks"]
            if b.get("run_type") != "aggregate" and f"/throughput/{variant}/" in b["name"]}


def ms(entry: dict) -> float:
    return entry["real_time"] * UNIT_MS[entry.get("time_unit", "us")]


def pineforge_ms(tree: Tree) -> dict[str, float]:
    """The PineForge sweep, slot -> ms per run with the magnifier on, from the
    window's two Google Benchmark batches (slots 001-100 and 101-201)."""
    out: dict[str, float] = {}
    for half in SWEEP:
        out.update({slug: ms(b) for slug, b in gbench(tree, half, "with_magnifier").items()})
    return out


def feed_bars(tree: Tree) -> int:
    return tree.json(SELECTION)["inputs"]["benchFeed"]["bars"]


def replacement(tree: Tree) -> tuple[int, int]:
    """(the replacement slot, the slot it stands in for), from the selection."""
    (slot,) = [s for s in tree.json(SELECTION)["slots"] if s.get("replaces")]
    return int(slot["slot"][:3]), int(slot["replaces"][:3])


def window_engine(tree: Tree, directory: str) -> str:
    return tree.json(MANIFEST)["windows"][directory.rsplit("/", 1)[1]]["engine"]


def file_engine(tree: Tree, path: str) -> str:
    return tree.json(MANIFEST)["files"][path[len(RAW) + 1:]]["engine"]


def lock_version(text: str, package: str, source: str) -> str:
    return field(rf'^name = "{package}"\nversion = "([^"]+)"', text, source)


def uv_lock_version(tree: Tree, package: str) -> str:
    return lock_version(tree.text("benchmarks/uv.lock"), package, "uv.lock")


def previous_readme(tree: Tree) -> tuple[str, str]:
    """The README that carries the previous table, and its name. Its own
    numbers were traced by this check at that commit (--rev)."""
    return tree.pinned(PREV_TABLE, README), f"{PREV_TABLE}:{README}"


# ---------------------------------------------------------------------------
# The claims
# ---------------------------------------------------------------------------

@dataclass
class Claim:
    name: str
    pattern: str
    derive: Callable[[Tree, re.Match], dict]


CLAIMS: list[Claim] = []


def claim(pattern: str):
    def register(derive):
        CLAIMS.append(Claim(derive.__name__, pattern, derive))
        return derive
    return register


@claim(r"Last refresh \*\*(?P<asof>[\d-]+)\*\*\. Versions: engine `main` `(?P<engine>\w+)` "
       r"running the committed `generated\.cpp` \(codegen `(?P<codegen>\w+)`\), "
       r"PyneCore (?P<pynecore>[\d.]+), PineTS (?P<pinets>[\d.]+) and vectorbt "
       r"(?P<vectorbt>[\d.]+), on an? (?P<cpu>.+?) running (?P<os>\w+ \d+\.\d+)\.")
def versions(tree, m):
    speed = tree.text(SPEED)
    os_line = field(r"^- \*\*OS:\*\* (.+)$", speed, SPEED)
    return {"asof": field(r"^As of: ([\d-]+)\.", speed, SPEED),
            "engine": [field(r"PineForge engine main (\w+);", speed, SPEED), Commit(tree)],
            "codegen": field(r"\(codegen\s+`(\w+)`\)", speed, SPEED),
            "pynecore": uv_lock_version(tree, "pynesys-pynecore"),
            "pinets": field(r"; PineTS ([\d.]+);", speed, SPEED),
            "vectorbt": uv_lock_version(tree, "vectorbt"),
            "cpu": field(r"^- \*\*CPU:\*\* (.+)$", speed, SPEED),
            "os": field(r"^(\w+ \d+\.\d+)", os_line, f"{SPEED} OS line")}


@claim(r"\*\*The host changed\.\*\* The (?P<date>[\d-]+) table was timed on an (?P<old_cpu>[^,]+), "
       r"this one on an? (?P<host>[^:]+):")
def host_change(tree, m):
    readme, src = previous_readme(tree)
    cpu = field(r"^- \*\*CPU:\*\* (.+)$", tree.text(SPEED), SPEED)
    if not cpu.startswith(m["host"]):
        raise Mismatch(f"speed.md's CPU line, {cpu!r}, does not start with {m['host']!r}")
    return {"date": field(r"^Last refresh \*\*([\d-]+)\*\*", readme, src),
            "old_cpu": field(r"^- \*\*CPU:\*\* (.+)$", tree.pinned(PREV_TABLE, SPEED), f"{PREV_TABLE}:{SPEED}"),
            "host": m["host"]}


@claim(r"\*\*Population: (?P<strategies>\d+) strategies in (?P<slots>\d+) slots\*\*, "
       r"selected by \[`select_population\.py`\]\(select_population\.py\) with seed (?P<seed>\d+)\.")
def population(tree, m):
    slots = tree.json(SELECTION)["slots"]
    return {"strategies": sum(1 for s in slots if not s.get("replaces")),
            "slots": len(slots), "seed": tree.json(SELECTION)["seed"]}


@claim(r"\*\*(?P<n>\d+) corpus probes\*\* \(slots `(?P<lo>\d+)`–`(?P<hi>\d+)`\) are public\. "
       r"They come from `corpus/validation/` at gitlink `(?P<gitlink>\w+)`: at least "
       r"(?P<least>\w+) per each of (?P<families>\d+) mechanism families")
def corpus_half(tree, m):
    selection = tree.json(SELECTION)
    slots = [int(s["slot"][:3]) for s in selection["slots"] if s["source"] == "corpus"]
    families = selection["counts"]["corpus"]["families"]
    # The corpus pin the population was drawn at, not the tree's current pin:
    # a later corpus re-pin leaves the draw where it was.
    return {"n": len(slots), "lo": min(slots), "hi": max(slots),
            "gitlink": Prefix(selection["inputs"]["corpusGitlink"]),
            "least": min(f["slots"] for f in families.values()), "families": len(families)}


@claim(r"\*\*(?P<n>\d+) closed strategies\*\* \(slots `(?P<lo>\d+)`–`(?P<hi>\d+)`\) are "
       r"TradingView-scraped community scripts on `BINANCE:ETHUSDT\.P` (?P<tf>\d+m), .*?"
       r"\(sha256 `(?P<evidence>[0-9a-f]{64})`\)")
def closed_half(tree, m):
    slots = [int(s["slot"][:3]) for s in tree.json(SELECTION)["slots"]
             if s["source"] == "closed" and not s.get("replaces")]
    return {"n": len(slots), "lo": min(slots), "hi": max(slots),
            "tf": field(r'`timeframe == "(\d+)"`', tree.text(f"{RESULTS}/selection.md"),
                        "selection.md") + "m",
            "evidence": tree.json(MANIFEST)["evidence"]["sha256"]}


@claim(r"\*\*Slot `(?P<new>\d+)` stands in for slot `(?P<lost>\d+)` in the PyneCore count\.\*\* "
       r"PyneSys rejects `(?P<lost_again>\d+)`'s source with `\"Empty document\.\"`, so the next "
       r"strategy from the same bin became slot `(?P<new_again>\d+)`\. PineForge runs all "
       r"(?P<pineforge>\d+) slots\.")
def replacement_slot(tree, m):
    new, lost = replacement(tree)
    return {"new": new, "new_again": new, "lost": lost, "lost_again": lost,
            "pineforge": tallies(tree)[("all", "PineForge")]["graded"]}


@claim(r"graded against TradingView's own export \((?P<tv>[\d,]+) trades\)")
def tv_trades(tree, m):
    return {"tv": tallies(tree)[("all", "PineForge")]["tv"]}


@claim(r"^\| (?:\*\*)?(?P<scope>corpus|closed|all)(?:\*\*)? \| (?P<engine>PineForge|PyneCore|vectorbt) \| "
       r"(?P<graded>\d+) \| (?P<emitted>[\d,]+) \| (?P<tv>[\d,]+) \| \**(?P<excellent>\d+)\** \| "
       r"(?P<strong>\d+) \| (?P<moderate>\d+) \| (?P<weak>\d+) \| (?P<minimal>\d+) \| "
       r"(?P<na>\d+|—) \|$")
def tier_row(tree, m):
    row = tallies(tree)[(m["scope"], m["engine"])]
    if row["anomaly"] or row["engine_only"]:
        raise Mismatch(f"summary.md counts anomaly/engine_only rows the table has no column for: {row}")
    expected = {k: row[k] for k in TALLY[:8]}
    expected["na"] = "—" if m["engine"] == "vectorbt" else row["n/a"]
    return expected


@claim(r"Counting the (?P<n>\d+) strategies \(slot `(?P<new>\d+)` in place of `(?P<lost>\d+)`\), "
       r"PineForge grades all (?P<pf_excellent>\d+) excellent\. PyneCore grades (?P<pc_excellent>\d+) "
       r"excellent, (?P<pc_strong>\d+) strong, (?P<pc_moderate>\d+) moderate, (?P<pc_weak>\d+) weak "
       r"and (?P<pc_minimal>\d+) minimal\.")
def two_hundred(tree, m):
    new, lost = replacement(tree)
    labels = [v for k, v in slot_labels(tree).items() if k != lost]

    def count(engine: str, label: str) -> int:
        return sum(1 for v in labels if v[engine] == label)
    expected = {"n": len(labels), "new": new, "lost": lost,
                "pf_excellent": count("PineForge", "excellent")}
    for label in ("excellent", "strong", "moderate", "weak", "minimal"):
        expected[f"pc_{label}"] = count("PyneCore", label)
    if expected["pf_excellent"] != len(labels):
        raise Mismatch("PineForge has tiers other than excellent, which the sentence does not list")
    if sum(v for k, v in expected.items() if k.startswith("pc_")) != len(labels):
        raise Mismatch("PyneCore has tiers the sentence does not list")
    return expected


@claim(r"Every engine was timed in the same window on this host, at engine `(?P<engine>\w+)`, with the "
       r"quiet-host gate re-checked before every timing batch")
def window(tree, m):
    return {"engine": [field(r"PineForge engine main (\w+);", tree.text(SPEED), SPEED),
                       window_engine(tree, NOW), Commit(tree)]}


BPS = r"(?P<q1>[\d.]+)(?P<q1_unit>[kM]) · \*\*(?P<median>[\d.]+)(?P<median_unit>[kM])\*\* · (?P<q3>[\d.]+)(?P<q3_unit>[kM])"
SCALE = {"k": 1e3, "M": 1e6}


def bars_per_second(m: re.Match, bars: int, times: list[float]) -> dict:
    """The Q1 / median / Q3 bars/s a row prints, each in the unit it prints (k or M)."""
    rates = [bars / (t / 1000) for t in times]
    return {g: Num(quantile(rates, q), scale=SCALE[m[f"{g}_unit"]])
            for g, q in (("q1", .25), ("median", .5), ("q3", .75))}


@claim(r"^\| PineForge \| in-process Google Benchmark, bar magnifier on \| (?P<n>\d+) \| "
       r"(?P<ms>[\d.]+) ms \| " + BPS + r" \|$")
def pineforge_row(tree, m):
    times = list(pineforge_ms(tree).values())
    return {"n": len(times), "ms": Num(quantile(times, .5)),
            **bars_per_second(m, feed_bars(tree), times)}


@claim(r"^\| PyneCore \| subprocess wall time \(interpreter start, import, backtest\), "
       r"(?P<workers>\d+) concurrent \| (?P<n>\d+) \| (?P<ms>[\d,]+) ms \| " + BPS + r" \|$")
def pynecore_row(tree, m):
    times = [v["median_ms"] for v in tree.json(f"{NOW}/pc_speed.json").values()]
    return {"workers": int(field(r"(\d+) strategies timed concurrently", tree.text(SPEED), SPEED)),
            "n": len(times), "ms": Num(quantile(times, .5)),
            **bars_per_second(m, feed_bars(tree), times)}


@claim(r"^\| vectorbt \| in-process, the (?P<ports>\d+) ports that load \| (?P<n>\d+) \| "
       r"(?P<ms>[\d.]+) ms \| — \|$")
def vectorbt_row(tree, m):
    times = [v["median_ms"] for v in tree.json(f"{NOW}/vbt_speed.json").values()]
    return {"ports": len(times), "n": len(times), "ms": Num(quantile(times, .5))}


@claim(r"^\| PineTS \| subprocess wall time of the canonical (?P<indicators>\d+)-indicator script \| "
       r"(?P<n>\d+) \| (?P<ms>[\d.]+) ms \| — \|$")
def pinets_row(tree, m):
    timings = tree.json(f"{NOW}/pt_speed.json")
    return {"indicators": int(field(r"canonical indicator script \((\d+) indicators\)",
                                    tree.text(SPEED), SPEED)),
            "n": len(timings), "ms": Num(timings["canonical"]["median_ms"])}


@claim(r"PineForge's median speedup is \*\*(?P<x>\d+)× over PyneCore\*\*, per strategy across the "
       r"(?P<n>\d+) strategies both engines time \(p5 (?P<p5>\d+)×, p95 (?P<p95>\d+)×\), and "
       r"(?P<vbt>[\d.]+)× over vectorbt across its (?P<vbt_n>\d+) ports\.")
def speedup(tree, m):
    pf = pineforge_ms(tree)
    pc = tree.json(f"{NOW}/pc_speed.json")
    vbt = tree.json(f"{NOW}/vbt_speed.json")
    over_pc = [pc[s]["median_ms"] / pf[s] for s in pf if s in pc]
    over_vbt = [vbt[s]["median_ms"] / pf[s] for s in pf if s in vbt]
    return {"x": Num(quantile(over_pc, .5)), "n": len(over_pc),
            "p5": Num(quantile(over_pc, .05)), "p95": Num(quantile(over_pc, .95)),
            "vbt": Num(quantile(over_vbt, .5)), "vbt_n": len(over_vbt)}


@claim(r"measures the magnifier-off hot loop at a median of \*\*(?P<tp>[\d.]+) M bars/s\*\* per "
       r"strategy\. That figure is over all (?P<n>\d+) slots, and is the median of (?P<runs>\w+) "
       r"quiet runs\.")
def throughput(tree, m):
    window_dir = NOW.rsplit("/", 1)[1]
    runs = [f"{RAW}/{p}" for p in tree.json(MANIFEST)["files"]
            if re.fullmatch(rf"{re.escape(window_dir)}/throughput/r\d+\.json", p)]
    medians, sizes = {}, set()
    for run in runs:
        rates = [b["items_per_second"] for b in gbench(tree, run, "no_magnifier").values()]
        medians[run] = quantile(rates, .5)
        sizes.add(len(rates))
    median_run = quantile(list(medians.values()), .5)
    published = tree.bytes("benchmarks/throughput/benchmark_results.json")
    kept = [run for run in runs if tree.bytes(run) == published]
    if len(kept) != 1 or medians[kept[0]] != median_run:
        raise Mismatch("throughput/benchmark_results.json is not the median run's raw file")
    if len(sizes) != 1:
        raise Mismatch(f"the throughput runs time different slot counts: {sorted(sizes)}")
    return {"tp": Num(median_run, scale=1e6), "n": sizes.pop(), "runs": len(runs)}


@claim(r"The PineForge sweep ran at a (?P<minute>\d+)-minute load of (?P<load_a>[\d.]+) \(slots "
       r"(?P<a_lo>\d+)–(?P<a_hi>\d+)\) and (?P<load_b>[\d.]+) \(slots (?P<b_lo>\d+)–(?P<b_hi>\d+)\)\. "
       r"Against engine `(?P<old>\w+)`, timed alternately with `(?P<new>\w+)` in (?P<windows>\w+) "
       r"window on (?P<n>\d+) slots, `(?P<new_again>\w+)` takes (?P<ratio>[\d.]+)× the time "
       r"per strategy at the median")
def sweep_loads_and_ab(tree, m):
    speed = tree.text(SPEED)
    batches = re.findall(r"^\| pineforge (\d+)-(\d+) \(\w+\) \| (\S+) \| ([\d.]+) \|", speed, re.M)
    gate = {row.split("\t")[1]: row.split("\t")[2]
            for row in tree.text(f"{NOW}/gate.tsv").splitlines()}
    if len(batches) != 2 or any(gate.get(utc) != load for *_, utc, load in batches):
        raise Mismatch("speed.md's PineForge batch loads are not the raw gate readings")
    (a_lo, a_hi, _, load_a), (b_lo, b_hi, _, load_b) = batches
    passes = ("new1", "old1", "old2", "new2")
    ab = {p: gbench(tree, f"{NOW}/ab/{p}.json", "with_magnifier") for p in passes}
    slots = sorted(ab["new1"])
    if any(sorted(ab[p]) != slots for p in passes):
        raise Mismatch("the A/B passes do not time the same slots")
    ratio = [(ms(ab["new1"][s]) + ms(ab["new2"][s])) / (ms(ab["old1"][s]) + ms(ab["old2"][s]))
             for s in slots]
    dates = [datetime.fromisoformat(tree.json(f"{NOW}/ab/{p}.json")["context"]["date"])
             for p in passes + ("june1", "june2")]
    new = [file_engine(tree, f"{NOW}/ab/{p}.json") for p in ("new1", "new2")] + [Commit(tree)]
    return {"minute": int(field(r"\| (\d+)-min load \|", speed, SPEED)),
            "load_a": load_a, "a_lo": int(a_lo), "a_hi": int(a_hi),
            "load_b": load_b, "b_lo": int(b_lo), "b_hi": int(b_hi),
            "old": [file_engine(tree, f"{NOW}/ab/{p}.json") for p in ("old1", "old2")] + [Commit(tree)],
            "new": new, "new_again": new,
            "windows": 1 if max(dates) - min(dates) < timedelta(hours=1) else 2,
            "n": len(slots), "ratio": Num(quantile(ratio, .5))}


@claim(r"\*\*Against the (?P<date>[\d-]+) table\*\* \(engine `(?P<engine>\w+)`: PineForge "
       r"(?P<pf_excellent>\d+) excellent and (?P<pf_strong>\d+) strong; PyneCore (?P<pc>[\d.]+): "
       r"(?P<pc_excellent>\d+) excellent; (?P<x>\d+)× over PyneCore at a median (?P<bps>\d+)k bars/s "
       r"per PineForge strategy\)")
def previous_table(tree, m):
    readme, src = previous_readme(tree)
    pf = re.search(r"^\| \*\*all\*\* \| PineForge \| \d+ \| [\d,]+ \| [\d,]+ \| \*\*(\d+)\*\* \| (\d+) \|",
                   readme, re.M)
    pc = re.search(r"^\| \*\*all\*\* \| PyneCore \| \d+ \| [\d,]+ \| [\d,]+ \| (\d+) \|", readme, re.M)
    if not (pf and pc):
        raise Unsourced(f"{src} has no 'all' tier rows")
    return {"date": field(r"^Last refresh \*\*([\d-]+)\*\*", readme, src),
            "engine": [field(r"engine `main` `(\w+)`", readme, src), Commit(tree)],
            "pf_excellent": int(pf[1]), "pf_strong": int(pf[2]),
            "pc": field(r"\), PyneCore ([\d.]+), PineTS", readme, src), "pc_excellent": int(pc[1]),
            "x": int(field(r"median speedup is \*\*(\d+)× over PyneCore\*\*", readme, src)),
            "bps": int(field(r"· \*\*(\d+)k\*\* ·", readme, src))}


@claim(r"PineForge's strong row, closed slot `(?P<pf_slot>\d+)`, grades excellent\. PyneCore "
       r"(?P<pc_new>[\d.]+) grades (?P<up>\d+) slots higher than (?P<pc_old>[\d.]+) did and none "
       r"lower; among them are `(?P<a>\d+)`, `(?P<b>\d+)` and `(?P<c>\d+)`, which raised a "
       r"`RuntimeError` on (?P<pc_old_again>[\d.]+) and now run\.")
def tier_changes(tree, m):
    before = labels_of(tree.pinned(PREV_TABLE, SUMMARY))
    now = labels_of(tree.text(SUMMARY))
    moved = {e: [k for k in now if tier(now[k][e]) != tier(before[k][e])] for e in ("PineForge", "PyneCore")}
    pf_up = [k for k in moved["PineForge"]
             if (tier(before[k]["PineForge"]), tier(now[k]["PineForge"])) == ("strong", "excellent")]
    if len(pf_up) != 1 or len(moved["PineForge"]) != 1:
        raise Mismatch(f"PineForge tiers moved on slots {moved['PineForge']}, not on one strong row")
    up = [k for k in moved["PyneCore"] if RANK[tier(now[k]["PyneCore"])] > RANK[tier(before[k]["PyneCore"])]]
    if len(up) != len(moved["PyneCore"]):
        raise Mismatch(f"PyneCore grades slots {sorted(set(moved['PyneCore']) - set(up))} lower")
    ran = sorted(k for k in up if "RuntimeError" in before[k]["PyneCore"])
    if len(ran) != 3:
        raise Mismatch(f"PyneCore ran {ran} after a RuntimeError, not three slots")
    old = lock_version(tree.pinned(PREV_TABLE, "benchmarks/uv.lock"), "pynesys-pynecore",
                       f"{PREV_TABLE}:benchmarks/uv.lock")
    return {"pf_slot": pf_up[0], "pc_new": uv_lock_version(tree, "pynesys-pynecore"), "up": len(up),
            "pc_old": old, "pc_old_again": old, "a": ran[0], "b": ran[1], "c": ran[2]}


@claim(r"On the same host and in the same window, PyneCore (?P<new>[\d.]+) takes (?P<ratio>[\d.]+)× the "
       r"time of (?P<old>[\d.]+) per strategy at the median, over (?P<n>\d+) public slots timed alternately")
def pynecore_versions(tree, m):
    passes = {p: tree.json(f"{NOW}/pc_ab/{p}.json") for p in PC_AB}
    slots = sorted(passes[PC_AB[0]])
    if any(sorted(v) != slots for v in passes.values()) or any(int(k[:3]) > 100 for k in slots):
        raise Mismatch("the PyneCore version passes do not time the same public slots")
    new_passes, old_passes = PC_AB[0::3], PC_AB[1:3]  # new, old, old, new
    ratio = [sum(passes[p][k]["median_ms"] for p in new_passes) / sum(passes[p][k]["median_ms"] for p in old_passes)
             for k in slots]
    versions = {tree.json(MANIFEST)["files"][f"{NOW[len(RAW) + 1:]}/pc_ab/{p}.json"]["pynecore"] for p in new_passes}
    olds = {tree.json(MANIFEST)["files"][f"{NOW[len(RAW) + 1:]}/pc_ab/{p}.json"]["pynecore"] for p in old_passes}
    old_lock = lock_version(tree.pinned(PREV_TABLE, "benchmarks/uv.lock"), "pynesys-pynecore",
                            f"{PREV_TABLE}:benchmarks/uv.lock")
    return {"new": list(versions) + [uv_lock_version(tree, "pynesys-pynecore")],
            "old": list(olds) + [old_lock], "n": len(slots), "ratio": Num(quantile(ratio, .5))}


@claim(r"\*\*These numbers are not comparable with the (?P<date>[\d-]+) table\*\* \(PineForge "
       r"(?P<pf>\d+)/(?P<pf_n>\d+) excellent, PyneCore (?P<pc>\d+)/(?P<pc_n>\d+), (?P<x>\d+)×\):")
def june_table(tree, m):
    readme = tree.pinned(JUNE_TABLE, README)
    tiers = re.search(r"^\| 🟢 excellent \| \*\*(\d+) / (\d+)\*\* \| (\d+) / (\d+) \|", readme, re.M)
    if not tiers:
        raise Unsourced(f"{JUNE_TABLE}:{README} has no excellent row")
    pf = gbench(tree, f"{JUNE}/pf_speed.json", "with_magnifier")
    pc = tree.json(f"{JUNE}/pc_speed.json")
    return {"date": field(r"^Last refresh: \*\*([\d-]+)\*\*", readme, f"{JUNE_TABLE}:{README}"),
            "pf": int(tiers[1]), "pf_n": int(tiers[2]), "pc": int(tiers[3]), "pc_n": int(tiers[4]),
            "x": [int(field(r"Median speed: \*\*(\d+)× faster than PyneCore\*\*", readme,
                            f"{JUNE_TABLE}:{README}")),
                  Num(quantile([pc[s]["median_ms"] / ms(pf[s]) for s in pf if s in pc], .5))]}


@claim(r"that table graded a different (?P<population>\d+)-strategy population .*?PyneCore moved "
       r"from (?P<old>[\d.]+) to (?P<new>[\d.]+) in between\.")
def june_population(tree, m):
    summary = tree.pinned(JUNE_TABLE, SUMMARY)
    return {"population": len(re.findall(r"^\| \d+-\S+ \| (?:strict|production) \|", summary, re.M)),
            "old": field(r"PyneCore (\d+\.\d+\.\d+)", tree.pinned(JUNE_TABLE, README),
                         f"{JUNE_TABLE}:{README}"),
            "new": uv_lock_version(tree, "pynesys-pynecore")}


@claim(r"it was timed on an (?P<old_cpu>[^.]+?)\. The (?P<date>[\d-]+) engine, rebuilt on this host and "
       r"timed in the same window, runs the (?P<probes>\w+) probes both populations share "
       r"(?P<x_lo>[\d.]+)–(?P<x_hi>[\d.]+)× faster than `(?P<engine>\w+)` with the magnifier on")
def june_engine(tree, m):
    def by_probe(path: str) -> dict[str, float]:  # the slot number differs across populations
        return {slug.split("-", 1)[1]: ms(b) for slug, b in gbench(tree, path, "with_magnifier").items()}
    ab = {p: by_probe(f"{NOW}/ab/{p}.json") for p in ("june1", "june2", "new1", "new2")}
    probes = sorted(set(ab["june1"]) & set(ab["new1"]))
    slower = [(ab["new1"][p] + ab["new2"][p]) / (ab["june1"][p] + ab["june2"][p]) for p in probes]
    if min(slower) <= 1:
        raise Mismatch(f"the current engine is not slower than the June engine on every probe: {slower}")
    june_readme = tree.pinned(JUNE_TABLE, README)
    return {"old_cpu": field(r"windows ran on the same (.+?) host", tree.text(f"{RAW}/README.md"),
                             f"{RAW}/README.md"),
            "date": field(r"^Last refresh: \*\*([\d-]+)\*\*", june_readme, f"{JUNE_TABLE}:{README}"),
            "probes": len(probes), "x_lo": Num(min(slower)), "x_hi": Num(max(slower)),
            "engine": [file_engine(tree, f"{NOW}/ab/{p}.json") for p in ("new1", "new2")] + [Commit(tree)]}


# ---------------------------------------------------------------------------
# The check
# ---------------------------------------------------------------------------

def check_manifest(tree: Tree) -> list[str]:
    """Every file under results/raw/ is listed in manifest.json with its sha256."""
    errors = []
    listed = set()
    for rel, entry in tree.json(MANIFEST)["files"].items():
        listed.add(f"{RAW}/{rel}")
        try:
            data = tree.bytes(f"{RAW}/{rel}")
        except Unsourced as exc:
            errors.append(f"manifest.json: {exc}")
            continue
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            errors.append(f"{RAW}/{rel}: sha256 differs from manifest.json")
    for path in tree.files(RAW):
        if path not in listed and path not in (f"{RAW}/README.md", MANIFEST):
            errors.append(f"{path} is not listed in manifest.json")
    return errors


def check(readme: str, tree: Tree, listing: list[str] | None = None) -> list[str]:
    """The errors of README text ``readme`` against ``tree``; ``listing``
    collects one line per claim match: the numbers and their sources."""
    section = SECTION.search(readme)
    if not section:
        return ["README has no '## Headline' section ending at '### Where the non-excellent rows come from'"]
    text, first_line = section.group(1), readme.count("\n", 0, section.start(1)) + 1
    errors: list[str] = []
    try:
        errors += check_manifest(tree)
    except Unsourced as exc:
        errors.append(str(exc))
    captured: list[tuple[int, int]] = []
    for c in CLAIMS:
        matches = list(re.finditer(c.pattern, text, re.M))
        if not matches:
            errors.append(f"{c.name}: its phrase is not in the headline any more")
        for m in matches:
            numbered = {g: m[g] for g in m.re.groupindex if m[g] and NUMBER.search(m[g])}
            captured += [m.span(g) for g in numbered]
            tree.reads = []
            try:
                expected = c.derive(tree, m)
            except (Unsourced, Mismatch) as exc:
                errors.append(f"{c.name}: {exc}")
                continue
            for group, value in numbered.items():
                if group not in expected:
                    errors.append(f"{c.name}: '{value}' ({group}) is captured but not derived")
                    continue
                wants = expected[group] if isinstance(expected[group], list) else [expected[group]]
                for want in wants:
                    if not agrees(value, want):
                        errors.append(f"{c.name}: the README prints {group} = {value}, "
                                      f"its sources give {shown(want)}")
            if listing is not None:
                line = first_line + text.count("\n", 0, m.start())
                listing.append(f"line {line} {c.name}: {', '.join(numbered.values())} <- "
                               f"{', '.join(dict.fromkeys(tree.reads))}")
    for token in NUMBER.finditer(text):
        if LABEL.fullmatch(token[0]):
            continue
        if not any(lo < token.end() and token.start() < hi for lo, hi in captured):
            line = first_line + text.count("\n", 0, token.start())
            errors.append(f"{README}:{line}: '{token[0]}' has no source (no claim captures it)")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rev", help="check the README and sources of this commit")
    ap.add_argument("--readme", type=Path, help="check this README text instead")
    ap.add_argument("--list", action="store_true", help="print each claim's numbers and sources")
    args = ap.parse_args()
    tree = Tree(args.rev)
    try:
        readme = args.readme.read_text(encoding="utf-8") if args.readme else tree.text(README)
    except Unsourced as exc:
        print(f"FAIL: {exc}")
        return 1
    listing: list[str] = []
    errors = check(readme, tree, listing)
    if args.list:
        print("\n".join(listing))
    for error in errors:
        print(f"FAIL: {error}")
    if errors:
        print(f"{len(errors)} provenance error(s) in {README}'s headline")
        return 1
    numbers = sum(1 for t in NUMBER.finditer(SECTION.search(readme).group(1)) if not LABEL.fullmatch(t[0]))
    print(f"OK: all {numbers} numbers in {README}'s headline trace to committed sources "
          f"({len(listing)} claim matches)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
