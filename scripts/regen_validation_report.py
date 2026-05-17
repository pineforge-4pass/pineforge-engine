#!/usr/bin/env python3
"""Regenerate `corpus/validation_report.md` from the current corpus state.

Standalone Python (stdlib only) that imports `verify_corpus.py`'s tier-
classification machinery and emits the canonical Markdown report for the
single-tree `corpus/validation/` layout introduced post-publication.

Probe links resolve to the public GitHub blob URL by default
(``CORPUS_REPO_BLOB``). Override via ``--blob-url`` for forks / mirrors,
or pass ``--blob-url ""`` to fall back to local relative paths.

Sections:
  1. Headline       — total + tier counts, generation date, commit SHAs.
  2. Anomalies      — every probe NOT excellent, with one-line reason.
  3. Per-category   — summary table (category | total | excellent | ...).
  4. Per-strategy   — full table sorted by category, then slug.

Usage:
  scripts/regen_validation_report.py
  scripts/regen_validation_report.py --output /tmp/foo.md

Categories are derived from the leading hyphen-token of each probe slug
(e.g. `ta-bb-rsi-...` -> `ta`). Mirrors how strategies are grouped on disk
under `corpus/validation/`.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

# Reuse verify_corpus.py's tier classifier so the report stays in lock-step
# with whatever the canonical aggregate verifier is currently emitting.
SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import verify_corpus as vc  # noqa: E402

CORPUS_ROOT = SCRIPT_DIR.parent / "corpus"
VALIDATION_ROOT = CORPUS_ROOT / "validation"

TIER_ORDER = ["excellent", "strong", "moderate", "weak", "minimal",
              "anomaly", "engine_only", "missing"]


def _git_sha(path: Path) -> str:
    try:
        out = subprocess.check_output(
            ["git", "-C", str(path), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip()
    except Exception:
        return "unknown"


def _category_of(slug: str) -> str:
    """Leading hyphen-token of the slug.

    `ta-bb-rsi-mean-reversion-01` -> `ta`
    `udt-method-default-param-kwargs-01` -> `udt`
    """
    return slug.split("-", 1)[0] if "-" in slug else slug


def _verify_probe(strategy_dir: Path) -> dict:
    """Run verify_corpus.verify_one but capture all the headline numbers.

    We replicate the inner logic (cheaper than parsing stdout) so we can
    pull entry/exit/pnl p90, count delta, tv/eng/matched counts.
    """
    rel = f"validation/{strategy_dir.name}"
    meta = vc.load_strategy_metadata(strategy_dir)
    tv_path = strategy_dir / str(meta.get("tv_trades_csv", "tv_trades.csv"))
    eng_path = strategy_dir / "engine_trades.csv"
    if not tv_path.exists() or not eng_path.exists():
        return {
            "slug": strategy_dir.name,
            "rel": rel,
            "tier": "missing",
            "tv": 0, "eng": 0, "matched": 0,
            "count_delta": 0.0, "entry_p90": 0.0, "exit_p90": 0.0, "pnl_p90": 0.0,
            "profile": "n/a", "notes": "tv_trades.csv or engine_trades.csv missing",
        }

    tv = vc.parse_trades(tv_path, tz_offset_hours=vc.tv_timezone_offset(meta))
    eng = vc.parse_trades(eng_path, tz_offset_hours=vc.ENGINE_CSV_TZ_OFFSET_HOURS)
    matched = vc.align_by_time(tv, eng)
    tv_cmp, eng_cmp = vc.trim_to_common_match_window(tv, eng, matched)
    matched = vc.align_by_time(tv_cmp, eng_cmp)

    profile = vc.resolve_profile(strategy_dir, meta)
    thresh = vc.parity_for_profile(profile)

    trim_bars = int(meta.get("trim_bars", 0) or 0)
    warmup_bars = int(meta.get("warmup_bars", 0) or 0)
    first_ms, last_ms, bar_ms = vc._ohlcv_span_ms(strategy_dir)
    bounds = vc.interior_time_bounds(trim_bars, warmup_bars,
                                     first_ms, last_ms, bar_ms)

    if bounds is not None:
        interior_matched = [(t, e) for (t, e) in matched
                            if vc.is_interior(t.entry_time * 1000, bounds)]
        gating_matched = interior_matched if interior_matched else matched
        tv_gate = [t for t in tv_cmp if vc.is_interior(t.entry_time * 1000, bounds)]
        eng_gate = [e for e in eng_cmp if vc.is_interior(e.entry_time * 1000, bounds)]
    else:
        gating_matched = matched
        tv_gate, eng_gate = tv_cmp, eng_cmp

    if not gating_matched:
        # Mirror verify_one's no-aligned-trades branch.
        tier = "excellent" if (len(tv_cmp) == 0 and len(eng_cmp) == 0) else "minimal"
        return {
            "slug": strategy_dir.name,
            "rel": rel,
            "tier": tier,
            "tv": len(tv_cmp), "eng": len(eng_cmp), "matched": 0,
            "count_delta": vc.relative_max(len(tv_gate), len(eng_gate)),
            "entry_p90": 0.0, "exit_p90": 0.0, "pnl_p90": 0.0,
            "profile": profile,
            "notes": meta.get("notes", "no aligned trades"),
        }

    count_delta = vc.relative_max(len(tv_gate), len(eng_gate))
    entry_deltas = [vc.relative_max(t.entry_price, e.entry_price) for t, e in gating_matched]
    exit_deltas  = [vc.relative_max(t.exit_price,  e.exit_price)  for t, e in gating_matched]
    pnl_deltas: list[float] = []
    for t, e in gating_matched:
        if abs(t.pnl) < vc.PNL_NEAR_ZERO_USD:
            continue
        pnl_deltas.append(abs(t.pnl - e.pnl) / abs(t.pnl))

    entry_p90 = vc.percentile(entry_deltas, 0.90)
    exit_p90  = vc.percentile(exit_deltas,  0.90)
    pnl_p90   = vc.percentile(pnl_deltas,   0.90) if pnl_deltas else 0.0

    count_ok = count_delta < thresh["count"]
    entry_ok = entry_p90  < thresh["entry"]
    exit_ok  = exit_p90   < thresh["exit"]
    pnl_ok   = pnl_p90    < thresh["pnl"]
    all_ok   = count_ok and entry_ok and exit_ok and pnl_ok
    if all_ok:
        tier = "excellent"
    elif (
        len(gating_matched) / max(len(tv_gate), 1) >= 0.99
        and count_delta < vc.STRONG_COUNT_DELTA
        and entry_p90 < vc.STRONG_ENTRY_DELTA
        and exit_p90 < vc.STRONG_EXIT_DELTA
        and pnl_p90 < vc.STRONG_PNL_DELTA
    ):
        tier = "strong"
    elif len(gating_matched) / max(len(tv_gate), 1) >= 0.90:
        tier = "moderate"
    elif gating_matched:
        tier = "weak"
    else:
        tier = "minimal"

    expected_tier = str(meta.get("expected_tier", "")).strip().lower()
    if expected_tier in {"anomaly", "engine_only"} and tier != "excellent":
        tier = expected_tier
    val_overrides = meta.get("validation_overrides") or {}
    expect_tv_match = bool(val_overrides.get("expect_tv_match", True))
    if not expect_tv_match and tier != "excellent":
        tier = "engine_only"

    notes = ""
    if tier != "excellent":
        notes = str(meta.get("notes", "")).strip()
        if not notes:
            # Synthesize a one-line reason from which gate failed.
            failures = []
            if not count_ok: failures.append(f"count Δ {count_delta*100:.2f}%")
            if not entry_ok: failures.append(f"entry p90 {entry_p90*100:.4f}%")
            if not exit_ok:  failures.append(f"exit p90 {exit_p90*100:.4f}%")
            if not pnl_ok:   failures.append(f"pnl p90 {pnl_p90*100:.4f}%")
            notes = "; ".join(failures) if failures else "non-excellent"

    return {
        "slug": strategy_dir.name,
        "rel": rel,
        "tier": tier,
        "tv": len(tv_cmp),
        "eng": len(eng_cmp),
        "matched": len(matched),
        "count_delta": count_delta,
        "entry_p90": entry_p90,
        "exit_p90": exit_p90,
        "pnl_p90": pnl_p90,
        "profile": profile,
        "notes": notes,
    }


def _fmt_pct(x: float) -> str:
    return f"{x*100:.4f}%"


CORPUS_REPO_BLOB = (
    "https://github.com/fullpass-4pass/pineforge-corpus/tree/main"
)


def _probe_link(slug: str, blob_url: str) -> str:
    """Render probe slug as Markdown link. Uses GitHub blob URL when set;
    falls back to local relative path when ``blob_url`` is empty."""
    if blob_url:
        return f"[`{slug}`]({blob_url}/validation/{slug}/)"
    return f"[`{slug}`](./validation/{slug}/)"


def _emit(rows: list[dict], engine_sha: str, corpus_sha: str,
          blob_url: str = CORPUS_REPO_BLOB) -> str:
    counts = {k: 0 for k in TIER_ORDER}
    for r in rows:
        counts[r["tier"]] = counts.get(r["tier"], 0) + 1
    total = len(rows)
    excellent_pct = (counts["excellent"] / total * 100.0) if total else 0.0

    cats: dict[str, list[dict]] = {}
    for r in rows:
        cats.setdefault(_category_of(r["slug"]), []).append(r)

    lines: list[str] = []
    lines.append("# PineForge Corpus Validation Report")
    lines.append("")
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    lines.append(f"**Generated:** {today} (UTC) — engine `{engine_sha}`, corpus `{corpus_sha}`")
    lines.append("")
    lines.append("All probes live under `corpus/validation/`; categories below are derived")
    lines.append("from each slug's leading hyphen-token (e.g. `ta-...`, `composite-...`).")
    lines.append("")

    # 1. Headline.
    lines.append("## Headline")
    lines.append("")
    lines.append(f"- **Total probes verified:** {total}")
    lines.append(f"- **Excellent:** {counts['excellent']} ({excellent_pct:.1f}%)")
    lines.append(f"- **Strong:** {counts['strong']}")
    lines.append(f"- **Moderate:** {counts['moderate']}")
    lines.append(f"- **Weak:** {counts['weak']}")
    lines.append(f"- **Minimal:** {counts['minimal']}")
    lines.append(f"- **Anomaly (TV-side, expected):** {counts['anomaly']}")
    lines.append(f"- **Engine-only by design:** {counts['engine_only']}")
    lines.append(f"- **Missing:** {counts['missing']}")
    lines.append("")

    # 2. Anomalies & non-excellent.
    non_excellent = [r for r in rows if r["tier"] != "excellent"]
    lines.append("## Anomalies & Non-Excellent")
    lines.append("")
    if not non_excellent:
        lines.append("_All probes verified at the `excellent` tier — no anomalies surfaced._")
    else:
        lines.append("Each row points at a probe under `corpus/validation/`; rationale is")
        lines.append("captured in the probe's `inputs.json` notes field where present.")
        lines.append("")
        lines.append("| Tier | Probe | TV | Eng | Count Δ | PnL p90 | Reason |")
        lines.append("|---|---|---:|---:|---:|---:|---|")
        for r in sorted(non_excellent, key=lambda x: (x["tier"], x["slug"])):
            link = _probe_link(r['slug'], blob_url)
            lines.append(
                f"| **{r['tier']}** | {link} | {r['tv']} | {r['eng']} | "
                f"{_fmt_pct(r['count_delta'])} | {_fmt_pct(r['pnl_p90'])} | "
                f"{r['notes']} |"
            )
    lines.append("")

    # 3. Per-category summary.
    lines.append("## Per-Category Summary")
    lines.append("")
    lines.append("| Category | Total | Excellent | Strong | Moderate | Weak | Minimal | Anomaly | Engine-only |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for cat in sorted(cats.keys()):
        cat_rows = cats[cat]
        cnt = {k: 0 for k in TIER_ORDER}
        for r in cat_rows:
            cnt[r["tier"]] = cnt.get(r["tier"], 0) + 1
        lines.append(
            f"| `{cat}` | {len(cat_rows)} | {cnt['excellent']} | {cnt['strong']} | "
            f"{cnt['moderate']} | {cnt['weak']} | {cnt['minimal']} | "
            f"{cnt['anomaly']} | {cnt['engine_only']} |"
        )
    lines.append("")

    # 4. Per-strategy table.
    lines.append("## Per-Strategy Detail")
    lines.append("")
    lines.append("Sorted by category, then slug. `count Δ` is `|tv-eng|/max(tv,eng)`;")
    lines.append("`entry/exit/pnl p90` are 90th-percentile relative deltas across matched")
    lines.append("trades (PnL excludes scratch trades with `|tv_pnl| < $0.01`).")
    lines.append("")
    lines.append("| Slug | Category | Tier | Profile | TV | Engine | Matched | Count Δ | Entry p90 | Exit p90 | PnL p90 |")
    lines.append("|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|")
    for r in sorted(rows, key=lambda x: (_category_of(x["slug"]), x["slug"])):
        link = _probe_link(r['slug'], blob_url)
        lines.append(
            f"| {link} | `{_category_of(r['slug'])}` | {r['tier']} | "
            f"`{r.get('profile','strict')}` | "
            f"{r['tv']} | {r['eng']} | {r['matched']} | "
            f"{_fmt_pct(r['count_delta'])} | {_fmt_pct(r['entry_p90'])} | "
            f"{_fmt_pct(r['exit_p90'])} | {_fmt_pct(r['pnl_p90'])} |"
        )
    lines.append("")

    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", type=Path,
                    default=CORPUS_ROOT / "validation_report.md",
                    help="Output path (default: corpus/validation_report.md)")
    ap.add_argument("--blob-url", type=str, default=CORPUS_REPO_BLOB,
                    help=(f"Base URL for probe links (default: {CORPUS_REPO_BLOB!r}). "
                          "Pass empty string to fall back to local relative paths."))
    args = ap.parse_args()

    if not VALIDATION_ROOT.is_dir():
        print(f"ERROR: {VALIDATION_ROOT} not found", file=sys.stderr)
        return 1

    engine_sha = _git_sha(SCRIPT_DIR.parent)
    corpus_sha = _git_sha(CORPUS_ROOT)

    # Skip the symbol-specified/ container — its children require non-default
    # OHLCV + per-symbol syminfo (pending pineforge-data integration).
    # Excluded from corpus headline; engine correctness for those surfaces
    # is validated via ctest, not corpus parity.
    probes = sorted(p for p in VALIDATION_ROOT.iterdir()
                    if p.is_dir() and p.name != "symbol-specified")
    rows: list[dict] = []
    for p in probes:
        rows.append(_verify_probe(p))

    md = _emit(rows, engine_sha, corpus_sha, blob_url=args.blob_url)
    args.output.write_text(md, encoding="utf-8")
    counts = {k: 0 for k in TIER_ORDER}
    for r in rows:
        counts[r["tier"]] = counts.get(r["tier"], 0) + 1
    print(
        f"Wrote {args.output} — {len(rows)} probes — "
        + ", ".join(f"{k}={v}" for k, v in counts.items() if v)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
