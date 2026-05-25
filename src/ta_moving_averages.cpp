/*
 * ta_moving_averages.cpp — moving averages: RMA, SMA, EMA, WMA, HMA, VWMA, ALMA, SWMA
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

RMA::RMA(int length)
    : output_val(na<double>()), sum(0.0), length(length), bar_count(0) {}

double RMA::compute(double src) {
    save();
    if (is_na(src)) {
        return na<double>();
    }

    bar_count++;

    if (bar_count < length) {
        sum += src;
        return na<double>();
    } else if (bar_count == length) {
        sum += src;
        output_val = sum / length;
        return output_val;
    } else {
        // Pine reference formula (TradingView ta.rma):
        //   rma := (src + (length - 1) * rma[1]) / length
        // Use the same expression order as Pine to match per-bar values to
        // the last ULP. The mathematically equivalent form
        //   alpha*src + (1-alpha)*rma[1]   with alpha = 1/length
        // produces 1-3 ULP drift per bar that compounds across the series
        // and can flip RSI/ATR threshold-crossings on close calls. See
        // tests/test_ta_rma_warmup.cpp for fixed Pine-formula reference
        // values.
        output_val = (src + static_cast<double>(length - 1) * output_val)
                     / static_cast<double>(length);
        return output_val;
    }
}

// --- SMA ---

SMA::SMA(int length)
    : buffer(length), length(length), bar_count(0), running_sum(0.0) {}

double SMA::recalculate_exact_sum() const {
    double sum = 0.0;
    std::size_t sz = buffer.size();
    for (std::size_t i = 0; i < sz; ++i) {
        double val = buffer[i];
        if (!is_na(val)) {
            sum += val;
        }
    }
    return sum;
}

double SMA::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    double popped = buffer[length - 1];
    buffer.push_front(src);
    bar_count++;

    if (bar_count < length) {
        if (!is_na(src)) {
            running_sum += src;
        }
        return na<double>();
    }

    if (bar_count == length) {
        if (!is_na(src)) {
            running_sum += src;
        }
        running_sum = recalculate_exact_sum();
        return running_sum / length;
    }

    if (!is_na(popped)) {
        running_sum -= popped;
    }
    running_sum += src;

    // Self-correct precision drift periodically using bitwise AND
    if ((bar_count & 255) == 0) {
        running_sum = recalculate_exact_sum();
    }

    return running_sum / length;
}

// --- EMA ---

EMA::EMA(int length)
    : output_val(na<double>()), alpha(2.0 / (length + 1)), sum(0.0),
      length(length), bar_count(0) {}

double EMA::compute(double src) {
    save();
    if (is_na(src)) {
        // Pine semantics: ignore na inputs and keep prior EMA value.
        return output_val;
    }

    // Pine ta.ema reference:
    //   ema := na(ema[1]) ? src : alpha * src + (1 - alpha) * ema[1]
    // Seed from the first non-na source value (not SMA warmup).
    if (is_na(output_val)) {
        output_val = src;
        sum = src;
        bar_count = 1;
        return output_val;
    }

    output_val = alpha * src + (1.0 - alpha) * output_val;
    bar_count++;
    return output_val;
}

// --- WMA (Weighted Moving Average) ---

WMA::WMA(int length) : length_(length) {}

double WMA::compute(double src) {
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

    // Linear weights: oldest has weight 1, newest has weight length_
    double weighted_sum = 0.0;
    double weight_total = 0.0;
    for (int i = 0; i < length_; i++) {
        double weight = i + 1;
        weighted_sum += buffer_[i] * weight;
        weight_total += weight;
    }
    return weighted_sum / weight_total;
}

// --- HMA (Hull Moving Average) ---
// Pine: ta.hma(src, length) = ta.wma(2 * ta.wma(src, length/2) - ta.wma(src, length), math.round(math.sqrt(length)))
// Empirically matches TV: length/2 is truncation, sqrt uses truncation too (not round),
// verified by `validation/05-hma-cross` matching 100% of TV trades with this behavior.

HMA::HMA(int length)
    : length_(length),
      wma_half_(std::max(length / 2, 1)),
      wma_full_(length),
      wma_sqrt_(std::max((int)std::sqrt((double)length), 1)) {}

double HMA::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    double half_val = wma_half_.compute(src);
    double full_val = wma_full_.compute(src);

    if (is_na(half_val) || is_na(full_val)) {
        return na<double>();
    }

    double diff = 2.0 * half_val - full_val;
    return wma_sqrt_.compute(diff);
}

// --- VWMA (Volume-Weighted Moving Average) ---

VWMA::VWMA(int length) : length_(length), sv_sum_(0.0), v_sum_(0.0) {}

double VWMA::compute(double src, double vol) {
    if (is_na(src) || is_na(vol)) {
        return na<double>();
    }

    double sv = src * vol;
    sv_buffer_.push_back(sv);
    v_buffer_.push_back(vol);
    sv_sum_ += sv;
    v_sum_ += vol;

    while ((int)sv_buffer_.size() > length_) {
        sv_sum_ -= sv_buffer_.front();
        sv_buffer_.pop_front();
        v_sum_ -= v_buffer_.front();
        v_buffer_.pop_front();
    }

    if ((int)sv_buffer_.size() < length_) {
        return na<double>();
    }

    if (v_sum_ == 0.0) {
        return na<double>();
    }

    return sv_sum_ / v_sum_;
}

// ============================================================================
// ALMA (Arnaud Legoux Moving Average)
// ============================================================================

ALMA::ALMA(int length, double offset, double sigma)
    : length_(length), offset_(offset), sigma_(sigma) {}

double ALMA::compute(double src) {
    buffer_.push_back(src);
    if ((int)buffer_.size() > length_) buffer_.pop_front();
    int sz = (int)buffer_.size();
    if (sz < length_) return na<double>();
    double m = offset_ * (length_ - 1);
    double s = length_ / sigma_;
    double norm = 0, sum = 0;
    for (int i = 0; i < length_; i++) {
        double w = std::exp(-((i - m) * (i - m)) / (2.0 * s * s));
        norm += w;
        sum += buffer_[i] * w;
    }
    return sum / norm;
}

// ============================================================================
// SWMA (Symmetrically Weighted Moving Average, fixed period 4)
// ============================================================================

SWMA::SWMA() {}

double SWMA::compute(double src) {
    buffer_.push_back(src);
    if ((int)buffer_.size() > 4) buffer_.pop_front();
    if ((int)buffer_.size() < 4) return na<double>();
    // Weights: 1/6, 2/6, 2/6, 1/6
    return (buffer_[0] + 2.0 * buffer_[1] + 2.0 * buffer_[2] + buffer_[3]) / 6.0;
}

// ============================================================================
// recompute() implementations
// ============================================================================

// --- RMA ---
void RMA::save() {
    saved_output_val_ = output_val;
    saved_sum_ = sum;
    saved_bar_count_ = bar_count;
}
void RMA::restore() {
    output_val = saved_output_val_;
    sum = saved_sum_;
    bar_count = saved_bar_count_;
}
double RMA::recompute(double src) {
    restore();
    return compute(src);
}

// --- EMA ---
void EMA::save() {
    saved_output_val_ = output_val;
    saved_sum_ = sum;
    saved_bar_count_ = bar_count;
}
void EMA::restore() {
    output_val = saved_output_val_;
    sum = saved_sum_;
    bar_count = saved_bar_count_;
}
double EMA::recompute(double src) {
    restore();
    return compute(src);
}

// --- SMA ---
double SMA::recompute(double src) {
    if (is_na(src)) {
        return na<double>();
    }
    if (buffer.size() == 0) {
        return compute(src);
    }

    double old_val = buffer[0];
    buffer.update_front(src);

    if (!is_na(old_val)) {
        running_sum -= old_val;
    }
    if (!is_na(src)) {
        running_sum += src;
    }

    if (bar_count < length) {
        return na<double>();
    }
    return running_sum / length;
}

// --- WMA ---
double WMA::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    double weighted_sum = 0.0;
    double weight_total = 0.0;
    for (int i = 0; i < length_; i++) {
        double weight = i + 1;
        weighted_sum += buffer_[i] * weight;
        weight_total += weight;
    }
    return weighted_sum / weight_total;
}

// --- HMA ---
double HMA::recompute(double src) {
    if (is_na(src)) return na<double>();

    double half_val = wma_half_.recompute(src);
    double full_val = wma_full_.recompute(src);

    if (is_na(half_val) || is_na(full_val)) {
        return na<double>();
    }

    double diff = 2.0 * half_val - full_val;
    // HMA also has diff_buffer_ which was populated in compute
    // but diff_buffer_ is not used directly - wma_sqrt_ handles its own buffer
    return wma_sqrt_.recompute(diff);
}

// --- VWMA ---
double VWMA::recompute(double src, double vol) {
    if (sv_buffer_.empty()) return compute(src, vol);
    if (is_na(src) || is_na(vol)) return na<double>();

    double old_sv = sv_buffer_.back();
    double old_v = v_buffer_.back();
    double new_sv = src * vol;

    sv_buffer_.back() = new_sv;
    v_buffer_.back() = vol;
    sv_sum_ = sv_sum_ - old_sv + new_sv;
    v_sum_ = v_sum_ - old_v + vol;

    if ((int)sv_buffer_.size() < length_) return na<double>();
    if (v_sum_ == 0.0) return na<double>();
    return sv_sum_ / v_sum_;
}

// --- ALMA ---
double ALMA::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    int sz = (int)buffer_.size();
    if (sz < length_) return na<double>();

    double m = offset_ * (length_ - 1);
    double s = length_ / sigma_;
    double norm = 0, sum = 0;
    for (int i = 0; i < length_; i++) {
        double w = std::exp(-((i - m) * (i - m)) / (2.0 * s * s));
        norm += w;
        sum += buffer_[i] * w;
    }
    return sum / norm;
}

// --- SWMA ---
double SWMA::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;
    if ((int)buffer_.size() < 4) return na<double>();
    return (buffer_[0] + 2.0 * buffer_[1] + 2.0 * buffer_[2] + buffer_[3]) / 6.0;
}

} // namespace ta
} // namespace pineforge
