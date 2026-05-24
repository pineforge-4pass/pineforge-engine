# Engine Core Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 8 critical Pine v6 compatibility issues across the C++ backtest engine and Python transpiler, including tracking last bar metrics, robust strategy accessors, proper variable-level close time calculations, and color.rgb parameters.

**Architecture:** Use a hybrid model where runtime-heavy variables and calculations are managed as clean member properties/state within the C++ `BacktestEngine`, and the Python `pineforge-codegen` is updated to cleanly emit references to these helpers.

**Tech Stack:** C++17, CMake, Python 3, Pytest.

---

### Task 1: C++ Engine State Variables & Run Loop Updates

**Files:**
- Modify: `/Users/haoliangwen/code/pineforge-engine/include/pineforge/engine.hpp:288-300`
- Modify: `/Users/haoliangwen/code/pineforge-engine/src/engine_run.cpp:15-55`, `src/engine_run.cpp:180-220`

- [ ] **Step 1: Declare state fields**
  Open `/Users/haoliangwen/code/pineforge-engine/include/pineforge/engine.hpp` and add `last_bar_time_` and `last_bar_index_` fields to the `BacktestEngine` class definition around line 288 (protected section):
  ```cpp
  // --- SymInfo + Input injection ---
  SymInfo syminfo_;
  int64_t last_bar_time_ = 0;
  int last_bar_index_ = 0;
  ```

- [ ] **Step 2: Update multi-timeframe run loop**
  Open `/Users/haoliangwen/code/pineforge-engine/src/engine_run.cpp` and update the multi-timeframe `run` overload:
  ```cpp
  void BacktestEngine::run(const Bar* input_bars, int n_input,
                           const std::string& input_tf,
                           const std::string& script_tf,
                           const std::unordered_map<std::string, std::string>& inputs,
                           const SymInfo& syminfo,
                           const StrategyOverrides* overrides,
                           bool bar_magnifier,
                           int magnifier_samples,
                           MagnifierDistribution magnifier_dist) {
      last_error_.clear();
      if (n_input > 0 && input_bars != nullptr) {
          last_bar_time_ = input_bars[n_input - 1].timestamp;
      } else {
          last_bar_time_ = 0;
      }
      // ... (rest of method stays same)
  ```
  And inside the expected script bars computation logic, update `last_bar_index_`:
  ```cpp
  // once expected_script_bars is computed
  last_bar_index_ = expected_script_bars - 1;
  ```

- [ ] **Step 3: Update single-timeframe run loop**
  In `/Users/haoliangwen/code/pineforge-engine/src/engine_run.cpp`, update the single-timeframe `run(const Bar* bars, int n)` overload:
  ```cpp
  void BacktestEngine::run(const Bar* bars, int n) {
      last_error_.clear();
      if (n > 0 && bars != nullptr) {
          last_bar_time_ = bars[n - 1].timestamp;
          last_bar_index_ = n - 1;
      } else {
          last_bar_time_ = 0;
          last_bar_index_ = 0;
      }
      // ... (rest of method stays same)
  ```

- [ ] **Step 4: Rebuild and run C++ tests**
  Run CMake configure and build, then execute tests.
  Run: `cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON && cmake --build build -j4 && ctest --test-dir build --output-on-failure`
  Expected: Compile succeeds, all existing 50+ tests pass.

- [ ] **Step 5: Commit changes**
  ```bash
  git add include/pineforge/engine.hpp src/engine_run.cpp
  git commit -m "feat(engine): track last_bar_time_ and last_bar_index_ in run loops"
  ```

---

### Task 2: C++ Engine Strategy Helper Methods

**Files:**
- Modify: `/Users/haoliangwen/code/pineforge-engine/include/pineforge/engine.hpp:910-925`
- Create / Modify: `/Users/haoliangwen/code/pineforge-engine/tests/test_engine_trade_accessors.cpp:320-360` (add new test cases)

- [ ] **Step 1: Implement helper methods in BacktestEngine**
  Open `/Users/haoliangwen/code/pineforge-engine/include/pineforge/engine.hpp` and add these methods in the public or protected accessor section (around line 910):
  ```cpp
  std::string position_entry_name() const {
      if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return "";
      return pyramid_entries_.back().entry_id;
  }

  double max_drawdown_percent() const {
      return (initial_capital_ > 0.0) ? (max_drawdown_ / initial_capital_) * 100.0 : 0.0;
  }

  int64_t time_close() const {
      return pine_time_close(current_bar_.timestamp, script_tf_, syminfo_.session, syminfo_.timezone, script_tf_);
  }
  ```

- [ ] **Step 2: Add C++ unit tests for new helpers**
  Open `/Users/haoliangwen/code/pineforge-engine/tests/test_engine_trade_accessors.cpp` and add tests asserting correct outputs for `position_entry_name()`, `max_drawdown_percent()`, and `time_close()` on flat, long, and short positions:
  ```cpp
  TEST_CASE("Engine Core Helpers Verification", "[helpers]") {
      BacktestEngineTestHarness engine;
      // Flat asserts
      REQUIRE(engine.position_entry_name() == "");
      REQUIRE(engine.max_drawdown_percent() == 0.0);
  }
  ```

- [ ] **Step 3: Run ctest to verify failure/success**
  Run: `cmake --build build -j4 && ctest --test-dir build --output-on-failure`
  Expected: Compile and test pass.

