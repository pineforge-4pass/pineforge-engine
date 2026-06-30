/*
 * ta_extremes_volume.cpp — range extremes + cumulative + volume: Highest, Lowest, HighestBars, LowestBars, AllTimeMax, AllTimeMin, Median, Range, Mode, Cum, PivotHigh, PivotLow, OBV, AccDist, NVI, PVI, PVT, WAD, WVAD, III, VWAP
 *
 * Carved out of ta.cpp during the v0.1 file-split (phase 6) so the
 * 66-class TA library becomes navigable. Every class declared in
 * <pineforge/ta.hpp> is implemented in exactly one of the ta_*.cpp
 * partitions.
 */

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>
#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace pineforge {
namespace ta {


// --- Highest ---

Highest::Highest(int length)
    : length(length) {}

double Highest::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer.push_back(src);
    while ((int)buffer.size() > length) {
        buffer.pop_front();
    }

    if ((int)buffer.size() < length) {
        return na<double>();
    }

    double hi = buffer[0];
    for (int i = 1; i < (int)buffer.size(); i++) {
        if (buffer[i] > hi) hi = buffer[i];
    }
    return hi;
}

// --- Lowest ---

Lowest::Lowest(int length)
    : length(length) {}

double Lowest::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer.push_back(src);
    while ((int)buffer.size() > length) {
        buffer.pop_front();
    }

    if ((int)buffer.size() < length) {
        return na<double>();
    }

    double lo = buffer[0];
    for (int i = 1; i < (int)buffer.size(); i++) {
        if (buffer[i] < lo) lo = buffer[i];
    }
    return lo;
}

// --- PivotHigh ---

PivotHigh::PivotHigh(int left_bars, int right_bars)
    : left_bars_(left_bars), right_bars_(right_bars) {}

double PivotHigh::compute(double src) {
    buffer_.push_back(src);

    int total = left_bars_ + right_bars_ + 1;
    while ((int)buffer_.size() > total) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < total) {
        return na<double>();
    }

    // The pivot candidate is at index left_bars_ (0-indexed)
    double pivot = buffer_[left_bars_];
    if (is_na(pivot)) {
        return na<double>();
    }

    // Pivot candidate rules (validated against TradingView's `ta.pivothigh`
    // semantics by exporting per-bar pivot_high values from a TV indicator
    // and diffing against this engine; see
    // docs/per-bar-trace/tv_trace_helper.pine).
    //
    // LEFT side: equal-high left bars are allowed (non-strict). A run of
    // identical highs to the left of the candidate should still confirm
    // the right-most one as the pivot — TV reports the pivot exactly one
    // bar after each flat-top, never on the first bar of a flat-top run.
    //
    // RIGHT side: equal-high right bars invalidate the candidate (strict).
    // While the flat-top continues to the right, no pivot has yet been
    // confirmed; the pivot is reported only on the bar AFTER the
    // flat-top finishes.
    for (int i = 0; i < left_bars_; i++) {
        if (is_na(buffer_[i]) || buffer_[i] > pivot) {
            return na<double>();
        }
    }

    for (int i = left_bars_ + 1; i < total; i++) {
        if (is_na(buffer_[i]) || buffer_[i] >= pivot) {
            return na<double>();
        }
    }

    return pivot;
}

// --- PivotLow ---

PivotLow::PivotLow(int left_bars, int right_bars)
    : left_bars_(left_bars), right_bars_(right_bars) {}

double PivotLow::compute(double src) {
    buffer_.push_back(src);

    int total = left_bars_ + right_bars_ + 1;
    while ((int)buffer_.size() > total) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < total) {
        return na<double>();
    }

    // The pivot candidate is at index left_bars_ (0-indexed)
    double pivot = buffer_[left_bars_];
    if (is_na(pivot)) {
        return na<double>();
    }

    // Mirror of PivotHigh — see that function's comment for the TV-empirical
    // rule rationale. LEFT non-strict (equal lows on left allowed), RIGHT
    // strict (equal lows on right invalidate). Pivot lows confirm exactly
    // one bar after each flat-bottom run.
    for (int i = 0; i < left_bars_; i++) {
        if (is_na(buffer_[i]) || buffer_[i] < pivot) {
            return na<double>();
        }
    }

    for (int i = left_bars_ + 1; i < total; i++) {
        if (is_na(buffer_[i]) || buffer_[i] <= pivot) {
            return na<double>();
        }
    }

    return pivot;
}

// --- Cum (Cumulative Sum) ---

