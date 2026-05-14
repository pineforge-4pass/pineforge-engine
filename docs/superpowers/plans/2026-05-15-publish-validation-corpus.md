# Publish Validation Corpus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `corpus/` repo publishable: clean-room rewrite of every `basic/` + `community/` script as new probes under a single `validation/` tree, license every Pine file Apache-2.0, scrub author/source references, drop `generated.cpp` from public tree, regenerate validation report, then flip the corpus submodule public.

**Architecture:** 5 sequential phases. Phase 1 = author-only (license sweep over existing files). Phases 2–4 = per-probe loop, human-gated on TradingView CSV exports. Phase 5 = consolidation + report regen + repo flip.

**Tech Stack:** PineScript v6, Apache-2.0, CMake (`PINEFORGE_BUILD_CORPUS_STRATEGIES`), Python harness (`scripts/run_strategy.py`, `scripts/run_corpus.sh`, `scripts/verify_corpus.py`).

**Spec:** `docs/superpowers/specs/2026-05-15-publish-validation-corpus-design.md`

---

## File Structure

**New files:**
- `corpus/LICENSE` — Apache-2.0 text (verbatim copy of `LICENSE` from engine root).
- `corpus/.gitignore` — ignore `**/generated.cpp`, `**/strategy.dylib`, `**/strategy.so`, `**/strategy.dll`.
- `corpus/NOTICE` — Apache-2.0 NOTICE; OHLCV (Binance) attribution.
- `corpus/validation/97-tp-sl-gap-reversal-oca/strategy.pine` (and 8 more for Phase 2: 98-…105-…).
- `corpus/validation/ies-probe-09-integration/strategy.pine`.
- `corpus/validation/vcp-probe-08-integration/strategy.pine`.
- `corpus/validation/<family>-probe-NN-<surface>/strategy.pine` for 9 community families (Phase 4).
- `corpus/validation/anomaly-equity-mirror/` (renamed from `corpus/parity-anomalies/equity-mirror`, Phase 5).

**Modified files:**
- Every existing `corpus/validation/*/strategy.pine`, `corpus/validation_*/*/strategy.pine`, `corpus/parity-anomalies/*/strategy.pine` → prepend license header; for ones missing a rationale, add a one-line `// Purpose:` line; replace `community/IES` / `community/VCP` / `community/<name>` path references with neutral phrasing.
- `corpus/CMakeLists.txt` → drop `basic/`, `community/`, `validation_*/`, `parity-anomalies/` glob entries (Phase 5).
- `corpus/README.md` → rewrite Layout section, tuple table 4→3 files, recompute headline parity (Phase 5).
- `corpus/LEGAL.md` → drop "private" framing (Phase 5).
- `scripts/verify_corpus.py` → update `--all` category list, drop `parity-anomalies` references (Phase 5).
- `scripts/run_corpus.sh` → comment "162 strategy targets" → updated count (Phase 5).
- `.gitmodules` (engine root) → potential URL change (Phase 5, deferred to open item C).

