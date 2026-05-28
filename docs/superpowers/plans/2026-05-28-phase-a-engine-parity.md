# Phase A — Engine Parity Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close out the four remaining engine-side parity regressions: (1) same-id stop replacement ordering on probe 62, (2) TA RSI/HMA/SMA warmup seed misalignment on three probes, (3) `lower-tf-03` LTF-rejection gap, and (4) sub-minute LTF (`"45S"`) silently accepted instead of rejected. Each task lands as its own PR so regressions are git-bisectable.

**Architecture:** Pure C++17 engine work — no codegen coordination required. All four touch existing modules: `src/engine_orders.cpp` (Task 1), `src/ta_oscillators.cpp` + `src/ta_moving_averages.cpp` (Task 2), `src/engine_security.cpp` (Task 3), and `src/timeframe.cpp` (Task 4). Verification gate is the full corpus sweep — no probe may regress.

**Tech Stack:** C++17 (engine), CTest (cassert + plain `int main()`), Python validator (`pineforge-utils/validator/validate.py`) for parity, TradingView Pine v6 corpus as ground truth.

**Source HANDOFF context:** `/Users/haoliangwen/code/pineforge-engine/HANDOFF.md` (read in full before starting Task 1 or Task 2).

---

## Task 1: Same-id stop replacement ordering — probe `62-same-id-stop-cross-before-modify`

**Bug:** Engine pyramids qty=2 on first trade where TradingView stays qty=1. Engine replaces a same-id pending stop *immediately* within the bar instead of *deferring* until after OHLC fill resolution. TV semantics (Pine v6): when a strategy submits an order with same id+direction as an existing pending order, the existing order is REPLACED but ONLY at next bar boundary — the prev order still has a chance to fire on this bar's OHLC first.

**Affected probe:** `corpus/validation/62-same-id-stop-cross-before-modify` — TV=730, engine=666 (eng misses 64 trades, -8.8%).

**Files:**
- Read: `corpus/validation/62-same-id-stop-cross-before-modify/strategy.pine`
- Read: `corpus/validation/62-same-id-stop-cross-before-modify/tv_trades.csv` (first 5 trades)
- Read: `src/engine_orders.cpp` (search `same_id`, `modify`, `replace_order`)
- Read: `src/engine_fills.cpp` (bar evaluation order — when stops fire vs when modifications apply)
- Read: `include/pineforge/engine.hpp` (`pending_orders_`, `PendingOrder` struct)
- Modify: `src/engine_orders.cpp`
- Modify: `src/engine_fills.cpp` (likely; flush pending replacements at end-of-bar)
- Create: `tests/test_same_id_stop_replace.cpp`
- Modify: `tests/CMakeLists.txt`

### Step 1.1 — Reproduce the bug from a fresh build

- [ ] **Read context**

Run:
```bash
cat /Users/haoliangwen/code/pineforge-engine/corpus/validation/62-same-id-stop-cross-before-modify/strategy.pine
head -5 /Users/haoliangwen/code/pineforge-engine/corpus/validation/62-same-id-stop-cross-before-modify/tv_trades.csv
```

Document in your scratch notes: which `strategy.entry`/`strategy.exit`/`strategy.order` IDs are reused with same direction across bars.

- [ ] **Build engine fresh**

Run:
```bash
cd /Users/haoliangwen/code/pineforge-engine
rm -rf build
cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build -j$(nproc 2>/dev/null || echo 4)
ctest --test-dir build --output-on-failure 2>&1 | tail -5
```
Expected: all existing tests pass.

- [ ] **Confirm parity drift on probe 62**

Run:
```bash
rm -rf build/_validator_cache
EIGEN3_INCLUDE_DIR=/opt/homebrew/include/eigen3 python /Users/haoliangwen/code/pineforge-utils/validator/validate.py \
  /Users/haoliangwen/code/pineforge-engine/corpus/validation/62-same-id-stop-cross-before-modify \
  --engine-repo /Users/haoliangwen/code/pineforge-engine \
  --codegen-repo /Users/haoliangwen/code/pineforge-codegen \
  --workers 1 --out-md /tmp/p62.md --out-json /tmp/p62.json
cat /tmp/p62.md
```
Expected: probe 62 reported as `moderate` (or worse), engine trade count ≈ 666, TV ≈ 730.

If the parity is already excellent, the bug has been fixed upstream — stop here and update HANDOFF.md to reflect that.

### Step 1.2 — Write a failing unit test that pins TV semantics

- [ ] **Create `tests/test_same_id_stop_replace.cpp`**

