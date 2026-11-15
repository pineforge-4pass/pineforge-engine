// One target: bounded owning-window checkpoint work during real fill
// reexecution, with independent value, ordinary-execution, and failure controls.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
namespace heap = global_allocation;

constexpr int bars_count = 384;
constexpr int first_length = 20;
constexpr int indicators_count = 40;
constexpr int last_length = first_length + indicators_count - 1;
enum class Kind { sma, stdev, bb, change, ema, highest, wma, none };

std::vector<Bar> feed() {
    std::vector<Bar> result;
    for (int i = 0; i < bars_count; ++i) {
        const int phase = i % 48;
        const int triangle = phase < 24 ? phase : 48 - phase;
        const double close = 100.0 + triangle / 4.0 + ((i * 17) % 7) / 16.0;
        result.push_back({close - 0.125, close + 0.25, close - 0.5,
                          close, 1000.0, 1'700'000'000'000LL + i * 3'600'000LL});
    }
    return result;
}

// Independent finite-input references. The dyadic inputs make window sums
// exact; the two-pass deviation does not use KahanWindowSum or StdDev.
template<Kind K>
double reference(const std::vector<Bar>& bars, int bar, int length) {
    if constexpr (K == Kind::change) {
        return bars[bar].close - bars[bar - length].close;
    } else if constexpr (K == Kind::ema) {
        double value = bars[0].close;
        const double alpha = 2.0 / (length + 1.0);
        for (int i = 1; i <= bar; ++i)
            value = alpha * bars[i].close + (1.0 - alpha) * value;
        return value;
    } else {
        double sum = 0.0, weighted = 0.0;
        double highest = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < length; ++i) {
            const auto& value = bars[bar - length + 1 + i];
            sum += value.close;
            weighted += value.close * (i + 1);
            highest = std::max(highest, value.high);
        }
        const double mean = sum / length;
        if constexpr (K == Kind::sma) return mean;
        if constexpr (K == Kind::highest) return highest;
        if constexpr (K == Kind::wma) return weighted / (length * (length + 1.0) / 2.0);
        double squares = 0.0;
        for (int i = bar - length + 1; i <= bar; ++i) {
            const double deviation = bars[i].close - mean;
            squares += deviation * deviation;
        }
        const double deviation = std::sqrt(squares / length);
        if constexpr (K == Kind::bb) return mean + 2.0 * deviation;
        return deviation;
    }
}

template<Kind K>
using IndicatorType = std::conditional_t<K == Kind::sma, ta::SMA,
    std::conditional_t<K == Kind::stdev, ta::StdDev,
    std::conditional_t<K == Kind::bb, ta::BB,
    std::conditional_t<K == Kind::change, ta::Change,
    std::conditional_t<K == Kind::highest, ta::Highest,
    std::conditional_t<K == Kind::wma, ta::WMA, ta::EMA>>>>>>;

template<Kind K>
struct Indicator {
    int length;
    IndicatorType<K> value;

    static IndicatorType<K> make(int length) {
        if constexpr (K == Kind::bb) return ta::BB(length, 2.0);
        else if constexpr (K == Kind::ema) return ta::EMA(length, ta::EmaSeeding::FirstValue);
        else return IndicatorType<K>(length);
    }
    explicit Indicator(int length) : length(length), value(make(length)) {}

    double apply(const Bar& bar, bool advance) {
        if constexpr (K == Kind::bb) {
            return (advance ? value.compute(bar.close) : value.recompute(bar.close)).upper;
        } else if constexpr (K == Kind::change) {
            return advance ? value.compute(bar.close, length)
                           : value.recompute(bar.close, length);
        } else {
            const double src = K == Kind::highest ? bar.high : bar.close;
            return advance ? value.compute(src) : value.recompute(src);
        }
    }
};

template<Kind K, std::size_t... I>
auto indicators(std::index_sequence<I...>) {
    return std::array<Indicator<K>, sizeof...(I)>{{
        Indicator<K>(first_length + static_cast<int>(I))...}};
}

