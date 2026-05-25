# PineForge Core Engine & Codegen Performance Spec

This specification details the architecture and implementation design for major speed improvements across the `pineforge-engine` and `pineforge-codegen` systems.

---

## 1. Goal

Maximize backtest throughput by:
1. **Eliminating all run-time heap allocations** inside the core event-driven backtesting bar/tick loop (Approach A: C++ Engine Ring Buffers).
2. **Pre-calculating static technical indicators** in contiguous SIMD-vectorizable blocks before the backtest starts (Approach B: Codegen-assisted Pre-computation), fully compatible with bar magnifier upsampling and multi-timeframe aggregation.

---

## 2. Approach A: Zero-Allocation C++ Ring Buffers

Currently, `Series<T>` and heavy indicator moving-windows (`ta::SMA`, `ta::Highest`, `ta::Lowest`, `ta::Change`) utilize `std::deque<T>` buffers. Each bar progression calls `push_back`/`push_front` and `pop_front`/`pop_back`, causing heap allocation churn, pointer-chasing across non-contiguous blocks, and CPU cache misses.

### 2.1. Circular Ring Buffer Design
We will introduce a highly optimized, header-only `RingBuffer<T>` template inside `include/pineforge/series.hpp`. It pre-allocates a single contiguous block of memory of the specified maximum capacity at construction time, requiring **exactly zero heap allocations** during run-time execution.

```cpp
template<typename T, std::size_t MaxCapacity = 500>
class FixedRingBuffer {
    T buffer_[MaxCapacity];
    std::size_t head_ = 0;
    std::size_t size_ = 0;

public:
    FixedRingBuffer() = default;

    void push_front(T val) {
        if (size_ == 0) {
            buffer_[0] = val;
            size_ = 1;
            head_ = 0;
        } else {
            head_ = (head_ == 0) ? MaxCapacity - 1 : head_ - 1;
            buffer_[head_] = val;
            if (size_ < MaxCapacity) {
                size_++;
            }
        }
    }

    void update_front(T val) {
        if (size_ == 0) {
            push_front(val);
        } else {
            buffer_[head_] = val;
        }
    }

    T operator[](std::size_t offset) const {
        if (offset >= size_) {
            return na<T>();
        }
        return buffer_[(head_ + offset) % MaxCapacity];
    }

    std::size_t size() const { return size_; }
    void clear() { head_ = 0; size_ = 0; }
};
```

### 2.2. Integration Plan
1. Replace `std::deque` with `FixedRingBuffer` (or dynamic capacity equivalent `RingBuffer` using `std::vector` pre-allocated inside constructor) in:
   - `Series<T>`
   - `ta::SMA`
   - `ta::Highest` / `ta::Lowest`
   - `ta::Change`
2. **Optimize `ta::SMA` performance from $O(L)$ to $O(1)$**:
   - Keep a running sum `double sum_` inside `ta::SMA`.
   - When a new value is pushed, subtract the popped value and add the new value.
   - Periodically recalculate the sum every 500 bars to prevent floating-point precision drift. This yields an incredible order-of-magnitude speedup for large SMA lengths.

---

## 3. Approach B: Codegen-Assisted Contiguous Pre-Calculation

Technical indicators (e.g. `ta.sma(close, 20)`) are currently computed sequentially bar-by-bar inside `on_bar()`. If the inputs to these indicators depend strictly on raw historical prices (and do not depend on dynamic strategy states like open positions or conditionally assigned variables), we can pre-compute the entire indicator series into a contiguous `std::vector` before starting the loop.

### 3.1. Timeframe Aggregation & Bar Magnifier Compatibility
To ensure 100% mathematical parity with the original event-driven engine under **bar magnifier** or **multi-timeframe aggregation**:
1. **Pre-aggregation pass:** If the script timeframe coarser than the input timeframe (e.g. 15m script on 1m input), the engine executes a rapid, non-stateful aggregation pass over the input bars to construct the full array of script-level bars.
2. **Contiguous loop calculation:** Run optimized vector calculations (like SIMD-vectorized SMA/EMA loops) over the aggregated script-level price series.
3. **Bar magnifier invariance:** Since the bar magnifier only triggers order-matching logic on intermediate intra-bar ticks and only executes strategy logic (`on_bar()`) once per script bar at the final tick using the aggregated script bar's final values, the indicators' inputs are identical to the closing prices of the script-level bars. Pre-calculated values remain perfectly accurate.
4. **Run-time fetching:** Inside `on_bar()`, the generated code replaces `_ta_sma.compute(price)` with `mafast = _ta_sma_precalc_[bar_index_]`, resulting in a simple constant-time array index lookup!

### 3.2. Codegen Spec Analysis
`pineforge-codegen` will analyze the AST (Abstract Syntax Tree) to flag static indicators:
- An indicator is **static** if all its inputs are derived from:
  - `open`, `high`, `low`, `close`, `volume`, `time` (standard OHLCV series)
  - Literal numbers or constant inputs
  - Other static indicator results
- If static, the code generator:
  - Allocates a contiguous `std::vector<double>` inside `GeneratedStrategy` class.
  - Generates the contiguous pre-calculation loop inside `GeneratedStrategy::run` or a helper method called right before the bar loop begins.
  - Replaces calculation in `on_bar()` with vector lookup: `value = _s_indicator_precalc_[bar_index_]`.

---

## 4. Test & Verification Plan

1. **Parity Check:** All 100 benchmark strategies must pass the validation corpus sweep (`./scripts/run_corpus.sh`) with exactly 0.00% drift and zero order differences.
2. **Speed Sweep:** Run `reproduce.sh` and `time_vectorbt.py` to compare performance before and after. Expecting an additional 2× to 5× throughput increase (million of bars per second) across all 100 strategies.