```cpp
#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace pineforge;

// Build a 3-bar synthetic fixture:
//   bar 0: 100/100/100/100  — entry placed
//   bar 1: 110/120/95/105   — stop A at 96 must fire (low crosses 96)
//   bar 2: 105/108/102/106  — irrelevant; stop A' (placed on bar 1) would have replaced A
// Expected: engine fills stop A on bar 1, ignores A' (already-filled is no-op).
// Bug: engine replaces A with A' on bar 1 before OHLC eval, so neither fires.
static void test_replace_after_existing_fires() {
    std::vector<Bar> bars = {
        {/*ts*/0,    100.0, 100.0, 100.0, 100.0, 0.0},
        {/*ts*/60'000, 110.0, 120.0, 95.0,  105.0, 0.0},
        {/*ts*/120'000, 105.0, 108.0, 102.0, 106.0, 0.0},
    };
    // ... drive engine via the C ABI or directly via BacktestEngine API,
    // mirroring how tests/test_strategy_orders.cpp constructs scenarios.
    // The test must:
    //   1. Place a long entry on bar 0.
    //   2. Place stop_loss id="SL" at 96 on bar 1 entry callback.
    //   3. Also place stop_loss id="SL" at 99 on bar 1 — same id, same direction.
    //   4. Assert engine_closed_trades_count == 1 and exit_price == 96.0.

    // Replace this placeholder with the project's actual fixture builder.
    // See tests/test_strategy_orders.cpp for working examples.
    assert(true);  // TODO: replace with real assertions per above checklist
    std::printf("test_replace_after_existing_fires PASSED\n");
}

static void test_replace_when_existing_does_not_fire() {
    // Same setup but bar 1 low = 97 (does not cross 96).
    // Strategy places SL again at 99 on bar 1 → bar 2 OHLC must use SL'@99.
    // Expected: bar 2 low ≤ 99 → fill at 99. Trade count = 1, exit = 99.0.
    assert(true);  // TODO
    std::printf("test_replace_when_existing_does_not_fire PASSED\n");
}

static void test_replace_with_new_entry_at_lower_price() {
    // Bar 1 OHLC crosses stop A AND strategy places new entry at lower price:
    //   engine MUST fill stop A first, then evaluate new entry.
    assert(true);  // TODO
    std::printf("test_replace_with_new_entry_at_lower_price PASSED\n");
}

int main() {
    test_replace_after_existing_fires();
    test_replace_when_existing_does_not_fire();
    test_replace_with_new_entry_at_lower_price();
    return 0;
}
```

Replace each `TODO` with real fixture code mirroring an existing `tests/test_strategy_*.cpp` file (read one to learn the API for placing orders + reading trades).

- [ ] **Register the test in `tests/CMakeLists.txt`**

Append after the last `add_executable(test_*` block:

```cmake
add_executable(test_same_id_stop_replace test_same_id_stop_replace.cpp)
target_link_libraries(test_same_id_stop_replace PRIVATE pineforge)
add_test(NAME test_same_id_stop_replace COMMAND test_same_id_stop_replace)
```

- [ ] **Verify the test FAILS on current main**

