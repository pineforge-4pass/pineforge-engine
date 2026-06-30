/*
 * ta_volatility_trend.cpp — volatility + trend: ATR, TR, StdDev, Variance, Dev, BB, BBW, KC, KCW, MACD, DMI, SAR, Supertrend
 *
 * Carved out of ta.cpp during the v0.1 file-split (phase 6) so the
 * 66-class TA library becomes navigable. Every class declared in
 * <pineforge/ta.hpp> is implemented in exactly one of the ta_*.cpp
 * partitions.
 */

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace pineforge {
namespace ta {


// --- MACD ---

MACD::MACD(int fast_length, int slow_length, int signal_length)
    : fast_ema(fast_length), slow_ema(slow_length), signal_ema(signal_length) {}

MACDResult MACD::compute(double src) {
    MACDResult result;
    result.macd_line = na<double>();
    result.signal_line = na<double>();
    result.histogram = na<double>();

    double fast_val = fast_ema.compute(src);
    double slow_val = slow_ema.compute(src);

    if (is_na(fast_val) || is_na(slow_val)) {
        return result;
    }

    result.macd_line = fast_val - slow_val;

    double signal_val = signal_ema.compute(result.macd_line);
    result.signal_line = signal_val;

    if (!is_na(signal_val)) {
        result.histogram = result.macd_line - signal_val;
    }

    return result;
}

// --- ATR ---

ATR::ATR(int length)
    : rma(length), prev_close(na<double>()), bar_count(0),
      // Mirror the initial committed state (see RMA::RMA) so a recompute()
      // before the first compute() restores a well-defined pristine state.
      saved_prev_close_(na<double>()), saved_bar_count_(0) {}

double ATR::compute(double high, double low, double close) {
    saved_prev_close_ = prev_close;
    saved_bar_count_ = bar_count;
    rma.save();

    if (is_na(high) || is_na(low) || is_na(close)) {
        return na<double>();
    }

    bar_count++;

    double tr;
    if (is_na(prev_close)) {
        // First bar: true range is simply high - low
        tr = high - low;
    } else {
        tr = std::max({high - low,
                       std::abs(high - prev_close),
                       std::abs(low - prev_close)});
    }

    prev_close = close;
    return rma.compute(tr);
}

// --- StdDev (Standard Deviation) ---

StdDev::StdDev(int length) : length_(length) {}

double StdDev::compute(double src) {
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

    // Population standard deviation
    double sum = 0.0;
    for (double v : buffer_) {
        sum += v;
    }
    double mean = sum / length_;

    double sq_sum = 0.0;
    for (double v : buffer_) {
        double diff = v - mean;
        sq_sum += diff * diff;
    }
    return std::sqrt(sq_sum / length_);
}

// --- Supertrend ---

Supertrend::Supertrend(double factor, int atr_period)
    : factor_(factor), atr_(atr_period),
      prev_upper_(na<double>()), prev_lower_(na<double>()),
      prev_st_(na<double>()), prev_direction_(1.0),
      prev_close_(na<double>()), initialized_(false),
      // Mirror the initial committed state (see RMA::RMA) so a recompute()
      // before the first compute() restores a well-defined pristine state.
      saved_prev_upper_(na<double>()), saved_prev_lower_(na<double>()),
      saved_prev_st_(na<double>()), saved_prev_direction_(1.0),
      saved_prev_close_(na<double>()), saved_initialized_(false) {}

SupertrendResult Supertrend::compute(double high, double low, double close) {
    saved_prev_upper_ = prev_upper_;
    saved_prev_lower_ = prev_lower_;
    saved_prev_st_ = prev_st_;
    saved_prev_direction_ = prev_direction_;
    saved_prev_close_ = prev_close_;
    saved_initialized_ = initialized_;

    SupertrendResult result;
    result.value = na<double>();
    result.direction = na<double>();

    if (is_na(high) || is_na(low) || is_na(close)) {
        return result;
    }

    double atr_val = atr_.compute(high, low, close);
    if (is_na(atr_val)) {
        prev_close_ = close;
        return result;
    }

    double hl2 = (high + low) / 2.0;
    double basic_upper = hl2 + factor_ * atr_val;
    double basic_lower = hl2 - factor_ * atr_val;

    double final_upper, final_lower;

    if (!initialized_) {
        final_upper = basic_upper;
        final_lower = basic_lower;
        // Initial direction: -1 = uptrend (bullish), 1 = downtrend (bearish)
        // If close > upper band, we're in an uptrend
        prev_direction_ = (close > final_upper) ? -1.0 : 1.0;
        initialized_ = true;
    } else {
        // Final upper band: take min of basic and previous if prev close <= prev upper
        if (!is_na(prev_close_) && prev_close_ <= prev_upper_) {
            final_upper = std::min(basic_upper, prev_upper_);
        } else {
            final_upper = basic_upper;
        }

        // Final lower band: take max of basic and previous if prev close >= prev lower
        if (!is_na(prev_close_) && prev_close_ >= prev_lower_) {
            final_lower = std::max(basic_lower, prev_lower_);
        } else {
            final_lower = basic_lower;
        }
    }

    // Determine direction
    // In TradingView: direction = -1 means uptrend (bullish), 1 means downtrend (bearish)
    // Uptrend (-1) uses lower band; reversal when close drops below lower band
    // Downtrend (1) uses upper band; reversal when close rises above upper band
    // Note: direction check uses CURRENT bar's final bands
    double direction;
    if (prev_direction_ == 1.0 && close > final_upper) {
        direction = -1.0;  // Switch to uptrend (bullish)
    } else if (prev_direction_ == -1.0 && close < final_lower) {
        direction = 1.0;   // Switch to downtrend (bearish)
    } else {
        direction = prev_direction_;
    }

    // Supertrend value: per Pine v6 ta.supertrend reference impl,
    //   `superTrend := _direction == -1 ? lowerBand : upperBand`
    // i.e. an UPTREND (direction = -1) trails the LOWER band as
    // support; a DOWNTREND (direction = +1) trails the UPPER band as
    // resistance. The previous version picked the OPPOSITE band per
    // direction, which left `direction` correct (the engine matched TV
    // bar-for-bar on the dir series) but inflated the line in uptrends
    // by ~2× ATR (and depressed it in downtrends). Caught by the TA
    // correctness sweep — direction had 0 mismatches across 4694 bars
    // while st_line had 4694/4694 mismatches.
    double st_val = (direction == 1.0) ? final_upper : final_lower;

    result.value = st_val;
    result.direction = direction;

    prev_upper_ = final_upper;
    prev_lower_ = final_lower;
    prev_st_ = st_val;
    prev_direction_ = direction;
    prev_close_ = close;

    return result;
}

// --- DMI (Directional Movement Index) ---

DMI::DMI(int di_length, int adx_smoothing)
    : rma_plus_(di_length), rma_minus_(di_length), rma_tr_(di_length),
      rma_adx_(adx_smoothing),
      prev_high_(na<double>()), prev_low_(na<double>()),
      prev_close_(na<double>()), first_bar_(true),
      // Mirror the initial committed state (see RMA::RMA) so a recompute()
      // before the first compute() restores a well-defined pristine state.
      saved_prev_high_(na<double>()), saved_prev_low_(na<double>()),
      saved_prev_close_(na<double>()), saved_first_bar_(true) {}

DMIResult DMI::compute(double high, double low, double close) {
    saved_prev_high_ = prev_high_;
    saved_prev_low_ = prev_low_;
    saved_prev_close_ = prev_close_;
    saved_first_bar_ = first_bar_;
    rma_plus_.save();
    rma_minus_.save();
    rma_tr_.save();
    rma_adx_.save();

    DMIResult result;
    result.diplus = na<double>();
    result.diminus = na<double>();
    result.adx = na<double>();

    if (is_na(high) || is_na(low) || is_na(close)) {
        return result;
    }

    if (first_bar_) {
        prev_high_ = high;
        prev_low_ = low;
        prev_close_ = close;
        first_bar_ = false;
        return result;
    }

    double up_move = high - prev_high_;
    double down_move = prev_low_ - low;

    double plus_dm = (up_move > down_move && up_move > 0.0) ? up_move : 0.0;
    double minus_dm = (down_move > up_move && down_move > 0.0) ? down_move : 0.0;

    // True range (Pine / ta.tr: max(high-low, |high-prev_close|, |low-prev_close|))
    double tr = std::max({high - low,
                          std::abs(high - prev_close_),
                          std::abs(low - prev_close_)});

    double smoothed_plus = rma_plus_.compute(plus_dm);
    double smoothed_minus = rma_minus_.compute(minus_dm);
    double smoothed_tr = rma_tr_.compute(tr);

    prev_high_ = high;
    prev_low_ = low;
    prev_close_ = close;

    if (is_na(smoothed_plus) || is_na(smoothed_minus) || is_na(smoothed_tr) || smoothed_tr == 0.0) {
        return result;
    }

    result.diplus = 100.0 * smoothed_plus / smoothed_tr;
    result.diminus = 100.0 * smoothed_minus / smoothed_tr;

    double di_sum = result.diplus + result.diminus;
    double dx = (di_sum == 0.0) ? 0.0 : 100.0 * std::abs(result.diplus - result.diminus) / di_sum;

    double adx_val = rma_adx_.compute(dx);
    result.adx = adx_val;

    return result;
}

// --- SAR (Parabolic SAR) ---

SAR::SAR(double start, double increment, double maximum)
    : start_(start), increment_(increment), maximum_(maximum),
      af_(0.0), ep_(0.0), sar_(0.0),
      is_long_(true), initialized_(false),
      prev_high_(na<double>()), prev_low_(na<double>()), prev_close_(na<double>()),
      prev_prev_high_(na<double>()), prev_prev_low_(na<double>()),
      // Mirror the initial committed state (see RMA::RMA) so a recompute()
      // before the first compute() restores a well-defined pristine state.
      saved_af_(0.0), saved_ep_(0.0), saved_sar_(0.0),
      saved_is_long_(true), saved_initialized_(false),
      saved_prev_high_(na<double>()), saved_prev_low_(na<double>()),
      saved_prev_close_(na<double>()),
      saved_prev_prev_high_(na<double>()), saved_prev_prev_low_(na<double>()) {}

namespace {

// TradingView's parabolic SAR clamps the next-bar SAR so it cannot push
// past the prior two bars' extremes in the trade direction (long: cannot
// rise above the lows; short: cannot drop below the highs). Pulled out as
// a value-only helper since it's a named, self-contained step of the SAR
// recipe and keeps ``SAR::compute`` at one screen.
double sar_clamp_to_recent_pivots(double new_sar, bool is_long,
                                    double prev_low, double prev_prev_low,
                                    double prev_high, double prev_prev_high) {
    if (is_long) {
        if (!is_na(prev_low)) new_sar = std::min(new_sar, prev_low);
        if (!is_na(prev_prev_low)) new_sar = std::min(new_sar, prev_prev_low);
    } else {
        if (!is_na(prev_high)) new_sar = std::max(new_sar, prev_high);
        if (!is_na(prev_prev_high)) new_sar = std::max(new_sar, prev_prev_high);
    }
    return new_sar;
}

}  // namespace

double SAR::compute(double high, double low, double close) {
    saved_af_ = af_;
    saved_ep_ = ep_;
    saved_sar_ = sar_;
    saved_is_long_ = is_long_;
    saved_initialized_ = initialized_;
    saved_prev_high_ = prev_high_;
    saved_prev_low_ = prev_low_;
    saved_prev_close_ = prev_close_;
    saved_prev_prev_high_ = prev_prev_high_;
    saved_prev_prev_low_ = prev_prev_low_;

    if (is_na(high) || is_na(low) || is_na(close)) {
        return na<double>();
    }

    if (is_na(prev_close_)) {
        prev_high_ = high;
        prev_low_ = low;
        prev_close_ = close;
        return na<double>();
    }

    bool is_first_trend_bar = false;
    if (!initialized_) {
        initialized_ = true;
        is_long_ = close > prev_close_;
        ep_ = is_long_ ? high : low;
        sar_ = is_long_ ? prev_low_ : prev_high_;
        af_ = start_;
        is_first_trend_bar = true;
    }

    double new_sar = sar_ + af_ * (ep_ - sar_);

    if (is_long_) {
        if (low < new_sar) {
            is_first_trend_bar = true;
            is_long_ = false;
            new_sar = std::max(high, ep_);
            af_ = start_;
            ep_ = low;
        }
    } else {
        if (high > new_sar) {
            is_first_trend_bar = true;
            is_long_ = true;
            new_sar = std::min(low, ep_);
            af_ = start_;
            ep_ = high;
        }
    }

    if (!is_first_trend_bar) {
        if (is_long_) {
            if (high > ep_) {
                ep_ = high;
                af_ = std::min(af_ + increment_, maximum_);
            }
        } else {
            if (low < ep_) {
                ep_ = low;
                af_ = std::min(af_ + increment_, maximum_);
            }
        }
    }

    new_sar = sar_clamp_to_recent_pivots(new_sar, is_long_,
                                          prev_low_, prev_prev_low_,
                                          prev_high_, prev_prev_high_);

    sar_ = new_sar;
    prev_prev_high_ = prev_high_;
    prev_prev_low_ = prev_low_;
    prev_high_ = high;
    prev_low_ = low;
    prev_close_ = close;

    return sar_;
}

// --- BB (Bollinger Bands) ---

BB::BB(int length, double mult)
    : mult_(mult), sma_(length), stdev_(length) {}

BBResult BB::compute(double src) {
    BBResult result;
    result.middle = na<double>();
    result.upper = na<double>();
    result.lower = na<double>();

    double mid = sma_.compute(src);
    double sd = stdev_.compute(src);

    if (is_na(mid) || is_na(sd)) {
        return result;
    }

    result.middle = mid;
    result.upper = mid + mult_ * sd;
    result.lower = mid - mult_ * sd;
    return result;
}

// --- KC (Keltner Channels) ---

KC::KC(int length, double mult)
    : mult_(mult), ema_(length), range_ema_(length) {}

KCResult KC::compute(double src, double high, double low, double close) {
    KCResult result;
    result.middle = na<double>();
    result.upper = na<double>();
    result.lower = na<double>();

    saved_prev_close_ = prev_close_;

    double mid = ema_.compute(src);

    double span = na<double>();
    if (!is_na(high) && !is_na(low) && !is_na(close) && !is_na(prev_close_)) {
        span = std::max(high - low,
                        std::max(std::abs(high - prev_close_), std::abs(low - prev_close_)));
    }
    double range_ema = range_ema_.compute(span);
    prev_close_ = close;

    if (is_na(mid) || is_na(range_ema)) {
        return result;
    }

    result.middle = mid;
    result.upper = mid + mult_ * range_ema;
    result.lower = mid - mult_ * range_ema;
    return result;
}

// --- Variance ---

Variance::Variance(int length) : length_(length) {}

double Variance::compute(double src) {
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

    double sum = 0.0;
    for (double v : buffer_) {
        sum += v;
    }
    double mean = sum / length_;

    double sq_sum = 0.0;
    for (double v : buffer_) {
        double diff = v - mean;
        sq_sum += diff * diff;
    }
    return sq_sum / length_;
}

// ============================================================================
// BBW (Bollinger Bands Width)
// ============================================================================

BBW::BBW(int length, double mult) : bb_(length, mult) {}

// Pine v6's `ta.bbw` returns the BB width as a PERCENTAGE of the basis
// (i.e., `(upper-lower)/basis * 100`) — the docs example explicitly
// multiplies by 100. KCW (`ta.kcw`) is a sibling but returns the same
// quantity AS A FRACTION (no ×100). The asymmetry is deliberate in
// TradingView's reference implementation; mirror it here.
double BBW::compute(double src) {
    auto r = bb_.compute(src);
    if (is_na(r.middle) || r.middle == 0) return na<double>();
    return (r.upper - r.lower) / r.middle * 100.0;
}

// ============================================================================
// KCW (Keltner Channel Width)
// ============================================================================

KCW::KCW(int length, double mult) : kc_(length, mult) {}

double KCW::compute(double src, double high, double low, double close) {
    auto r = kc_.compute(src, high, low, close);
    if (is_na(r.middle) || r.middle == 0) return na<double>();
    return (r.upper - r.lower) / r.middle;
}

// ============================================================================
// TR (True Range as function)
// ============================================================================

TR::TR(bool handle_na)
    : prev_close_(na<double>()), bar_count_(0), handle_na_(handle_na),
      // Mirror the initial committed state (see RMA::RMA) so a recompute()
      // before the first compute() restores a well-defined pristine state.
      saved_prev_close_(na<double>()), saved_bar_count_(0) {}

double TR::compute(double high, double low, double close) {
    saved_prev_close_ = prev_close_;
    saved_bar_count_ = bar_count_;
    bar_count_++;
    double tr;
    if (is_na(prev_close_)) {
        // First bar: TV v6 default returns na; legacy handle_na=true returns high - low.
        tr = handle_na_ ? (high - low) : na<double>();
    } else {
        tr = std::max(high - low, std::max(std::abs(high - prev_close_), std::abs(low - prev_close_)));
    }
    prev_close_ = close;
    return tr;
}

// --- Dev (Mean Absolute Deviation) ---
double Dev::compute(double src) {
    if (is_na(src)) return na<double>();
    buffer_.push_back(src);
    if ((int)buffer_.size() > length_) buffer_.pop_front();
    if ((int)buffer_.size() < length_) return na<double>();
    double mean = 0.0;
    for (auto v : buffer_) mean += v;
    mean /= length_;
    double mad = 0.0;
    for (auto v : buffer_) mad += std::abs(v - mean);
    return mad / length_;
}

// --- MACD ---
MACDResult MACD::recompute(double src) {
    MACDResult result;
    result.macd_line = na<double>();
    result.signal_line = na<double>();
    result.histogram = na<double>();

    // Check if the previous compute() produced non-na fast/slow
    // by checking if signal_ema was called (i.e., fast/slow were non-na)
    // We can determine this: if fast_ema and slow_ema both have enough bars,
    // signal_ema was called. For correctness, always recompute fast and slow,
    // and only recompute signal if it was called in the original compute.
    // Since fast_ema.recompute() restores and re-calls compute(), and
    // slow_ema.recompute() does the same, we need to check the ORIGINAL
    // fast/slow results. But we can infer: if fast_ema produced non-na
    // before recompute, it will after too (EMA is monotonically non-na after warmup).
    // So the only edge case is at the exact warmup boundary.
    // For simplicity and correctness, save whether signal was called.

    double fast_val = fast_ema.recompute(src);
    double slow_val = slow_ema.recompute(src);

    if (is_na(fast_val) || is_na(slow_val)) {
        return result;
    }

    result.macd_line = fast_val - slow_val;
    double signal_val = signal_ema.recompute(result.macd_line);
    result.signal_line = signal_val;

    if (!is_na(signal_val)) {
        result.histogram = result.macd_line - signal_val;
    }

    return result;
}

// --- ATR ---
double ATR::recompute(double high, double low, double close) {
    prev_close = saved_prev_close_;
    bar_count = saved_bar_count_;
    rma.restore();

    if (is_na(high) || is_na(low) || is_na(close)) {
        return na<double>();
    }

    bar_count++;

    double tr;
    if (is_na(prev_close)) {
        tr = high - low;
    } else {
        tr = std::max({high - low,
                       std::abs(high - prev_close),
                       std::abs(low - prev_close)});
    }

    prev_close = close;
    return rma.compute(tr);
}

// --- StdDev ---
double StdDev::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    double sum = 0.0;
    for (double v : buffer_) sum += v;
    double mean = sum / length_;

    double sq_sum = 0.0;
    for (double v : buffer_) {
        double diff = v - mean;
        sq_sum += diff * diff;
    }
    return std::sqrt(sq_sum / length_);
}

// --- Supertrend ---
SupertrendResult Supertrend::recompute(double high, double low, double close) {
    // Restore saved state
    prev_upper_ = saved_prev_upper_;
    prev_lower_ = saved_prev_lower_;
    prev_st_ = saved_prev_st_;
    prev_direction_ = saved_prev_direction_;
    prev_close_ = saved_prev_close_;
    initialized_ = saved_initialized_;

    return compute(high, low, close);
}

// --- DMI ---
DMIResult DMI::recompute(double high, double low, double close) {
    prev_high_ = saved_prev_high_;
    prev_low_ = saved_prev_low_;
    prev_close_ = saved_prev_close_;
    first_bar_ = saved_first_bar_;
    rma_plus_.restore();
    rma_minus_.restore();
    rma_tr_.restore();
    rma_adx_.restore();

    return compute(high, low, close);
}

// --- SAR ---
double SAR::recompute(double high, double low, double close) {
    af_ = saved_af_;
    ep_ = saved_ep_;
    sar_ = saved_sar_;
    is_long_ = saved_is_long_;
    initialized_ = saved_initialized_;
    prev_high_ = saved_prev_high_;
    prev_low_ = saved_prev_low_;
    prev_close_ = saved_prev_close_;
    prev_prev_high_ = saved_prev_prev_high_;
    prev_prev_low_ = saved_prev_prev_low_;

    return compute(high, low, close);
}

// --- BB ---
BBResult BB::recompute(double src) {
    BBResult result;
    result.middle = na<double>();
    result.upper = na<double>();
    result.lower = na<double>();

    double mid = sma_.recompute(src);
    double sd = stdev_.recompute(src);

    if (is_na(mid) || is_na(sd)) return result;

    result.middle = mid;
    result.upper = mid + mult_ * sd;
    result.lower = mid - mult_ * sd;
    return result;
}

// --- KC ---
KCResult KC::recompute(double src, double high, double low, double close) {
    KCResult result;
    result.middle = na<double>();
    result.upper = na<double>();
    result.lower = na<double>();

    // Restore per-bar committed state then recompute current bar candidate.
    prev_close_ = saved_prev_close_;

    double mid = ema_.recompute(src);
    double span = na<double>();
    if (!is_na(high) && !is_na(low) && !is_na(close) && !is_na(prev_close_)) {
        span = std::max(high - low,
                        std::max(std::abs(high - prev_close_), std::abs(low - prev_close_)));
    }
    double range_ema = range_ema_.recompute(span);
    prev_close_ = close;

    if (is_na(mid) || is_na(range_ema)) return result;

    result.middle = mid;
    result.upper = mid + mult_ * range_ema;
    result.lower = mid - mult_ * range_ema;
    return result;
}

// --- Variance ---
double Variance::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    double sum = 0.0;
    for (double v : buffer_) sum += v;
    double mean = sum / length_;

    double sq_sum = 0.0;
    for (double v : buffer_) {
        double diff = v - mean;
        sq_sum += diff * diff;
    }
    return sq_sum / length_;
}

// --- BBW ---
// Mirror of compute(): Pine's ta.bbw is in PERCENTAGE (×100); see the
// compute() docstring for the asymmetry vs ta.kcw which uses fraction.
double BBW::recompute(double src) {
    auto r = bb_.recompute(src);
    if (is_na(r.middle) || r.middle == 0) return na<double>();
    return (r.upper - r.lower) / r.middle * 100.0;
}

// --- KCW ---
double KCW::recompute(double src, double high, double low, double close) {
    auto r = kc_.recompute(src, high, low, close);
    if (is_na(r.middle) || r.middle == 0) return na<double>();
    return (r.upper - r.lower) / r.middle;
}

// --- TR ---
double TR::recompute(double high, double low, double close) {
    prev_close_ = saved_prev_close_;
    bar_count_ = saved_bar_count_;
    return compute(high, low, close);
}

// --- Dev ---
double Dev::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    double mean = 0.0;
    for (auto v : buffer_) mean += v;
    mean /= length_;
    double mad = 0.0;
    for (auto v : buffer_) mad += std::abs(v - mean);
    return mad / length_;
}

} // namespace ta
} // namespace pineforge
