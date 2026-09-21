#!/usr/bin/env python3
"""Multi-way trade-list comparator: TV ↔ PineForge ↔ PyneCore ↔ vectorbt.

Every engine's trade list is graded against the slot's TradingView tape with
the canonical corpus rubric itself, ``scripts/verify_corpus.py::
analyze_strategy`` — the single source of truth for the tiers (align-then-trim
common match window, fragment consolidation, range-end mark pairing, the
exact-count and coverage gates, strict/production threshold profiles and the
``inputs.json`` overrides). The rubric reads ``<dir>/engine_trades.csv``, so
each engine's CSV is graded in a scratch directory that links the slot's
``strategy.pine``, ``inputs.json`` and tape next to that CSV.

Inputs per slot (every ``<NNN-slug>/`` under ``paths.STRATEGY_ROOTS``: the
public assets and, when present, the maintainer-local closed root):

    tv_trades.csv          TradingView ground truth
    pineforge_trades.csv   scripts/run_strategy.py output (run_all.sh)
    pynecore_trades.csv    runners/run_pynecore.py output (run_all.sh)
    vectorbt_trades.csv    speed/time_vectorbt.py --write-trades (optional)

An engine without output is reported ``n/a`` with its reason: the PyneSys
compile error for a slot without ``strategy_pyne.py`` (from the compile log
ledger), else the first line of the runner's ``_<engine>_error.log``.

Output lives in ``benchmarks/results/``:
    - trade_comparison.md   per-strategy metrics per engine
    - summary.md            the per-strategy tier table and the tallies

Usage:
    python benchmarks/compare.py
    python benchmarks/compare.py --strategy 001-analyzer-anvil-percent-costs-01
    python benchmarks/compare.py --quiet
    # stage one engine's grades, the other columns marked pending:
    python benchmarks/compare.py --engines PyneCore --pending "pending wave D (BENCH2)" \
        --out-dir benchmarks/results/staged --summary-name pynecore_summary.md \
        --detail-name pynecore_trade_comparison.md
"""
from __future__ import annotations

import argparse
import csv
import json
import shutil
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

_SYS_BENCH = Path(__file__).resolve().parent
if str(_SYS_BENCH) not in sys.path:
    sys.path.insert(0, str(_SYS_BENCH))
from paths import BENCH, REPO_ROOT, STRATEGIES, STRATEGY_ROOTS  # noqa: E402

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import verify_corpus  # noqa: E402

BENCH_DIR = BENCH
ENGINES = [("PineForge", "pineforge_trades.csv"),
           ("PyneCore", "pynecore_trades.csv"),
           ("vectorbt", "vectorbt_trades.csv")]
LABELS = ("excellent", "strong", "moderate", "weak", "minimal", "anomaly", "engine_only", "n/a")
_LABEL_EMOJI = {
    "excellent": "🟢", "strong": "🟢", "moderate": "🟡", "weak": "🟠",
    "minimal": "🔴", "anomaly": "🔵", "engine_only": "🟣", "n/a": "⚪",
}
COMPILE_LEDGER = BENCH_DIR / "_workdir" / "pynesys_requests.jsonl"


@dataclass
class Grade:
    engine: str
    label: str
    emitted: int = 0            # closed trades in the engine CSV (whole run)
    result: "verify_corpus.VerificationResult | None" = None
    reason: str = ""