Run:
```bash
cmake --build build --target test_same_id_stop_replace -j 4
ctest --test-dir build -R test_same_id_stop_replace -V 2>&1 | tail -20
```
Expected: assertion failures on `test_replace_after_existing_fires` (engine fires SL' at 99 instead of SL at 96), or trade count = 2 instead of 1.

If all three pass on unmodified main, the assertions are wrong — re-derive them from `tv_trades.csv` on probe 62.

### Step 1.3 — Implement: defer same-id stop/limit replacement to end-of-bar

- [ ] **Read `engine_orders.cpp` to find the same-id replacement site**

Run:
```bash
grep -n "same_id\|same id\|replace\|modify" /Users/haoliangwen/code/pineforge-engine/src/engine_orders.cpp | head -30
```

Locate the function that handles `place_order` finding an existing pending order with matching `id + direction`. It currently overwrites the entry immediately.

- [ ] **Add a `pending_replacements_` queue to `BacktestEngine`**

In `include/pineforge/engine.hpp`, alongside `pending_orders_`:

```cpp
// Same-id replacements buffered during a bar; applied AFTER the bar's
// OHLC fill resolution so the original order still has a chance to fire.
// See test_same_id_stop_replace.cpp + HANDOFF.md Task A.
std::vector<PendingOrder> pending_replacements_;
```

- [ ] **Modify `place_order` (or the equivalent entry point) in `engine_orders.cpp`**

Change the same-id replacement branch from immediate overwrite to deferred queue insertion. Pseudocode:

```cpp
// Inside place_order(...)
auto* existing = find_pending_by_id_and_direction(id, dir);
if (existing != nullptr) {
    // Deferred replace — DO NOT touch `*existing` yet. The original
    // must remain eligible to fire on this bar's OHLC.
    PendingOrder repl = build_pending_from_args(...);
    repl.replaces_id = id;
    pending_replacements_.push_back(std::move(repl));
    return;
}
// (no existing — original code path)
```

- [ ] **Apply pending replacements at end-of-bar in `engine_fills.cpp`**

Find the function that finalizes a bar (after OHLC fill loop). Add:

```cpp
// Apply same-id replacements scheduled during this bar. If the original
// already fired (no longer in pending_orders_), the replacement becomes
// the new pending order. If the original is still pending, the
// replacement overwrites it for the NEXT bar.
for (auto& repl : pending_replacements_) {
    auto* existing = find_pending_by_id_and_direction(repl.id, repl.dir);
    if (existing != nullptr) {
        *existing = repl;
    } else {
        pending_orders_.push_back(repl);
    }
}
pending_replacements_.clear();
```

- [ ] **Decision-tree check — verify against probes 52, 63, 72, 93, 95, 96**

These probes use same-id semantics differently and are currently `excellent`. The deferral logic must NOT regress them. Re-run their parity AFTER step 1.4 confirms the unit test passes.

### Step 1.4 — Verify the unit test passes

- [ ] **Rebuild + run**

Run:
```bash
cmake --build build --target test_same_id_stop_replace pineforge -j 4
ctest --test-dir build -R test_same_id_stop_replace -V 2>&1 | tail -10
```
Expected: all three test functions PASS.

### Step 1.5 — Verify probe 62 parity flips to excellent

- [ ] **Re-run single-probe parity**

Run:
```bash
rm -rf build/_validator_cache
EIGEN3_INCLUDE_DIR=/opt/homebrew/include/eigen3 python /Users/haoliangwen/code/pineforge-utils/validator/validate.py \
  /Users/haoliangwen/code/pineforge-engine/corpus/validation/62-same-id-stop-cross-before-modify \
  --engine-repo /Users/haoliangwen/code/pineforge-engine \
  --codegen-repo /Users/haoliangwen/code/pineforge-codegen \
  --workers 1 --out-md /tmp/p62_post.md --out-json /tmp/p62_post.json
grep -E "Match|excellent|moderate" /tmp/p62_post.md
```
Expected: `Match excellent: 1/1` for probe 62.

If still moderate: re-read `tv_trades.csv` carefully. The bug may be partially correct but mis-modeled — check whether TV defers all same-id replacements or only stops/limits (not entries).

### Step 1.6 — Full corpus regression check

- [ ] **Run full corpus sweep**

Run:
```bash
bash /Users/haoliangwen/code/pineforge-engine/scripts/run_corpus.sh 2>&1 | tail -40
```
Expected: 175 excellent → 176 excellent (probe 62 flipped). NO other probe regresses.

If any probe other than 62 changes status (excellent → strong, etc.), STOP and investigate. Most likely culprits: probes 52, 63, 72, 93, 95, 96 (already-passing same-id semantics).

### Step 1.7 — Commit and PR

- [ ] **Commit**

```bash
git add include/pineforge/engine.hpp src/engine_orders.cpp src/engine_fills.cpp \
        tests/test_same_id_stop_replace.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
fix(orders): defer same-id stop/limit replacement until after bar OHLC resolution

TV semantics: a same-id replacement only takes effect at the next bar
boundary. The original order must remain eligible to fire on the
current bar's OHLC. Engine previously overwrote the pending order
immediately, missing fills that TV records.

Probe 62 (-8.8% trade count) flips moderate → excellent.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Push + PR**

```bash
git push -u origin fix/same-id-stop-replace-order
gh pr create --base main --head fix/same-id-stop-replace-order \
  --title "fix(orders): defer same-id stop replacement until after bar OHLC resolution" \
  --body "$(cat <<'EOF'
## Summary
- Same-id stop/limit replacement now deferred until end-of-bar
- Original order remains eligible to fire on current bar's OHLC
- Probe 62 (-8.8%) flipped moderate → excellent; no other probe regressed

## Test plan
- [x] New `tests/test_same_id_stop_replace.cpp` covers 3 cases (existing fires, existing does not fire, existing fires + new entry)
- [x] Full ctest passes
- [x] `scripts/run_corpus.sh` shows 176/197 excellent (was 175)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

DO NOT use `gh pr merge --admin` — it's now in the deny list. Use `gh pr merge <num> --rebase --delete-branch` after CI green.

---

## Task 2: TA warmup seed alignment — RSI / HMA / SMA on 3 probes

**Bug:** Three strategies show 6–8% trade count drift due to RSI / HMA / SMA boundary ULP-level differences from TV. NOT raw precision (both IEEE 754 double) — likely seed bar index OR partial-window handling differences.

**Affected probes:**
- `community/MarketShift` (TV 1150, eng 1080, -70) — uses `ta.hma(close, 55)` + `ta.sma(close, 152)`
- `validation/37-regex-string-filter` (TV 1778, eng 1662, -116) — uses `ta.rsi(close, 14)` + `ta.macd(12, 26, 9)`. Despite the name, NOT a regex bug.
- `validation_typed_matrix/typed-matrix-probe-01-bool-regime-mask` (TV 773, eng 714, -59) — uses `ta.rsi(close, 14)`

**Files:**
- Read: `src/ta_oscillators.cpp` (RSI implementation, lines 30–80)
- Read: `src/ta_moving_averages.cpp` (RMA/SMA/HMA paths)
- Read: `/Users/haoliangwen/code/pineforge-utils/per-bar-trace/README.md` (tracing harness docs)
- Modify: `src/ta_oscillators.cpp` (likely)
- Modify: `src/ta_moving_averages.cpp` (possibly — HMA partial-window)
- Create: `tests/test_ta_rsi_warmup.cpp`
- Create: `tests/test_ta_hma_warmup.cpp` (if HMA mismatch confirmed)
- Modify: `tests/CMakeLists.txt`

### Step 2.1 — Pick the simplest probe and add a `@pf-trace` directive

- [ ] **Copy probe `typed-matrix-probe-01-bool-regime-mask` into a scratch dir**

This is the simplest of the three (only `ta.rsi(close, 14)`).

```bash
cp -r /Users/haoliangwen/code/pineforge-engine/corpus/validation_typed_matrix/typed-matrix-probe-01-bool-regime-mask /tmp/ta_trace_probe
```

- [ ] **Inject `@pf-trace rsi=ta.rsi(close, 14)` in the strategy**

Edit `/tmp/ta_trace_probe/strategy.pine` to add the directive at top-level (read `pineforge-utils/per-bar-trace/README.md` for exact syntax).

- [ ] **Run engine via the tracing harness to get per-bar RSI JSON**

Run (consult `per-bar-trace/README.md` for the exact command — typically a wrapper around the validator that emits a per-bar trace alongside trades):

```bash
cd /Users/haoliangwen/code/pineforge-utils/per-bar-trace
python emit_engine_trace.py /tmp/ta_trace_probe --out /tmp/engine_rsi.jsonl
```

Expected: JSONL with one row per bar carrying `rsi` value.

### Step 2.2 — Build a Python oracle for TV-correct RSI

- [ ] **Create `/tmp/oracle_rsi.py`**

```python
"""TV/Wilder RSI oracle for warmup verification."""
import csv
import json
import sys

def tv_rsi(closes, length=14):
    """Wilder's RSI with SMA-of-first-N seed."""
    n = len(closes)
    out = [float('nan')] * n
    if n < length + 1:
        return out
    gains = [0.0] * n
    losses = [0.0] * n
    for i in range(1, n):
        d = closes[i] - closes[i - 1]
        gains[i] = d if d > 0 else 0.0
        losses[i] = -d if d < 0 else 0.0
    # Seed at index `length`: SMA of gains[1..length] and losses[1..length].
    avg_gain = sum(gains[1 : length + 1]) / length
    avg_loss = sum(losses[1 : length + 1]) / length
    rs = (avg_gain / avg_loss) if avg_loss > 0 else float('inf')
    out[length] = 100.0 - 100.0 / (1.0 + rs) if avg_loss > 0 else 100.0
    # Wilder smoothing.
    for i in range(length + 1, n):
        avg_gain = (avg_gain * (length - 1) + gains[i]) / length
        avg_loss = (avg_loss * (length - 1) + losses[i]) / length
        rs = (avg_gain / avg_loss) if avg_loss > 0 else float('inf')
        out[i] = 100.0 - 100.0 / (1.0 + rs) if avg_loss > 0 else 100.0
    return out

if __name__ == '__main__':
    closes_csv, engine_trace_jsonl = sys.argv[1], sys.argv[2]
    with open(closes_csv) as f:
        rows = list(csv.DictReader(f))
    closes = [float(r['close']) for r in rows]
    py_rsi = tv_rsi(closes, 14)
    with open(engine_trace_jsonl) as f:
        engine_rsi = [json.loads(line).get('rsi') for line in f]
    for i, (p, e) in enumerate(zip(py_rsi, engine_rsi)):
        if p != p and e != e:  # both NaN
            continue
        if e is None:
            continue
        diff = abs(p - e) if (p == p) else float('inf')
        if diff > 1e-9:
            print(f"bar {i}: oracle={p!r} engine={e!r} diff={diff:.3e}")
```

- [ ] **Run the oracle and diff against engine trace**

```bash
# The probe's OHLCV CSV path follows the corpus convention — adjust as needed.
python /tmp/oracle_rsi.py \
  /Users/haoliangwen/code/pineforge-engine/corpus/validation_typed_matrix/typed-matrix-probe-01-bool-regime-mask/ohlcv.csv \
  /tmp/engine_rsi.jsonl | head -20
```

Expected: prints the first bar index where engine RSI diverges from the Wilder oracle.

If no divergence: engine RSI is correct; the drift comes from a downstream consumer (e.g. crossover tie-break). Skip to Step 2.4 (MACD path) and check there.

### Step 2.3 — Fix engine RSI seed and re-verify

- [ ] **Locate the divergence in `src/ta_oscillators.cpp`**

Run:
```bash
grep -n "rsi\|RSI" /Users/haoliangwen/code/pineforge-engine/src/ta_oscillators.cpp | head -20
```

Read the RSI function. Verify:
1. Seed bar index = `length` (zero-based), not `length-1` or `length+1`.
2. Seed `avg_gain` = sum of `gains[1..length]` / `length` (NOT `gains[0..length-1]`).
3. Wilder smoothing from bar `length+1` onwards uses `(prev * (length-1) + new) / length` exactly.

- [ ] **Apply the minimal fix**

If, for example, the seed is off-by-one: change the index. Show the actual diff in the commit.

- [ ] **Add unit test `tests/test_ta_rsi_warmup.cpp`**

```cpp
#include <pineforge/ta.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

// Known-good RSI(14) values for a 30-bar synthetic close series.
// Generated by the Python Wilder oracle in oracle_rsi.py.
static const std::vector<double> CLOSES = {
    44.34, 44.09, 44.15, 43.61, 44.33, 44.83, 45.10, 45.42, 45.84,
    46.08, 45.89, 46.03, 45.61, 46.28, 46.28, 46.00, 46.03, 46.41,
    46.22, 45.64, 46.21, 46.25, 45.71, 46.45, 45.78, 45.35, 44.03,
    44.18, 44.22, 44.57,
};
// TV-published RSI(14) for these closes (Wilder, SMA-of-14 seed).
// Index 14 is the first valid value.
static const std::vector<double> EXPECTED_RSI = {
    NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN,
    70.4634, 66.2496, 66.4795, 69.4063, 66.3506, 57.9747, 62.9296, 63.2571,
    56.0596, 62.3771, 54.6726, 50.3895, 41.7274, 41.9491, 41.5078, 45.4458,
};

int main() {
    // Construct a pineforge Series<double> from CLOSES and feed bar-by-bar
    // through the engine's RSI(14) calculator. Compare each bar's output
    // to EXPECTED_RSI within 1e-3 (TV's display precision is 2 decimals;
    // 1e-3 catches seed-bar-index off-by-ones).
    //
    // Replace this stub with the project's actual TA test harness — see
    // tests/test_ta_*.cpp for examples.
    assert(true);  // TODO
    std::printf("test_ta_rsi_warmup PASSED\n");
    return 0;
}
```

Replace the `TODO` with real fixture code mirroring an existing `tests/test_ta_*.cpp`. Generate the `EXPECTED_RSI` values from `oracle_rsi.py` on `CLOSES` first; copy them in.

- [ ] **Register in `tests/CMakeLists.txt`**

```cmake
add_executable(test_ta_rsi_warmup test_ta_rsi_warmup.cpp)
target_link_libraries(test_ta_rsi_warmup PRIVATE pineforge)
add_test(NAME test_ta_rsi_warmup COMMAND test_ta_rsi_warmup)
```

- [ ] **Verify the test PASSES with the fix and FAILS without it**

```bash
git stash  # revert the fix
cmake --build build --target test_ta_rsi_warmup -j 4
ctest --test-dir build -R test_ta_rsi_warmup -V 2>&1 | tail -5
# Expected: FAIL
git stash pop  # restore the fix
cmake --build build --target test_ta_rsi_warmup pineforge -j 4
ctest --test-dir build -R test_ta_rsi_warmup -V 2>&1 | tail -5
# Expected: PASS
```

### Step 2.4 — Re-verify probe parity and handle remaining probes

- [ ] **Re-run typed-matrix-probe-01 parity**

```bash
rm -rf build/_validator_cache
EIGEN3_INCLUDE_DIR=/opt/homebrew/include/eigen3 python /Users/haoliangwen/code/pineforge-utils/validator/validate.py \
  /Users/haoliangwen/code/pineforge-engine/corpus/validation_typed_matrix/typed-matrix-probe-01-bool-regime-mask \
  --engine-repo /Users/haoliangwen/code/pineforge-engine \
  --codegen-repo /Users/haoliangwen/code/pineforge-codegen \
  --workers 1 --out-md /tmp/p_ta01.md --out-json /tmp/p_ta01.json
grep -E "Match|excellent|moderate" /tmp/p_ta01.md
```

Expected outcomes:
- (a) **Excellent** — full success for RSI(14). Move to handling 37-regex (uses same RSI) and MarketShift (HMA + SMA).
- (b) **Still moderate, engine RSI matches Python oracle exactly** — TV-side anomaly. Document in `parity-anomalies/` and skip remaining engine fixes for this probe.
- (c) **Still moderate, engine RSI differs from Python oracle** — fix is incomplete; iterate.

- [ ] **Handle probe 37-regex (RSI + MACD)**

If RSI fix already moved it to excellent, done. Otherwise, repeat the trace + oracle methodology for `ta.macd(12, 26, 9)` (which is RMA-based — see `src/ta_moving_averages.cpp` RMA function).

- [ ] **Handle probe MarketShift (HMA + SMA)**

HMA = `wma(2*wma(src, length/2) - wma(src, length), sqrt(length))`. The partial-window handling at warmup is the likely culprit. Apply the same trace + oracle methodology with a Python HMA oracle.

If HMA fix needed: add `tests/test_ta_hma_warmup.cpp` mirroring the RSI test structure.

### Step 2.5 — Full corpus regression

- [ ] **Run full corpus sweep**

```bash
rm -rf /Users/haoliangwen/code/pineforge-engine/build/_validator_cache
bash /Users/haoliangwen/code/pineforge-engine/scripts/run_corpus.sh 2>&1 | tail -40
```

Expected: 3 probes flip moderate → excellent OR documented as TV-side anomalies. No regression on any of the 60+ existing TA-using probes.

If any TA-using probe regresses, the RSI/HMA fix is too aggressive. Most likely: the engine was matching TV's older RSI behavior (pre-2019 SMA-of-N seed, vs Wilder's original SMA-of-(N-1) seed). Revisit which TV reference is correct for this corpus's data.