// saved_sum_ mirrors the initial committed sum_ (see RMA::RMA) so a
// recompute() before the first compute() restores a well-defined pristine
// state instead of reading uninitialized save-state.
Cum::Cum() : sum_(0.0), saved_sum_(0.0) {}

double Cum::compute(double src) {
    saved_sum_ = sum_;
    if (is_na(src)) {
        return sum_;
    }
    sum_ += src;
    return sum_;
}

// --- All-time max/min (chart series) ---

// saved_* mirror the initial committed state (see RMA::RMA) so a recompute()
// before the first compute() restores a well-defined pristine state.
AllTimeMax::AllTimeMax()
    : max_(na<double>()), has_(false),
      saved_max_(na<double>()), saved_has_(false) {}

double AllTimeMax::compute(double src) {
    saved_max_ = max_;
    saved_has_ = has_;
    if (is_na(src)) {
        return has_ ? max_ : na<double>();
    }
    if (!has_) {
        max_ = src;
        has_ = true;
    } else if (src > max_) {
        max_ = src;
    }
    return max_;
}

// saved_* mirror the initial committed state (see RMA::RMA) so a recompute()
// before the first compute() restores a well-defined pristine state.
AllTimeMin::AllTimeMin()
    : min_(na<double>()), has_(false),
      saved_min_(na<double>()), saved_has_(false) {}

double AllTimeMin::compute(double src) {
    saved_min_ = min_;
    saved_has_ = has_;
    if (is_na(src)) {
        return has_ ? min_ : na<double>();
    }
    if (!has_) {
        min_ = src;
        has_ = true;
    } else if (src < min_) {
        min_ = src;
    }
    return min_;
}

// --- Median ---

Median::Median(int length) : length_(length) {}

double Median::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer_.push_back(src);
    while ((int)buffer_.size() > length_) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < length_) {
        return na<double>();
    }

    // Copy and sort
    std::vector<double> sorted(buffer_.begin(), buffer_.end());
    std::sort(sorted.begin(), sorted.end());

    int n = (int)sorted.size();
    if (n % 2 == 1) {
        return sorted[n / 2];
    } else {
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    }
}

// --- HighestBars ---

HighestBars::HighestBars(int length) : length_(length) {}

double HighestBars::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer_.push_back(src);
    while ((int)buffer_.size() > length_) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < length_) {
        return na<double>();
    }

    int max_idx = 0;
    double max_val = buffer_[0];
    for (int i = 1; i < (int)buffer_.size(); i++) {
        if (buffer_[i] > max_val) {
            max_val = buffer_[i];
            max_idx = i;
        }
    }
    // Return negative offset from current bar
    return (double)(max_idx - ((int)buffer_.size() - 1));
}

// --- LowestBars ---

LowestBars::LowestBars(int length) : length_(length) {}

double LowestBars::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer_.push_back(src);
    while ((int)buffer_.size() > length_) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < length_) {
        return na<double>();
    }

    int min_idx = 0;
    double min_val = buffer_[0];
    for (int i = 1; i < (int)buffer_.size(); i++) {
        if (buffer_[i] < min_val) {
            min_val = buffer_[i];
            min_idx = i;
        }
    }
    // Return negative offset from current bar
    return (double)(min_idx - ((int)buffer_.size() - 1));
}

// --- OBV ---
double OBV::compute(double close, double volume) {
    saved_sum_ = sum_;
    saved_prev_close_ = prev_close_;
    saved_bar_count_ = bar_count_;
    if (is_na(close) || is_na(volume)) return na<double>();
    if (bar_count_ == 0) {
        prev_close_ = close;
        bar_count_++;
        return 0.0;
    }
    if (close > prev_close_) sum_ += volume;
    else if (close < prev_close_) sum_ -= volume;
    prev_close_ = close;
    bar_count_++;
    return sum_;
}

// --- AccDist ---
double AccDist::compute(double high, double low, double close, double volume) {
    saved_sum_ = sum_;
    if (is_na(high) || is_na(low) || is_na(close) || is_na(volume)) return na<double>();
    double range = high - low;
    if (range == 0.0) return sum_;
    double mfm = ((close - low) - (high - close)) / range;
    sum_ += mfm * volume;
    return sum_;
}

