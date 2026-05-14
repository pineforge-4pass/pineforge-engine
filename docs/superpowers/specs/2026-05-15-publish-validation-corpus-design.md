# Design — Publish validation corpus (clean-room rewrite)

Date: 2026-05-15
Status: approved (brainstorming)
Owner: PineForge contributors
Spec: `docs/superpowers/specs/2026-05-15-publish-validation-corpus-design.md`

## 1. Goal & scope

Make the `corpus/` repository publishable. To remove third-party IP risk
that currently prevents public release:

1. Replace `corpus/basic/*` (TradingView built-in strategy templates,
   TV/PineCoders IP) and `corpus/community/*` (mixed MPL-2.0 + unlicensed
   third-party scripts) with clean-room PineForge-original probes under
   `corpus/validation/`.
2. License every Pine file in the corpus under Apache-2.0 (matches the
   engine repo). Format: TradingView-style attribution + SPDX.
3. Every probe carries a rationale header explaining its validation goal.
4. Drop transpiler-output `generated.cpp` from the public tree.
5. Consolidate `corpus/validation_*/` topical sibling dirs and
   `corpus/parity-anomalies/` into a SINGLE `corpus/validation/` tree;
   anomalies tagged in the report, not segregated by path.
6. Regenerate `corpus/validation_report.{md,html,pdf}` so it covers
   every probe and explains every anomaly inline.

### Non-goals

- Engine code changes (except bugs surfaced by new probes — those are
  improvements, not part of the spec).
- Changing the OHLCV reference dataset.
- Redesigning the artifact tuple shape (still per-probe-dir; just shrinks
  from 4 files to 3 publicly).

## 2. Header template (license + rationale)

Every NEW probe `strategy.pine` opens with:

```pine
// This Pine Script® code is licensed under Apache-2.0
// SPDX-License-Identifier: Apache-2.0
// © PineForge contributors 2026
//
// PF probe NN — <one-line surface name>
// Clean-room: written from validation goal, not from source.
//
// Purpose: <2–6 lines. What engine surface this isolates. Why parity here
// matters (which engine subsystems touch this surface, what divergence
// pattern was historically observed or anticipated).>
//
// Validation goal: <1–2 lines. What "pass" looks like — bit-exact trades,
// order timing, regime classification match, etc.>
//
// TV setup: <chart, symbol, timeframe, dataset path>. Export "List of
// trades" → tv_trades.csv.
//@version=6
strategy(...)
```

EXISTING probes that already carry a `// <name> — <purpose>` block
(`ies-probe-*`, `vcp-probe-*`, `mtf-probe-*`, `parity-probe-*`,
`udt-method-probe-*`, `96-*`, etc.): prepend the 3-line license header;
keep the existing rationale block; SCRUB any references to original
third-party author handles (e.g., `officialjackofalltrades`,
`ChartPrime`) — replace with neutral phrasing.

EXISTING probes with NO rationale (numbered `01-…` through ~`30-…` etc.):
prepend license + add a minimal one-line rationale:

```pine
// This Pine Script® code is licensed under Apache-2.0
// SPDX-License-Identifier: Apache-2.0
// © PineForge contributors 2026
//
// PF probe 01 — MACD histogram reversal
// Purpose: parity test for ta.macd() tuple-return + crossover-driven entry.
//@version=6
```

## 3. Naming + numbering

New probe directories slot into the existing `validation/` namespace.

**`basic/` replacements (9 entries, numbered `97-…105-…`):**

| Old | New |
| --- | --- |
| `basic/greedy` | `validation/97-tp-sl-gap-reversal-oca` |
| `basic/inside-bar` | `validation/98-inside-bar-engulf` |
| `basic/keltner` | `validation/99-keltner-channel-break` |
| `basic/ma-cros` | `validation/100-ma-dual-cross` |
| `basic/parabolic-asr` | `validation/101-parabolic-sar-flip` |
| `basic/pivot-ext` | `validation/102-pivot-extension-break` |
| `basic/stochastic-slow` | `validation/103-stochastic-slow-cross` |
| `basic/supertrend` | `validation/104-supertrend-flip` |
| `basic/volty-expan` | `validation/105-volty-expansion-close` |

**Existing IES/VCP probe families — extend with integration probe:**