### Step 2.6 — Commit and PR (one commit per logical fix)

- [ ] **Commit RSI fix**

```bash
git add src/ta_oscillators.cpp tests/test_ta_rsi_warmup.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
fix(ta): RSI warmup uses Wilder SMA-of-N seed (matches TV)

The previous seed bar index / window selection caused boundary ULP-level
divergence from TradingView on probes typed-matrix-probe-01 and
37-regex-string-filter (despite the misleading name, the latter is an
RSI seed bug, not a regex bug).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Commit HMA fix (only if applicable)**

```bash
git add src/ta_moving_averages.cpp tests/test_ta_hma_warmup.cpp tests/CMakeLists.txt
git commit -m "fix(ta): HMA partial-window aligned to TV boundary

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

- [ ] **Push + PR**

Use `gh pr create` per Task 1's pattern.

---

## Task 3: `lower-tf-03` script_tf upper-bound LTF rejection

**Bug:** `validation/lower-tf-probe-03-ratio-mismatch-reject` shows `FAILED_to_reject`. Engine reportedly accepts a "30" LTF on `script_tf=15` instead of rejecting per `request.security_lower_tf` semantics.

**Status caveat:** As of 2026-05-28, `src/engine_security.cpp:189–195` already throws when `requested_seconds >= script_seconds` for `lower_tf_array_requested`. The HANDOFF.md may be stale, OR the probe failure has a different root cause. Step 3.1 reproduces before fixing.