def closed_trades(path: Path) -> int:
    """Trade numbers with both an entry and an exit row (TV-schema CSV)."""
    entries, exits = set(), set()
    with path.open(encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            n = row.get("Trade #") or row.get("Trade number")
            (entries if row["Type"].startswith("Entry") else exits).add(n)
    return len(entries & exits)


def vectorbt_as_tv_schema(src: Path, dst: Path) -> int:
    """Rewrite vectorbt's flat CSV (id,direction,entry_time,...) in TV schema."""
    rows = []
    with src.open(encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            side = "long" if "long" in r["direction"].lower() or r["direction"] == "buy" else "short"
            def stamp(v: str) -> str:
                v = v.strip()
                if v.replace(".", "").isdigit():
                    ts = float(v)
                    ts = ts / 1000 if ts > 10**12 else ts
                    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")
                return datetime.strptime(v, "%Y-%m-%dT%H:%M:%SZ").strftime("%Y-%m-%d %H:%M")
            n = int(r["id"]) + 1
            rows.append([n, f"Exit {side}", stamp(r["exit_time"]), r["exit_price"], r["qty"], r["pnl"]])
            rows.append([n, f"Entry {side}", stamp(r["entry_time"]), r["entry_price"], r["qty"], r["pnl"]])
    with dst.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Trade #", "Type", "Date and time", "Price", "Qty", "Net PnL"])
        w.writerows(rows)
    return len(rows) // 2


def compile_errors() -> dict[str, str]:
    """slot -> first line of its PyneSys compile error, from the request ledger."""
    out: dict[str, str] = {}
    if COMPILE_LEDGER.exists():
        for line in COMPILE_LEDGER.read_text().splitlines():
            r = json.loads(line)
            if r.get("kind") == "compile":
                if r["outcome"] == "compile-error":
                    out[r["slot"]] = r.get("message", "")
                elif r["outcome"] == "ok":
                    out.pop(r["slot"], None)
    return out


def first_error_line(path: Path) -> str:
    lines = [ln.strip() for ln in path.read_text(errors="replace").splitlines() if ln.strip()]
    for ln in reversed(lines):
        if "Error" in ln or "error" in ln:
            return ln[:200]
    return (lines[-1] if lines else "no output")[:200]


def na_reason(engine: str, slot: Path, csv_name: str, errors: dict[str, str]) -> str:
    if engine == "PineForge":
        if not (slot / "generated.cpp").exists():
            return "codegen transpile error (no generated.cpp)"
        if (slot / "_pineforge_error.log").exists():
            return "run error: " + first_error_line(slot / "_pineforge_error.log")
        return f"no {csv_name}"
    if engine == "PyneCore":
        if not (slot / "strategy_pyne.py").exists():
            if slot.name in errors:
                return f"PyneSys compile error: {errors[slot.name]}"
            return "no strategy_pyne.py"
        if (slot / "_pynecore_error.log").exists():
            return "PyneCore runtime error: " + first_error_line(slot / "_pynecore_error.log")
        return f"no {csv_name}"
    return "no strategy_vbt.py port" if not (slot / "strategy_vbt.py").exists() else f"no {csv_name}"


def grade(slot: Path, engine: str, csv_name: str, scratch: Path, errors: dict[str, str]) -> Grade:
    src = slot / csv_name
    if not src.exists():
        return Grade(engine, "n/a", reason=na_reason(engine, slot, csv_name, errors))
    d = scratch / engine / slot.name
    d.mkdir(parents=True, exist_ok=True)
    meta = verify_corpus.load_strategy_metadata(slot)
    for name in ("strategy.pine", "inputs.json", str(meta.get("tv_trades_csv", "tv_trades.csv"))):
        if (slot / name).exists():
            (d / name).symlink_to((slot / name).resolve())
    if engine == "vectorbt":
        emitted = vectorbt_as_tv_schema(src, d / "engine_trades.csv")
    else:
        (d / "engine_trades.csv").symlink_to(src.resolve())
        emitted = closed_trades(src)
    return Grade(engine, "", emitted=emitted, result=verify_corpus.analyze_strategy(d))


def slot_dirs(only: str | None) -> list[tuple[str, Path]]:
    out = []
    for root in STRATEGY_ROOTS:
        group = "corpus" if root == STRATEGIES else "closed"
        for d in root.iterdir():
            if d.is_dir() and d.name[:1].isdigit() and (only is None or d.name == only):
                out.append((group, d))
    return sorted(out, key=lambda gd: (int(gd[1].name.split("-", 1)[0]), gd[1].name))


def pct(x: float) -> str:
    return f"{x * 100:.4f}%"


def cell(g: Grade) -> str:
    if g.label == "pending":
        return f"⏳ {g.reason}"
    if g.result is None:
        return f"{_LABEL_EMOJI['n/a']} n/a — {g.reason}"
    r = g.result
    eng_n, tv_n = ((r.eng_count, r.tv_count) if r.no_aligned_trades
                   else (r.eng_gate_count, r.tv_gate_count))
    return f"{_LABEL_EMOJI.get(r.label, '🔴')} {r.label} ({eng_n} / {tv_n})"


def label_of(g: Grade) -> str:
    if g.label == "pending":
        return "pending"
    return "n/a" if g.result is None else g.result.label


def detail_block(name: str, group: str, tv_raw: int, grades: list[Grade]) -> str:
    profile = next((g.result.profile for g in grades if g.result is not None), "n/a")
    lines = [f"### {name}  *({group}, profile: {profile})*", "",
             f"- TV closed trades: **{tv_raw}**"]
    for g in grades:
        if g.label == "pending":
            lines.append(f"- **{g.engine}** ⏳ {g.reason}")
            continue
        if g.result is None:
            lines.append(f"- **{g.engine}** ⚪ n/a — {g.reason}")
            continue
        r = g.result
        lines.append(
            f"- **{g.engine}** {_LABEL_EMOJI.get(r.label, '🔴')} **{r.label}**  "
            f"(emitted {g.emitted}; in window engine {r.eng_gate_count} / TV {r.tv_gate_count}; "
            f"matched {r.gating_matched_count}; coverage {r.coverage * 100:.1f}%)\n"
            f"    - count delta: `{pct(r.count_delta)}` (abs {r.count_abs_delta})\n"
            f"    - entry p90:   `{pct(r.entry_p90)}`\n"
            f"    - exit  p90:   `{pct(r.exit_p90)}`\n"
            f"    - PnL   p90:   `{pct(r.pnl_p90)}`"
            + (f"\n    - gates: {r.notes}" if r.label != "excellent" and r.notes else ""))
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--strategy", help="Limit to one slot folder (e.g. 001-analyzer-anvil-percent-costs-01)")
    ap.add_argument("--quiet", action="store_true", help="Only print summary")
    ap.add_argument("--no-write", action="store_true",
                    help="Don't write Markdown reports — print to stdout instead")
    ap.add_argument("--engines", default=",".join(e for e, _ in ENGINES),
                    help="Comma list of engines to grade (default: all); the others read --pending")
    ap.add_argument("--pending", default="not graded",
                    help="Cell text for the engines left out of --engines")
    ap.add_argument("--out-dir", type=Path, default=BENCH_DIR / "results")
    ap.add_argument("--summary-name", default="summary.md")
    ap.add_argument("--detail-name", default="trade_comparison.md")
    args = ap.parse_args()
    graded_engines = {e.strip() for e in args.engines.split(",") if e.strip()}
    unknown = graded_engines - {e for e, _ in ENGINES}
    if unknown:
        ap.error(f"unknown engine(s): {', '.join(sorted(unknown))}")

    errors = compile_errors()
    rows: list[tuple[str, str, int, list[Grade]]] = []
    with tempfile.TemporaryDirectory(prefix="bench-compare-") as tmp:
        scratch = Path(tmp)
        for group, slot in slot_dirs(args.strategy):
            meta = verify_corpus.load_strategy_metadata(slot)
            tape = slot / str(meta.get("tv_trades_csv", "tv_trades.csv"))
            if not tape.exists():
                print(f"SKIP {slot.name}: no TV tape", file=sys.stderr)
                continue
            grades = [grade(slot, e, f, scratch, errors) if e in graded_engines
                      else Grade(e, "pending", reason=args.pending)
                      for e, f in ENGINES]
            rows.append((slot.name, group, closed_trades(tape), grades))
            if not args.quiet:
                print(f"=== {slot.name} ({group})  TV={rows[-1][2]}")
                for g in grades:
                    print(f"  {g.engine:9s} {cell(g)}")
        shutil.rmtree(scratch, ignore_errors=True)

    groups = ["corpus", "closed"]
    present = [g for g in groups if any(r[1] == g for r in rows)]
    scope_note = ("" if graded_engines == {e for e, _ in ENGINES} else
                  f"**Staged:** graded here: {', '.join(e for e, _ in ENGINES if e in graded_engines)}; "
                  f"the other engines read ⏳ *{args.pending}*.\n")
    sections = [
        "# Trade comparison\n",
        scope_note,
        "Each strategy runs through PineForge, PyneCore and (where a `strategy_vbt.py` port exists) "
        "vectorbt on the same 53,929-bar Binance ETH/USDT-USDT 15m feed and is graded against its "
        "TradingView tape by the canonical corpus rubric, `scripts/verify_corpus.py::analyze_strategy` "
        "(align-then-trim common window, fragment consolidation, range-end mark pairing, exact-count "
        "and ≥99% coverage gates for *excellent*, strict/production threshold profiles, `inputs.json` "
        "overrides). PineTS has no strategy backtester upstream and is excluded here.\n",
    ]
    for name, group, tv_raw, grades in rows:
        sections.append(detail_block(name, group, tv_raw, grades))

    summary = [
        "# Summary\n",
        scope_note,
        "Match degree per the canonical corpus rubric (`scripts/verify_corpus.py::analyze_strategy`). "
        "Cells: tier (engine trades / TV trades inside the gated common window). ⚪ n/a = the engine "
        "produced no trade list for that strategy (reason in the cell).\n",
        "| Strategy | Group | Profile | TV trades | PineForge | PyneCore | vectorbt |",
        "|---|---|---|---:|---|---|---|",
    ]
    for name, group, tv_raw, grades in rows:
        profile = next((g.result.profile for g in grades if g.result is not None), "n/a")
        vbt = cell(grades[2]) if grades[2].result is not None or grades[2].label == "pending" else "—"
        summary.append(f"| {name} | {group} | {profile} | {tv_raw} | "
                       f"{cell(grades[0])} | {cell(grades[1])} | {vbt} |")
    summary.append("")
    tallies = ["## Tallies\n",
               "| Scope | Engine | Strategies graded | Trades emitted | TV trades | "
               + " | ".join(LABELS) + " |",
               "|---|---|---:|---:|---:|" + "---:|" * len(LABELS)]
    for scope in present + ["all"]:
        scoped = [r for r in rows if scope == "all" or r[1] == scope]
        tv_total = sum(r[2] for r in scoped)
        for i, (engine, _) in enumerate(ENGINES):
            gs = [r[3][i] for r in scoped]
            if engine not in graded_engines:
                continue
            if engine == "vectorbt" and not any(g.result is not None for g in gs):
                continue
            counts = {lbl: sum(1 for g in gs if label_of(g) == lbl) for lbl in LABELS}
            if engine == "vectorbt":
                counts["n/a"] = 0
                graded = sum(1 for g in gs if g.result is not None)
                tv_scope = sum(r[2] for r, g in zip(scoped, gs) if g.result is not None)
            else:
                graded = len(gs)
                tv_scope = tv_total
            emitted = sum(g.emitted for g in gs)
            tallies.append(f"| {scope} ({len(scoped)}) | {engine} | {graded} | {emitted:,} | "
                           f"{tv_scope:,} | " + " | ".join(str(counts[l]) for l in LABELS) + " |")
    tallies.append("")

    text_summary = "\n".join(summary + tallies)
    if args.no_write:
        print("\n".join(sections))
        print(text_summary)
    else:
        out_dir = args.out_dir.resolve()
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / args.detail_name).write_text("\n".join(sections), encoding="utf-8")
        (out_dir / args.summary_name).write_text(text_summary, encoding="utf-8")
        print(f"wrote {out_dir.relative_to(REPO_ROOT)}/{args.detail_name}")
        print(f"wrote {out_dir.relative_to(REPO_ROOT)}/{args.summary_name}")
        print()
        print("\n".join(tallies))
    return 0


if __name__ == "__main__":
    sys.exit(main())
