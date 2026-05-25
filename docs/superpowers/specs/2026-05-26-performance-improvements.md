# PineForge Core Engine & Codegen Performance Spec

This specification details the architecture and implementation design for major speed improvements across the `pineforge-engine` and `pineforge-codegen` systems, fully refined through our C++/quantitative compiler systems audit.

---

## 1. Goal

Maximize backtest throughput by:
1. **Eliminating all run-time heap allocations** inside the core event-driven backtesting bar/tick loop (Approach A: C++ Engine Ring Buffers).
2. **Pre-calculating static technical indicators** in contiguous SIMD-vectorizable blocks before the backtest starts (Approach B: Codegen-assisted Pre-computation), fully compatible with bar magnifier upsampling and multi-timeframe aggregation.

---

## 2. Approach A: Zero-Allocation C++ Ring Buffers

Currently, `Series<T>` and heavy indicator moving-windows (`ta::SMA`, `ta::Highest`, `ta::Lowest`, `ta::Change`) utilize `std::deque<T>` buffers. Each bar progression calls `push_back`/`push_front` and `pop_front`/`pop_back`, causing heap allocation churn, pointer-chasing across non-contiguous blocks, and CPU cache misses.

### 2.1. Dynamic Ring Buffer Design (`DynamicRingBuffer`)
To avoid template instantiation bloat and support run-time variable indicator periods, we will use a **run-time dynamic capacity ring buffer** backed by a contiguous `std::vector` pre-allocated *once* in its constructor. This achieves exactly zero heap allocations in the hot loop while providing full capacity flexibility.

Further, we enforce the **initialized memory invariant**: the entire buffer is explicitly filled with `na<T>()` at construction to guarantee that any out-of-history lookback safely returns the designated null representation instead of undefined behavior (UB).

```cpp
template<typename T>
class DynamicRingBuffer {
    std::vector<T> buffer_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;

public:
    explicit DynamicRingBuffer(std::size_t capacity)
        : buffer_(capacity, na<T>()), capacity_(capacity) {}

    void push_front(T val) {
        if (capacity_ == 0) return;
        if (size_ == 0) {
            buffer_[0] = val;
            size_ = 1;
            head_ = 0;
        } else {
            head_ = (head_ == 0) ? capacity_ - 1 : head_ - 1;
            buffer_[head_] = val;
            if (size_ < capacity_) {
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
        if (offset >= size_ || capacity_ == 0) {
            return na<T>();
        }
        return buffer_[(head_ + offset) % capacity_];
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }

    void clear() {
        head_ = 0;
        size_ = 0;
        std::fill(buffer_.begin(), buffer_.end(), na<T>());
    }

    void resize(std::size_t new_capacity) {
        if (new_capacity == capacity_) return;
        std::vector<T> new_buffer(new_capacity, na<T>());
        std::size_t new_size = std::min(size_, new_capacity);
        for (std::size_t i = 0; i < new_size; ++i) {
            new_buffer[i] = (*this)[i];
        }
        buffer_ = std::move(new_buffer);
        capacity_ = new_capacity;
        head_ = 0;
        size_ = new_size;
    }
};
```

### 2.2. Integration Plan
1. Replace `std::deque` with `DynamicRingBuffer` inside:
   - `Series<T>`
   - `ta::SMA`, `ta::Highest`, `ta::Lowest`, `ta::Change`
2. **Self-Correcting, Fast $O(1)$ SMA Implementation**:
   - Keep a running sum `running_sum` inside `ta::SMA`.
   - On new bar, subtract oldest popped element and add the new element ($O(1)$ updates instead of $O(L)$ loops).
   - **Precision Drift Correction:** To eliminate catastrophic cancellation floating-point drift over $10^5+$ bars, perform a full recalculated sum periodically.
   - **Low-Level Bitwise Optimization:** To avoid the slow hardware integer division instruction (`idiv`), trigger full recalculations using a bitwise power-of-two mask: `if ((bar_count & 255) == 0)` (every 256 bars). This compiles to a single-cycle bitwise AND.

---

## 3. Approach B: Codegen-Assisted Contiguous Pre-Calculation

Technical indicators (e.g. `ta.sma(close, 20)`) are currently computed sequentially bar-by-bar inside `on_bar()`. If the inputs to these indicators depend strictly on raw historical prices (and do not depend on dynamic strategy states like open positions or conditionally assigned variables), we can pre-compute the entire indicator series into a contiguous `std::vector` before starting the loop.

### 3.1. Timeframe Aggregation & Bar Magnifier Compatibility
To ensure 100% mathematical parity with the original event-driven engine under **bar magnifier** or **multi-timeframe aggregation**:
1. **Pre-aggregation pass:** If the script timeframe coarser than the input timeframe (e.g. 15m script on 1m input), the engine executes a rapid, non-stateful aggregation pass over the input bars to construct the full array of script-level bars.
2. **Contiguous loop calculation:** Run optimized vector calculations (like SIMD-vectorized SMA/EMA loops) over the aggregated script-level price series.
3. **Bar magnifier invariance:** Since the bar magnifier only triggers order-matching logic on intermediate intra-bar ticks and only executes strategy logic (`on_bar()`) once per script bar at the final tick using the aggregated script bar's final values, the indicators' inputs are identical to the closing prices of the script-level bars. Pre-calculated values remain perfectly accurate.
4. **Run-time fetching:** Inside `on_bar()`, the generated code replaces `_ta_sma.compute(price)` with `mafast = _ta_sma_precalc_[bar_index_]`, resulting in a simple constant-time array index lookup!

### 3.2. Strict Global Scope Invariant
If an indicator is executed inside a conditional block (`if`, `for`, `while`, ternaries), its state updates depend on runtime conditions.
* **The Rule:** The code generator's AST analyzer must **only** pre-calculate indicators that are invoked unconditionally at global scope. Any indicator inside a conditional block must continue to use the runtime event-driven path.

### 3.3. AST Dependency Analysis for Static vs. Dynamic Arguments
The transpiler recursively verifies the static nature of all expressions:
* **Static Series:** Native OHLCV series, literal constants, inputs (`input.*`), and other static indicators/variables (e.g. `my_val = close > open ? high : low` is static because its state on bar $t$ depends purely on native price coordinates on bar $t$).
* **Dynamic Variables:** Any variable depending on execution state, positions, or filled trades (e.g. `strategy.position_size`, `strategy.equity`).
* **The Rule:** An indicator can be pre-calculated if and only if **all its arguments are static series** and it is defined at **global scope**.

### 3.4. Multi-Timeframe (`request.security`) Pre-calculation
For indicators loaded via `request.security` (e.g., `daily_sma = request.security(ticker, "1D", ta.sma(close, 20))`):
1. Pre-aggregate input bars to daily bars.
2. Pre-calculate the SMA(20) on the contiguous daily bars.
3. Map/Merge the resulting Daily SMA vector back to the main timeframe series according to lookahead rules.
4. Inside the hot loop, fetching the daily indicator reduces to a constant-time vector index lookup.

---

## 4. Test & Verification Plan

1. **Parity Check:** All 100 benchmark strategies must pass the validation corpus sweep (`./scripts/run_corpus.sh`) with exactly 0.00% drift and zero order differences.
2. **Speed Sweep:** Run `reproduce.sh` and `time_vectorbt.py` to compare performance before and after. Expecting an additional 2× to 5× throughput increase (million of bars per second) across all 100 strategies.