- `ies-probe-09-integration` (clean-room IES-style multi-layer composition)
- `vcp-probe-08-integration`

**9 uncovered community-script families — new probe families.** Family
slug uses topic/theme, NOT original author handle. Each family: 2–4
surface probes (highest-value parity surfaces) + 1 `-integration`
probe.

| Old `community/` | New family slug |
| --- | --- |
| `4ema_rsi` | `4ema-rsi-probe-*` |
| `BOS_curv` | `bos-curv-probe-*` |
| `kanuck` | `kanuck-probe-*` |
| `KKB` | `kkb-probe-*` |
| `LiquitySweep` | `liquidity-sweep-probe-*` |
| `MarketShift` | `market-shift-probe-*` |
| `scalping-strategy` | `scalping-probe-*` |
| `scalping-wunder-bots` | `wunder-scalper-probe-*` |
| `trendmaster` | `trendmaster-probe-*` |

Surface selection per family: pick the 2–4 surfaces most likely to expose
engine divergence (bespoke TA chains, MTF security flows, OCA grouping,
percent-of-equity sizing, etc.). Decided per-family during Phase 4
authorship.

### Single-tree consolidation

Final public layout: ONE `corpus/validation/` directory. Each subdir
holds the 3-file tuple (`strategy.pine`, `tv_trades.csv`,
`engine_trades.csv`). No topical sibling dirs.

Fold-in scope:

- `corpus/validation_barstate/` (2)
- `corpus/validation_lower_tf/` (3)
- `corpus/validation_magnifier/` (9)
- `corpus/validation_na_chain/` (1)
- `corpus/validation_oca/` (2)
- `corpus/validation_process_orders/` (2)
- `corpus/validation_pyramid/` (2)
- `corpus/validation_recompute/` (2)
- `corpus/validation_session/` (1)
- `corpus/validation_ta_isolate/` (12 probe entries; preserve existing
  `ta-isolate-NN-…` names)
- `corpus/validation_typed_matrix/` (2)
- `corpus/validation_varip/` (1)
- `corpus/parity-anomalies/equity-mirror` (1 known TV-side anomaly)

Naming on fold: each entry takes a topic-prefixed slug if it doesn't
already have one (e.g., `validation_barstate/foo` →
`validation/barstate-probe-foo`; `validation_oca/bar` →
`validation/oca-probe-bar`). Existing `ta-isolate-NN-…` entries keep
names verbatim. `parity-anomalies/equity-mirror` →
`validation/anomaly-equity-mirror`.

Anomalies are NOT a separate directory in the published tree. Each
probe's disposition (`excellent` / `strong` / `anomaly`) lives in the
parity-report aggregator (Section 6) as a per-probe row, not as a path
prefix.

## 4. Clean-room rewrite rule

Defensible posture vs derivative-work claims:

1. Rewrite from **validation goal**, not from source code. Probe author
   writes the header (purpose, validation goal, TV setup) BEFORE looking
   at the body of the original. Source script consulted only to identify
   the engine surface to exercise (e.g., "ta.dmi tuple",
   "request.security with gaps_on", "stop entry + cancel-opposite").
2. Use **PineForge-idiomatic** patterns: standard `input.int/float/bool`,
   built-in `ta.*` where the original used custom UDFs (unless the
   custom path IS the validation surface — then implement equivalent math
   in our own structure with our own variable names + comments).
3. **No copy-paste** from `community/*/strategy.pine`. Variable names,
   structure, group labels, comment style all written fresh.