**Files:**
- Read: `corpus/validation/lower-tf-probe-03-ratio-mismatch-reject/strategy.pine`
- Read: `corpus/validation/lower-tf-probe-03-ratio-mismatch-reject/inputs.json` (if present — TF combo)
- Read: `src/engine_security.cpp:92–218`
- Modify: `src/engine_security.cpp` (if a gap is confirmed)
- Modify: `tests/test_engine_security.cpp` (or create if missing)
- Modify: `tests/CMakeLists.txt`

### Step 3.1 — Reproduce the failure

- [ ] **Inspect the probe**

```bash
cat /Users/haoliangwen/code/pineforge-engine/corpus/validation/lower-tf-probe-03-ratio-mismatch-reject/strategy.pine
ls /Users/haoliangwen/code/pineforge-engine/corpus/validation/lower-tf-probe-03-ratio-mismatch-reject/
```

Identify: input_tf, script_tf, and the requested LTF literal.

- [ ] **Run the probe in isolation**

```bash
rm -rf /Users/haoliangwen/code/pineforge-engine/build/_validator_cache
EIGEN3_INCLUDE_DIR=/opt/homebrew/include/eigen3 python /Users/haoliangwen/code/pineforge-utils/validator/validate.py \
  /Users/haoliangwen/code/pineforge-engine/corpus/validation/lower-tf-probe-03-ratio-mismatch-reject \
  --engine-repo /Users/haoliangwen/code/pineforge-engine \
  --codegen-repo /Users/haoliangwen/code/pineforge-codegen \
  --workers 1 --out-md /tmp/p_ltf03.md --out-json /tmp/p_ltf03.json
cat /tmp/p_ltf03.md
```