**Deleted (Phase 5):**
- `corpus/basic/` (whole subtree).
- `corpus/community/` (whole subtree).
- All `corpus/validation_*/` topical sibling dirs (entries `git mv`'d into `corpus/validation/` first).
- `corpus/parity-anomalies/` (entry `git mv`'d, README absorbed into corpus README).
- Every checked-in `generated.cpp` and `strategy.dylib` / `strategy.so` under `corpus/`.

---

## License header constants (used in many tasks)

**3-line license header (prepend before `//@version=6`):**

```
// This Pine Script® code is licensed under Apache-2.0
// SPDX-License-Identifier: Apache-2.0
// © PineForge contributors 2026
```

**Minimal one-line rationale form (for existing probes that lack a rationale block):**

```
// PF probe <slug> — <one-line surface name>
// Purpose: <short, one-line>
```

**Full new-probe rationale template (Phases 2–4):**

```
// PF probe <slug> — <one-line surface name>
// Clean-room: written from validation goal, not from source.
//
// Purpose: <2–6 lines. What engine surface this isolates. Why parity here matters.>
//
// Validation goal: <1–2 lines. What "pass" looks like.>
//
// TV setup: <chart, symbol, timeframe, dataset path>. Export "List of trades" → tv_trades.csv.
```

---

## Phase 1 — License sweep + housekeeping foundation

No TV-export dependency. All authoring + scripted. Single commit at end.

### Task 1.1: Add `corpus/LICENSE`

**Files:**
- Create: `corpus/LICENSE` (copy of `/Users/haoliangwen/code/pineforge-engine/LICENSE`)

- [ ] **Step 1: Verify engine LICENSE is Apache-2.0**

Run:
```bash
head -3 /Users/haoliangwen/code/pineforge-engine/LICENSE
```

Expected:
```
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/
```

- [ ] **Step 2: Copy to corpus/**

Run:
```bash
cp /Users/haoliangwen/code/pineforge-engine/LICENSE /Users/haoliangwen/code/pineforge-engine/corpus/LICENSE
```

- [ ] **Step 3: Verify copy**

Run:
```bash
diff /Users/haoliangwen/code/pineforge-engine/LICENSE /Users/haoliangwen/code/pineforge-engine/corpus/LICENSE && echo OK
```

Expected: `OK`

### Task 1.2: Add `corpus/.gitignore`

**Files:**
- Create: `corpus/.gitignore`

- [ ] **Step 1: Write `.gitignore`**

Write file `/Users/haoliangwen/code/pineforge-engine/corpus/.gitignore`:
```
# Build / transpiler outputs — regenerated locally, not redistributed.
**/generated.cpp
**/strategy.dylib
**/strategy.so
**/strategy.dll
```

### Task 1.3: Audit author + source references in current `validation/` tree

**Files:**
- Read-only: `corpus/validation/**/strategy.pine`, `corpus/validation_*/*/strategy.pine`, `corpus/parity-anomalies/*/strategy.pine`

- [ ] **Step 1: Run reference grep**

Run:
```bash
grep -rn 'community/IES\|community/VCP\|community/MarketShift\|community/LiquitySweep\|community/4ema_rsi\|community/BOS_curv\|community/kanuck\|community/KKB\|community/scalping-strategy\|community/scalping-wunder-bots\|community/trendmaster\|JOAT\|officialjackofalltrades\|ChartPrime\|AIScripts\|WunderTrading\|Effort 01\|Canuck\|Kinetic Kalman\|TrendMaster Pro' /Users/haoliangwen/code/pineforge-engine/corpus/validation /Users/haoliangwen/code/pineforge-engine/corpus/validation_* /Users/haoliangwen/code/pineforge-engine/corpus/parity-anomalies --include='*.pine' > /tmp/corpus_refs.txt
wc -l /tmp/corpus_refs.txt
cat /tmp/corpus_refs.txt
```

Expected: file lists ~23 hits; mostly path-style `community/IES`, `community/VCP`. Confirm no actual handles (`officialjackofalltrades`, `ChartPrime`, `AIScripts`, `WunderTrading`, `Effort 01`) appear in `validation/` tree (verified during plan-write: only path mentions present).

### Task 1.4: Scrub source-script path references

For every file listed in `/tmp/corpus_refs.txt`:
- Replace `community/IES`'s bespoke regime detector → "the multi-layer regime-adaptive integration probe in this tree"
- Replace `community/IES` → "the IES integration probe family" (or theme-neutral phrasing per context)
- Replace `community/VCP` → "the VCP integration probe family"
- Replace `community/MarketShift` → "the market-shift integration probe family"
- Replace `JOAT` → drop entirely
- Path-style `community/<name>` mentions → neutral phrasing referencing the new family slug or removed.

- [ ] **Step 1: Apply replacements file-by-file**

For each file in the grep output, use Edit tool to replace each occurrence with neutral phrasing. Maintain comment context (don't break sentence structure). Examples:

In `corpus/validation/ies-probe-01-adx-regime-classify/strategy.pine`, replace:
```
// Purpose: isolate community/IES's bespoke regime detector. IES does NOT use
```
with:
```
// Purpose: isolate the multi-layer regime-adaptive integration probe's bespoke
// regime detector. That probe does NOT use
```

In `corpus/validation/parity-probe-02-choch-bos-isolator/strategy.pine` line 32, replace:
```
// ---- swing tracking (IDENTICAL signature to community/VCP) ----------
```
with:
```
// ---- swing tracking (matches the VCP integration probe family) -------
```

(Apply analogous edits to all 23 hits. Each is a small Edit per file.)

- [ ] **Step 2: Re-run grep to confirm zero matches in validation tree**

Run:
```bash
grep -rn 'community/IES\|community/VCP\|community/MarketShift\|community/LiquitySweep\|community/4ema_rsi\|community/BOS_curv\|community/kanuck\|community/KKB\|community/scalping-strategy\|community/scalping-wunder-bots\|community/trendmaster\|JOAT\|officialjackofalltrades\|ChartPrime\|AIScripts\|WunderTrading\|Effort 01\|Canuck\|Kinetic Kalman\|TrendMaster Pro' /Users/haoliangwen/code/pineforge-engine/corpus/validation /Users/haoliangwen/code/pineforge-engine/corpus/validation_* /Users/haoliangwen/code/pineforge-engine/corpus/parity-anomalies --include='*.pine' | wc -l
```

Expected: `0`

### Task 1.5: Prepend license header to every existing `*.pine` in validation tree

**Files:**
- Modify: every `corpus/validation/*/strategy.pine`, `corpus/validation_*/*/strategy.pine`, `corpus/parity-anomalies/*/strategy.pine` (basic/ + community/ NOT touched here — they will be deleted in Phase 5).

- [ ] **Step 1: Build file list**

Run:
```bash
find /Users/haoliangwen/code/pineforge-engine/corpus/validation /Users/haoliangwen/code/pineforge-engine/corpus/validation_* /Users/haoliangwen/code/pineforge-engine/corpus/parity-anomalies -name strategy.pine 2>/dev/null > /tmp/corpus_pines.txt
wc -l /tmp/corpus_pines.txt
```

Expected: ~190 files (147 `validation/` + 12 `validation_ta_isolate/` + ~30 across other `validation_*/` + 1 `parity-anomalies/`).

- [ ] **Step 2: Write a one-shot license-prepend script**

Write file `/tmp/prepend_license.py`:
```python
#!/usr/bin/env python3
"""Prepend Apache-2.0 license header to every Pine file in a list, idempotent."""
import sys
from pathlib import Path

HEADER = (
    "// This Pine Script® code is licensed under Apache-2.0\n"
    "// SPDX-License-Identifier: Apache-2.0\n"
    "// © PineForge contributors 2026\n"
    "//\n"
)
MARKER = "SPDX-License-Identifier: Apache-2.0"

def process(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return "skip"
    path.write_text(HEADER + text, encoding="utf-8")
    return "added"

def main(list_file: str) -> None:
    counts = {"added": 0, "skip": 0}
    for line in Path(list_file).read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        result = process(Path(line))
        counts[result] += 1
    print(f"added={counts['added']} skip={counts['skip']}")

if __name__ == "__main__":
    main(sys.argv[1])
```

- [ ] **Step 3: Run it**

Run:
```bash
python3 /tmp/prepend_license.py /tmp/corpus_pines.txt
```

Expected: `added=<N> skip=0` where N matches `wc -l` output of step 1.

- [ ] **Step 4: Spot-check three files**

Run:
```bash
head -5 /Users/haoliangwen/code/pineforge-engine/corpus/validation/01-macd-histogram/strategy.pine
echo ---
head -5 /Users/haoliangwen/code/pineforge-engine/corpus/validation/ies-probe-01-adx-regime-classify/strategy.pine
echo ---
head -5 /Users/haoliangwen/code/pineforge-engine/corpus/validation_ta_isolate/ta-isolate-01-rsi14-cross-50/strategy.pine
```

Expected: each starts with the 3-line license + blank `//` line.

- [ ] **Step 5: Re-run script (idempotency check)**

Run:
```bash
python3 /tmp/prepend_license.py /tmp/corpus_pines.txt
```

Expected: `added=0 skip=<N>`

### Task 1.6: Add minimal one-line rationale to probes that lack one

**Files:**
- Modify: existing `corpus/validation/01-…` through `corpus/validation/30-…` (and any other lacking a `// Purpose:` line) — identify by grep.

- [ ] **Step 1: Find probes missing a Purpose line**

Run:
```bash
for f in $(cat /tmp/corpus_pines.txt); do
  if ! grep -q "^// Purpose:" "$f"; then
    echo "$f"
  fi
done > /tmp/no_rationale.txt
wc -l /tmp/no_rationale.txt
head -20 /tmp/no_rationale.txt
```

- [ ] **Step 2: For each file in `/tmp/no_rationale.txt`, manually craft a one-line rationale**

For each path, read the first 30 lines of the file (already has license + `//` separator + `//@version=6` + `strategy(...)` + body). Identify the validation goal from indicator/order calls (e.g., `ta.macd` + `ta.crossover` → "MACD tuple-return + crossover entry").

Use Edit tool to insert rationale block AFTER the license `//\n` separator and BEFORE `//@version=6`:

Example for `corpus/validation/01-macd-histogram/strategy.pine` (already shown in spec § 2):

old_string:
```
// © PineForge contributors 2026
//
//@version=6
```

new_string:
```
// © PineForge contributors 2026
//
// PF probe 01 — MACD histogram reversal
// Purpose: parity test for ta.macd() tuple-return + crossover-driven entry.
//
//@version=6
```

(Apply analogous one-liners to every file in `/tmp/no_rationale.txt`. Each is one Edit.)

- [ ] **Step 3: Confirm every Pine file now has a Purpose line**

Run:
```bash
for f in $(cat /tmp/corpus_pines.txt); do
  if ! grep -q "^// Purpose:" "$f"; then
    echo "MISSING: $f"
  fi
done
```

Expected: no output.

### Task 1.7: Phase 1 commit

- [ ] **Step 1: Stage**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
git -C corpus add LICENSE .gitignore
git -C corpus add validation validation_* parity-anomalies
git -C corpus status --short
```

Expected: shows new LICENSE, .gitignore, and modified strategy.pine files. No `basic/` or `community/` changes.

- [ ] **Step 2: Commit**

Run:
```bash
git -C corpus commit -m "chore(license): add Apache-2.0 header + LICENSE + .gitignore

Prepend SPDX/Apache-2.0 header to every existing PineForge-original
*.pine under validation/, validation_*/, parity-anomalies/.

Add minimal one-line rationale to probes that lacked a // Purpose:
block. Replace community/IES, community/VCP, etc. path references
with neutral phrasing (basic/ + community/ trees deleted in a later
commit).

basic/ + community/ left intact for now; deleted in the consolidation
phase.

Refs: docs/superpowers/specs/2026-05-15-publish-validation-corpus-design.md"
```

- [ ] **Step 3: Bump submodule pointer in engine repo**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
git add corpus
git commit -m "chore(corpus): bump submodule for license-header sweep"
```

---

## Phase 2 — `basic/*` replacements (9 probes)

Each probe goes through the per-probe loop. **Strict commit gate: no commit until ALL 9 probes are excellent or documented as anomalies.**

### Per-probe loop template (used for every Phase 2 probe AND every Phase 3/4 probe)

For probe `<slug>`:

- [ ] **A. Author writes `strategy.pine`**

Create `/Users/haoliangwen/code/pineforge-engine/corpus/validation/<slug>/strategy.pine` with:
- 3-line license header (constants section).
- Full new-probe rationale (constants section).
- `//@version=6` and `strategy(...)` declaration with PineForge-standard arguments (`initial_capital=1000000`, `currency=currency.USD`, `commission_type=strategy.commission.percent`, `commission_value=0`, `slippage=0`, `default_qty_type=strategy.fixed`, `default_qty_value=1`, `process_orders_on_close=false`, `pyramiding=1`).
- Body: clean-room (from validation goal, not from source). PineForge-idiomatic (built-in `ta.*` unless the bespoke chain IS the validation surface).
- Variable names, comments, group labels written fresh — no carryover.

- [ ] **B. WAIT for user TV export** ⚠️ HUMAN GATE

Notify user with the probe's TV setup line (chart, symbol, timeframe, dataset). User loads it, exports "List of Trades" CSV → `corpus/validation/<slug>/tv_trades.csv`.

Do NOT proceed until file exists.

Confirm:
```bash
test -s /Users/haoliangwen/code/pineforge-engine/corpus/validation/<slug>/tv_trades.csv && head -2 /Users/haoliangwen/code/pineforge-engine/corpus/validation/<slug>/tv_trades.csv
```

- [ ] **C. Build the new probe target**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON -Wno-dev
```

Expected: configure prints "162 strategy targets" + new probe added to enumeration on subsequent re-configure (after transpiler runs).

Generate `generated.cpp` for the new probe (closed-source transpiler — invocation is environment-specific; if not auto-invoked, run the transpiler over the new `strategy.pine` to produce `corpus/validation/<slug>/generated.cpp`).

Then:
```bash
cmake --build build --target corpus_strategies -j
```

Expected: builds `corpus/validation/<slug>/strategy.dylib`.

- [ ] **D. Run the harness on the new probe only**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
ONLY="<slug>" SKIP_BUILD=1 SKIP_VERIFY=1 scripts/run_corpus.sh
```

Expected: `engine_trades.csv` written next to `strategy.dylib`. Copy it back into the source tree:
```bash
cp build/corpus/validation/<slug>/engine_trades.csv corpus/validation/<slug>/engine_trades.csv
```

(Or, if `run_corpus.sh` already writes into the source tree, skip the copy.)

- [ ] **E. Diff vs TV export**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
scripts/verify_corpus.py corpus/validation/<slug>
```

Expected: `excellent` or `strong` disposition.

- [ ] **F. Diagnose mismatch (only if not excellent)**

Decision:
- **Engine bug** → fix `src/`, regen `.dylib` (re-run step C), re-run step D, re-run step E. Then run full parity sweep `scripts/run_corpus.sh` (no `ONLY=`) to confirm no regression.
- **Probe bug** → edit `strategy.pine`. Notify user to re-export. Loop back to step B.
- **TV non-determinism** → tag as anomaly. Move dir from `corpus/validation/<slug>` → `corpus/validation/anomaly-<slug>` (Phase 5 anomaly-naming convention adopted early). Add a `notes.md` next to `strategy.pine` explaining the divergence pattern.

- [ ] **G. Mark disposition**

Append to `/tmp/phase_<N>_dispositions.txt`:
```
<slug>  excellent|strong|anomaly  <one-line note>
```

### Phase 2 probe list

Run the per-probe loop for each, in order:

- [ ] **2.1 — `97-tp-sl-gap-reversal-oca`** (replaces `basic/greedy`)

Surface: TP/SL bracket via `strategy.order` with `oca.reduce` group; gap-reversal entry on opposite-side gap-up/gap-down with `strategy.cancel` of pending stop entries; `max_intraday_filled_orders` risk gate.

TV setup line for header: `15m chart, ETH-USDT-USDT (Binance Perpetual), full window of corpus/data/ohlcv_ETH-USDT-USDT_15m.csv. Export "List of trades" → tv_trades.csv.`

- [ ] **2.2 — `98-inside-bar-engulf`** (replaces `basic/inside-bar`)

Surface: bar-relational pattern (`high < high[1] and low > low[1]`) gating `strategy.entry` long/short by candle direction.

- [ ] **2.3 — `99-keltner-channel-break`** (replaces `basic/keltner`)

Surface: `ta.ema` + `ta.atr` channel construction + `ta.crossover/crossunder` entries; verifies KC math + boundary-cross detection.

- [ ] **2.4 — `100-ma-dual-cross`** (replaces `basic/ma-cros`)

Surface: dual-MA crossover (`ta.sma` fast/slow + `ta.crossover`) — the quintessential parity baseline.

- [ ] **2.5 — `101-parabolic-sar-flip`** (replaces `basic/parabolic-asr`)

Surface: `ta.sar(start, increment, max)` flip detection + entry on flip; verifies SAR's stateful acceleration-factor logic.

- [ ] **2.6 — `102-pivot-extension-break`** (replaces `basic/pivot-ext`)

Surface: `ta.pivothigh` / `ta.pivotlow` + lookback offset semantics + breakout entry above last pivot.

- [ ] **2.7 — `103-stochastic-slow-cross`** (replaces `basic/stochastic-slow`)

Surface: `ta.stoch` + smoothing (`ta.sma` of %K) + crossover entry on %K-vs-%D; verifies dual-smoothing chain.

- [ ] **2.8 — `104-supertrend-flip`** (replaces `basic/supertrend`)

Surface: `ta.supertrend(factor, atrPeriod)` tuple return + direction-flip entry; verifies stateful direction tracking.

- [ ] **2.9 — `105-volty-expansion-close`** (replaces `basic/volty-expan`)

Surface: rolling stdev expansion (`ta.stdev`) + close-vs-band breakout entry; verifies stdev numeric stability.

### Phase 2 commit

- [ ] **Step 1: Confirm all 9 dispositions**

Run:
```bash
cat /tmp/phase_2_dispositions.txt | wc -l
```

Expected: `9`. Every line is `excellent`, `strong`, or `anomaly`.

- [ ] **Step 2: Stage + commit**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git add validation/97-* validation/98-* validation/99-* validation/100-* validation/101-* validation/102-* validation/103-* validation/104-* validation/105-* validation/anomaly-*
git status --short
git commit -m "feat(corpus): add probes 97-105 (basic/ replacements, clean-room)

Replaces basic/{greedy,inside-bar,keltner,ma-cros,parabolic-asr,
pivot-ext,stochastic-slow,supertrend,volty-expan} with PineForge-
original probes. basic/ subtree deleted in the consolidation commit.

Each probe: 3-line Apache-2.0 header + clean-room rationale + TV
export + engine trades. All <excellent|strong> per
scripts/verify_corpus.py.

Refs: docs/superpowers/specs/2026-05-15-publish-validation-corpus-design.md"
```

- [ ] **Step 3: Bump submodule pointer**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
git add corpus
git commit -m "chore(corpus): bump submodule for basic/ replacements"
```

---

## Phase 3 — IES/VCP integration probes (2 probes)

Apply the per-probe loop (Phase 2 template, steps A–G) for each:

- [ ] **3.1 — `ies-probe-09-integration`**

Surface: clean-room composition of regime + bias + momentum + structure + cooldown + sizing layers (the surfaces already isolated by `ies-probe-01..08`). Composes them into a single end-to-end strategy that proves they integrate parity-stably together.

Header rationale should reference the existing 8 surface probes: "Companion to `ies-probe-01..08`. Each of those isolates one layer; this probe proves their composition (`bias_score = regime_factor * (rsi_score + macd_score) + ...`) stays bit-exact under TradingView's broker emulator."

TV setup: same as `ies-probe-01` (15m, ETH-USDT-USDT, warmup-prepended OHLCV).

- [ ] **3.2 — `vcp-probe-08-integration`**

Surface: clean-room composition of pivot + FVG + momentum + volume-anomaly + CVD + ADX-regime + session layers (per existing `vcp-probe-01..07`).

Header rationale should reference the existing 7 surface probes. TV setup matches `vcp-probe-01`.

### Phase 3 commit

- [ ] **Step 1: Confirm dispositions**

Both excellent or strong.

- [ ] **Step 2: Stage + commit**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git add validation/ies-probe-09-integration validation/vcp-probe-08-integration
git commit -m "feat(corpus): add IES + VCP integration probes

Companion integration probes for the existing per-surface families:
- ies-probe-09-integration (composes 01..08)
- vcp-probe-08-integration (composes 01..07)

Both clean-room; both pass parity strict-profile.

Refs: docs/superpowers/specs/2026-05-15-publish-validation-corpus-design.md"
```

- [ ] **Step 3: Bump submodule pointer**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
git add corpus
git commit -m "chore(corpus): bump submodule for IES/VCP integration probes"
```

---

## Phase 4 — uncovered community-script families (9 families)

Per family: 2–4 surface probes + 1 integration probe. Surface selection done at family-authorship time (validation goals chosen from each script's most parity-relevant features).

Each probe goes through the per-probe loop (template above). Commit after each family is fully excellent/strong.

### Family 4.1 — `4ema-rsi-probe-*` (from `community/4ema_rsi`)

Original surface: 4 EMAs (slow + fast pair × HTF + LTF) + RSI pullback + binary-options-style entry timing.

Surface picks (3 probes + 1 integration):
- [ ] **4.1.1 — `4ema-rsi-probe-01-quad-ema-stack`**

  Surface: 4-EMA stacking order check (e.g., `ema8 > ema21 > ema55 > ema200`).

- [ ] **4.1.2 — `4ema-rsi-probe-02-rsi-pullback`**

  Surface: RSI dip-and-recover trigger (`ta.rsi` crossing pullback band).

- [ ] **4.1.3 — `4ema-rsi-probe-03-binary-bar-window`**

  Surface: bar-time / session-window gating around entry (binary-options-style fixed-expiry).

- [ ] **4.1.4 — `4ema-rsi-probe-integration`**

  Composition of 01–03.

- [ ] **4.1 commit:**

```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git add validation/4ema-rsi-probe-*
git commit -m "feat(corpus): add 4ema-rsi probe family (4 probes)"
cd /Users/haoliangwen/code/pineforge-engine
git add corpus
git commit -m "chore(corpus): bump submodule for 4ema-rsi family"
```

### Family 4.2 — `bos-curv-probe-*` (from `community/BOS_curv`)

Original surface: BOS (break-of-structure) wave detection with curved channel boundaries.

Surface picks (2 probes + 1 integration):
- [ ] **4.2.1 — `bos-curv-probe-01-swing-bos-trigger`**

  Surface: pivot-based BOS detection (`ta.pivothigh/low` + break test).

- [ ] **4.2.2 — `bos-curv-probe-02-curved-channel`**

  Surface: curved (polynomial / log) channel boundary computation.

- [ ] **4.2.3 — `bos-curv-probe-integration`**

- [ ] **4.2 commit** (analogous to 4.1).

### Family 4.3 — `kanuck-probe-*` (from `community/kanuck`, neutral slug)

Original: Canuck KAMA strategy (Kaufman Adaptive MA) with `calc_on_every_tick=true` + `max_bars_back=500`.

Surface picks (3 probes + 1 integration):
- [ ] **4.3.1 — `kanuck-probe-01-kama-state`**

  Surface: `ta.kama(length, fastEnd, slowEnd)` stateful adaptive smoothing.

- [ ] **4.3.2 — `kanuck-probe-02-calc-on-every-tick`**

  Surface: `calc_on_every_tick=true` evaluation semantics on the engine.

- [ ] **4.3.3 — `kanuck-probe-03-max-bars-back-500`**

  Surface: `max_bars_back=500` requirement; verifies engine handles deep history without truncation artefacts.

- [ ] **4.3.4 — `kanuck-probe-integration`**

- [ ] **4.3 commit.**

### Family 4.4 — `kkb-probe-*` (from `community/KKB`)

Original: Kinetic Kalman Breakout with `margin_long=100, margin_short=100`.

Surface picks (3 probes + 1 integration):
- [ ] **4.4.1 — `kkb-probe-01-kalman-filter`**

  Surface: per-bar Kalman update (state + covariance recurrence).

- [ ] **4.4.2 — `kkb-probe-02-breakout-trigger`**

  Surface: breakout cross above smoothed band.

- [ ] **4.4.3 — `kkb-probe-03-margin-100-pct`**

  Surface: `margin_long=100 / margin_short=100` sizing semantics in TV's broker emulator.

- [ ] **4.4.4 — `kkb-probe-integration`**

- [ ] **4.4 commit.**

### Family 4.5 — `liquidity-sweep-probe-*` (from `community/LiquitySweep`)

Original: liquidity-sweep (stop-hunt) detection at swing highs/lows.

Surface picks (3 probes + 1 integration):
- [ ] **4.5.1 — `liquidity-sweep-probe-01-pivot-swing`**

  Surface: pivot-high/-low detection with directional confirm.

- [ ] **4.5.2 — `liquidity-sweep-probe-02-sweep-bar`**

  Surface: sweep-bar pattern (wick beyond pivot, body closing back inside).

- [ ] **4.5.3 — `liquidity-sweep-probe-03-reentry-on-sweep`**

  Surface: opposite-direction entry after sweep confirmation.

- [ ] **4.5.4 — `liquidity-sweep-probe-integration`**

- [ ] **4.5 commit.**

### Family 4.6 — `market-shift-probe-*` (from `community/MarketShift`)

Original: ChartPrime "Market Shift" — regime + structure shift detection.

Surface picks (3 probes + 1 integration):
- [ ] **4.6.1 — `market-shift-probe-01-shift-state`**

  Surface: state machine that switches between bullish/bearish on structural shift.

- [ ] **4.6.2 — `market-shift-probe-02-rolling-extreme`**

  Surface: rolling-window highest/lowest tracking (`ta.highest`, `ta.lowest`).

- [ ] **4.6.3 — `market-shift-probe-03-shift-driven-entry`**

  Surface: entry triggered on `shift_state` transitions.

- [ ] **4.6.4 — `market-shift-probe-integration`**

- [ ] **4.6 commit.**

### Family 4.7 — `scalping-probe-*` (from `community/scalping-strategy`)

Original: "Scalping Strategy Improved v2" — short-term scalp with tight TP/SL.

Surface picks (2 probes + 1 integration):
- [ ] **4.7.1 — `scalping-probe-01-tight-tp-sl-points`**

  Surface: small-points TP/SL via `strategy.exit(stop=, limit=)` (no offset).

- [ ] **4.7.2 — `scalping-probe-02-fast-ma-trigger`**

  Surface: short-period EMA cross trigger (e.g., `ema(close, 5)` vs `ema(close, 13)`).

- [ ] **4.7.3 — `scalping-probe-integration`**

- [ ] **4.7 commit.**

### Family 4.8 — `wunder-scalper-probe-*` (from `community/scalping-wunder-bots`)

Original: WunderTrading-compatible reverse-logic scalper.

Surface picks (2 probes + 1 integration):
- [ ] **4.8.1 — `wunder-scalper-probe-01-reverse-on-signal`**

  Surface: explicit reverse-position logic (close opposite + open same-direction) within one bar.

- [ ] **4.8.2 — `wunder-scalper-probe-02-alert-message-templates`**

  Surface: alert-message placeholder strings (`{{strategy.order.action}}`, etc.) — verifies engine's alert hook substitution if exercised; if engine doesn't run alerts, drop this probe and pick another surface.

- [ ] **4.8.3 — `wunder-scalper-probe-integration`**

- [ ] **4.8 commit.**

### Family 4.9 — `trendmaster-probe-*` (from `community/trendmaster`)

Original: "TrendMaster Pro 2.3 with Alerts" — multi-line label/level visualisation + alerts. `max_lines_count=500`.

Surface picks (4 probes + 1 integration):
- [ ] **4.9.1 — `trendmaster-probe-01-trend-line-projection`**

  Surface: line.new() chain + max_lines_count=500 stress.

- [ ] **4.9.2 — `trendmaster-probe-02-multi-tier-ma`**

  Surface: 3+ MA stack with bullish/bearish state derivation.

- [ ] **4.9.3 — `trendmaster-probe-03-pivot-tp-sl`**

  Surface: TP/SL anchored to recent pivot (not fixed offset).

- [ ] **4.9.4 — `trendmaster-probe-04-trend-entry-gate`**

  Surface: composite entry gate (trend + momentum + structure all true).

- [ ] **4.9.5 — `trendmaster-probe-integration`**

- [ ] **4.9 commit.**

---

## Phase 5 — Consolidate, regenerate report, flip public

No TV-export gate. All scripted + author tasks. Single commit at end (or split: consolidation commit + report-regen commit + repo-flip commit).

### Task 5.1: Inventory consolidation moves

- [ ] **Step 1: Build move-list**

Run:
```bash
ls -d /Users/haoliangwen/code/pineforge-engine/corpus/validation_*/* /Users/haoliangwen/code/pineforge-engine/corpus/parity-anomalies/*/ 2>/dev/null | grep -v README > /tmp/consolidate_src.txt
wc -l /tmp/consolidate_src.txt
```

Expected: ~40 source dirs.

- [ ] **Step 2: Compute target paths**

For each source path, derive target under `corpus/validation/`:
- `corpus/validation_barstate/<entry>` → `corpus/validation/<entry>` (entry already prefixed `barstate-…` per `barstate-magnifier-probe-*`).
- `corpus/validation_lower_tf/<entry>` → `corpus/validation/lower-tf-<entry>` if `<entry>` lacks `lower-tf` prefix; else verbatim.
- `corpus/validation_magnifier/<entry>` → `corpus/validation/<entry>` (already prefixed `magnifier-`); else add prefix.
- `corpus/validation_na_chain/<entry>` → `corpus/validation/na-chain-<entry>` (or verbatim if prefixed).
- `corpus/validation_oca/<entry>` → `corpus/validation/<entry>` (entries `oca-three-way-probe-*`).
- `corpus/validation_process_orders/<entry>` → `corpus/validation/process-orders-<entry>` (or verbatim).
- `corpus/validation_pyramid/<entry>` → `corpus/validation/pyramid-<entry>` (or verbatim).
- `corpus/validation_recompute/<entry>` → `corpus/validation/recompute-<entry>` (or verbatim).
- `corpus/validation_session/<entry>` → `corpus/validation/session-<entry>` (or verbatim).
- `corpus/validation_ta_isolate/<entry>` → `corpus/validation/<entry>` (verbatim, already `ta-isolate-NN-…`).
- `corpus/validation_typed_matrix/<entry>` → `corpus/validation/typed-matrix-<entry>` (or verbatim).
- `corpus/validation_varip/<entry>` → `corpus/validation/varip-<entry>` (or verbatim).
- `corpus/parity-anomalies/equity-mirror` → `corpus/validation/anomaly-equity-mirror`.

Build `/tmp/consolidate_moves.txt` lines `src=>dst`. Verify each src actually starts with the expected prefix (some may not — fall back to `<topic>-<basename>`).

- [ ] **Step 3: Manually inspect each entry's existing slug**

Run:
```bash
for d in $(cat /tmp/consolidate_src.txt); do
  base=$(basename "$d")
  cat=$(basename $(dirname "$d") | sed 's/^validation_//; s/^parity-anomalies/anomaly/')
  if [[ "$base" == "$cat-"* || "$base" == "$cat"* ]]; then
    echo "VERBATIM  $d"
  else
    echo "PREFIX    $d  =>  ${cat}-${base}"
  fi
done > /tmp/consolidate_moves.txt
cat /tmp/consolidate_moves.txt
```

### Task 5.2: Apply git moves

- [ ] **Step 1: Run `git mv` per entry**

For each line in `/tmp/consolidate_moves.txt`:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
# VERBATIM line:
git mv <src>  validation/<basename>
# PREFIX line:
git mv <src>  validation/<prefixed-name>
```

Loop in shell — example:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
while IFS= read -r line; do
  kind=$(echo "$line" | awk '{print $1}')
  src=$(echo "$line" | awk '{print $2}')
  if [[ "$kind" == "VERBATIM" ]]; then
    base=$(basename "$src")
    git mv "$src" "validation/$base"
  else
    dst_name=$(echo "$line" | awk '{print $4}')
    git mv "$src" "validation/$dst_name"
  fi
done < /tmp/consolidate_moves.txt
```

- [ ] **Step 2: Remove now-empty topical dirs**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
rmdir validation_barstate validation_lower_tf validation_magnifier validation_na_chain validation_oca validation_process_orders validation_pyramid validation_recompute validation_session validation_ta_isolate validation_typed_matrix validation_varip
git add -u  # picks up the dir removals + README displacements
```

- [ ] **Step 3: Handle `parity-anomalies/README.md`**

Move its useful content (anomaly explanations) into the validation report (Task 5.5) or into `corpus/validation/anomaly-equity-mirror/notes.md`. Then delete:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git rm parity-anomalies/README.md
rmdir parity-anomalies
```

- [ ] **Step 4: Move `validation_ta_isolate/README.md`**

Either fold its content into the main `corpus/README.md` Layout description, or into a new `corpus/validation/README-ta-isolate.md` (separate doc per topic family). Deciding: drop it; the per-probe rationale headers carry the surface explanations.

```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git rm validation_ta_isolate/README.md  # if not already removed by `git mv`
```

### Task 5.3: Delete `basic/` + `community/`

- [ ] **Step 1: Delete trees**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git rm -r basic community
```

### Task 5.4: Strip `generated.cpp` (and any `strategy.dylib/.so/.dll`)

- [ ] **Step 1: Find checked-in build outputs**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git ls-files | grep -E '(generated\.cpp|strategy\.(dylib|so|dll))$' > /tmp/build_outputs.txt
wc -l /tmp/build_outputs.txt
```

Expected: ~150 entries.

- [ ] **Step 2: Remove from index (keep nothing on disk; .gitignore catches future)**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
xargs git rm < /tmp/build_outputs.txt
```

- [ ] **Step 3: Verify ignore catches the deleted files**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git check-ignore -v validation/01-macd-histogram/generated.cpp
git check-ignore -v validation/01-macd-histogram/strategy.dylib
```

Expected: both lines confirm `.gitignore` rule.

### Task 5.5: Regenerate validation report

The report generator lives outside this engine repo (referenced by `verify_corpus.py` docstring as the canonical aggregator and by `corpus/README.md` as `validation_report.md/.html/.pdf`). It is part of `pineforge-utils`.

- [ ] **Step 1: Locate report generator**

Run:
```bash
ls /Users/haoliangwen/code/pineforge-codegen 2>/dev/null
ls /Users/haoliangwen/code/pineforge-utils 2>/dev/null
find /Users/haoliangwen/code -maxdepth 5 -name "*.py" 2>/dev/null | xargs grep -l "validation_report\|generate.*report" 2>/dev/null | head -5
```

Document the path. If absent, ask the user where the report-generation tooling lives before proceeding.

- [ ] **Step 2: Run full corpus parity sweep**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
scripts/run_corpus.sh
```

Expected: builds all (now-consolidated) `validation/*` strategies via the closed-source transpiler regenerating local `generated.cpp` + `.dylib`, runs harness, runs `verify_corpus.py --all`. Should print per-strategy disposition; final tally is the new headline parity figure.

- [ ] **Step 3: Run report generator**

Invoke whichever tool was located in Step 1, configured to point at the consolidated `corpus/validation/` tree. It must:
- Emit `corpus/validation_report.md`, `.html`, `.pdf`.
- Per-probe row: slug, topic tag (extracted from slug prefix), disposition, trade-count delta, max entry-price delta, max exit-price delta, max P&L delta, notes.
- Anomaly rows include a one-paragraph explanation. For `anomaly-equity-mirror`: "TradingView's broker-emulator margin check is non-deterministic at the exact 1× equity boundary; engine produces the deterministic correct trade list. Probe is excluded from the headline parity figure."

- [ ] **Step 4: Update headline parity figure in the spec / commit messages**

Note the new excellent/strong/anomaly counts from `validation_report.md` for use in subsequent README rewrite.

### Task 5.6: Rewrite `corpus/README.md`

**Files:**
- Modify: `corpus/README.md`

- [ ] **Step 1: Update headline parity sentence**

Find current "168 strategies" + "165=excellent, 2=strong" sentences. Replace counts with the new figures from `validation_report.md`.

- [ ] **Step 2: Replace artifact tuple table**

Edit the 4-row table (`strategy.pine`, `tv_trades.csv`, `generated.cpp`, `engine_trades.csv`) to a 3-row table (drop the `generated.cpp` row). Add a short paragraph below: "`generated.cpp` is regenerated locally by the closed-source PineForge transpiler; the public corpus does not redistribute it."

- [ ] **Step 3: Replace Layout block**

Replace the entire `## Layout` section. New version describes only the single `corpus/validation/` tree, grouped by slug-prefix theme:
- numbered probes `01-…105-…`
- IES family (`ies-probe-01..09`)
- VCP family (`vcp-probe-01..08`)
- MTF family (`mtf-probe-01..11`)
- UDT-method family (`udt-method-probe-01..21`)
- Parity-probe family (`parity-probe-01..06`)
- TA-isolate family (`ta-isolate-01..12`)
- Topic families folded in: barstate, lower-tf, magnifier, na-chain, oca, process-orders, pyramid, recompute, session, typed-matrix, varip
- Community families: 4ema-rsi, bos-curv, kanuck, kkb, liquidity-sweep, market-shift, scalping, wunder-scalper, trendmaster
- Anomalies: `anomaly-*` (currently just `anomaly-equity-mirror`).

- [ ] **Step 4: Drop the "Where the numbers come from" stale references**

Remove or update mentions of `basic/`, `community/`, `parity-anomalies/`. Point at `validation_report.md` as the authoritative source.

### Task 5.7: Rewrite `corpus/LEGAL.md`

**Files:**
- Modify: `corpus/LEGAL.md`

- [ ] **Step 1: Replace file**

Write `corpus/LEGAL.md` with new content:

```markdown
# Legal — PineForge Validation Corpus

Apache License 2.0 (see `LICENSE`). Same license as the PineForge engine
runtime.

## What This Tree Contains

- PineScript v6 sources (`strategy.pine`) — clean-room PineForge originals.
- TradingView "List of Trades" CSV exports (`tv_trades.csv`) — emitted by
  TradingView's broker emulator on our own clean-room scripts. We hold the
  right to redistribute.
- Engine trade lists (`engine_trades.csv`) — produced by PineForge against
  the same OHLCV in TradingView's row-and-column format.
- OHLCV market data under `data/` — Binance USDT-M futures
  (ETH/USDT-USDT 15-minute). Public market data; redistribution is
  factual-data territory.

`generated.cpp` (transpiler output) is NOT shipped publicly. The
PineForge transpiler is closed-source; reproducers regenerate
`generated.cpp` + `strategy.dylib` locally.

## Trademarks

TradingView and PineScript are trademarks of their respective owners.
This corpus is not affiliated with or endorsed by TradingView.

## License Field

SPDX: `Apache-2.0`.
```

### Task 5.8: Add `corpus/NOTICE`

**Files:**
- Create: `corpus/NOTICE`

- [ ] **Step 1: Write file**

Write `corpus/NOTICE`:
```
PineForge Validation Corpus
Copyright 2026 PineForge contributors

This work is licensed under the Apache License, Version 2.0.
You may obtain a copy of the License at
  http://www.apache.org/licenses/LICENSE-2.0

Reference market data under data/ is sourced from Binance USDT-M
futures (ETH/USDT-USDT 15-minute), public market price/volume series.

TradingView and PineScript are trademarks of their respective owners.
This corpus is not affiliated with or endorsed by TradingView.
```

### Task 5.9: Update `corpus/CMakeLists.txt`

**Files:**
- Modify: `corpus/CMakeLists.txt:18-34` (the `file(GLOB ...)` block)

- [ ] **Step 1: Replace glob block**

old_string:
```
file(GLOB STRATEGY_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     ${CMAKE_CURRENT_SOURCE_DIR}/basic/*
     ${CMAKE_CURRENT_SOURCE_DIR}/community/*
     ${CMAKE_CURRENT_SOURCE_DIR}/parity-anomalies/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_barstate/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_lower_tf/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_magnifier/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_na_chain/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_oca/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_process_orders/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_pyramid/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_recompute/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_session/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_ta_isolate/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_typed_matrix/*
     ${CMAKE_CURRENT_SOURCE_DIR}/validation_varip/*)
```

new_string:
```
file(GLOB STRATEGY_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     ${CMAKE_CURRENT_SOURCE_DIR}/validation/*)
```

- [ ] **Step 2: Update header comment**

Find line 1–14 of `corpus/CMakeLists.txt` (the comment block describing path layout). Update to reference single `validation/` tree only.

### Task 5.10: Update `scripts/verify_corpus.py`

**Files:**
- Modify: `scripts/verify_corpus.py` (docstring + any hard-coded category list)

- [ ] **Step 1: Read current category-handling code**

Run:
```bash
grep -n "basic\|community\|parity-anomalies\|validation_\|--category\|--all" /Users/haoliangwen/code/pineforge-engine/scripts/verify_corpus.py
```

- [ ] **Step 2: For each match, replace category references**

- Drop `basic`, `community`, `parity-anomalies`, `validation_*` from any hard-coded category list.
- `--all` now means: walk every subdir under `corpus/validation/`. Anomalies (slug-prefixed `anomaly-*`) excluded by default; `--include-anomalies` flag includes them.
- `--category` flag: keep, but the only valid value is `validation` (or accept arbitrary slug-prefix as a topic filter — a usability improvement, optional).

- [ ] **Step 3: Update docstring**

Replace the docstring example block with:
```python
"""Verify a strategy in the corpus against TradingView's exported trades.

Reads tv_trades.csv and engine_trades.csv from a strategy folder under
corpus/validation/, aligns trades by entry time + direction with a 1-hour
window, and reports the largest deviations.

Usage:
  scripts/verify_corpus.py corpus/validation/97-tp-sl-gap-reversal-oca
  scripts/verify_corpus.py --all                       # all probes, anomalies excluded
  scripts/verify_corpus.py --all --include-anomalies   # + anomaly-* probes
"""
```

- [ ] **Step 4: Run unit-level smoke**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
python3 scripts/verify_corpus.py --all > /tmp/verify_all.txt 2>&1 || true
tail -30 /tmp/verify_all.txt
```

Expected: prints per-probe results + summary tally consistent with `validation_report.md`.

### Task 5.11: Update `scripts/run_corpus.sh` log lines

**Files:**
- Modify: `scripts/run_corpus.sh` (the `log "building runtime + 162 strategy targets"` line)

- [ ] **Step 1: Update target-count comment**

Replace `"162 strategy targets"` with the new total from Task 5.5 Step 4 (e.g., `"<N> strategy targets"`). Or refactor to print the dynamic count from the CMake configure output to avoid drift.

### Task 5.12: Verify build + harness work end-to-end with consolidated tree

- [ ] **Step 1: Clean rebuild + full sweep**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
rm -rf build
scripts/run_corpus.sh
```

Expected: succeeds; final `verify_corpus.py --all` line is the new headline parity figure (matches `validation_report.md`).

- [ ] **Step 2: Re-run `ctest`**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
cmake --build build --target test
```

Expected: all tests green. Any failure is either an engine regression (revert + diagnose) or a test that referenced a deleted path (fix the test).

### Task 5.13: Update engine repo `CONTRIBUTING.md` + `.gitmodules`

**Files:**
- Modify: `CONTRIBUTING.md` (engine root)
- Modify: `.gitmodules` (engine root) — only if submodule URL changes (open item C resolution)

- [ ] **Step 1: Resolve open item C**

Decision needed before this step: does the corpus repo move to a new public URL, or does the existing remote flip from private to public visibility? Confirm with user before editing `.gitmodules`.

- [ ] **Step 2: Update `CONTRIBUTING.md` corpus section**

Find any mention of "private corpus submodule" / "treat tv exports as confidential". Replace with public-visibility framing pointing at the new URL or visibility note.

### Task 5.14: Final consolidation commit

- [ ] **Step 1: Stage everything**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine/corpus
git status --short
```

Expected: large diff — many `R` (renames), `D` (deleted basic/community + topical dirs + generated.cpp), `M` (README, LEGAL, CMakeLists, NOTICE additions), `A` (LICENSE, .gitignore, NOTICE).

```bash
git add -A
```

- [ ] **Step 2: Commit (corpus side)**

Run:
```bash
git commit -m "feat(corpus): consolidate to single validation/ tree, prep public release

- Delete basic/ + community/ (replaced by clean-room probes 97-105 +
  community-script probe families landed in earlier commits).
- Fold validation_*/ topical dirs and parity-anomalies/ into single
  validation/ tree; anomaly-* slug-prefix marks anomaly probes.
- Drop generated.cpp + strategy.dylib/.so/.dll from index (covered by
  .gitignore; regenerated locally by closed-source transpiler).
- Add NOTICE; rewrite LEGAL.md; update README.md (3-file tuple,
  recomputed headline parity figure).
- Update CMakeLists glob to single validation/ subtree.
- Regenerate validation_report.{md,html,pdf} over consolidated tree
  with anomaly explanations.

Refs: docs/superpowers/specs/2026-05-15-publish-validation-corpus-design.md"
```

- [ ] **Step 3: Stage + commit engine-side updates**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
git add scripts/verify_corpus.py scripts/run_corpus.sh CONTRIBUTING.md corpus
git commit -m "chore(corpus): consolidate to single validation/ tree

Engine-side updates accompanying the corpus consolidation:
- scripts/verify_corpus.py: --all walks corpus/validation/; --category
  takes slug-prefix; drop basic/community/parity-anomalies references.
- scripts/run_corpus.sh: target-count comment updated.
- CONTRIBUTING.md: drop 'private submodule' framing.
- corpus submodule pointer bumped to consolidation commit."
```

### Task 5.15: Flip corpus repo visibility public

- [ ] **Step 1: Confirm with user**

This step touches GitHub repo settings (or wherever the corpus is hosted). Confirm with user that all prior tasks are green and they're ready to flip visibility.

- [ ] **Step 2: Apply visibility change**

If GitHub: `gh repo edit <org>/pineforge-corpus --visibility public`.
If self-hosted Gitea / GitLab: use that platform's equivalent.

User performs this step (it is destructive of the privacy posture and irreversible without re-privatising).

- [ ] **Step 3: Verify clone-without-token works**

Run (ideally in a clean shell with no creds):
```bash
cd /tmp && rm -rf clone-test && git clone <public-corpus-url> clone-test && ls clone-test/validation | head
```

Expected: succeeds; lists probe dirs.

---

## Self-Review

**Spec coverage:**
- §1.1 (replace basic/+community/) → Phases 2–5 (deletes in 5.3).
- §1.2 (license every Pine) → Phase 1.5 + Phase 2/3/4 templates.
- §1.3 (rationale every probe) → Phase 1.6 (existing) + Phase 2/3/4 templates.
- §1.4 (drop generated.cpp) → 1.2 (.gitignore), 5.4 (strip from index).
- §1.5 (single validation/ tree) → 5.1–5.2 (consolidation moves).
- §1.6 (regenerate report) → 5.5.
- §2 (header template) → "License header constants" section + Phase 2 step A reference.
- §3 (naming + numbering) → Phase 2 probe list + Phase 4 family lists.
- §4 (clean-room rule) → Phase 2 step A authorship constraints; encoded in template.
- §5 (workflow + commit gating) → Per-probe loop A–G; per-phase commit gates.
- §6 (housekeeping) → Tasks 5.6–5.13.
- §7 (sequencing) → 5 phases match.
- §8 risk 3 (scrub author handles) → Phase 1.3–1.4 with grep-enforcement gate.
- §8 risk 5 (engine harness w/o generated.cpp) → 5.12.
- §9 success criteria → 5.12 + 5.15 verification steps cover each.

**Placeholder scan:**
- "Surface picks" lists in Phase 4 are concrete (named probes + named surfaces). No "TBD".
- Phase 4.8.2 (`wunder-scalper-probe-02-alert-message-templates`) explicitly conditional ("if engine doesn't run alerts, drop this probe and pick another surface") — acceptable explicit branch, not a placeholder.
- Phase 5.5 Step 1 has explicit "ask the user where the report-generation tooling lives before proceeding" fallback if the script is not auto-located. Acceptable explicit gate.
- Phase 5.13 Step 1 ("Resolve open item C") is also an explicit user-gate, not a placeholder — matches the open-item disposition in the spec.

**Type / signature consistency:**
- Slug naming consistent across phases: `<topic>-probe-NN-<surface>` for new probes, `<NN>-<surface>` for numbered series, `anomaly-<slug>` for anomalies, `<topic>-probe-integration` for integration variants.
- License header constant block referenced verbatim by all "step A" instances.
- `verify_corpus.py` flags (`--all`, `--include-anomalies`, `--category`) consistent across Phase 5.10 docstring + Phase 5.5 invocation + Phase 5.12 expected behaviour.
- Disposition labels (`excellent` / `strong` / `anomaly`) consistent everywhere they appear.

No issues found that need inline fixes.