// --- NVI ---
double NVI::compute(double close, double volume) {
    saved_nvi_ = nvi_;
    saved_prev_close_ = prev_close_;
    saved_prev_volume_ = prev_volume_;
    saved_bar_count_ = bar_count_;
    if (is_na(close) || is_na(volume)) return na<double>();
    if (bar_count_ == 0) {
        prev_close_ = close;
        prev_volume_ = volume;
        bar_count_++;
        return nvi_;
    }
    if (volume < prev_volume_ && prev_close_ != 0.0) {
        nvi_ += (close - prev_close_) / prev_close_ * nvi_;
    }
    prev_close_ = close;
    prev_volume_ = volume;
    bar_count_++;
    return nvi_;
}

// --- PVI ---
double PVI::compute(double close, double volume) {
    saved_pvi_ = pvi_;
    saved_prev_close_ = prev_close_;
    saved_prev_volume_ = prev_volume_;
    saved_bar_count_ = bar_count_;
    if (is_na(close) || is_na(volume)) return na<double>();
    if (bar_count_ == 0) {
        prev_close_ = close;
        prev_volume_ = volume;
        bar_count_++;
        return pvi_;
    }
    if (volume > prev_volume_ && prev_close_ != 0.0) {
        pvi_ += (close - prev_close_) / prev_close_ * pvi_;
    }
    prev_close_ = close;
    prev_volume_ = volume;
    bar_count_++;
    return pvi_;
}

// --- PVT ---
double PVT::compute(double close, double volume) {
    saved_pvt_ = pvt_;
    saved_prev_close_ = prev_close_;
    if (is_na(close) || is_na(volume)) return na<double>();
    if (is_na(prev_close_)) { prev_close_ = close; return pvt_; }
    if (prev_close_ != 0.0) {
        pvt_ += ((close - prev_close_) / prev_close_) * volume;
    }
    prev_close_ = close;
    return pvt_;
}

// --- WAD ---
double WAD::compute(double high, double low, double close) {
    saved_wad_ = wad_;
    saved_prev_close_ = prev_close_;
    if (is_na(high) || is_na(low) || is_na(close)) return na<double>();
    if (is_na(prev_close_)) { prev_close_ = close; return 0.0; }
    double true_high = std::max(high, prev_close_);
    double true_low = std::min(low, prev_close_);
    double gain = 0.0;
    if (close > prev_close_) gain = close - true_low;
    else if (close < prev_close_) gain = close - true_high;
    wad_ += gain;
    prev_close_ = close;
    return wad_;
}

// --- WVAD ---
double WVAD::compute(double open, double high, double low, double close, double volume) {
    if (is_na(open) || is_na(high) || is_na(low) || is_na(close) || is_na(volume))
        return na<double>();
    double range = high - low;
    if (range == 0.0) return 0.0;
    return (close - open) / range * volume;
}

// --- III ---
// Intraday Intensity Index. Pine's `ta.iii` per-bar formula
// (per the official reference manual):
//   ((close - low) - (high - close)) / (high - low) * volume
// = (2*close - high - low) / (high - low) * volume.
// Volume MULTIPLIES the close-position ratio; the previous version
// divided, producing values ~1e-6 instead of TV's ~1e+4 magnitudes
// (caught by the TA correctness sweep against TV — every bar diverged).
double III::compute(double high, double low, double close, double volume) {
    if (is_na(high) || is_na(low) || is_na(close) || is_na(volume))
        return na<double>();
    double range = high - low;
    if (range == 0.0) return 0.0;
    return (2.0 * close - high - low) / range * volume;
}

// --- VWAP ---
// Pine v6 `ta.vwap(source)` defaults to a Daily anchor — the cumulator
// resets at the start of every UTC day. Earlier the engine treated VWAP
// as a single chart-wide cumulator, which produced values that drifted
// from TV by ~30% on intra-day bars (and progressively further as the
// day advanced). Resetting on UTC-day boundaries restores parity. The
// `anchor_day_` member is initialised lazily on the first non-NA bar.
double VWAP::compute(double src, double volume, int64_t timestamp_ms) {
    saved_cum_pv_ = cum_pv_;
    saved_cum_vol_ = cum_vol_;
    saved_cum_pv_sq_ = cum_pv_sq_;
    saved_anchor_day_ = anchor_day_;
    if (is_na(src) || is_na(volume)) return na<double>();
    int64_t day = timestamp_ms / kMsPerDay;
    if (anchor_day_ == std::numeric_limits<int64_t>::min()) {
        anchor_day_ = day;
    } else if (day != anchor_day_) {
        cum_pv_ = 0.0;
        cum_vol_ = 0.0;
        cum_pv_sq_ = 0.0;
        anchor_day_ = day;
    }
    cum_pv_ += src * volume;
    cum_pv_sq_ += src * src * volume;
    cum_vol_ += volume;
    if (cum_vol_ == 0.0) return na<double>();
    return cum_pv_ / cum_vol_;
}