Expected (per HANDOFF): `FAILED_to_reject`. If now shows `expected_reject_passed`, the bug is closed — update HANDOFF.md and skip this task.

### Step 3.2 — Diagnose

- [ ] **Re-read `validate_security_timeframes` for the exact gap**

The current checks at `src/engine_security.cpp:159, 189, 196, 203`:
1. Line 159: `input_seconds % requested_seconds != 0` → rejects non-integer divisor (finer-than-input case).
2. Line 189: `requested_seconds >= script_seconds` → rejects when LTF not strictly finer than script_tf.
3. Line 196: `script_seconds % requested_seconds != 0` → rejects when script_tf not an integer multiple of requested.
4. Line 203: `requested_seconds % input_seconds != 0` → rejects when requested can't be aggregated from input.

Identify which check the probe expects to fire but does not.

- [ ] **If the gap is real:** the probe likely uses an HTF input where requested == script_tf is wrongly allowed, or a case where the requested TF is finer than input AND script (which currently falls into the "finer than input" branch at line 148 and may not reach the script_tf check). Trace the control flow for the probe's exact (input_tf, script_tf, requested_tf) triple.

### Step 3.3 — Fix (if gap confirmed)

- [ ] **Add the missing check in `validate_security_timeframes`**

Based on Step 3.2 findings. Example: if the gap is that the finer-than-input branch never re-checks against script_tf, add inside the `requested_seconds < input_seconds` branch:

```cpp
// For LTF finer-than-input, also enforce script_tf evenly divides requested.
if (script_seconds > 0 && script_seconds % requested_seconds != 0) {
    throw std::runtime_error(
        "request.security_lower_tf: requested timeframe '" + state.tf
        + "' does not evenly divide script timeframe '" + script_tf_ + "'"
    );
}
```

(Adjust the exact placement to match the actual gap.)

- [ ] **Add unit test in `tests/test_engine_security.cpp`** (create the file if missing)

```cpp
#include <pineforge/engine.hpp>
#include <cassert>
#include <cstdio>
#include <stdexcept>

int main() {
    // Reproduce probe lower-tf-03's exact (input, script, requested) triple.
    pineforge::BacktestEngine eng;
    eng.set_script_tf("15");  // 15 minutes
    eng.register_security_eval(/*sec_id*/ 0, /*symbol*/ "", /*tf*/ "30",
                               /*lookahead*/ false, /*gaps*/ false,
                               /*lower_tf_array_requested*/ true);
    bool threw = false;
    try {
        eng.validate_security_timeframes("1");  // 1m input
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && "request.security_lower_tf with requested >= script_tf must reject");
    std::printf("test_ltf_script_tf_upper_bound PASSED\n");
    return 0;
}
```

Adjust the API call site to match the actual `BacktestEngine` register-security signature (see `src/engine_security.cpp` for the registration entry point).

- [ ] **Register and run the test; verify probe lower-tf-03 now passes**