template<Kind K>
struct ScriptState {
    decltype(indicators<K>(std::make_index_sequence<
        K == Kind::none ? 0 : indicators_count>{})) calls =
        indicators<K>(std::make_index_sequence<K == Kind::none ? 0 : indicators_count>{});
    ta::EMA fast{3, ta::EmaSeeding::FirstValue}, slow{8, ta::EmaSeeding::FirstValue};
    ta::Crossover up;
    ta::Crossunder down;
};

template<Kind K>
class FillCheckpoint final : public source::PineStrategyHost {
    ScriptState<K> state_;
    std::optional<ScriptState<K>> checkpoint_;
    const std::vector<Bar>& bars_;

    void take_checkpoint() {
        const auto before = heap::allocations;
        // Like generated _PFScriptState: take independent values, then
        // transfer that aggregate into the optional checkpoint.
        checkpoint_.emplace(ScriptState<K>(state_));
        const auto work = heap::allocations - before;
        ++checkpoint_calls;
        if (snapshot_calls > last_length) {
            ++warm_checkpoint_calls;
            peak_checkpoint_allocations = std::max(peak_checkpoint_allocations, work);
        }
    }

public:
    int callbacks = 0, checked_values = 0;
    int snapshot_calls = 0, restore_calls = 0, checkpoint_calls = 0;
    int warm_checkpoint_calls = 0;
    std::size_t peak_checkpoint_allocations = 0, warm_restore_allocations = 0;
    std::array<double, bars_count> totals{};

    FillCheckpoint(const std::vector<Bar>& bars, bool fills) : bars_(bars) {
        source::PineStrategyConfig config;
        config.calc_on_order_fills = fills;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 0.01;
        config.pyramiding = 1;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar& bar) override {
        ++callbacks;
        assert(bar_index_ >= 0 && bar_index_ < bars_count);
        const bool advance = history_advances_new_bar();
        double total = K == Kind::none ? bar.close : 0.0;
        for (auto& call : state_.calls) {
            const double value = call.apply(bar, advance);
            if (bar_index_ >= last_length) {
                const double expected = reference<K>(bars_, bar_index_, call.length);
                assert(std::isfinite(value));
                assert(std::fabs(value - expected) <= 1e-8 * std::max(1.0, std::fabs(expected)));
                ++checked_values;
            }
            total += value;
        }
        totals[bar_index_] = total;

        const double fast = advance ? state_.fast.compute(bar.close)
                                    : state_.fast.recompute(bar.close);
        const double slow = advance ? state_.slow.compute(bar.close)
                                    : state_.slow.recompute(bar.close);
        const bool up = advance ? state_.up.compute(fast, slow) : state_.up.recompute(fast, slow);
        const bool down = advance ? state_.down.compute(fast, slow)
                                  : state_.down.recompute(fast, slow);
        if (up) strategy_entry("Long", true, na<double>(), na<double>(), 0.01);
        if (down) strategy_entry("Short", false, na<double>(), na<double>(), 0.01);
    }

protected:
    void snapshot_script_state() override {
        ++snapshot_calls;
        take_checkpoint();
    }
    void restore_script_state() override {
        assert(checkpoint_);
        const auto before = heap::allocations;
        state_ = *checkpoint_;
        const auto work = heap::allocations - before;
        ++restore_calls;
        if (snapshot_calls > last_length) warm_restore_allocations += work;
    }
    void commit_script_state() override { take_checkpoint(); }
};