VWAPBandsResult VWAP::compute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult) {
    saved_cum_pv_ = cum_pv_;
    saved_cum_vol_ = cum_vol_;
    saved_cum_pv_sq_ = cum_pv_sq_;
    saved_anchor_day_ = anchor_day_;
    if (is_na(src) || is_na(volume)) return {na<double>(), na<double>(), na<double>()};
    int64_t day = timestamp_ms / kMsPerDay;
    if (anchor_day_ == std::numeric_limits<int64_t>::min()) {
        anchor_day_ = day;
    } else if (day != anchor_day_) {
        cum_pv_ = 0.0;
        cum_vol_ = 0.0;
        cum_pv_sq_ = 0.0;
        anchor_day_ = day;
    }
    cum_pv_ += src * volume;
    cum_pv_sq_ += src * src * volume;
    cum_vol_ += volume;
    if (cum_vol_ == 0.0) return {na<double>(), na<double>(), na<double>()};
    double mean = cum_pv_ / cum_vol_;
    double variance = cum_pv_sq_ / cum_vol_ - mean * mean;
    if (variance < 0.0) variance = 0.0;  // guard against floating-point underflow
    double stdev = std::sqrt(variance);
    double band_offset = stdev_mult * stdev;
    return {mean, mean + band_offset, mean - band_offset};
}

// --- Mode ---
double Mode::compute(double src) {
    if (is_na(src)) return na<double>();
    buffer_.push_back(src);
    if ((int)buffer_.size() > length_) buffer_.pop_front();
    if ((int)buffer_.size() < length_) return na<double>();
    std::unordered_map<double, int> counts;
    for (auto v : buffer_) counts[v]++;
    int max_count = 0;
    double mode_val = na<double>();
    for (auto& [val, cnt] : counts) {
        if (cnt > max_count || (cnt == max_count && (is_na(mode_val) || val < mode_val))) {
            max_count = cnt;
            mode_val = val;
        }
    }
    return mode_val;
}

// --- Range ---
double Range::compute(double src) {
    double h = highest_.compute(src);
    double l = lowest_.compute(src);
    if (is_na(h) || is_na(l)) return na<double>();
    return h - l;
}

// --- Highest ---
double Highest::recompute(double src) {
    if (buffer.empty()) return compute(src);
    buffer.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer.size() < length) return na<double>();

    double hi = buffer[0];
    for (int i = 1; i < (int)buffer.size(); i++) {
        if (buffer[i] > hi) hi = buffer[i];
    }
    return hi;
}

// --- Lowest ---
double Lowest::recompute(double src) {
    if (buffer.empty()) return compute(src);
    buffer.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer.size() < length) return na<double>();

    double lo = buffer[0];
    for (int i = 1; i < (int)buffer.size(); i++) {
        if (buffer[i] < lo) lo = buffer[i];
    }
    return lo;
}

// --- PivotHigh ---
// Recompute mirrors compute(): LEFT non-strict (`>` rejects), RIGHT strict
// (`>=` rejects). Both must stay in lockstep — a delta here would surface as
// `recompute != compute` in test_pivothigh_recompute_matches_compute.
double PivotHigh::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    int total = left_bars_ + right_bars_ + 1;
    if ((int)buffer_.size() < total) return na<double>();

    double pivot = buffer_[left_bars_];
    if (is_na(pivot)) return na<double>();

    for (int i = 0; i < left_bars_; i++) {
        if (is_na(buffer_[i]) || buffer_[i] > pivot) return na<double>();
    }
    for (int i = left_bars_ + 1; i < total; i++) {
        if (is_na(buffer_[i]) || buffer_[i] >= pivot) return na<double>();
    }
    return pivot;
}

// --- PivotLow ---
// Recompute mirrors compute(): LEFT non-strict (`<` rejects), RIGHT strict
// (`<=` rejects). See PivotHigh::recompute() comment for invariant rationale.
double PivotLow::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    int total = left_bars_ + right_bars_ + 1;
    if ((int)buffer_.size() < total) return na<double>();

    double pivot = buffer_[left_bars_];
    if (is_na(pivot)) return na<double>();

    for (int i = 0; i < left_bars_; i++) {
        if (is_na(buffer_[i]) || buffer_[i] < pivot) return na<double>();
    }
    for (int i = left_bars_ + 1; i < total; i++) {
        if (is_na(buffer_[i]) || buffer_[i] <= pivot) return na<double>();
    }
    return pivot;
}

