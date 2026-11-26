/*
 * pine_ta_length.cpp — ta.* calls whose length is not a constant: checked
 * simple lengths, series-length window extremes, the first-execution
 * supertrend factor. The TradingView rules and their tapes are documented in
 * <pineforge/source/pine_ta_length.hpp>.
 */

#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>

#include "window_sum_expansion.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace pineforge {
namespace source {

namespace {

// Use the run-failure prefix for both evaluated calls and validation before
// an evaluation context exists.
[[noreturn]] void length_not_positive(long long value, const char* function,
                                      const char* argument) {
    throw coded<std::runtime_error>(
        RunFailureCode::pine_invalid_argument,
        {{"function", function},
         {"argument", argument},
         {"rule", "must_be_positive"}},
        run_error_bar_prefix() + "Invalid value of the '" + argument +
            "' argument (" + std::to_string(value) + ") in the '" +
            function + "' function. It must be > 0.");
}

[[noreturn]] void length_na(const char* function, const char* argument) {
    throw coded<std::runtime_error>(
        RunFailureCode::pine_invalid_argument,
        {{"function", function},
         {"argument", argument},
         {"rule", "must_not_be_na"}},
        run_error_bar_prefix() + "Invalid value of the '" + argument +
            "' argument in the '" + function +
            "' function. It must not be na");
}

[[noreturn]] void max_bars_back_exceeded(std::uint64_t offset, std::int64_t maximum,
                                         bool beyond_uint64 = false) {
    throw coded<std::runtime_error>(
        RunFailureCode::pine_runtime_limit,
        {{"limit", "max_bars_back"}, {"max", maximum}},
        run_error_bar_prefix() +
        "The script attempts to reference historical data that is too far from "
        "the current bar (" + (beyond_uint64 ? "more than " : "") + std::to_string(offset) +
        " bars back). The historical buffer's limit is " +
        std::to_string(maximum) + " bars.");
}

void check_lookback(long long length, const char* function, std::int64_t maximum) {
    if (length <= 0) length_not_positive(length, function, "length");
    if (maximum != 0 && length - 1 > maximum) max_bars_back_exceeded(length - 1, maximum);
}

constexpr std::size_t kMaxHistorySlots = std::numeric_limits<std::size_t>::max() / sizeof(double);

// A limit beyond addressable storage cannot cause eviction. The byte extent
// check in grow_history still refuses a physical allocation that cannot fit.
std::size_t history_limit(std::int64_t maximum) {
    if (maximum == 0 || static_cast<std::uint64_t>(maximum) >= kMaxHistorySlots)
        return 0;
    return static_cast<std::size_t>(maximum) + 1;
}

std::size_t grow_history(std::size_t current, std::size_t minimum, std::size_t limit) {
    if (minimum > kMaxHistorySlots)
        throw coded<std::length_error>(RunFailureCode::scratch_capacity_exceeded,
            {{"resource", "layout"}}, "history byte extent is not representable");
    const auto grown = current ? std::min(kMaxHistorySlots, 2 * current) : HeldHistory::kChunk;
    return std::min(limit ? limit : kMaxHistorySlots, std::max(minimum, grown));
}

// A Pine int length carried as a double: na stays na, anything else
// truncates toward zero (Pine's int()), saturating far outside any window.
bool length_value(double value, long long& out) {
    if (is_na(value)) return false;
    const double t = std::trunc(value);
    if (t > 1e15) out = static_cast<long long>(1e15);
    else if (t < -1e15) out = static_cast<long long>(-1e15);
    else out = static_cast<long long>(t);
    return true;
}

[[noreturn]] void factor_not_positive(double factor) {
    char text[64];
    if (factor == std::trunc(factor) && std::fabs(factor) < 1e15) {
        std::snprintf(text, sizeof text, "%lld", static_cast<long long>(factor));
    } else {
        std::snprintf(text, sizeof text, "%.15g", factor);
    }
    throw coded<std::runtime_error>(
        RunFailureCode::pine_invalid_argument,
        {{"function", "supertrend"},
         {"argument", "factor"},
         {"rule", "must_be_positive"}},
        run_error_bar_prefix() + "Invalid value of the 'factor' argument (" + text +
            ") in the 'supertrend' function. It must be > 0.");
}

}  // namespace

// --- simple lengths ---

int simple_ta_length(double value, const char* function, const char* argument) {
    long long length = 0;
    if (!length_value(value, length)) length_na(function, argument);
    if (length <= 0) length_not_positive(length, function, argument);
    return length > INT_MAX ? INT_MAX : static_cast<int>(length);
}

int simple_ta_length(int value, const char* function, const char* argument) {
    if (is_na(value)) length_na(function, argument);
    if (value <= 0) length_not_positive(value, function, argument);
    return value;
}

int simple_ta_length(std::int64_t value, const char* function, const char* argument) {
    if (is_na(value)) length_na(function, argument);
    if (value <= 0) length_not_positive(value, function, argument);
    return value > INT_MAX ? INT_MAX : static_cast<int>(value);
}

// --- HeldHistory ---

void HeldHistory::ensure_slots(std::size_t minimum, std::size_t keep) {
    if (minimum <= values_.size()) return;
    // The existing whole-chunk retirement can retain keep + kChunk - 1
    // members. Grow in whole chunks, leaving room for that write.
    const std::size_t max_chunks = kMaxHistorySlots / kChunk;
    if (minimum > max_chunks * kChunk)
        throw coded<std::length_error>(RunFailureCode::scratch_capacity_exceeded,
            {{"resource", "layout"}}, "history byte extent is not representable");
    const auto needed = minimum / kChunk + (minimum % kChunk != 0);
    const auto limit = keep == 0 ? max_chunks : keep / kChunk + (keep % kChunk != 0) + 1;
    const auto chunks = std::max(needed, std::min(std::min(limit, max_chunks),
        2 * (values_.size() / kChunk)));
    const std::size_t slots = chunks * kChunk;
    ::pineforge::detail::WindowValues<double> next(slots, na<double>(), values_.cache_capacity());
    for (long long i = 0; i < count_; ++i)
        next.write(static_cast<std::size_t>(i), at(first_bar_ + i));
    values_ = std::move(next);
    head_ = 0;
}

std::size_t HeldHistory::slot(std::size_t offset) const {
    const auto remaining = values_.size() - head_;
    return offset >= remaining ? offset - remaining : head_ + offset;
}

void HeldHistory::push(double value, std::size_t keep) {
    ensure_slots(static_cast<std::size_t>(count_) + 1, keep);
    values_.write(slot(static_cast<std::size_t>(count_)), value);
    ++count_;
}

void HeldHistory::drop_front_chunks(std::size_t keep) {
    if (keep == 0) return;
    while (count_ > static_cast<long long>(kChunk) &&
           static_cast<std::uint64_t>(count_ - static_cast<long long>(kChunk)) >= keep) {
        values_.retire_page(head_ / kChunk);
        head_ = slot(kChunk);
        count_ -= static_cast<long long>(kChunk);
        first_bar_ += static_cast<long long>(kChunk);
    }
}

void HeldHistory::write(long long bar, double value, std::size_t keep) {
    if (count_ == 0) {
        first_bar_ = bar;
        push(value, keep);
        return;
    }
    const long long last = last_bar();
    if (bar < last) return;           // an earlier bar is never rewritten
    if (bar == last) {
        values_.write(slot(static_cast<std::size_t>(count_ - 1)), value);
        return;
    }
    // Skipped bars hold the last value. With a cap, a longer gap restarts
    // the history at the first bar the window could still read.
    const double held = at(last);
    long long gap = bar - last - 1;
    if (keep != 0 && static_cast<std::uint64_t>(gap) >= keep) {
        values_.reset(values_.size(), na<double>());
        head_ = 0;
        count_ = 0;
        first_bar_ = bar - static_cast<long long>(keep);
        gap = static_cast<long long>(keep);
    }
    for (long long i = 0; i < gap; ++i) {
        push(held, keep);
        // Retiring complete chunks during the gap yields the same final base
        // as retiring after it, and keeps the physical ring bounded.
        drop_front_chunks(keep);
    }
    push(value, keep);
    drop_front_chunks(keep);
}

double HeldHistory::at(long long bar) const {
    return values_.read(slot(static_cast<std::size_t>(bar - first_bar_)));
}

// --- SeriesWindowExtreme ---

SeriesWindowExtreme::Result SeriesWindowExtreme::update(double src, long long length,
                                                        bool advance, bool want_max,
                                                        const char* function) {
    check_lookback(length, function, max_lookback_);
    long long bar;
    long long origin;
    const ta::BarContext& ctx = ta::bar_context();
    if (ctx.installed) {
        bar = ctx.bar_index;
        origin = ctx.origin;
    } else {
        // No context: every compute() is its own bar and recompute() rewrites
        // the current one, as ta::ExtremeRing counts.
        if (advance || !has_bar_) ++own_bar_;
        bar = own_bar_;
        origin = 0;
    }

    // Every call of one bar re-applies from the state the bar found.
    if (!has_bar_ || bar != current_bar_) {
        saved_state_ = state_;
        current_bar_ = bar;
        has_bar_ = true;
    } else {
        state_ = saved_state_;
    }
    BarState& s = state_;
    if (!s.started) first_exec_bar_ = bar;
    history_.write(bar, src, history_limit(max_lookback_));

    const auto better = [want_max](double a, double b) { return want_max ? a > b : a < b; };
    // Walks window offsets [from, to] oldest first into `running`.
    const auto walk = [&](long long from_k, long long to_k, Cache& running) {
        // Before origin there are no bars. Unrecorded bars from origin to
        // the first retained value all act as one na reset.
        from_k = std::min(from_k, bar - origin);
        const long long available = bar - std::max(first_exec_bar_, history_.first_bar());
        if (from_k >= to_k && from_k > available) {
            running.valid = false;
            from_k = available;
        }
        for (long long k = from_k; k >= to_k; --k) {
            const double v = history_.at(bar - k);
            if (is_na(v)) {
                running.valid = false;
                continue;
            }
            if (!running.valid || better(v, running.value)) {
                running.valid = true;
                running.value = v;
                running.bar = bar - k;
            }
        }
    };
    const auto rescan = [&]() {
        Cache fresh;
        walk(length - 1, 0, fresh);
        return fresh;
    };

    Cache answer;              // an answer this call gives without caching it
    bool na_source = false;
    if (is_na(src)) {
        s.cache.valid = false;
        s.started = true;
        na_source = true;
    } else if (!s.started) {
        s.cache = rescan();
        s.started = true;
    } else if (length > s.prev_length) {
        if (first_exec_bar_ > origin && length > s.max_length) s.cache.valid = false;
        if (!s.cache.valid || bar - s.cache.bar < length) {
            walk(length - 1, s.prev_length, s.cache);
        }
        if (!s.cache.valid) {
            answer.valid = true;
            answer.value = src;
            answer.bar = bar;
        } else if (better(src, s.cache.value)) {
            s.cache.value = src;
            s.cache.bar = bar;
        } else if (bar - s.cache.bar >= length) {
            s.cache = rescan();
        }
    } else {
        if (!s.cache.valid) {
            s.cache = rescan();
        } else if (better(src, s.cache.value)) {
            s.cache.value = src;
            s.cache.bar = bar;
        } else if (bar - s.cache.bar >= length) {
            s.cache = rescan();
        }
    }
    s.prev_length = length;
    s.max_length = std::max(s.max_length, length);

    if (bar - origin < length - 1) return Result{na<double>(), na<double>()};
    const Cache& result = answer.valid ? answer : s.cache;
    if (na_source || !result.valid) return Result{na<double>(), 0.0};
    return Result{result.value, -static_cast<double>(bar - result.bar)};
}

namespace {

std::uint64_t series_length(double length, const char* function, std::int64_t maximum) {
    if (!(length >= 1.0)) {
        long long value = 0;
        // TradingView reports an na length as 0 (RE10001).
        length_value(length, value);
        length_not_positive(value, function, "length");
    }
    if (length >= 0x1p64) {
        constexpr auto last = std::numeric_limits<std::uint64_t>::max();
        if (maximum != 0) max_bars_back_exceeded(last, maximum, length > 0x1p64);
        return last;
    }
    // The unsigned conversion preserves the integer part, including 2^63.
    const auto requested = static_cast<std::uint64_t>(length);
    if (maximum != 0 && requested - 1 > static_cast<std::uint64_t>(maximum))
        max_bars_back_exceeded(requested - 1, maximum);
    return requested;
}

}  // namespace

SeriesWindowExtreme::Result SeriesWindowExtreme::update_series(
        double src, double length, bool advance, bool want_max, const char* function) {
    const auto requested = series_length(length, function, max_lookback_);
    const auto window = std::min(requested, static_cast<std::uint64_t>(LLONG_MAX));
    return update(src, static_cast<long long>(window), advance, want_max, function);
}

double SeriesHighest::compute(double src, double length) {
    return core_.update_series(src, length, true, true, "highest").value;
}
double SeriesHighest::recompute(double src, double length) {
    return core_.update_series(src, length, false, true, "highest").value;
}
double SeriesLowest::compute(double src, double length) {
    return core_.update_series(src, length, true, false, "lowest").value;
}
double SeriesLowest::recompute(double src, double length) {
    return core_.update_series(src, length, false, false, "lowest").value;
}
double SeriesHighestBars::compute(double src, double length) {
    return core_.update_series(src, length, true, true, "highestbars").offset;
}
double SeriesHighestBars::recompute(double src, double length) {
    return core_.update_series(src, length, false, true, "highestbars").offset;
}
double SeriesLowestBars::compute(double src, double length) {
    return core_.update_series(src, length, true, false, "lowestbars").offset;
}
double SeriesLowestBars::recompute(double src, double length) {
    return core_.update_series(src, length, false, false, "lowestbars").offset;
}

// --- SeriesPercentileNearestRank ---

void SeriesPercentileNearestRank::append(double src) {
    const auto limit = history_limit(max_lookback_);
    if (limit == 0 || history_.size() < limit) {
        if (history_.size() == history_.capacity())
            history_.reserve(grow_history(history_.capacity(), history_.size() + 1, limit));
        history_.push_back(src);
        return;
    }
    history_[next_] = src;
    next_ = (next_ + 1) % history_.size();
}

void SeriesPercentileNearestRank::replace_last(double src) {
    if (history_.empty()) {
        append(src);
        return;
    }
    history_[next_ ? next_ - 1 : history_.size() - 1] = src;
}

double SeriesPercentileNearestRank::history_at(std::size_t offset) const {
    return history_[(next_ + offset) % history_.size()];
}

double SeriesPercentileNearestRank::evaluate(double src, std::uint64_t length,
                                              double percentage, bool replace) {
    const bool again = replace && !history_.empty();
    if (again) replace_last(src);
    else append(src);
    // The eventual window is replayed in the same append order. Avoid
    // allocating or ranking a requested window before it has enough samples.
    if (length > history_.size()) {
        window_.reset();
        return na<double>();
    }
    const auto size = static_cast<std::size_t>(length);
    if (window_ && window_length_ == size)
        return ta::nearest_rank_of(again ? window_->repush(src) : window_->push(src), percentage);

    window_.emplace(size, ta::PercentileWindow::ObservedLength{});
    window_length_ = size;
    const std::size_t before = history_.size() - 1;
    for (std::size_t offset = history_.size() - size; offset != before; ++offset)
        window_->push(history_at(offset));
    return ta::nearest_rank_of(window_->push(src), percentage);
}

double SeriesPercentileNearestRank::compute(double src, double length, double percentage) {
    const auto requested = series_length(length, "percentile_nearest_rank", max_lookback_);
    return evaluate(src, requested, percentage, false);
}

double SeriesPercentileNearestRank::recompute(double src, double length, double percentage) {
    const auto requested = series_length(length, "percentile_nearest_rank", max_lookback_);
    return evaluate(src, requested, percentage, true);
}

void SeriesPercentileNearestRank::observe_owned_storage(OwnedStorage& out) const noexcept {
    observe_owned_members(out, history_, window_);
}

// --- PineSupertrend ---

ta::SupertrendResult PineSupertrend::step(double high, double low, double close, bool again) {
    if (is_na(high) || is_na(low) || is_na(close)) {
        if (again) atr_->recompute(high, low, close);
        else atr_->compute(high, low, close);
        return ta::SupertrendResult{na<double>(), na<double>()};
    }
    const double atr = again ? atr_->recompute(high, low, close) : atr_->compute(high, low, close);
    const double src = (high + low) / 2.0;
    double upper = src + factor_ * atr;
    double lower = src - factor_ * atr;
    const double prev_lower = is_na(state_.prev_lower) ? 0.0 : state_.prev_lower;
    const double prev_upper = is_na(state_.prev_upper) ? 0.0 : state_.prev_upper;
    const double close1 = state_.prev_close;
    // Comparisons with na are false, as Pine's are.
    lower = (lower > prev_lower || close1 < prev_lower) ? lower : prev_lower;
    upper = (upper < prev_upper || close1 > prev_upper) ? upper : prev_upper;
    double direction;
    if (is_na(state_.prev_atr)) {
        direction = 1.0;
    } else if (state_.prev_st == prev_upper) {
        direction = close > upper ? -1.0 : 1.0;
    } else {
        direction = close < lower ? 1.0 : -1.0;
    }
    const double line = direction == -1.0 ? lower : upper;
    state_.prev_upper = upper;
    state_.prev_lower = lower;
    state_.prev_st = line;
    state_.prev_atr = atr;
    state_.prev_close = close;
    return ta::SupertrendResult{line, direction};
}

ta::SupertrendResult PineSupertrend::compute(double factor, double atr_period, double high,
                                             double low, double close) {
    if (!latched_) {
        // Only the latched factor is checked: an na one is accepted (and read
        // as na), zero or a negative one stops the run on the first execution
        // (tv/edge st-factor-zero / -neg: RE10001 on bar 0); a later 0 or -2
        // is never read (st-factor-zero-later, 60/60 equal to factor 3). The
        // check itself is supertrend_first_execution_period, at the end.
        atr_.emplace(supertrend_first_execution_period(factor, atr_period));
        factor_ = factor;
        latched_ = true;
    }
    saved_state_ = state_;
    return step(high, low, close, false);
}

ta::SupertrendResult PineSupertrend::recompute(double factor, double atr_period, double high,
                                               double low, double close) {
    if (!latched_) return compute(factor, atr_period, high, low, close);
    state_ = saved_state_;
    return step(high, low, close, true);
}

// --- SeriesSma ---

std::uint64_t SeriesSma::checked_length(double current_length) const {
    if (!std::isfinite(current_length)) length_na("sma", "length");
    return series_length(current_length, "sma", max_lookback_);
}

void SeriesSma::ensure_capacity(std::size_t count) {
    if (count <= history_.size()) return;
    const auto capacity = grow_history(history_.size(), count, history_limit(max_lookback_));
    // Growing storage has never wrapped, so the old prefix indices (including
    // the rollback checkpoint) stay valid after the copy.
    ::pineforge::detail::WindowValues<double> next(capacity, na<double>(), history_.cache_capacity());
    for (std::size_t i = 0; i < count_; ++i)
        next.write(i, history_.read(i));
    history_ = std::move(next);
}

double SeriesSma::update(double source, std::uint64_t length, bool replace) {
    const bool push = !is_na(source);
    const auto head = replace ? pre_update_head_ : head_;
    const auto count = replace ? pre_update_count_ : count_;
    const auto limit = history_limit(max_lookback_);
    const bool full = limit && count == limit;
    const double oldest = replace ? pre_update_evicted_
                                  : (full ? history_.read(head) : na<double>());
    // Offset one is the newest pre-bar source. At the cap, the oldest
    // may already have been overwritten by the provisional bar.
    const auto prior = [&](std::size_t back) {
        if (full && back == count) return oldest;
        return history_.read(back <= head ? head - back : history_.size() - (back - head));
    };

    Window next = replace ? pre_update_active_ : active_;
    const auto keep = static_cast<std::size_t>(std::min<std::uint64_t>(count, length - push));
    if (push || next.count != keep) {
        bool finite = !next.exact_recovery;
        const auto add = [&](double value) {
            return ::pineforge::detail::add_window_value(next.sum, next.correction, value);
        };
        // Adjust the previous suffix oldest first in both directions.
        for (auto back = next.count; finite && back > keep; --back)
            finite = add(-prior(back));
        for (auto back = keep; finite && back > next.count; --back)
            finite = add(prior(back));
        next.count = keep + push;
        if (!finite || (push && !add(source))) {
            const auto newest = [&](std::size_t i) {
                if (!push) return prior(i + 1);
                return i == 0 ? source : prior(i);
            };
            ::pineforge::detail::recover_window_sum(next.sum, next.correction,
                                                    next.exact_recovery, next.count, newest);
        }
    }

    // Complete all reads, growth and the single required write before
    // publishing either the logical window or the rewind record.
    if (push) {
        if (!full) ensure_capacity(count + 1);
        history_.write(head, source);
    } else if (replace && full && current_pushed_) {
        history_.write(head, oldest);
    }
    if (!replace) {
        pre_update_head_ = head;
        pre_update_count_ = count;
        pre_update_evicted_ = oldest;
        pre_update_active_ = active_;
    }
    const auto next_head = head + push;
    head_ = next_head == limit ? 0 : next_head;
    count_ = count + (push && !full);
    current_pushed_ = push;
    active_ = next;
    return next.count == length ? (next.sum + next.correction) / static_cast<double>(length)
                                : na<double>();
}

double SeriesSma::compute(double source, double current_length) {
    return update(source, checked_length(current_length), false);
}

double SeriesSma::recompute(double source, double current_length) {
    return update(source, checked_length(current_length), true);
}

// --- ta.supertrend's first-execution check ---

// atrPeriod first, then the factor: the order PineSupertrend::compute always
// checked them in, so a call with both invalid reports atrPeriod.
int supertrend_first_execution_period(double factor, double atr_period) {
    const int period = simple_ta_length(atr_period, "supertrend", "atrPeriod");
    if (!is_na(factor) && factor <= 0.0) factor_not_positive(factor);
    return period;
}

}  // namespace source
}  // namespace pineforge
