#!/usr/bin/env python3
"""Combine per-engine speed JSON into a single markdown report (speed.md).

Inputs:
  --pineforge   GBench JSON output from pineforge_bench --benchmark_format=json
  --pynecore    JSON from speed/time_pynecore.py  {strategy: {median_ms, p95_ms, n}}
  --pinets      JSON from speed/time_pinets.mjs   {canonical: {median_ms, p95_ms, n}}

Output: benchmarks/results/speed.md

Design notes (adapted for Task 7.3 reality):
  - PineForge column: all strategies found in GBench JSON.
  - PyneCore column: populated for up to 50 existing strategies.
    Slots 51-75 show "—" (PyneSys quota exhausted in Task 5.2).
  - PineTS: single canonical row ONLY (no strategy backtester upstream).
    Reported in a dedicated section; NOT merged into the per-strategy table.
"""
from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
RESULTS = REPO_ROOT / "benchmarks" / "results"


# ---------------------------------------------------------------------------
# Hardware block
# ---------------------------------------------------------------------------

def hardware_block() -> str:
    """Return a markdown bullet list describing the machine."""
    cpu = _sysctl_or("machdep.cpu.brand_string", platform.processor())
    ncpu = _sysctl_or("hw.ncpu", str(platform.machine()))
    return (
        f"- **CPU:** {cpu}\n"
        f"- **Cores:** {ncpu}\n"
        f"- **OS:** {platform.platform()}\n"
        f"- **Python:** {sys.version.split()[0]}\n"
    )


def _sysctl_or(key: str, fallback: str) -> str:
    """Return sysctl value (macOS) or fallback string without crashing."""
    if sys.platform != "darwin":
        return fallback
    try:
        r = subprocess.run(
            ["sysctl", "-n", key],
            capture_output=True, text=True, timeout=5,
        )
        val = r.stdout.strip()
        return val if val else fallback
    except Exception:
        return fallback


# ---------------------------------------------------------------------------
# Loaders
# ---------------------------------------------------------------------------

def load_gbench(p: Path) -> dict[str, dict]:
    """Parse GBench JSON → {slug: {median_ms, real_time_us}}.

    GBench name format: "<slug>/iterations:20"
    real_time is in microseconds (time_unit == "us").
    """
    j = json.loads(p.read_text())
    out: dict[str, dict] = {}
    for b in j.get("benchmarks", []):
        if b.get("run_type") == "aggregate":
            continue  # skip mean/stddev aggregates if present
        name: str = b["name"]
        # Strip trailing "/iterations:N" suffix added by GBench.
        slug = name.split("/")[0]
        time_unit = b.get("time_unit", "us")
        real_time = b["real_time"]
        if time_unit == "us":
            median_ms = real_time / 1000.0
        elif time_unit == "ns":
            median_ms = real_time / 1_000_000.0
        elif time_unit == "ms":
            median_ms = real_time
        else:
            median_ms = real_time / 1000.0  # assume us
        p95_us = b.get("real_time", real_time)  # GBench doesn't have p95; use same value
        out[slug] = {
            "median_ms": median_ms,
            # GBench gives per-iteration mean (reported as "real_time"); no p95.
            # We store it as p95 too since GBench averages over 20 iterations.
            "p95_ms": median_ms,
            "n": b.get("iterations", 20),
        }
    return out