// --- Cum ---
double Cum::recompute(double src) {
    sum_ = saved_sum_;
    return compute(src);
}

// --- AllTimeMax ---
double AllTimeMax::recompute(double src) {
    max_ = saved_max_;
    has_ = saved_has_;
    return compute(src);
}

// --- AllTimeMin ---
double AllTimeMin::recompute(double src) {
    min_ = saved_min_;
    has_ = saved_has_;
    return compute(src);
}

// --- Median ---
double Median::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    std::vector<double> sorted(buffer_.begin(), buffer_.end());
    std::sort(sorted.begin(), sorted.end());

    int n = (int)sorted.size();
    if (n % 2 == 1) return sorted[n / 2];
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

// --- HighestBars ---
double HighestBars::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    int max_idx = 0;
    double max_val = buffer_[0];
    for (int i = 1; i < (int)buffer_.size(); i++) {
        if (buffer_[i] > max_val) { max_val = buffer_[i]; max_idx = i; }
    }
    return (double)(max_idx - ((int)buffer_.size() - 1));
}

// --- LowestBars ---
double LowestBars::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    int min_idx = 0;
    double min_val = buffer_[0];
    for (int i = 1; i < (int)buffer_.size(); i++) {
        if (buffer_[i] < min_val) { min_val = buffer_[i]; min_idx = i; }
    }
    return (double)(min_idx - ((int)buffer_.size() - 1));
}

// --- OBV ---
double OBV::recompute(double close, double volume) {
    sum_ = saved_sum_;
    prev_close_ = saved_prev_close_;
    bar_count_ = saved_bar_count_;
    return compute(close, volume);
}

// --- AccDist ---
double AccDist::recompute(double high, double low, double close, double volume) {
    sum_ = saved_sum_;
    return compute(high, low, close, volume);
}

// --- NVI ---
double NVI::recompute(double close, double volume) {
    nvi_ = saved_nvi_;
    prev_close_ = saved_prev_close_;
    prev_volume_ = saved_prev_volume_;
    bar_count_ = saved_bar_count_;
    return compute(close, volume);
}

// --- PVI ---
double PVI::recompute(double close, double volume) {
    pvi_ = saved_pvi_;
    prev_close_ = saved_prev_close_;
    prev_volume_ = saved_prev_volume_;
    bar_count_ = saved_bar_count_;
    return compute(close, volume);
}

// --- PVT ---
double PVT::recompute(double close, double volume) {
    pvt_ = saved_pvt_;
    prev_close_ = saved_prev_close_;
    return compute(close, volume);
}

// --- WAD ---
double WAD::recompute(double high, double low, double close) {
    wad_ = saved_wad_;
    prev_close_ = saved_prev_close_;
    return compute(high, low, close);
}

// --- WVAD (stateless) ---
double WVAD::recompute(double open, double high, double low, double close, double volume) {
    return compute(open, high, low, close, volume);
}

// --- III (stateless) ---
double III::recompute(double high, double low, double close, double volume) {
    return compute(high, low, close, volume);
}

// --- VWAP ---
double VWAP::recompute(double src, double volume, int64_t timestamp_ms) {
    cum_pv_ = saved_cum_pv_;
    cum_vol_ = saved_cum_vol_;
    cum_pv_sq_ = saved_cum_pv_sq_;
    anchor_day_ = saved_anchor_day_;
    return compute(src, volume, timestamp_ms);
}

VWAPBandsResult VWAP::recompute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult) {
    cum_pv_ = saved_cum_pv_;
    cum_vol_ = saved_cum_vol_;
    cum_pv_sq_ = saved_cum_pv_sq_;
    anchor_day_ = saved_anchor_day_;
    return compute_bands(src, volume, timestamp_ms, stdev_mult);
}

// --- Mode ---
double Mode::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    std::unordered_map<double, int> counts;
    for (auto v : buffer_) counts[v]++;
    int max_count = 0;
    double mode_val = na<double>();
    for (auto& [val, cnt] : counts) {
        if (cnt > max_count || (cnt == max_count && (is_na(mode_val) || val < mode_val))) {
            max_count = cnt;
            mode_val = val;
        }
    }
    return mode_val;
}

// --- Range ---
double Range::recompute(double src) {
    double h = highest_.recompute(src);
    double l = lowest_.recompute(src);
    if (is_na(h) || is_na(l)) return na<double>();
    return h - l;
}

} // namespace ta
} // namespace pineforge
