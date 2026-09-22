#!/usr/bin/env python3
"""Select the benchmark population: 100 corpus probes + 100 closed scraped scripts.

Deterministic (``--seed``, default 20260921). Writes
``benchmarks/results/selection.json`` (machine manifest) and
``benchmarks/results/selection.md`` (the printed manifest).

Corpus half (public)
    Drawn from ``corpus/validation/<probe>/`` (the engine's corpus gitlink).
    A probe is eligible when it has a ``strategy.pine``, at least 5 closed TV
    trades, a TV tape window inside the bench feed, and needs no feed the
    bench does not ship (the 15m bench feed cannot serve an ``inputs.json``
    that points at the 1m corpus feed). Eligible probes are classified into
    mechanism families by their names (``FAMILY_RULES``, first match wins),
    the 100 slots are allocated to families in proportion to their size with
    at least one slot per family (largest remainder), and each family is
    sampled by TV trade-count quantiles: its members, sorted by trade count,
    are cut into as many contiguous bins as the family has slots and one
    member is drawn per bin.

Closed half (private artifacts, public manifest)
    Drawn from the campaign population document (``lab population show`` ->
    ``lab evidence get <fileSha256>``): ``source == "scrapper"``,
    ``symbol == "BINANCE:ETHUSDT.P"``, ``timeframe == "15"`` -- the only
    dataset the bench feed (Binance ETH/USDT-USDT perp 15m) and the TV tapes
    agree on -- with anomaly-surface probes excluded. The tape bytes are read
    from the scrapper evidence tree and must hash to the population's
    ``tvTradesSha256`` pin. A probe is excluded when its tape window is not
    inside the bench feed, or when the campaign's latest verify report ran it
    on the 1m feed (finer-TF ``request.security`` / ``request.security_lower_tf``:
    the 15m bench feed cannot serve those bars). Slots are allocated to the
    ``hard``/``target`` surfaces in their population proportion and each
    surface is sampled by TV trade-count quantiles exactly like a corpus
    family.

Every slot carries its replacement queue: the other members of its bin in
seeded order, the "next candidate from the same stratum" used when a slot is
lost (a PyneSys compile rejection). ``--replace NNN="reason"`` keeps slot NNN
and appends its replacement -- the first queue entry not already selected --
as the next free slot number (201, 202, ...), so the manifest stays a pure
function of its inputs and the recorded rejections.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import random
import re
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

BENCH = Path(__file__).resolve().parent
REPO_ROOT = BENCH.parent
sys.path.insert(0, str(BENCH))
from paths import DATA  # noqa: E402

SEED = 20260921
N_CORPUS = 100
N_CLOSED = 100
MIN_TV_TRADES_CORPUS = 5
CLOSED_SYMBOL = "BINANCE:ETHUSDT.P"
CLOSED_TIMEFRAME = "15"
CORPUS_LICENSE = "Apache-2.0 (pineforge-corpus LICENSE)"
CLOSED_LICENSE = "TradingView scraped, not redistributed"

# Mechanism families, first match wins: the cross-cutting broker/runtime
# mechanisms a probe name spells anywhere, then the corpus README's own
# prefix families, then -- for the multi-surface composite-*/analyzer-*
# probes -- the mechanism their name spells, else "composite".
FAMILY_RULES: list[tuple[str, str]] = [
    ("request.security/_lower_tf", r"^(mtf|ltf)-|-mtf-|security"),
    ("magnifier", r"magnifier"),
    ("margin", r"margin|^anomaly-equity-mirror"),
    ("commission kinds", r"commission|-costs-"),
    ("sizing bases", r"sizing|percent-of-equity|percent-equity|equity-fraction"),
    ("risk rules", r"^(cap|risk)-|max-intraday|max-contracts|daily-cap"),
    ("calc timing", r"^(recompute|barstate)-|process-on-close|calc-on|close-immediate|next-bar|pooc|immediate-close"),
    ("matrix", r"^matrix-"),
    ("map", r"^map-"),
    ("array", r"^array-"),
    ("UDT", r"^udt-"),
    ("drawing", r"^drawing-"),
    ("math", r"^math-"),
    ("sessions/calendars", r"^(session|calendar)-"),
    ("brackets/trails/OCA", r"^(bracket|oca)-"),
    ("order kinds", r"^(order|pyramid)-"),
    ("ta", r"^(ta|vwap|bands|volume)-"),
    ("language semantics", r"^(na|syntax|input|enum|control|timeframe|stats)-"),
    ("sessions/calendars", r"session|dst|new-?york|weekday|-tz-"),
    ("brackets/trails/OCA", r"bracket|trail|oca|tp-sl"),
    ("order kinds", r"stop|limit|flip|revers|entry|close"),
    ("composite", r"^(composite|analyzer|anomaly)-"),
]

TV_TZ_BY_NAME = {"utc_plus_8": 8, "asia_taipei": 8, "utc": 0}


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def family_of(name: str) -> str:
    for family, pattern in FAMILY_RULES:
        if re.search(pattern, name):
            return family
    raise ValueError(f"no family rule matches {name!r}")


def tv_tz(meta: dict) -> timezone:
    name = str(meta.get("tv_trades_csv_tz", "")).strip().lower()
    return timezone(timedelta(hours=TV_TZ_BY_NAME.get(name, 8)))


def tape_facts(path: Path, tz: timezone) -> dict:
    """Closed trades, CSV rows and the [first entry, last exit] window (UTC ms)."""
    entries: dict[int, int] = {}
    exits: dict[int, int] = {}
    rows = 0
    with path.open(encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            rows += 1
            n = int(row.get("Trade #") or row["Trade number"])
            ts = int(datetime.strptime(row["Date and time"], "%Y-%m-%d %H:%M")
                     .replace(tzinfo=tz).timestamp() * 1000)
            (entries if row["Type"].startswith("Entry") else exits)[n] = ts
    closed = sorted(set(entries) & set(exits))
    return {
        "tvTrades": len(closed),
        "tvRows": rows,
        "windowStartMs": min(entries[n] for n in closed) if closed else None,
        "windowEndMs": max(exits[n] for n in closed) if closed else None,
    }


def feed_span(path: Path) -> dict:
    first = last = None
    n = 0
    with path.open(newline="") as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            if not row:
                continue
            ts = int(row[0])
            first = ts if first is None else first
            last = ts
            n += 1
    return {"path": str(path.relative_to(REPO_ROOT)), "sha256": sha256_file(path),
            "bars": n, "firstMs": first, "lastMs": last}


def covered(facts: dict, feed: dict) -> bool:
    return (facts["windowStartMs"] is not None
            and feed["firstMs"] <= facts["windowStartMs"]
            and facts["windowEndMs"] <= feed["lastMs"])


def iso(ms: int | None) -> str | None:
    if ms is None:
        return None
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M")


def allocate(sizes: dict[str, int], total: int) -> dict[str, int]:
    """Proportional allocation, at least one slot per stratum, largest remainder."""
    n = sum(sizes.values())
    quota = {k: total * v / n for k, v in sizes.items()}
    alloc = {k: min(sizes[k], max(1, int(q))) for k, q in quota.items()}
    while sum(alloc.values()) < total:
        k = max((k for k in alloc if alloc[k] < sizes[k]),
                key=lambda k: (quota[k] - alloc[k], k))
        alloc[k] += 1
    while sum(alloc.values()) > total:
        k = min((k for k in alloc if alloc[k] > 1),
                key=lambda k: (quota[k] - alloc[k], k))
        alloc[k] -= 1
    return alloc


def quantile_draw(members: list[dict], k: int, rng: random.Random) -> list[tuple[dict, int, list[str]]]:
    """Cut members (sorted by trade count, then key) into k contiguous bins and
    draw one per bin. Returns (pick, bin index, replacement queue of keys)."""
    members = sorted(members, key=lambda m: (m["tvTrades"], m["key"]))
    out = []
    for b in range(k):
        lo = len(members) * b // k
        hi = len(members) * (b + 1) // k
        bin_members = members[lo:hi]
        order = list(bin_members)
        rng.shuffle(order)
        pick, rest = order[0], order[1:]
        out.append((pick, b, [m["key"] for m in rest]))
    return out


def corpus_candidates(corpus_dir: Path, feed: dict) -> tuple[list[dict], dict[str, list[str]]]:
    eligible: list[dict] = []
    excluded: dict[str, list[str]] = {}
    for d in sorted(p for p in corpus_dir.iterdir() if p.is_dir()):
        if not (d / "strategy.pine").is_file():
            excluded.setdefault("not a probe (no strategy.pine)", []).append(d.name)
            continue
        meta = {}
        if (d / "inputs.json").is_file():
            meta = json.loads((d / "inputs.json").read_text(encoding="utf-8"))
        feed_ref = str(meta.get("ohlcv_csv", ""))
        if ("_1m" in feed_ref or str(meta.get("input_tf", "")) == "1"
                or meta.get("aux_security_ohlcv_csv") or meta.get("native_security_feeds")):
            excluded.setdefault("needs the 1m corpus feed (bench ships 15m only)", []).append(d.name)
            continue
        tape = d / str(meta.get("tv_trades_csv", "tv_trades.csv"))
        facts = tape_facts(tape, tv_tz(meta))
        if facts["tvTrades"] < MIN_TV_TRADES_CORPUS:
            excluded.setdefault(f"fewer than {MIN_TV_TRADES_CORPUS} TV trades", []).append(d.name)
            continue
        if not covered(facts, feed):
            excluded.setdefault("TV tape window not inside the bench feed", []).append(d.name)
            continue
        eligible.append({"key": d.name, "family": family_of(d.name),
                         "tapeName": tape.name, "tapeSha256": sha256_file(tape), **facts})
    return eligible, excluded


def closed_candidates(population: dict, scrapper_data: Path, verify_rows: dict[str, dict],
                      feed: dict) -> tuple[list[dict], dict[str, list[str]], int]:
    eligible: list[dict] = []
    excluded: dict[str, list[str]] = {}
    lane = [p for p in population["probes"]
            if p["source"] == "scrapper" and p["symbol"] == CLOSED_SYMBOL
            and p["timeframe"] == CLOSED_TIMEFRAME]
    for p in sorted(lane, key=lambda p: p["slug"]):
        if p["surface"] not in ("hard", "target"):
            excluded.setdefault(f"surface {p['surface']}", []).append(p["slug"])
            continue
        tape = scrapper_data / p["group"] / p["slug"] / "tv_trades.csv"
        if not tape.is_file() or sha256_file(tape) != p["tvTradesSha256"]:
            excluded.setdefault("tape bytes differ from the population pin", []).append(p["slug"])
            continue
        report = verify_rows.get(p["slug"])
        feed_note = str(((report or {}).get("document") or {}).get("feed", ""))
        if "1m" in feed_note:
            excluded.setdefault("campaign runs it on the 1m feed (finer-TF / lower_tf security)", []).append(p["slug"])
            continue
        facts = tape_facts(tape, timezone(timedelta(hours=8)))
        if not covered(facts, feed):
            excluded.setdefault("TV tape window not inside the bench feed", []).append(p["slug"])
            continue
        eligible.append({"key": p["slug"], "probeId": p["probeId"], "surface": p["surface"],
                         "group": p["group"], "populationTvTradeRows": p.get("tvTradeRows"),
                         "tapeSha256": p["tvTradesSha256"], **facts})
    return eligible, excluded, len(lane)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=SEED)
    ap.add_argument("--corpus-dir", type=Path, default=REPO_ROOT / "corpus" / "validation")
    ap.add_argument("--feed", type=Path, default=DATA / "ETHUSDT_15.csv")
    ap.add_argument("--population", type=Path, required=True,
                    help="population document (lab evidence get <population fileSha256>)")
    ap.add_argument("--verify-facts", type=Path, required=True,
                    help="campaign verify-report export ({experimentId, rows:[{slug, document}]})")
    ap.add_argument("--scrapper-data", type=Path,
                    default=Path(os.environ.get("PINESCRIPT_SCRAPPER_DIR",
                                                Path.home() / "code" / "pinescript-scrapper")) / "data")
    ap.add_argument("--out-dir", type=Path, default=BENCH / "results")
    ap.add_argument("--replace", action="append", default=[], metavar='NNN="reason"',
                    help="slot number lost to a PyneSys compile rejection (repeatable)")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    feed = feed_span(args.feed)
    population = json.loads(args.population.read_text(encoding="utf-8"))
    facts_doc = json.loads(args.verify_facts.read_text(encoding="utf-8"))
    verify_rows = {r["slug"]: r for r in facts_doc["rows"]}
    corpus_gitlink = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "ls-tree", "HEAD", "corpus"],
        capture_output=True, text=True, check=True).stdout.split()[2]

    c_eligible, c_excluded = corpus_candidates(args.corpus_dir, feed)
    fam_sizes: dict[str, int] = {}
    for m in c_eligible:
        fam_sizes[m["family"]] = fam_sizes.get(m["family"], 0) + 1
    fam_alloc = allocate(fam_sizes, N_CORPUS)
    corpus_picks = []
    for family in sorted(fam_alloc):
        members = [m for m in c_eligible if m["family"] == family]
        for pick, b, queue in quantile_draw(members, fam_alloc[family], rng):
            corpus_picks.append({**pick, "bin": b, "replacements": queue})

    s_eligible, s_excluded, s_lane = closed_candidates(
        population, args.scrapper_data, verify_rows, feed)
    surf_sizes: dict[str, int] = {}
    for m in s_eligible:
        surf_sizes[m["surface"]] = surf_sizes.get(m["surface"], 0) + 1
    surf_alloc = allocate(surf_sizes, N_CLOSED)
    closed_picks = []
    for surface in sorted(surf_alloc):
        members = [m for m in s_eligible if m["surface"] == surface]
        for pick, b, queue in quantile_draw(members, surf_alloc[surface], rng):
            closed_picks.append({**pick, "bin": b, "replacements": queue})

    slots = []
    for i, m in enumerate(sorted(corpus_picks, key=lambda m: m["key"]), start=1):
        slots.append({
            "slot": f"{i:03d}-{m['key']}", "source": "corpus",
            "probeId": f"corpus:corpus/validation/{m['key']}",
            "corpusPath": f"corpus/validation/{m['key']}",
            "stratum": m["family"], "bin": m["bin"],
            "tvTrades": m["tvTrades"], "tvRows": m["tvRows"],
            "window": [iso(m["windowStartMs"]), iso(m["windowEndMs"])],
            "tape": m["tapeName"], "tapeSha256": m["tapeSha256"], "license": CORPUS_LICENSE,
            "replacements": m["replacements"],
        })
    for i, m in enumerate(sorted(closed_picks, key=lambda m: m["key"]), start=N_CORPUS + 1):
        slots.append({
            "slot": f"{i:03d}-{m['key']}", "source": "closed",
            "probeId": m["probeId"], "stratum": m["surface"], "bin": m["bin"],
            "tvTrades": m["tvTrades"], "tvRows": m["tvRows"],
            "populationTvTradeRows": m["populationTvTradeRows"],
            "window": [iso(m["windowStartMs"]), iso(m["windowEndMs"])],
            "tapeSha256": m["tapeSha256"], "license": CLOSED_LICENSE,
            "replacements": m["replacements"],
        })

    by_key = {m["key"]: m for m in c_eligible + s_eligible}
    selected = {x["slot"].split("-", 1)[1] for x in slots}
    for item in args.replace:
        number, _, reason = item.partition("=")
        lost = next(x for x in slots if int(x["slot"][:3]) == int(number))
        key = next(k for k in lost["replacements"] if k not in selected)
        selected.add(key)
        m = by_key[key]
        lost["lost"] = {"reason": reason.strip()}
        new = {k: v for k, v in lost.items() if k not in ("lost", "replacements")}
        new.update({
            "slot": f"{len(slots) + 1:03d}-{key}",
            "tvTrades": m["tvTrades"], "tvRows": m["tvRows"],
            "window": [iso(m["windowStartMs"]), iso(m["windowEndMs"])],
            "tapeSha256": m["tapeSha256"], "replaces": lost["slot"],
            "replacements": [k for k in lost["replacements"] if k not in selected],
        })
        if lost["source"] == "closed":
            new.update({"probeId": m["probeId"], "populationTvTradeRows": m["populationTvTradeRows"]})
        else:
            new.update({"probeId": f"corpus:corpus/validation/{key}",
                        "corpusPath": f"corpus/validation/{key}", "tape": m["tapeName"]})
        slots.append(new)

    manifest = {
        "schema": "pineforge-bench-selection/v1",
        "seed": args.seed,
        "inputs": {
            "corpusGitlink": corpus_gitlink,
            "populationVersionSha256": population.get("sha256"),
            "populationFileSha256": sha256_file(args.population),
            "populationHeads": population.get("heads"),
            "verifyFactsExperimentId": facts_doc.get("experimentId"),
            "verifyFactsSha256": sha256_file(args.verify_facts),
            "benchFeed": feed,
        },
        "counts": {
            "corpus": {
                "probeDirs": len(c_eligible) + sum(len(v) for v in c_excluded.values()),
                "excluded": {k: len(v) for k, v in sorted(c_excluded.items())},
                "eligible": len(c_eligible),
                "families": {f: {"eligible": fam_sizes[f], "slots": fam_alloc[f]}
                             for f in sorted(fam_alloc)},
            },
            "closed": {
                "laneProbes": s_lane,
                "excluded": {k: len(v) for k, v in sorted(s_excluded.items())},
                "eligible": len(s_eligible),
                "surfaces": {s: {"eligible": surf_sizes[s], "slots": surf_alloc[s]}
                             for s in sorted(surf_alloc)},
            },
        },
        "excludedCorpus": {k: v for k, v in sorted(c_excluded.items())},
        "slots": slots,
    }
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "selection.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    (args.out_dir / "selection.md").write_text(render_md(manifest), encoding="utf-8")
    print(render_md(manifest))
    return 0


def render_md(m: dict) -> str:
    c, s = m["counts"]["corpus"], m["counts"]["closed"]
    feed = m["inputs"]["benchFeed"]
    replaced = [x for x in m["slots"] if "replaces" in x]
    lines = [
        "# Benchmark population selection",
        "",
        f"Seed **{m['seed']}** · `benchmarks/select_population.py` · "
        f"{len(m['slots']) - len(replaced)} slots = "
        f"{sum(1 for x in m['slots'] if x['source'] == 'corpus' and 'replaces' not in x)} corpus + "
        f"{sum(1 for x in m['slots'] if x['source'] == 'closed' and 'replaces' not in x)} closed"
        + (f", plus {len(replaced)} replacement slot(s) for PyneSys compile rejections "
           "(the rejected slot is kept)." if replaced else "."),
        "",
        "## Inputs",
        "",
        f"- Corpus gitlink: `{m['inputs']['corpusGitlink']}` (`corpus/validation/`)",
        f"- Population version `{m['inputs']['populationVersionSha256']}` "
        f"(document sha256 `{m['inputs']['populationFileSha256']}`, "
        f"heads {json.dumps(m['inputs']['populationHeads'], sort_keys=True)})",
        f"- Campaign verify reports: experiment `{m['inputs']['verifyFactsExperimentId']}` "
        f"(export sha256 `{m['inputs']['verifyFactsSha256']}`)",
        f"- Bench feed: `{feed['path']}` sha256 `{feed['sha256']}`, {feed['bars']:,} bars, "
        f"{iso(feed['firstMs'])} → {iso(feed['lastMs'])} UTC",
        "",
        "## Rule",
        "",
        "- **Corpus 100:** probes under `corpus/validation/` with ≥ 5 closed TV trades, a TV window inside the "
        "bench feed and no dependency on the 1m corpus feed; classified into mechanism families by probe name "
        "(first matching rule in `FAMILY_RULES`); slots allocated per family in proportion to family size with "
        "at least one each (largest remainder); within a family, members sorted by TV trade count are cut into "
        "as many contiguous quantile bins as the family has slots and one member is drawn per bin.",
        "- **Closed 100:** population probes with `source == \"scrapper\"`, `symbol == \"BINANCE:ETHUSDT.P\"`, "
        "`timeframe == \"15\"` — the only dataset the bench feed (Binance ETH/USDT-USDT perp 15m) and the TV "
        "tapes agree on; symbols are not mixed. Anomaly surface excluded; tape bytes must hash to the "
        "population pin; excluded when the TV window is not inside the bench feed or the campaign runs the "
        "script on the 1m feed (the 15m bench feed cannot serve finer-TF or `request.security_lower_tf` bars). "
        "Slots allocated to `hard`/`target` in their population proportion, then drawn by TV trade-count "
        "quantile bins as above.",
        "- **Replacements:** each slot lists the other members of its bin in seeded order; a slot lost to a "
        "PyneSys compile rejection is backed by the first compilable entry of that queue.",
        "",
        "## Stratification counts",
        "",
        f"Corpus: {c['probeDirs']} directories → {c['eligible']} eligible "
        f"(excluded: {', '.join(f'{k} {v}' for k, v in c['excluded'].items()) or 'none'}).",
        "",
        "| Family | Eligible | Slots |",
        "|---|---:|---:|",
    ]
    for f, v in c["families"].items():
        lines.append(f"| {f} | {v['eligible']} | {v['slots']} |")
    lines += [
        "",
        f"Closed: {s['laneProbes']} BINANCE:ETHUSDT.P 15 scraped probes → {s['eligible']} eligible "
        f"(excluded: {', '.join(f'{k} {v}' for k, v in s['excluded'].items()) or 'none'}).",
        "",
        "| Surface | Eligible | Slots |",
        "|---|---:|---:|",
    ]
    for f, v in s["surfaces"].items():
        lines.append(f"| {f} | {v['eligible']} | {v['slots']} |")
    lines += [
        "",
        "## Manifest",
        "",
        "| Slot | Source | Probe id / corpus path | TV trades (CSV rows) | Family / surface | Bin | License / provenance |",
        "|---|---|---|---:|---|---:|---|",
    ]
    for x in m["slots"]:
        where = x.get("corpusPath") or x["probeId"]
        lines.append(f"| {x['slot']} | {x['source']} | `{where}` | {x['tvTrades']} ({x['tvRows']}) | "
                     f"{x['stratum']} | {x['bin']} | {x['license']} |")
    lines.append("")
    if replaced:
        lines += ["## Replacements", "",
                  "| Lost slot | Reason | Replacement (same stratum and bin) |", "|---|---|---|"]
        for x in replaced:
            lost = next(y for y in m["slots"] if y["slot"] == x["replaces"])
            lines.append(f"| {lost['slot']} | {lost['lost']['reason']} | {x['slot']} |")
        lines.append("")
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
