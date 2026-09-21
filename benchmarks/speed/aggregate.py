#!/usr/bin/env python3
"""Combine per-engine speed JSON into a single markdown report (speed.md).

Inputs:
  --pineforge   GBench JSON from pineforge_bench --benchmark_format=json (the
                ``<slug>/throughput/with_magnifier`` entries), or the docker
                ``subproc`` JSON; optional — without it the PineForge columns
                read ``--pending``
  --pynecore    JSON from speed/time_pynecore.py  {strategy: {median_ms, p95_ms, n}}
  --pinets      JSON from speed/time_pinets.mjs   {canonical: {median_ms, p95_ms, n}} (optional)
  --vectorbt    JSON from speed/time_vectorbt.py  {strategy: {median_ms, p95_ms, n}} (optional)
  --loads       TSV from run_all.sh's quiet_gate: label, UTC time, 1-min load, busy procs (optional)

Output: benchmarks/results/speed.md (``--out`` to write elsewhere).

Throughput counterpart: every per-strategy median wall time over the feed is
also expressed in bars/s (feed bars / median seconds). For PyneCore that wall
time is a subprocess's and includes interpreter startup and framework import;
PineForge's GBench time is an in-process backtest (dlopen outside the timing).
"""
from __future__ import annotations

import argparse
import importlib.metadata
import json
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BENCH = REPO_ROOT / "benchmarks"
RESULTS = BENCH / "results"
sys.path.insert(0, str(BENCH))
from paths import DATA  # noqa: E402


# ---------------------------------------------------------------------------
# Environment facts
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
        r = subprocess.run(["sysctl", "-n", key], capture_output=True, text=True, timeout=5)
        val = r.stdout.strip()
        return val if val else fallback
    except Exception:
        return fallback


def engine_version() -> str:
    header = REPO_ROOT / "build" / "include" / "pineforge" / "version.h"
    if header.exists():
        m = re.search(r'PINEFORGE_VERSION_FULL\s+"([^"]+)"', header.read_text())
        if m:
            return m.group(1)
    return (REPO_ROOT / "VERSION").read_text().strip()


def pinets_version() -> str:
    pkg = BENCH / "node_modules" / "pinets" / "package.json"
    return json.loads(pkg.read_text())["version"] if pkg.exists() else "not installed"


def feed_bars(path: Path) -> int:
    with path.open() as f:
        return sum(1 for line in f if line.strip()) - 1


# ---------------------------------------------------------------------------
# Loaders
# ---------------------------------------------------------------------------

def load_gbench(p: Path) -> dict[str, dict]:
    """GBench JSON -> {slug: {median_ms, p95_ms, n}} from the
    ``<slug>/throughput/with_magnifier/iterations:N`` entries (bar magnifier
    ON, the engine's most expensive configuration). ``real_time`` is GBench's
    per-iteration mean; GBench reports no p95, so p95 repeats it."""
    j = json.loads(p.read_text())
    out: dict[str, dict] = {}
    scale = {"ns": 1e-6, "us": 1e-3, "ms": 1.0, "s": 1e3}
    for b in j.get("benchmarks", []):
        if b.get("run_type") == "aggregate" or "/throughput/with_magnifier" not in b["name"]:
            continue
        ms = b["real_time"] * scale[b.get("time_unit", "us")]
        out[b["name"].split("/")[0]] = {"median_ms": ms, "p95_ms": ms, "n": b.get("iterations", 20)}
    if not out:
        raise SystemExit(f"{p}: no '<slug>/throughput/with_magnifier' GBench entries")
    return out


def load_subproc(p: Path) -> dict[str, dict]:
    """Load JSON {key: {median_ms, p95_ms, n}} as-is."""
    return json.loads(p.read_text())