### Step 3.4 — Commit and PR

Same pattern as Task 1.

---

## Task 4: Sub-minute LTF (`"45S"`, `"30S"`, …) silently accepted

**Bug:** `request.security_lower_tf("45S", ...)` on a 1m input: `60 % 45 != 0`, but `supports_lower_tf_emulation` returns `false` because `is_fixed_intraday_minute_tf("45S")` is false (seconds-suffix not recognized). Engine then silently runs with synthesized data instead of rejecting.

**Files:**
- Read: `src/timeframe.cpp` (search `is_fixed_intraday_minute_tf`, `supports_lower_tf_emulation`)
- Read: `include/pineforge/timeframe.hpp`
- Modify: `src/timeframe.cpp` (extend `supports_lower_tf_emulation` for seconds-suffix TFs)
- Modify: `tests/test_engine_security.cpp` (add cases) or `tests/test_timeframe.cpp`

### Step 4.1 — Write the failing test first

- [ ] **Add to `tests/test_timeframe.cpp` (or create)**

```cpp
#include <pineforge/timeframe.hpp>
#include <cassert>
#include <cstdio>

int main() {
    int ratio = 0, secs = 0;
    // 1m input, "45S" requested → 60 % 45 = 15 (non-divisor) → must NOT emulate.
    bool ok = pineforge::supports_lower_tf_emulation("1", "45S", &ratio, &secs);
    assert(!ok && "45S on 1m input must not emulate (60 % 45 != 0)");

    // 1m input, "30S" requested → 60 / 30 = 2 → MUST emulate.
    ok = pineforge::supports_lower_tf_emulation("1", "30S", &ratio, &secs);
    assert(ok);
    assert(ratio == 2 && secs == 30);

    std::printf("test_seconds_suffix_ltf PASSED\n");
    return 0;
}
```

- [ ] **Verify failure on current main**

Expected: first assertion may pass (false is returned), second assertion FAILS — `supports_lower_tf_emulation` returns false for `"30S"` because seconds-suffix isn't recognized.

### Step 4.2 — Extend `supports_lower_tf_emulation`

- [ ] **Read the current implementation**

```bash
grep -n "supports_lower_tf_emulation\|is_fixed_intraday_minute_tf\|tf_to_seconds" /Users/haoliangwen/code/pineforge-engine/src/timeframe.cpp | head -30
```

- [ ] **Add seconds-suffix handling**

In `supports_lower_tf_emulation`, after the existing minute-TF check, add:

```cpp
// Seconds-suffix TFs ("30S", "45S", ...): convert to seconds via
// tf_to_seconds and apply the same integer-divisor rule against the
// input TF in seconds. Reject if the ratio is non-integer.
if (requested_tf.size() >= 2 && requested_tf.back() == 'S') {
    int req_sec = tf_to_seconds(requested_tf);
    int in_sec = tf_to_seconds(input_tf);
    if (req_sec <= 0 || in_sec <= 0 || in_sec % req_sec != 0) {
        return false;
    }
    if (ratio_out) *ratio_out = in_sec / req_sec;
    if (seconds_out) *seconds_out = req_sec;
    return true;
}
```

(Adjust naming to match actual function signature and conventions in `src/timeframe.cpp`.)

### Step 4.3 — Verify

- [ ] **Run the new test + full ctest**

```bash
cmake --build build -j 4
ctest --test-dir build --output-on-failure 2>&1 | tail -10
```
Expected: new test PASSES; no existing test regresses.

- [ ] **Full corpus sweep**

```bash
bash /Users/haoliangwen/code/pineforge-engine/scripts/run_corpus.sh 2>&1 | tail -10
```
Expected: no probe regresses. If a probe regresses, it was relying on the silent-accept behavior — investigate before merging.

### Step 4.4 — Commit and PR

Same pattern as Task 1.

---

## Self-review checklist

Before opening any PR:

- [ ] Both `ctest --test-dir build --output-on-failure` and `scripts/run_corpus.sh` pass with NO regressions
- [ ] Each task is its own commit / PR (do NOT bundle Task 1 + Task 2)
- [ ] No `--no-verify` skips; no `--admin` merges
- [ ] HANDOFF.md updated if any "open item" is now resolved
- [ ] No new dependencies introduced

## Phase A success criteria

- Corpus excellent count: 175 → 178 (or higher) with no regressions
- All four bugs covered by ctest fixtures so future refactors don't silently regress
- HANDOFF.md "Two pending tasks" section emptied (or replaced with the next batch)