4. **No author attribution carryover.** No `// © originalhandle` lines.
   No script-name echoes (e.g., "Integrated Execution System", "Kinetic
   Kalman Breakout") — use neutral PineForge probe names.
5. Existing `ies-probe-*` / `vcp-probe-*` rationale headers reference
   original author handles — strip during Phase 1 license sweep; replace
   with neutral phrasing ("a community multi-layer regime-adaptive
   strategy", etc.).

Each new probe carries a `// Clean-room: written from validation goal,
not from source.` line in its header to mark the posture.

## 5. Workflow + commit gating

Per-probe loop:

1. Author (Claude) writes `strategy.pine` (license header + rationale +
   body, clean-room).
2. User loads it on TradingView (chart + symbol + timeframe per the
   header's TV-setup line), exports "List of Trades" CSV →
   `corpus/validation/<probe>/tv_trades.csv`.
3. Author runs transpiler + harness → produces `generated.cpp` (local
   only) + `engine_trades.csv` + `strategy.dylib`.
4. Diff `engine_trades.csv` vs `tv_trades.csv`.
5. Mismatch → diagnose. Fix locus:
   - **Engine bug** → fix `src/`, regen, rerun. Bug = improvement.
   - **Probe bug** (typo, ambiguous setup) → edit `strategy.pine`, user
     re-exports.
   - **TV non-determinism** (rare; equity-mirror class) → tag probe as
     `anomaly` in report with write-up. Pre-Phase-5: park at
     `corpus/parity-anomalies/<slug>/`. Post-Phase-5 (consolidated
     tree): land at `corpus/validation/anomaly-<slug>/`.
6. Mark probe **excellent** / **strong** / **anomaly** per existing
   convention.

### Commit policy

- No commits until ALL probes in current phase have TV exports + engine
  artifacts + parity disposition.
- Commits land in phase batches per Section 7 sequencing.
- Phase 1 (license sweep) commits independently — no TV-export
  dependency.

### Bug-found = improvement

Any engine fix triggered by a new probe also re-runs the entire existing
parity sweep (`ctest`-driven) before commit, to confirm no regression.
New probe + engine fix can ship in the same commit.

## 6. Repo housekeeping (publish prep)

One-time changes when the corpus flips public:

1. `corpus/LICENSE` — add Apache-2.0 (matches engine).
2. `corpus/NOTICE` — list third-party data: Binance USDT-M futures OHLCV
   source attribution. Engine NOTICE stays as-is.
3. `corpus/LEGAL.md` — rewrite. Drop "private" framing. Document:
   - Apache-2.0 license for all `*.pine`.
   - `tv_trades.csv` provenance: emitted by TradingView's broker
     emulator on our own clean-room scripts; we hold the right to
     redistribute.
   - OHLCV provenance: cite Binance source + state factual-data posture.
   - `generated.cpp` NOT shipped publicly; reproducer must regen via
     closed-source transpiler.
   - Trademark notice for TradingView/PineScript stays.
4. `corpus/README.md` — rewrite "Layout" section: drop `basic/` +
   `community/` + all `validation_*/` topical subsections + the
   `parity-anomalies/` block. Show single `validation/` tree only.
   Update artifact tuple table 4→3 files. Update headline parity
   figure (recount excellent/strong/anomaly across consolidated
   `validation/*` total). Update strategy + probe tallies.
5. Engine repo `.gitmodules` — change submodule URL once corpus repo
   flips public (or change visibility on existing remote). [Open item C]
6. `corpus/CMakeLists.txt` — drop `basic/` + `community/` enumeration;
   ensure new `validation/*` entries auto-pick-up via existing glob
   (verify in Phase 5).
7. `corpus/.gitignore` — add `**/generated.cpp` and `**/strategy.dylib`.
8. `corpus/validation_report.{md,html,pdf}` — regenerate to cover the
   single consolidated `validation/` tree. Report columns: probe slug,
   topic tag, disposition (`excellent` / `strong` / `anomaly`), trade-
   count delta, notes. Anomaly rows include a one-paragraph
   explanation (e.g., `anomaly-equity-mirror`: TV broker-emulator
   non-determinism at the 1× equity boundary; engine is correct).
   Headline parity figure recomputed from this aggregate.

## 7. Phased sequencing

### Phase 1 — license sweep + housekeeping foundation

Scope: existing PineForge-original probes only. No TV-export dependency.

- Add `corpus/LICENSE` (Apache-2.0).
- Prepend 3-line license header to every existing `strategy.pine` under
  `corpus/validation/`, `corpus/validation_*/`, `corpus/parity-anomalies/`.
- Scrub original third-party author references in
  `ies-probe-*`/`vcp-probe-*`/`mtf-probe-*` rationale headers.
- Add minimal one-line rationale to existing probes that lack one.
- Add `corpus/.gitignore` entries for `generated.cpp` + `strategy.dylib`.
- Keep `corpus/basic/` + `corpus/community/` intact for now (Phase 5
  deletes).

Commit gate: grep enforcement (zero matches for stripped author handles
in surviving files).

### Phase 2 — `basic/*` replacements (9 probes)

Author probes `97-…` through `105-…`. User exports TV CSVs. Engine runs.
Bugs fixed as encountered. Single commit when all 9 are excellent or
documented as anomalies.

### Phase 3 — IES/VCP integration probes (2 probes)

Author `ies-probe-09-integration` + `vcp-probe-08-integration`. User
exports. Engine runs. Single commit.

### Phase 4 — uncovered community-script families (9 families)

Author 2–4 surface probes + 1 integration probe per family. Batch by
complexity (small families like `4ema-rsi-probe-*` first; `trendmaster-*`
last). Commit per family or per pair of families.

### Phase 5 — consolidate, regenerate reports, flip public

- Delete `corpus/basic/` + `corpus/community/` (and their TV/engine/cpp
  artifacts).
- Fold all `corpus/validation_*/` topical dirs and
  `corpus/parity-anomalies/` into single `corpus/validation/` per
  Section 3 single-tree consolidation. Apply renames.
- Strip `generated.cpp` from every `corpus/validation/*/`.
- Verify `corpus/.gitignore` entries from Phase 1 still cover the
  consolidated tree.
- Regenerate `corpus/validation_report.{md,html,pdf}` covering the full
  consolidated tree, with anomaly rows + explanations.
- Rewrite `corpus/README.md` (Layout, tuple table, headline parity
  figure, point at consolidated report).
- Rewrite `corpus/LEGAL.md` (drop private framing).
- Add `corpus/LICENSE` (Apache-2.0 — also done in Phase 1; verify).
- Add `corpus/NOTICE`.
- Update `corpus/CMakeLists.txt` if needed (remove
  `validation_*`/`basic`/`community` add_subdirectory entries).
- Verify engine harness (`ctest` driver) runs without checked-in
  `generated.cpp` and against the consolidated `validation/` glob.
- Update engine repo's `CONTRIBUTING.md` and `.gitmodules`.
- Resolve open item C (submodule URL + visibility mechanics).
- Flip corpus repo visibility public.

## 8. Risks + open items

### Risks

1. **TV-export burden.** ~45–60 new probes = 45–60 manual TV chart loads
   + exports. Pace-bound by user, not author.
2. **Headline parity number changes.** Removing `basic/`+`community/`
   and adding ~50 new probes shifts the existing 165/167 figure. New
   baseline TBD; recount in Phase 5.
3. **Existing rationale headers reference original authors** (e.g.,
   `ies-probe-01` mentions `officialjackofalltrades`). License sweep
   MUST scrub these — easy to miss. Grep enforcement step in Phase 1.
4. **Submodule URL change.** Once public, existing private clones with
   token URL break. Document in engine `CONTRIBUTING.md`.
5. **Engine harness assumes `generated.cpp` checked in.** Phase 5 must
   verify the build/run path works when transpiler regenerates it
   locally on first run.

### Open items (decide before relevant phase)

- **A. OHLCV data fate** — RESOLVED: stays in repo. Binance public
  market data, safe to redistribute.
- **B. `generated.cpp` publishability** — RESOLVED: dropped from public
  tree. Reproducer regenerates via closed-source transpiler.
- **C. Submodule remote URL + repo-visibility mechanics** — TBD before
  Phase 5.

## 9. Success criteria

- `corpus/` repo can be set public with no third-party IP carryover.
- Final layout: single `corpus/validation/` tree; each subdir has the
  3-file tuple `strategy.pine` + `tv_trades.csv` + `engine_trades.csv`.
- No `corpus/basic/`, `corpus/community/`, `corpus/validation_*/`, or
  `corpus/parity-anomalies/` directories remain.
- Every `*.pine` under `corpus/` carries the license + rationale header.
- `grep` for stripped author handles returns zero matches.
- `corpus/validation_report.{md,html,pdf}` covers every probe, lists
  disposition (`excellent` / `strong` / `anomaly`), and includes a
  one-paragraph explanation for each anomaly row.
- Headline parity figure recomputed from the consolidated report and
  documented in `corpus/README.md`.
- `ctest`-driven parity sweep stays green throughout (red only allowed
  during a single phase's open work, never on a commit).