template<Kind K>
bool order_fill_checkpoint_work() {
    const auto bars = feed();
    FillCheckpoint<K> ordinary(bars, false), fills(bars, true);
    ordinary.run(bars.data(), static_cast<int>(bars.size()));
    fills.run(bars.data(), static_cast<int>(bars.size()));
    assert(ordinary.last_error().empty() && fills.last_error().empty());
    assert(ordinary.callbacks == bars_count);
    assert(fills.callbacks > ordinary.callbacks); // Real fills actually reexecuted the body.
    assert(ordinary.trade_count() > 0 && ordinary.trade_count() == fills.trade_count());
    for (int i = 0; i < ordinary.trade_count(); ++i) {
        const auto& a = ordinary.get_trade(i);
        const auto& b = fills.get_trade(i);
        assert(a.entry_price == b.entry_price && a.exit_price == b.exit_price);
        assert(a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index);
        assert(a.qty == b.qty && a.pnl == b.pnl && a.is_long == b.is_long);
        assert(a.entry_time == b.entry_time && a.exit_time == b.exit_time);
    }
    assert(ordinary.checkpoint_calls == 0 && ordinary.restore_calls == 0);
    assert(fills.snapshot_calls > 0 && fills.restore_calls > 0 && fills.warm_checkpoint_calls > 0);
    if constexpr (K != Kind::none) {
        assert(ordinary.checked_values > 0 && fills.checked_values > ordinary.checked_values);
    }
    for (int i = last_length; i < bars_count; ++i) {
        assert(std::isfinite(fills.totals[i]));
        assert(fills.totals[i] == ordinary.totals[i]);
    }
    assert(*std::min_element(fills.totals.begin() + last_length, fills.totals.end())
           < *std::max_element(fills.totals.begin() + last_length, fills.totals.end()));

    if constexpr (K == Kind::sma || K == Kind::stdev || K == Kind::bb || K == Kind::change) {
        constexpr int buffers = K == Kind::stdev ? 2 : K == Kind::bb ? 3 : 1;
        // Two value transfers, 40 indicators: each window may allocate ONE
        // owning buffer per transfer. This is an operation bound, not timing.
        assert(fills.peak_checkpoint_allocations > 0);
        const bool bounded = fills.peak_checkpoint_allocations <= 2 * indicators_count * buffers
            && fills.warm_restore_allocations == 0;
        if (!bounded) {
            constexpr const char* names[] = {"sma", "stdev", "bb", "change"};
            std::fprintf(stderr, "%s: peak checkpoint allocations=%zu (bound=%d), "
                         "warm restore allocations=%zu\n",
                         names[static_cast<std::size_t>(K)], fills.peak_checkpoint_allocations,
                         2 * indicators_count * buffers, fills.warm_restore_allocations);
        }
        return bounded;
    }
    return true;
}

double source_value(int i) {
    if (i % 29 == 0) return -0.0;
    return std::ldexp(static_cast<double>((i * 37) % 101 - 50) / 10.0, i % 19 - 9);
}

KahanWindowSum replay(int length, const std::vector<double>& history) {
    KahanWindowSum result(length);
    for (double value : history) {
        if (is_na(value)) result.note_no_push();
        else result.push(value);
    }
    return result;
}

void same(const KahanWindowSum& a, const KahanWindowSum& b) {
    assert(a.length() == b.length() && a.count() == b.count() && a.seeded() == b.seeded());
    const double x = a.sum(), y = b.sum();
    assert(std::memcmp(&x, &y, sizeof(double)) == 0);
}