def load_loads(p: Path | None) -> list[list[str]]:
    if p is None or not p.exists():
        return []
    return [line.split("\t") for line in p.read_text().splitlines() if line.strip()]


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def quantile(vals: list[float], q: float) -> float:
    """Linear-interpolation quantile (numpy's default), 0 for an empty list."""
    s = sorted(vals)
    if not s:
        return 0.0
    k = (len(s) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def _median(vals: list[float]) -> float:
    return quantile(vals, 0.5)


def fmt_ratio(x: float) -> str:
    return f"{x:.2f}×" if x < 1.0 else (f"{x:.1f}×" if x < 10.0 else f"{x:.0f}×")


def fmt_bps(x: float) -> str:
    return f"{x / 1e6:.2f} M" if x >= 1e6 else f"{x / 1e3:.1f} k"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pineforge", type=Path, default=None,
                    help="PineForge timing JSON (gbench: pineforge_bench --benchmark_format=json, "
                         "AUTHORITATIVE; subproc: speed/time_pineforge_docker.py)")
    ap.add_argument("--pineforge-format", choices=["gbench", "subproc"], default="gbench")
    ap.add_argument("--pynecore", type=Path, required=True, help="PyneCore JSON (speed/time_pynecore.py)")
    ap.add_argument("--pinets", type=Path, default=None, help="PineTS JSON (speed/time_pinets.mjs)")
    ap.add_argument("--vectorbt", type=Path, default=None, help="vectorbt JSON (speed/time_vectorbt.py)")
    ap.add_argument("--loads", type=Path, default=None, help="quiet_gate TSV (label, UTC, load, busy)")
    ap.add_argument("--pending", default="pending", help="PineForge cell text without --pineforge")
    ap.add_argument("--as-of", default=datetime.now(timezone.utc).strftime("%Y-%m-%d"))
    ap.add_argument("--feed", type=Path, default=DATA / "ETHUSDT_15.csv")
    ap.add_argument("--pynecore-workers", type=int, default=8,
                    help="concurrent PyneCore timing workers used (time_pynecore.py --workers)")
    ap.add_argument("--out", type=Path, default=RESULTS / "speed.md")
    args = ap.parse_args()

    pf: dict[str, dict] = {}
    if args.pineforge is not None:
        pf = (load_gbench(args.pineforge) if args.pineforge_format == "gbench"
              else load_subproc(args.pineforge))
    pc = load_subproc(args.pynecore)
    pt = load_subproc(args.pinets) if args.pinets and args.pinets.exists() else {}
    vbt = load_subproc(args.vectorbt) if args.vectorbt and args.vectorbt.exists() else {}
    loads = load_loads(args.loads)
    bars = feed_bars(args.feed)
    pinets_canonical = pt.get("canonical")
    all_strategies = sorted(set(pf) | set(pc) | set(vbt),
                            key=lambda s: (int(s.split("-", 1)[0]) if s[:1].isdigit() else 10**9, s))

    def bps(entry: dict | None) -> float | None:
        return bars / (entry["median_ms"] / 1000.0) if entry and entry["median_ms"] > 0 else None

    pc_speedups = [pc[s]["median_ms"] / pf[s]["median_ms"] for s in all_strategies
                   if s in pf and s in pc and pf[s]["median_ms"] > 0]
    vbt_speedups = [vbt[s]["median_ms"] / pf[s]["median_ms"] for s in all_strategies
                    if s in pf and s in vbt and pf[s]["median_ms"] > 0]
    pf_bps = [bps(v) for v in pf.values()]
    pc_bps = [bps(v) for v in pc.values()]
    pf_times = [v["median_ms"] for v in pf.values()]
    pc_times = [v["median_ms"] for v in pc.values()]
    pc_p95s = [v["p95_ms"] for v in pc.values()]
    vbt_times = [v["median_ms"] for v in vbt.values()]
    pf_label = (f"PineForge engine {engine_version()}" if pf else f"PineForge: {args.pending}")

    lines: list[str] = [
        "# Per-strategy speed table",
        "",
        f"As of: {args.as_of}. {pf_label}; PyneCore "
        f"{importlib.metadata.version('pynesys-pynecore')}; PineTS {pinets_version()}; "
        f"{len(all_strategies)} strategies on the {bars:,}-bar ETHUSDT 15m feed "
        f"(`{args.feed.relative_to(REPO_ROOT)}`).",
        "",
        "## Hardware",
        "",
        hardware_block(),
    ]
    if loads:
        lines += [
            "## Host load at each timing batch",
            "",
            "Timing is valid only on a quiet host: 1-minute load average below 6.0 and no "
            "`cmake --build` / `ctest` / `ci_verify` process, re-checked before every batch.",
            "",
            "| Batch | UTC | 1-min load | build/test processes |",
            "|---|---|---:|---:|",
        ] + [f"| {row[0]} | {row[1]} | {row[2]} | {row[3]} |" for row in loads] + [""]

    lines += [
        "## Methodology",
        "",
        "- **PineForge:** Google Benchmark (v1.9.0), in-process hot loop with **bar",
        "  magnifier ON** (1→4 ENDPOINTS sub-bar sampling): the `<slug>/throughput/with_magnifier`",
        "  entries. The strategy `.dylib` is `dlopen`ed once *outside* the timed region; each",
        f"  timed iteration calls `strategy_create` + `run_backtest_full` over the {bars:,}-bar",
        "  feed. `N=20` iterations; GBench's `real_time` is the per-iteration mean (no p95).",
        "- **PyneCore:** subprocess wall time of `uv run python runners/run_pynecore.py",
        "  <strategy> --no-write`, including Python interpreter startup, PyneCore framework",
        f"  import and the full backtest; median and p95 over `N=20` invocations, "
        f"{args.pynecore_workers} strategies timed concurrently.",
        "- **Bars/s:** feed bars ÷ median seconds per strategy. For PyneCore that is the",
        "  subprocess wall time above, so startup and import are inside the figure;",
        "  PineForge's is the in-process backtest alone.",
        "- **vectorbt:** in-process timing of the slots that ship a `strategy_vbt.py` port",
        "  (vectorized Pandas/NumPy + Numba). Median over `N=20` iterations.",
        "- **PineTS:** subprocess wall time of `node runners/run_pinets_canonical.mjs`. PineTS has",
        "  no strategy backtester upstream; the canonical indicator script (10 indicators) is",
        "  timed as its indicator-layer cost. Single entry, not per-strategy.",
        "",
        "**Mixed-methodology note:** PineForge and vectorbt are timed in-process while",
        "PyneCore/PineTS are timed as subprocesses. GBench in-process is the realistic cost for an",
        "FFI-callable native engine (the host amortizes loading); subprocess wall time is the",
        "realistic cost for engines whose entry point IS the process.",
        "",
        "## PineTS canonical indicator timing",
        "",
        "| Run | median_ms | p95_ms | N |",
        "|---|---:|---:|---:|",
    ]
    if pinets_canonical:
        lines.append(f"| canonical (10 indicators × {bars:,} bars) | {pinets_canonical['median_ms']:.1f} | "
                     f"{pinets_canonical['p95_ms']:.1f} | {pinets_canonical.get('n', '?')} |")
    else:
        lines.append("| canonical | — | — | — |")
    lines += [
        "",
        "## Per-strategy timing",
        "",
        "| Strategy | PF median (ms) | PF bars/s | PC median (ms) | PC p95 (ms) | PC bars/s | "
        "vbt median (ms) | Speedup PF vs PC | Speedup PF vs vbt |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for slug in all_strategies:
        f, c, v = pf.get(slug), pc.get(slug), vbt.get(slug)
        pf_med = f"{f['median_ms']:.2f}" if f else ("⏳" if not pf else "—")
        pf_bps_s = fmt_bps(bps(f)) if f else ("⏳" if not pf else "—")
        pc_med = f"{c['median_ms']:.0f}" if c else "—"
        pc_p95 = f"{c['p95_ms']:.0f}" if c else "—"
        pc_bps_s = fmt_bps(bps(c)) if c else "—"
        v_med = f"{v['median_ms']:.1f}" if v else "—"
        s_pc = fmt_ratio(c["median_ms"] / f["median_ms"]) if f and c else "—"
        s_v = fmt_ratio(v["median_ms"] / f["median_ms"]) if f and v else "—"
        lines.append(f"| {slug} | {pf_med} | {pf_bps_s} | {pc_med} | {pc_p95} | {pc_bps_s} | "
                     f"{v_med} | {s_pc} | {s_v} |")
    lines += ["", "", "## Headline numbers", ""]
    if pf_times:
        lines.append(f"- **PineForge per-strategy range:** {min(pf_times):.2f} ms … {max(pf_times):.2f} ms "
                     f"(median {_median(pf_times):.2f} ms)")
    else:
        lines.append(f"- **PineForge:** {args.pending}")
    if pc_times:
        lines.append(f"- **PyneCore per-strategy range:** {min(pc_times):.0f} ms … {max(pc_times):.0f} ms "
                     f"(median {_median(pc_times):.0f} ms; median p95 {_median(pc_p95s):.0f} ms; "
                     f"{len(pc_times)} strategies timed)")
    if vbt_times:
        lines.append(f"- **vectorbt per-strategy range:** {min(vbt_times):.1f} ms … {max(vbt_times):.1f} ms "
                     f"(median {_median(vbt_times):.1f} ms)")
    lines += ["", "| Throughput (bars/s) | Q1 | Median | Q3 |", "|---|---:|---:|---:|"]
    if pf_bps:
        lines.append(f"| PineForge (in-process, magnifier ON) | {fmt_bps(quantile(pf_bps, .25))} | "
                     f"**{fmt_bps(quantile(pf_bps, .5))}** | {fmt_bps(quantile(pf_bps, .75))} |")
    else:
        lines.append(f"| PineForge | ⏳ | ⏳ {args.pending} | ⏳ |")
    if pc_bps:
        lines.append(f"| PyneCore (subprocess wall time) | {fmt_bps(quantile(pc_bps, .25))} | "
                     f"**{fmt_bps(quantile(pc_bps, .5))}** | {fmt_bps(quantile(pc_bps, .75))} |")
    lines.append("")
    if pc_speedups:
        lines.append(f"- **Median speedup PineForge vs PyneCore** (across {len(pc_speedups)} commonly-timed "
                     f"strategies): **{_median(pc_speedups):.0f}×** (p5 {quantile(pc_speedups, .05):.0f}×, "
                     f"p95 {quantile(pc_speedups, .95):.0f}×)")
    if vbt_speedups:
        lines.append(f"- **Median speedup PineForge vs vectorbt** (across {len(vbt_speedups)} commonly-timed "
                     f"strategies): **{_median(vbt_speedups):.1f}×**")
    if pinets_canonical:
        lines.append(f"- **PineTS canonical indicator:** {pinets_canonical['median_ms']:.1f} ms median")
    lines.append("")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n")
    print(f"Wrote {args.out}")
    print(f"  PineForge strategies: {len(pf) or args.pending}", file=sys.stderr)
    print(f"  PyneCore strategies:  {len(pc)}", file=sys.stderr)
    if vbt:
        print(f"  vectorbt strategies:  {len(vbt)}", file=sys.stderr)
    print(f"  PineTS canonical:     {'yes' if pinets_canonical else 'no'}", file=sys.stderr)
    if pc_speedups:
        print(f"  Median speedup (PF vs PC, n={len(pc_speedups)}): {_median(pc_speedups):.0f}×",
              file=sys.stderr)


if __name__ == "__main__":
    main()
