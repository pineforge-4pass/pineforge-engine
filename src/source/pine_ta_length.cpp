/*
 * pine_ta_length.cpp — ta.* calls whose length is not a constant: checked
 * simple lengths, series-length window extremes, the first-execution
 * supertrend factor. The TradingView rules and their tapes are documented in
 * <pineforge/source/pine_ta_length.hpp>.
 */

#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/na.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace pineforge {
namespace source {

namespace {

// "Error on bar N: " as TradingView prefixes a runtime error, when an
// evaluation context names the bar.
std::string bar_prefix() {
    const ta::BarContext& ctx = ta::bar_context();
    if (!ctx.installed) return std::string();
    return "Error on bar " + std::to_string(ctx.bar_index) + ": ";
}

[[noreturn]] void length_not_positive(long long value, const char* function,
                                      const char* argument) {
    throw std::runtime_error(bar_prefix() + "Invalid value of the '" + argument +
                             "' argument (" + std::to_string(value) + ") in the '" +
                             function + "' function. It must be > 0.");
}

[[noreturn]] void length_na(const char* function, const char* argument) {
    throw std::runtime_error(bar_prefix() + "Invalid value of the '" + argument +
                             "' argument in the '" + function +
                             "' function. It must not be na");
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
    throw std::runtime_error(bar_prefix() + "Invalid value of the 'factor' argument (" + text +
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

void HeldHistory::push(double value) {
    if (tail_.size() == kChunk) {
        frozen_.push_back(std::make_shared<const std::vector<double>>(std::move(tail_)));
        tail_ = std::vector<double>();
    }
    if (tail_.capacity() == 0) tail_.reserve(kChunk);
    tail_.push_back(value);
    ++count_;
}

void HeldHistory::drop_front_chunks(std::size_t keep) {
    std::size_t drop = 0;
    while (drop < frozen_.size() &&
           count_ - static_cast<long long>(kChunk) >= static_cast<long long>(keep)) {
        count_ -= static_cast<long long>(kChunk);
        first_bar_ += static_cast<long long>(kChunk);
        ++drop;
    }
    if (drop) frozen_.erase(frozen_.begin(), frozen_.begin() + static_cast<long>(drop));
}

void HeldHistory::write(long long bar, double value, std::size_t keep) {
    if (count_ == 0) {
        first_bar_ = bar;
        push(value);
        return;
    }
    const long long last = last_bar();
    if (bar < last) return;           // an earlier bar is never rewritten
    if (bar == last) {
        tail_.back() = value;
        return;
    }
    // Skipped bars hold the last value. Only the `keep` newest bars can ever
    // be read, so a longer gap restarts the history at its readable part.
    const double held = tail_.back();
    long long gap = bar - last - 1;
    if (gap >= static_cast<long long>(keep)) {
        frozen_.clear();
        tail_ = std::vector<double>();
        count_ = 0;
        first_bar_ = bar - static_cast<long long>(keep);
        gap = static_cast<long long>(keep);
    }
    for (long long i = 0; i < gap; ++i) push(held);
    push(value);
    drop_front_chunks(keep);
}

double HeldHistory::at(long long bar) const {
    const long long i = bar - first_bar_;
    const std::size_t chunk = static_cast<std::size_t>(i) / kChunk;
    const std::size_t offset = static_cast<std::size_t>(i) % kChunk;
    if (chunk < frozen_.size()) return (*frozen_[chunk])[offset];
    return tail_[offset];
}

// --- SeriesWindowExtreme ---

SeriesWindowExtreme::Result SeriesWindowExtreme::update(double src, long long length,
                                                        bool advance, bool want_max,
                                                        const char* function) {
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

    if (length <= 0) length_not_positive(length, function, "length");
    if (length - 1 > kMaxBarsBack) {
        throw std::runtime_error(
            bar_prefix() +
            "The script attempts to reference historical data that is too far from "
            "the current bar (" + std::to_string(length - 1) +
            " bars back). The historical buffer's limit is " +
            std::to_string(kMaxBarsBack) + " bars.");
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
    history_.write(bar, src, static_cast<std::size_t>(kMaxBarsBack) + 1);

    const auto better = [want_max](double a, double b) { return want_max ? a > b : a < b; };
    // A member of the window: 1 = a value, 0 = na (resets the running
    // extremum), -1 = before the context's first bar (does not exist).
    const auto member = [&](long long at, double& value) -> int {
        if (at < origin) return -1;
        if (at < first_exec_bar_ || at < history_.first_bar()) return 0;
        value = history_.at(at);
        return is_na(value) ? 0 : 1;
    };
    // Walks window offsets [from, to] oldest first into `running`.
    const auto walk = [&](long long from_k, long long to_k, Cache& running) {
        for (long long k = from_k; k >= to_k; --k) {
            double v = 0.0;
            const int kind = member(bar - k, v);
            if (kind < 0) continue;
            if (kind == 0) {
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

long long series_length(double length, const char* function) {
    long long value = 0;
    // TradingView reports an na length as 0 (RE10001).
    if (!length_value(length, value)) length_not_positive(0, function, "length");
    return value;
}

}  // namespace

double SeriesHighest::compute(double src, double length) {
    return core_.update(src, series_length(length, "highest"), true, true, "highest").value;
}
double SeriesHighest::recompute(double src, double length) {
    return core_.update(src, series_length(length, "highest"), false, true, "highest").value;
}
double SeriesLowest::compute(double src, double length) {
    return core_.update(src, series_length(length, "lowest"), true, false, "lowest").value;
}
double SeriesLowest::recompute(double src, double length) {
    return core_.update(src, series_length(length, "lowest"), false, false, "lowest").value;
}
double SeriesHighestBars::compute(double src, double length) {
    return core_.update(src, series_length(length, "highestbars"), true, true, "highestbars")
        .offset;
}
double SeriesHighestBars::recompute(double src, double length) {
    return core_.update(src, series_length(length, "highestbars"), false, true, "highestbars")
        .offset;
}
double SeriesLowestBars::compute(double src, double length) {
    return core_.update(src, series_length(length, "lowestbars"), true, false, "lowestbars")
        .offset;
}
double SeriesLowestBars::recompute(double src, double length) {
    return core_.update(src, series_length(length, "lowestbars"), false, false, "lowestbars")
        .offset;
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
        atr_.emplace(simple_ta_length(atr_period, "supertrend", "atrPeriod"));
        // Only the latched factor is checked: an na one is accepted (and read
        // as na), zero or a negative one stops the run on the first execution
        // (tv/edge st-factor-zero / -neg: RE10001 on bar 0); a later 0 or -2
        // is never read (st-factor-zero-later, 60/60 equal to factor 3).
        if (!is_na(factor) && factor <= 0.0) factor_not_positive(factor);
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

}  // namespace source
}  // namespace pineforge