void window_rewind_and_failure() {
    bool saw_growth = false, saw_copy_failure = false;
    for (int length : {1, 2, 7, 8, 9, 20, 31, 32, 33, 59, 65, 129}) {
        KahanWindowSum window(length);
        std::vector<double> history;
        for (int bar = 0; bar < 3 * length + 17; ++bar) {
            const auto before = window;
            auto probe = window;
            const auto allocations = heap::allocations;
            probe.push(source_value(bar));
            const auto growth = heap::allocations - allocations;
            for (std::size_t failure = 0; failure < growth; ++failure) {
                saw_growth = true;
                heap::fail_after = failure;
                const auto refused = heap::refused;
                bool caught = false;
                try { window.push(source_value(bar)); }
                catch (const std::bad_alloc&) { caught = true; }
                heap::fail_after = std::numeric_limits<std::size_t>::max();
                assert(caught && heap::refused == refused + 1);
                same(window, before);
                auto rewind = window, expected = before;
                rewind.unpush();
                expected.unpush();
                same(rewind, expected);
            }
            if (bar % 11 == 0) {
                window.note_no_push();
                const auto checkpoint = window;
                window.repush(source_value(bar));
                window = checkpoint;
                window.unpush(); // NA must not rewind the previous bar.
                same(window, before);
                history.push_back(na<double>());
                continue;
            }
            window.push(source_value(bar));
            const auto checkpoint = window;
            window.repush(source_value(bar + 23));
            window.repush(source_value(bar + 23));
            auto replaced = history;
            replaced.push_back(source_value(bar + 23));
            same(window, replay(length, replaced));
            window.unpush();
            window.unpush();
            same(window, before);
            window = checkpoint;
            auto copy = checkpoint;
            copy.repush(source_value(bar + 71));
            replaced.back() = source_value(bar + 71);
            same(copy, replay(length, replaced));
            same(window, checkpoint);
            window.unpush();
            same(window, before);
            window = checkpoint;
            history.push_back(source_value(bar));
            same(window, replay(length, history));
            const auto* self = &window;
            window = *self;
            same(window, replay(length, history));
        }

        KahanWindowSum target(1);
        if (length > 1) target.push(source_value(700)); // Assignment must need a larger buffer.
        const auto before = target;
        const auto allocations = heap::allocations;
        { const auto copy = window; same(copy, window); }
        const auto copy_work = heap::allocations - allocations;
        assert(copy_work > 0);
        for (std::size_t failure = 0; failure < copy_work; ++failure) {
            saw_copy_failure = true;
            heap::fail_after = failure;
            const auto refused = heap::refused;
            bool caught = false;
            try { target = window; }
            catch (const std::bad_alloc&) { caught = true; }
            heap::fail_after = std::numeric_limits<std::size_t>::max();
            assert(caught && heap::refused == refused + 1);
            same(target, before);
            auto expected = before;
            target.repush(source_value(701));
            expected.repush(source_value(701));
            same(target, expected);
            target = before;
        }
    }
    assert(saw_growth && saw_copy_failure);
}

double change_reference(const std::vector<double>& history, int length) {
    if (history.size() <= static_cast<std::size_t>(length)) return na<double>();
    return history.back() - history[history.size() - 1 - length];
}

void same_value(double actual, double expected) {
    assert((is_na(actual) && is_na(expected)) || actual == expected);
}

void change_history_and_failure() {
    bool saw_growth = false;
    for (int maximum : {1, 20, 59}) {
        ta::Change change(maximum);
        std::vector<double> history;
        for (int bar = 0; bar < 240; ++bar) {
            const int length = (bar * 13) % 67; // Grow AND shrink the requested history, including zero.
            const double value = bar % 17 == 0 ? na<double>() : source_value(bar);
            auto probe = change;
            const auto allocations = heap::allocations;
            probe.compute(value, length);
            const auto growth = heap::allocations - allocations;
            for (std::size_t failure = 0; failure < growth; ++failure) {
                saw_growth = true;
                heap::fail_after = failure;
                const auto refused = heap::refused;
                bool caught = false;
                try { change.compute(value, length); }
                catch (const std::bad_alloc&) { caught = true; }
                heap::fail_after = std::numeric_limits<std::size_t>::max();
                assert(caught && heap::refused == refused + 1);
                auto after_failure = change;
                auto before_inputs = history;
                if (!before_inputs.empty()) {
                    before_inputs.back() = source_value(bar + 71);
                    same_value(after_failure.recompute(before_inputs.back(), length),
                               change_reference(before_inputs, length));
                }
            }
            history.push_back(value);
            const auto keep = static_cast<std::size_t>(std::max(maximum, length)) + 1;
            if (history.size() > keep) history.erase(history.begin(), history.end() - keep);
            same_value(change.compute(value, length), change_reference(history, length));
            const auto checkpoint = change;
            auto replaced = history;
            replaced.back() = source_value(bar + 23);
            same_value(change.recompute(replaced.back(), length), change_reference(replaced, length));
            same_value(change.recompute(replaced.back(), length), change_reference(replaced, length));
            change = checkpoint;
            same_value(change.recompute(value, length), change_reference(history, length));
        }
    }
    assert(saw_growth);
}

