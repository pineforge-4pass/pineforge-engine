# PineForge Performance Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement zero-allocation `DynamicRingBuffer` in the C++ engine to optimize `Series<T>` and moving indicators, and integrate AST static pre-calculation inside `pineforge-codegen`.

**Architecture:**
- Create dynamic, allocation-free `DynamicRingBuffer` template inside `include/pineforge/series.hpp` to replace `std::deque`.
- Optimize `ta::SMA` to a rolling-sum $O(1)$ computation with high-performance power-of-two mask self-correction (`& 255`).
- Enhance `pineforge-codegen` parsing to recursively detect unconditionally defined global static indicators, pre-compute them in contiguous arrays before backtest start, and use $O(1)$ vector index lookups during execution.

**Tech Stack:** Modern C++17, Python 3.

---

### Task 1: Create and Test `DynamicRingBuffer`

**Files:**
- Create: `tests/test_ringbuffer.cpp`
- Modify: `include/pineforge/series.hpp`

- [ ] **Step 1: Write a unit test for `DynamicRingBuffer`**

  Create `tests/test_ringbuffer.cpp` and implement rigorous checks for wrapping, resizing, and `na<T>()` invariants:

  ```cpp
  #include <pineforge/series.hpp>
  #include <pineforge/na.hpp>
  #include <gtest/gtest.h>

  TEST(DynamicRingBufferTest, BasicOperations) {
      using namespace pineforge;
      DynamicRingBuffer<double> rb(3);
      EXPECT_EQ(rb.size(), 0);

      rb.push_front(1.0);
      rb.push_front(2.0);
      rb.push_front(3.0);
      EXPECT_EQ(rb.size(), 3);
      EXPECT_DOUBLE_EQ(rb[0], 3.0);
      EXPECT_DOUBLE_EQ(rb[1], 2.0);
      EXPECT_DOUBLE_EQ(rb[2], 1.0);

      // Verify wrapping
      rb.push_front(4.0);
      EXPECT_EQ(rb.size(), 3);
      EXPECT_DOUBLE_EQ(rb[0], 4.0);
      EXPECT_DOUBLE_EQ(rb[1], 3.0);
      EXPECT_DOUBLE_EQ(rb[2], 2.0);
      EXPECT_TRUE(is_na(rb[3]));
  }
  ```

- [ ] **Step 2: Add test to CMake configuration and run to verify it fails**

  Modify `tests/CMakeLists.txt` (or root `CMakeLists.txt`) to include `tests/test_ringbuffer.cpp` as part of the unit tests executable.
  Run:
  ```bash
  cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON
  cmake --build build -j4
  ```
  Expected: Compile error due to `DynamicRingBuffer` undefined.

- [ ] **Step 3: Implement `DynamicRingBuffer` template**

  Modify `include/pineforge/series.hpp` to add the `DynamicRingBuffer<T>` template as designed in Section 2.1 of the performance spec.

- [ ] **Step 4: Run unit tests to verify they pass**

  Run:
  ```bash
  cmake --build build -j4
  ctest --test-dir build --output-on-failure -R DynamicRingBufferTest
  ```
  Expected: All checks PASS.

- [ ] **Step 5: Commit**

  ```bash
  git add include/pineforge/series.hpp tests/test_ringbuffer.cpp
  git commit -m "feat(engine): implement high-performance DynamicRingBuffer template"
  ```


### Task 2: Refactor `Series<T>` to use `DynamicRingBuffer`

**Files:**
- Modify: `include/pineforge/series.hpp`

- [ ] **Step 1: Swap `std::deque` with `DynamicRingBuffer` inside `Series<T>`**

  Modify `Series<T>` inside `include/pineforge/series.hpp` to hold `DynamicRingBuffer<T> buf;` instead of `std::deque<T> buf;`. Ensure constructors propagate `max_len` correctly to the ring buffer's capacity.

- [ ] **Step 2: Run complete C++ test suite to verify no regressions**

  Run:
  ```bash
  cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
  cmake --build build -j4
  ctest --test-dir build --output-on-failure
  ```
  Expected: All tests pass.

