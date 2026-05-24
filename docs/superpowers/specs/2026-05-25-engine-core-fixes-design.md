---
name: engine-core-fixes
description: Fixes 8 critical issues in engine and codegen (Approach A)
metadata:
  type: project
---

# Design Spec — Approach A (Core Engine State & Accessors)

## 1. Engine Changes (`BacktestEngine` in `pineforge-engine`)

- **State fields (`include/pineforge/engine.hpp`)**:
  - `int64_t last_bar_time_ = 0;`
  - `int last_bar_index_ = 0;`

- **Update run loops (`src/engine_run.cpp`)**:
  - At start of `run(const Bar* input_bars, int n_input, ...)` and single-timeframe `run(const Bar* bars, int n)`:
    - Guard: `if (n_input > 0 && input_bars != nullptr) { last_bar_time_ = input_bars[n_input - 1].timestamp; }`
  - Inside `run_simple_bar_loop` and `run_aggregation_bar_loop` once expected script bars are computed:
    `last_bar_index_ = expected_script_bars - 1;`

- **New member fns (`include/pineforge/engine.hpp` / `src/engine.cpp`)**:
  - `std::string position_entry_name() const`
    ```cpp
    std::string position_entry_name() const {
        if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return "";
        return pyramid_entries_.back().entry_id;
    }
    ```
  - `double max_drawdown_percent() const`
    ```cpp
    double max_drawdown_percent() const {
        return (initial_capital_ > 0.0) ? (max_drawdown_ / initial_capital_) * 100.0 : 0.0;
    }
    ```
  - `int64_t time_close() const`
    ```cpp
    int64_t time_close() const {
        return pine_time_close(current_bar_.timestamp, script_tf_, syminfo_.session, syminfo_.timezone, script_tf_);
    }
    ```

## 2. Codegen Changes (`pineforge-codegen`)

- **Signature Registrations (`pineforge_codegen/signatures.py`)**:
  - Add `"last_bar_time": I` (or similar depending on signature file format) to support variable registration.

- **Analyzer Tables (`pineforge_codegen/analyzer/tables.py`)**:
  - Add `"last_bar_time": PineType.INT` to `BUILTIN_VARS`.

- **Variable Table Mapping (`pineforge_codegen/codegen/tables.py`)**:
  - `"last_bar_time": "last_bar_time_"`
  - `"last_bar_index": "last_bar_index_"`
  - `"time_close": "time_close()"`

- **Member Access (`pineforge_codegen/codegen/visit_expr.py`)**:
  - `strategy.position_entry_name` → `"position_entry_name()"`
  - `strategy.max_drawdown_percent` → `"max_drawdown_percent()"`
  - `strategy.account_currency` → `"syminfo_.currency"`
  - Under `sub == "commission"`:
    - `node.member == "percent"` → `"0"`
    - `node.member == "cash_per_order"` → `"1"`
    - `node.member == "cash_per_contract"` → `"2"`

- **Function Calls (`pineforge_codegen/codegen/visit_call.py`)**:
  - `color.rgb()`: If 4 args, pass 4th arg (`transp`) as second param to `pine_color::new_color`.

## 3. Verification Protocol

- Run pytest in `pineforge-codegen`
- Run ctest in `pineforge-engine`
- Rebuild engine, regenerate CPP, run `scripts/run_corpus.sh` to verify no regressions on existing trades.