void unbiased_checkpoint_control() {
    for (int length : {2, 7}) {
        ta::StdDev deviation(length, false);
        std::vector<double> history;
        const auto expected = [&](const std::vector<double>& inputs) {
            if (inputs.size() < static_cast<std::size_t>(length)) return na<double>();
            double sum = 0.0;
            for (double value : inputs) sum += value;
            const double mean = sum / length;
            double squares = 0.0;
            for (double value : inputs) squares += (value - mean) * (value - mean);
            return std::sqrt(squares / (length - 1));
        };
        for (int bar = 0; bar < 40; ++bar) {
            const double value = bar % 9 == 0 ? na<double>() : source_value(bar);
            if (!is_na(value)) {
                history.push_back(value);
                if (history.size() > static_cast<std::size_t>(length))
                    history.erase(history.begin());
            }
            same_value(deviation.compute(value), expected(history));
            const auto checkpoint = deviation;
            auto replacement = history;
            if (replacement.empty()) replacement.push_back(source_value(bar + 23));
            else replacement.back() = source_value(bar + 23);
            same_value(deviation.recompute(replacement.back()), expected(replacement));
            ta::StdDev restored(1); // Restoring also changes from biased to unbiased storage.
            restored = checkpoint;
            same_value(restored.recompute(replacement.back()), expected(replacement));
            deviation = checkpoint;
        }
    }
}

template<class Window, class Push>
void observed_storage(Window& window, Push push) {
    // Only this owner's real allocation requests are marked. Observer and
    // comparison scratch allocations remain outside the census.
    heap::recording = true;
    for (int i = 0; i < 200; ++i) push(window, source_value(i));
    heap::recording = false;
    const auto source_bytes = heap::live_bytes, source_blocks = heap::live_blocks;
    assert(source_bytes > 0 && source_blocks > 0);
    {
        heap::recording = true;
        const auto copy = window;
        heap::recording = false;
        OwnedStorage storage;
        window.observe_owned_storage(storage);
        copy.observe_owned_storage(storage);
        copy.observe_owned_storage(storage);
        assert(storage.usage().complete());
        assert(storage.usage().resident_bytes() == heap::live_bytes);
    }
    assert(heap::live_bytes == source_bytes && heap::live_blocks == source_blocks);
}

void storage_ownership() {
    {
        KahanWindowSum window(65);
        observed_storage(window, [](auto& value, double src) { value.push(src); });
    }
    assert(heap::live_bytes == 0 && heap::live_blocks == 0);
    {
        ta::Change change(65);
        observed_storage(change, [](auto& value, double src) { value.compute(src, 65); });
    }
    assert(heap::live_bytes == 0 && heap::live_blocks == 0);
}
} // namespace

int main() {
    // Run every reported row before the allocation-work verdict, so a main
    // failure includes genuine fill work and diagnostics for all four rows.
    bool bounded = true;
    bounded &= order_fill_checkpoint_work<Kind::sma>();
    bounded &= order_fill_checkpoint_work<Kind::stdev>();
    bounded &= order_fill_checkpoint_work<Kind::bb>();
    bounded &= order_fill_checkpoint_work<Kind::change>();
    bounded &= order_fill_checkpoint_work<Kind::ema>();
    bounded &= order_fill_checkpoint_work<Kind::highest>();
    bounded &= order_fill_checkpoint_work<Kind::wma>();
    bounded &= order_fill_checkpoint_work<Kind::none>();
    assert(bounded && "reported indicator checkpoint allocation work is bounded");
    window_rewind_and_failure();
    change_history_and_failure();
    unbiased_checkpoint_control();
    storage_ownership();
}