- [ ] **Step 3: Commit**

  ```bash
  git add include/pineforge/series.hpp
  git commit -m "perf(engine): refactor Series to use dynamic ring buffers for zero-allocation"
  ```


### Task 3: Refactor moving average indicators (`ta::SMA`) to $O(1)$ Rolling Sum

**Files:**
- Modify: `include/pineforge/ta.hpp`, `src/ta_moving_averages.cpp`

- [ ] **Step 1: Modify `ta::SMA` buffer storage in `include/pineforge/ta.hpp`**

  Swap `std::deque<double> buffer;` with `DynamicRingBuffer<double> buffer;` in `class SMA`. Add a `double running_sum;` private member variable.

- [ ] **Step 2: Implement self-correcting rolling sum logic**

  Modify `SMA::compute` and `SMA::recompute` inside `src/ta_moving_averages.cpp` to execute the $O(1)$ running sum logic, utilizing the bitwise `(bar_count & 255) == 0` check for fast periodic reset.

- [ ] **Step 3: Run indicators unit test suite**

  Run:
  ```bash
  ctest --test-dir build --output-on-failure -R ta_moving_averages
  ```
  Expected: PASS.

- [ ] **Step 4: Run full validation corpus sweep**

  Run:
  ```bash
  ./scripts/run_corpus.sh
  ```
  Expected: 0.00% drift and zero order differences across all 100 corpus strategies.

- [ ] **Step 5: Commit**

  ```bash
  git add include/pineforge/ta.hpp src/ta_moving_averages.cpp
  git commit -m "perf(ta): optimize SMA to O(1) rolling sum with power-of-two bitwise self-correction"
  ```


### Task 4: Codegen-Assisted Pre-Calculation (Static AST Analyzer)

**Files:**
- Modify: `../pineforge-codegen/pineforge_codegen/analyzer/types.py` (or relevant analyzer file)

- [ ] **Step 1: Add AST analysis static tracking rules**

  Modify `pineforge_codegen` to recursively analyze AST expression nodes. Assign an `is_static_series` flag to indicators and variables:
  - Global scope calls are eligible.
  - Native OHLCV inputs, literals, and parameters are base static series.
  - Recursively trace variable dependencies to verify that no dynamic strategy variables (like `strategy.position_size`) are referenced in any child branch.

- [ ] **Step 2: Add unit tests to verify AST static analysis flags**

  Modify/create analyzer tests in `../pineforge-codegen/tests/` to assert that:
  - `ta.sma(close, 20)` evaluates to `is_static = True`.
  - `ta.sma(strategy.position_size, 20)` evaluates to `is_static = False`.

- [ ] **Step 3: Commit**

  ```bash
  git add ../pineforge-codegen/pineforge_codegen/analyzer/ ../pineforge-codegen/tests/
  git commit -m "feat(codegen): implement recursive AST static series analyzer rules"
  ```


### Task 5: Codegen Emitter for Contiguous Pre-Calculation Loops

**Files:**
- Modify: `../pineforge-codegen/pineforge_codegen/codegen/emit_top.py`, `../pineforge-codegen/pineforge_codegen/codegen/base.py`

- [ ] **Step 1: Generate precalc vectors and bulk loop**

  Modify code generation to declare `std::vector<double> _ta_precalc_...;` inside the `GeneratedStrategy` class. Generate a pre-calculation contiguous loop inside `GeneratedStrategy::run` (or an optimized helper called beforehand).

- [ ] **Step 2: Replace runtime call with constant-time lookup**

  Update the transpiler expression generator. For any static indicator, replace `_ta_sma_1.compute(price)` in the generated `on_bar()` callback with simple index retrieval: `_ta_precalc_sma_1_[bar_index_]`.

- [ ] **Step 3: Recompile and run all benchmark strategies**

  Re-run transpilation on all 100 strategies and run benchmarks:
  ```bash
  ./reproduce.sh
  ```
  Expected: Benchmark compile and execution are fully functional.

- [ ] **Step 4: Commit and finalize**

  ```bash
  git add ../pineforge-codegen/pineforge_codegen/codegen/
  git commit -m "feat(codegen): implement contiguous SIMD pre-calculation loops and runtime lookups"
  ```