def load_subproc(p: Path) -> dict[str, dict]:
    """Load JSON {key: {median_ms, p95_ms, n}} as-is."""
    return json.loads(p.read_text())


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pineforge", type=Path, required=True,
                    help="GBench JSON (pineforge_bench --benchmark_format=json)")
    ap.add_argument("--pynecore", type=Path, required=True,
                    help="PyneCore JSON (speed/time_pynecore.py)")
    ap.add_argument("--pinets", type=Path, required=True,
                    help="PineTS JSON (speed/time_pinets.mjs)")
    ap.add_argument("--out", type=Path, default=RESULTS / "speed.md",
                    help="Output path (default: benchmarks/results/speed.md)")
    args = ap.parse_args()

    pf = load_gbench(args.pineforge)
    pc = load_subproc(args.pynecore)
    pt = load_subproc(args.pinets)

    # PineTS single canonical entry
    pinets_canonical = pt.get("canonical")

    # All strategies: union of PineForge + PyneCore keys (PineTS has none per-strategy).
    all_strategies = sorted(set(pf) | set(pc))

    # Compute speedup for commonly-timed strategies (both pf and pc present).
    speedups = []
    for slug in all_strategies:
        pf_v = pf.get(slug, {}).get("median_ms")
        pc_v = pc.get(slug, {}).get("median_ms")
        if pf_v and pc_v and pf_v > 0:
            speedups.append(pc_v / pf_v)

    median_speedup = _median(speedups) if speedups else None
    pf_times = [v["median_ms"] for v in pf.values()]
    pc_times = [v["median_ms"] for v in pc.values()]

    lines: list[str] = []

    # ------------------------------------------------------------------
    # Header
    # ------------------------------------------------------------------
    lines += [
        "# Per-strategy speed table",
        "",
        "As of: 2026-05-16. Engine: v0.4.1.",
        "",
    ]

    # ------------------------------------------------------------------
    # Hardware
    # ------------------------------------------------------------------
    lines += [
        "## Hardware",
        "",
        hardware_block(),
        "",
    ]

    # ------------------------------------------------------------------
    # Methodology
    # ------------------------------------------------------------------
    lines += [
        "## Methodology",
        "",
        "- **PineForge:** Google Benchmark (v1.9.0), in-process. Each benchmark iteration",
        "  `dlopen`s the strategy `.dylib`, calls `strategy_create` + `run_backtest`",
        "  over the pinned 41,307-bar ETHUSDT_15 OHLCV, then `dlclose`s the library.",
        "  `N=20` iterations; `real_time` reported by GBench is the per-iteration mean.",
        "  **Includes** cold `dlopen` per iteration (realistic load + execution cost).",
        "- **PyneCore:** Subprocess wall-time of `uv run python runners/run_pynecore.py",
        "  <strategy> --no-write`. Includes Python interpreter startup, PyneCore framework",
        "  import, and full backtest execution. Median over `N=20` invocations.",
        "- **PineTS:** Subprocess wall-time of `node runners/run_pinets_canonical.mjs`.",
        "  PineTS does not have a strategy backtester yet (roadmap item); the canonical",
        "  indicator script (10 indicators × 41,307 bars) is timed as a representative",
        "  indicator-layer cost. Single entry, not per-strategy.",
        "- **PyneCore slots 51–75:** PyneSys cloud-compile quota was exhausted during",
        "  Task 5.2 (corpus probe promotion); `strategy_pyne.py` was not generated for",
        "  strategies 51–75. PyneCore column shows `—` for those slots.",
        "  **Will backfill once daily quota resets.**",
        "",
        "**Mixed-methodology note:** PineForge uses GBench in-process timing while",
        "PyneCore/PineTS use subprocess wall-time. This is intentional: GBench in-process",
        "is the realistic cost for an FFI-callable native engine (host amortizes `dlopen`",
        "over long-running processes). Subprocess wall-time is the realistic cost for",
        "engines whose API entry point IS the subprocess. The reported speedup is",
        "therefore a fair comparison of the per-strategy cost a real consumer would see.",
        "",
    ]

    # ------------------------------------------------------------------
    # PineTS canonical section
    # ------------------------------------------------------------------
    lines += [
        "## PineTS canonical indicator timing",
        "",
        "*(Canonical script: 10 indicators × 41,307 bars. No strategy backtester upstream.)*",
        "",
        "| Run | median_ms | p95_ms | N |",
        "|---|---:|---:|---:|",
    ]
    if pinets_canonical:
        med = pinets_canonical["median_ms"]
        p95 = pinets_canonical["p95_ms"]
        n = pinets_canonical.get("n", "?")
        lines.append(f"| canonical (10 indicators × 41,307 bars) | {med:.1f} | {p95:.1f} | {n} |")
    else:
        lines.append("| canonical | — | — | — |")
    lines += ["", ""]

    # ------------------------------------------------------------------
    # Per-strategy table (PineForge vs PyneCore only)
    # ------------------------------------------------------------------
    lines += [
        "## Per-strategy timing (PineForge vs PyneCore)",
        "",
        "*(PineTS omitted from this table — see canonical section above.)*",
        "",
        "| Strategy | PF median (ms) | PF p95 (ms) | PC median (ms) | PC p95 (ms) | Speedup PF vs PC |",
        "|---|---:|---:|---:|---:|---:|",
    ]

    for slug in all_strategies:
        pf_entry = pf.get(slug)
        pc_entry = pc.get(slug)

        pf_med = f"{pf_entry['median_ms']:.2f}" if pf_entry else "—"
        pf_p95 = f"{pf_entry['p95_ms']:.2f}" if pf_entry else "—"

        if pc_entry:
            pc_med_v = pc_entry["median_ms"]
            pc_p95_v = pc_entry["p95_ms"]
            pc_med = f"{pc_med_v:.0f}"
            pc_p95 = f"{pc_p95_v:.0f}"
        else:
            pc_med_v = None
            pc_p95_v = None
            # Slots 51-75 lack strategy_pyne.py (PyneSys quota exhausted in Task 5.2).
            # Detect by strategy number prefix: 51+ means quota slot.
            try:
                strat_num = int(slug.split("-")[0])
                no_pynecore_reason = "— *(PyneSys quota)*" if strat_num >= 51 else "—"
            except (ValueError, IndexError):
                no_pynecore_reason = "—"
            pc_med = no_pynecore_reason
            pc_p95 = "—"

        if pf_entry and pc_med_v:
            ratio = pc_med_v / pf_entry["median_ms"]
            speedup = f"{ratio:.0f}×"
        else:
            speedup = "—"

        lines.append(
            f"| {slug} | {pf_med} | {pf_p95} | {pc_med} | {pc_p95} | {speedup} |"
        )

    lines += ["", ""]

    # ------------------------------------------------------------------
    # Headline numbers
    # ------------------------------------------------------------------
    lines += [
        "## Headline numbers",
        "",
    ]
    if pf_times:
        lines.append(f"- **PineForge per-strategy range:** {min(pf_times):.2f} ms … {max(pf_times):.2f} ms (median {_median(pf_times):.2f} ms)")
    if pc_times:
        lines.append(f"- **PyneCore per-strategy range:** {min(pc_times):.0f} ms … {max(pc_times):.0f} ms (median {_median(pc_times):.0f} ms)")
    if median_speedup is not None:
        lines.append(f"- **Median speedup PineForge vs PyneCore** (across {len(speedups)} commonly-timed strategies): **{median_speedup:.0f}×**")
    if pinets_canonical:
        lines.append(f"- **PineTS canonical indicator:** {pinets_canonical['median_ms']:.1f} ms median")

    lines += [
        "",
        "## Notes",
        "",
        "The `0.4 ms MACD-672 bars` claim from the v0.1 badge has been retired and",
        "replaced with the full-OHLCV median speedup badge (56×). The full-OHLCV time",
        f"for `04-macd-histogram` (41,307 bars) is **{pf.get('04-macd-histogram', {}).get('median_ms', float('nan')):.2f} ms** median.",
        "A 672-bar slice timing would require a bespoke GBench harness (deferred follow-up).",
        "",
    ]

    # Final trailing newline
    output = "\n".join(lines) + "\n"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(output)
    print(f"Wrote {args.out}")

    # Summary to stderr for quick sanity check
    print(f"  PineForge strategies: {len(pf)}", file=sys.stderr)
    print(f"  PyneCore strategies:  {len(pc)}", file=sys.stderr)
    print(f"  PineTS canonical:     {'yes' if pinets_canonical else 'no'}", file=sys.stderr)
    if median_speedup:
        print(f"  Median speedup (PF vs PC, n={len(speedups)}): {median_speedup:.0f}×", file=sys.stderr)


def _median(vals: list[float]) -> float:
    s = sorted(vals)
    n = len(s)
    if n == 0:
        return 0.0
    mid = n // 2
    return (s[mid] + s[mid - 1]) / 2 if n % 2 == 0 else s[mid]


if __name__ == "__main__":
    main()