- [ ] **Step 4: Commit changes**
  ```bash
  git add include/pineforge/engine.hpp tests/test_engine_trade_accessors.cpp
  git commit -m "feat(engine): add position_entry_name, max_drawdown_percent, time_close accessors"
  ```

---

### Task 3: Python Codegen Signature & Analyzer Registration

**Files:**
- Modify: `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/signatures.py:450-500`
- Modify: `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/analyzer/tables.py:170-195`

- [ ] **Step 1: Add last_bar_time to Signatures**
  Open `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/signatures.py` and register `last_bar_time` under `BUILTIN_VARIABLES` (around line 486):
  ```python
  "bar_index": I, "time": I, "time_close": I, "last_bar_time": I,
  ```

- [ ] **Step 2: Add last_bar_time to Analyzer BUILTIN_VARS**
  Open `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/analyzer/tables.py` and register it under `BUILTIN_VARS` (around line 181):
  ```python
  "bar_index": PineType.INT, "time": PineType.INT, "time_close": PineType.INT, "last_bar_time": PineType.INT,
  ```

- [ ] **Step 3: Run codegen tests**
  Configure environment variable and run pytest:
  ```bash
  export PINEFORGE_ENGINE_INCLUDE=/Users/haoliangwen/code/pineforge-engine/include
  pytest tests/test_analyzer.py
  ```
  Expected: Passes cleanly.

- [ ] **Step 4: Commit changes**
  ```bash
  git -C /Users/haoliangwen/code/pineforge-codegen add pineforge_codegen/signatures.py pineforge_codegen/analyzer/tables.py
  git -C /Users/haoliangwen/code/pineforge-codegen commit -m "feat(codegen): register last_bar_time in signatures and analyzer tables"
  ```

---

### Task 4: Python Codegen Table Mappings & Member Access

**Files:**
- Modify: `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/codegen/tables.py:30-45`
- Modify: `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/codegen/visit_expr.py:255-300`, `pineforge_codegen/codegen/visit_expr.py:440-475`
- Modify: `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/codegen/visit_call.py:1015-1025`

- [ ] **Step 1: Update tables.py mappings**
  Open `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/codegen/tables.py` and map variables:
  ```python
  "time_close": "time_close()",
  "last_bar_time": "last_bar_time_",
  "last_bar_index": "last_bar_index_",
  ```

- [ ] **Step 2: Update visit_expr.py member mappings**
  Open `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/codegen/visit_expr.py` and update mappings around line 265-270:
  ```python
                  if node.member == "max_drawdown_percent":
                      return "max_drawdown_percent()"
                  if node.member == "account_currency":
                      return "syminfo_.currency"
  ```
  And under nested strategy property routing (around line 465-475):
  ```python
                      if sub == "commission":
                          if node.member == "percent":
                              return "0"
                          if node.member == "cash_per_order":
                              return "1"
                          if node.member == "cash_per_contract":
                              return "2"
                          return "0"
                      if sub == "closedtrades" and node.member == "first_index":
                          return "0"
  ```
  And strategy member access for position_entry_name (around line 1180-1200 / visit_expr):
  ```python
                  if node.member == "position_entry_name":
                      return "position_entry_name()"
  ```

- [ ] **Step 3: Update visit_call.py for color.rgb**
  Open `/Users/haoliangwen/code/pineforge-codegen/pineforge_codegen/codegen/visit_call.py` and support the optional 4th argument of `color.rgb()`:
  ```python
          if func_name == "rgb":
              if len(args) >= 4:
                  return f"pine_color::new_color(((int64_t)({args[0]}) << 16 | (int64_t)({args[1]}) << 8 | (int64_t)({args[2]})), (int)({args[3]}))"
              elif len(args) >= 3:
                  return f"pine_color::new_color(((int64_t)({args[0]}) << 16 | (int64_t)({args[1]}) << 8 | (int64_t)({args[2]})), 0)"
              return "0"
  ```

- [ ] **Step 4: Run pytest with engine env**
  Run:
  ```bash
  export PINEFORGE_ENGINE_INCLUDE=/Users/haoliangwen/code/pineforge-engine/include
  pytest
  ```
  Expected: All 940+ tests pass successfully.

- [ ] **Step 5: Commit changes**
  ```bash
  git -C /Users/haoliangwen/code/pineforge-codegen add pineforge_codegen/codegen/tables.py pineforge_codegen/codegen/visit_expr.py pineforge_codegen/codegen/visit_call.py
  git -C /Users/haoliangwen/code/pineforge-codegen commit -m "feat(codegen): emit references to new engine accessors and fix color.rgb optional arg"
  ```

---

### Task 5: End-to-End Corpus Verification

**Files:**
- None (verification phase)

- [ ] **Step 1: Clean build the engine**
  ```bash
  cd /Users/haoliangwen/code/pineforge-engine
  rm -rf build
  cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
  cmake --build build -j4
  ```

- [ ] **Step 2: Run C++ tests**
  ```bash
  ctest --test-dir build --output-on-failure
  ```
  Expected: All tests pass.

- [ ] **Step 3: Execute complete corpus sweep**
  ```bash
  ./scripts/run_corpus.sh
  ```
  Expected: 0 regressions, all 162 strategies pass, parity score matches or improves reference.
