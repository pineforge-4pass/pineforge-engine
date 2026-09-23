/*
 * ta_misc.cpp — stats + helpers + free functions: Linreg, PercentRank, PercentileNearestRank, PercentileLinearInterpolation, Correlation, BarsSince, ValueWhen, pivot_point_levels, PivotPointLevels
 *
 * Carved out of ta.cpp during the v0.1 file-split (phase 6) so the
 * 66-class TA library becomes navigable. Every class declared in
 * <pineforge/ta.hpp> is implemented in exactly one of the ta_*.cpp
 * partitions.
 */

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta_compare_band.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace pineforge {
namespace ta {

namespace {

// The tolerant `<=` (ta_compare_band.hpp): the band that used to live
// here is the engine-wide constant now that ta.dmi decides on it too.
bool percentrank_less_equal(double lhs, double rhs) {
    return float_band_le(lhs, rhs);
}

}  // namespace


// --- Linreg (Linear Regression) ---

Linreg::Linreg(int length) : length_(length) {}

double Linreg::compute(double src, double offset) {
    // Pine parity: one slot per bar; na values stay in the window (ta.linreg includes na).
    buffer_.push_back(src);
    while ((int)buffer_.size() > length_) {
        buffer_.pop_front();
    }

    int N = (int)buffer_.size();
    if (N < length_) {
        return na<double>();
    }

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
    for (int i = 0; i < N; i++) {
        double y = buffer_[i];
        if (is_na(y)) {
            return na<double>();
        }
        double x = (double)i;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    double denom = N * sum_x2 - sum_x * sum_x;
    if (denom == 0.0) {
        return sum_y / N;
    }

    double slope = (N * sum_xy - sum_x * sum_y) / denom;
    double intercept = (sum_y - slope * sum_x) / N;
    return intercept + slope * (N - 1 - offset);
}

// --- PercentRank ---

PercentRank::PercentRank(int length) : length_(length) {}

double PercentRank::compute(double src) {
    // Pine parity: keep one slot per bar (na advances the window). Rank counts only
    // non-na values in the lookback, but the denominator is always LENGTH, never the
    // non-na count — even in the partial (na-lead) warmup window. TV tapes prove
    // ta.percentrank emits count/length there (Lab finding 315; trendmatrix pair
    // verified byte-exact 100.0/0/0). Dividing by the valid count (the PineTS model)
    // mismatches TV as soon as count > 0.
    buffer_.push_back(src);
    while ((int)buffer_.size() > length_ + 1) {
        buffer_.pop_front();
    }

    if (is_na(src)) {
        return na<double>();
    }

    if ((int)buffer_.size() < length_ + 1) {
        return na<double>();
    }

    double current = buffer_.back();
    int count = 0;
    int valid = 0;
    int start = (int)buffer_.size() - 1 - length_;
    for (int i = start; i < (int)buffer_.size() - 1; i++) {
        double v = buffer_[i];
        if (is_na(v)) continue;
        valid++;
        if (percentrank_less_equal(v, current)) {
            count++;
        }
    }
    if (valid == 0) {
        return na<double>();
    }
    return ((double)count / (double)length_) * 100.0;
}

// ============================================================================
// BarsSince
// ============================================================================
// saved_* mirror the initial committed state (see RMA::RMA) so a recompute()
// before the first compute() restores a well-defined pristine state.
BarsSince::BarsSince()
    : count_(0), ever_true_(false),
      saved_count_(0), saved_ever_true_(false) {}

double BarsSince::compute(bool condition) {
    saved_count_ = count_;
    saved_ever_true_ = ever_true_;
    if (condition) {
        ever_true_ = true;
        count_ = 0;
        return 0.0;
    }
    if (!ever_true_) return na<double>();
    count_++;
    return (double)count_;
}

// ============================================================================
// ValueWhen
// ============================================================================

ValueWhen::ValueWhen(int max_occurrence) : max_occurrence_(std::max(1, max_occurrence + 1)) {}

double ValueWhen::compute(bool condition, double source, int occurrence) {
    saved_values_ = values_;
    if (condition) {
        values_.push_front(source);
        if ((int)values_.size() > max_occurrence_) values_.pop_back();
    }
    if (occurrence < (int)values_.size()) return values_[occurrence];
    return na<double>();
}

// ============================================================================
// Correlation
// ============================================================================

Correlation::Correlation(int length) : length_(length) {}

double Correlation::compute(double src1, double src2) {
    x_buffer_.push_back(src1);
    y_buffer_.push_back(src2);
    if ((int)x_buffer_.size() > length_) { x_buffer_.pop_front(); y_buffer_.pop_front(); }
    int n = (int)x_buffer_.size();
    if (n < length_) return na<double>();
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        sx += x_buffer_[i]; sy += y_buffer_[i];
        sxx += x_buffer_[i] * x_buffer_[i];
        syy += y_buffer_[i] * y_buffer_[i];
        sxy += x_buffer_[i] * y_buffer_[i];
    }
    double den = std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
    if (den == 0) return 0.0;
    return (n * sxy - sx * sy) / den;
}

// ============================================================================
// PercentileNearestRank
// ============================================================================

PercentileNearestRank::PercentileNearestRank(int length) : length_(length) {}

double PercentileNearestRank::compute(double src, double percentage) {
    buffer_.push_back(src);
    if ((int)buffer_.size() > length_) buffer_.pop_front();
    if ((int)buffer_.size() < length_) return na<double>();
    std::vector<double> sorted(buffer_.begin(), buffer_.end());
    std::sort(sorted.begin(), sorted.end());
    int idx = std::max(0, std::min((int)sorted.size() - 1,
        (int)std::ceil(percentage / 100.0 * sorted.size()) - 1));
    return sorted[idx];
}

// ============================================================================
// PercentileLinearInterpolation
// ============================================================================

PercentileLinearInterpolation::PercentileLinearInterpolation(int length) : length_(length) {}

double PercentileLinearInterpolation::compute(double src, double percentage) {
    buffer_.push_back(src);
    if ((int)buffer_.size() > length_) buffer_.pop_front();
    if ((int)buffer_.size() < length_) return na<double>();
    std::vector<double> sorted(buffer_.begin(), buffer_.end());
    std::sort(sorted.begin(), sorted.end());
    double rank = percentage / 100.0 * ((int)sorted.size() - 1);
    int lo = (int)std::floor(rank);
    int hi = (int)std::ceil(rank);
    if (lo == hi || hi >= (int)sorted.size()) return sorted[lo];
    double frac = rank - lo;
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

// --- Linreg ---
double Linreg::recompute(double src, double offset) {
    if (buffer_.empty()) return compute(src, offset);
    buffer_.back() = src;

    int N = (int)buffer_.size();
    if (N < length_) return na<double>();

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
    for (int i = 0; i < N; i++) {
        double y = buffer_[i];
        if (is_na(y)) return na<double>();
        double x = (double)i;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    double denom = N * sum_x2 - sum_x * sum_x;
    if (denom == 0.0) return sum_y / N;

    double slope = (N * sum_xy - sum_x * sum_y) / denom;
    double intercept = (sum_y - slope * sum_x) / N;
    return intercept + slope * (N - 1 - offset);
}

// --- PercentRank ---
double PercentRank::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_ + 1) return na<double>();

    double current = buffer_.back();
    int count = 0;
    int valid = 0;
    int start = (int)buffer_.size() - 1 - length_;
    for (int i = start; i < (int)buffer_.size() - 1; i++) {
        double v = buffer_[i];
        if (is_na(v)) continue;
        valid++;
        if (percentrank_less_equal(v, current)) count++;
    }
    if (valid == 0) return na<double>();
    // Denominator is LENGTH, not the non-na count — same TV rule as compute()
    // (Lab finding 315).
    return ((double)count / (double)length_) * 100.0;
}

// --- BarsSince ---
double BarsSince::recompute(bool condition) {
    count_ = saved_count_;
    ever_true_ = saved_ever_true_;
    return compute(condition);
}

// --- ValueWhen ---
double ValueWhen::recompute(bool condition, double source, int occurrence) {
    values_ = saved_values_;
    return compute(condition, source, occurrence);
}

// --- Correlation ---
double Correlation::recompute(double src1, double src2) {
    if (x_buffer_.empty()) return compute(src1, src2);
    x_buffer_.back() = src1;
    y_buffer_.back() = src2;

    int n = (int)x_buffer_.size();
    if (n < length_) return na<double>();

    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        sx += x_buffer_[i]; sy += y_buffer_[i];
        sxx += x_buffer_[i] * x_buffer_[i];
        syy += y_buffer_[i] * y_buffer_[i];
        sxy += x_buffer_[i] * y_buffer_[i];
    }
    double den = std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
    if (den == 0) return 0.0;
    return (n * sxy - sx * sy) / den;
}

// --- PercentileNearestRank ---
double PercentileNearestRank::recompute(double src, double percentage) {
    if (buffer_.empty()) return compute(src, percentage);
    buffer_.back() = src;
    if ((int)buffer_.size() < length_) return na<double>();
    std::vector<double> sorted(buffer_.begin(), buffer_.end());
    std::sort(sorted.begin(), sorted.end());
    int idx = std::max(0, std::min((int)sorted.size() - 1,
        (int)std::ceil(percentage / 100.0 * sorted.size()) - 1));
    return sorted[idx];
}

// --- PercentileLinearInterpolation ---
double PercentileLinearInterpolation::recompute(double src, double percentage) {
    if (buffer_.empty()) return compute(src, percentage);
    buffer_.back() = src;
    if ((int)buffer_.size() < length_) return na<double>();
    std::vector<double> sorted(buffer_.begin(), buffer_.end());
    std::sort(sorted.begin(), sorted.end());
    double rank = percentage / 100.0 * ((int)sorted.size() - 1);
    int lo = (int)std::floor(rank);
    int hi = (int)std::ceil(rank);
    if (lo == hi || hi >= (int)sorted.size()) return sorted[lo];
    double frac = rank - lo;
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

// ============================================================================
// pivot_point_levels (free function)
// ============================================================================

std::vector<double> pivot_point_levels(const std::string& method,
                                       double high, double low, double close) {
    // Codegen lowers `ta.pivot_point_levels(type, anchor)` to pass the
    // PREVIOUS bar's HLC (`_s_high[1]/_s_low[1]/_s_close[1]`), so the very
    // first bar receives NaN inputs. Mirror Pine's na propagation by
    // returning a vector of 11 NaNs in that case.
    if (is_na(high) || is_na(low) || is_na(close)) {
        return std::vector<double>(11, na<double>());
    }
    double P = (high + low + close) / 3.0;
    std::vector<double> out(11, na<double>());
    out[0] = P;

    if (method == "Traditional") {
        double S1 = 2.0 * P - high;
        double R1 = 2.0 * P - low;
        double S2 = P - (high - low);
        double R2 = P + (high - low);
        double S3 = low - 2.0 * (high - P);
        double R3 = high + 2.0 * (P - low);
        double S4 = P - 3.0 * (high - low);
        double R4 = P + 3.0 * (high - low);
        double S5 = P - 4.0 * (high - low);
        double R5 = P + 4.0 * (high - low);
        out[1] = R1; out[2] = S1;
        out[3] = R2; out[4] = S2;
        out[5] = R3; out[6] = S3;
        out[7] = R4; out[8] = S4;
        out[9] = R5; out[10] = S5;
        return out;
    }
    if (method == "Fibonacci") {
        double range = high - low;
        double S1 = P - 0.382 * range;
        double S2 = P - 0.618 * range;
        double S3 = P - range;
        double R1 = P + 0.382 * range;
        double R2 = P + 0.618 * range;
        double R3 = P + range;
        out[1] = R1; out[2] = S1;
        out[3] = R2; out[4] = S2;
        out[5] = R3; out[6] = S3;
        return out;
    }
    if (method == "Woodie") {
        P = (high + low + 2.0 * close) / 4.0;
        out[0] = P;
        double S1 = 2.0 * P - high;
        double R1 = 2.0 * P - low;
        double S2 = P - (high - low);
        double R2 = P + (high - low);
        double S3 = P - 2.0 * (high - low);
        double R3 = P + 2.0 * (high - low);
        double S4 = P - 3.0 * (high - low);
        double R4 = P + 3.0 * (high - low);
        out[1] = R1; out[2] = S1;
        out[3] = R2; out[4] = S2;
        out[5] = R3; out[6] = S3;
        out[7] = R4; out[8] = S4;
        return out;
    }
    if (method == "Classic") {
        double S1 = 2.0 * P - high;
        double R1 = 2.0 * P - low;
        double S2 = P - (high - low);
        double R2 = P + (high - low);
        double S3 = P - 2.0 * (high - low);
        double R3 = P + 2.0 * (high - low);
        double S4 = P - 3.0 * (high - low);
        double R4 = P + 3.0 * (high - low);
        out[1] = R1; out[2] = S1;
        out[3] = R2; out[4] = S2;
        out[5] = R3; out[6] = S3;
        out[7] = R4; out[8] = S4;
        return out;
    }
    if (method == "DM") {
        double x;
        if (close == high)       x = high + 2.0 * low + close;
        else if (close == low)   x = 2.0 * high + low + close;
        else                     x = high + low + 2.0 * close;
        P  = x / 4.0;
        out[0] = P;
        double S1 = x / 2.0 - high;
        double R1 = x / 2.0 - low;
        out[1] = R1; out[2] = S1;
        return out;
    }
    if (method == "Camarilla") {
        double r = high - low;
        double S1 = close - r * 1.1 / 12.0;
        double S2 = close - r * 1.1 / 6.0;
        double S3 = close - r * 1.1 / 4.0;
        double S4 = close - r * 1.1 / 2.0;
        double R1 = close + r * 1.1 / 12.0;
        double R2 = close + r * 1.1 / 6.0;
        double R3 = close + r * 1.1 / 4.0;
        double R4 = close + r * 1.1 / 2.0;
        double R5 = low != 0.0 ? (high / low) * close : na<double>();
        double S5 = is_na(R5) ? na<double>() : close - (R5 - close);
        out[1] = R1; out[2] = S1;
        out[3] = R2; out[4] = S2;
        out[5] = R3; out[6] = S3;
        out[7] = R4; out[8] = S4;
        out[9] = R5; out[10] = S5;
        return out;
    }
    // Unknown method: return P and leave absent levels as na.
    return out;
}

// ============================================================================
// Pivot point levels of an anchored period
// ============================================================================

namespace {

// The levels of the period (o, h, l, c) -- next_open is the open of the
// period the levels are for, which only Woodie reads -- into out[11], each
// formula evaluated operation by operation as the standard definition spells
// it. A formula missing one of its inputs leaves all eleven na, as the free
// function does for a missing high, low or close.
void pivot_levels_of(PivotLevelsType type, double o, double h, double l, double c,
                     double next_open, double* out) {
    for (int i = 0; i < 11; ++i) out[i] = na<double>();
    switch (type) {
    case PivotLevelsType::Woodie: {
        if (is_na(h) || is_na(l) || is_na(next_open)) return;
        const double P = (h + l + 2.0 * next_open) / 4.0;
        const double R3 = h + 2.0 * (P - l);
        const double S3 = l - 2.0 * (h - P);
        out[0] = P;
        out[1] = 2.0 * P - l;           out[2] = 2.0 * P - h;
        out[3] = P + (h - l);           out[4] = P - (h - l);
        out[5] = R3;                    out[6] = S3;
        out[7] = R3 + (h - l);          out[8] = S3 - (h - l);
        return;
    }
    case PivotLevelsType::DM: {
        if (is_na(o) || is_na(h) || is_na(l) || is_na(c)) return;
        double x;
        if (o == c)      x = h + l + 2.0 * c;
        else if (c > o)  x = 2.0 * h + l + c;
        else             x = 2.0 * l + h + c;
        out[0] = x / 4.0;
        out[1] = x / 2.0 - l;           out[2] = x / 2.0 - h;
        return;
    }
    case PivotLevelsType::Traditional:
    case PivotLevelsType::Fibonacci:
    case PivotLevelsType::Classic:
    case PivotLevelsType::Camarilla:
        break;
    }
    if (is_na(h) || is_na(l) || is_na(c)) return;
    const double P = (h + l + c) / 3.0;
    out[0] = P;
    switch (type) {
    case PivotLevelsType::Traditional:
        out[1] = P * 2.0 - l;                   out[2] = P * 2.0 - h;
        out[3] = P + (h - l);                   out[4] = P - (h - l);
        out[5] = P * 2.0 + (h - 2.0 * l);       out[6] = P * 2.0 - (2.0 * h - l);
        out[7] = P * 3.0 + (h - 3.0 * l);       out[8] = P * 3.0 - (3.0 * h - l);
        out[9] = P * 4.0 + (h - 4.0 * l);       out[10] = P * 4.0 - (4.0 * h - l);
        return;
    case PivotLevelsType::Fibonacci:
        out[1] = P + 0.382 * (h - l);           out[2] = P - 0.382 * (h - l);
        out[3] = P + 0.618 * (h - l);           out[4] = P - 0.618 * (h - l);
        out[5] = P + (h - l);                   out[6] = P - (h - l);
        return;
    case PivotLevelsType::Classic:
        out[1] = 2.0 * P - l;                   out[2] = 2.0 * P - h;
        out[3] = P + (h - l);                   out[4] = P - (h - l);
        out[5] = P + 2.0 * (h - l);             out[6] = P - 2.0 * (h - l);
        out[7] = P + 3.0 * (h - l);             out[8] = P - 3.0 * (h - l);
        return;
    case PivotLevelsType::Camarilla: {
        out[1] = c + 1.1 * (h - l) / 12.0;      out[2] = c - 1.1 * (h - l) / 12.0;
        out[3] = c + 1.1 * (h - l) / 6.0;       out[4] = c - 1.1 * (h - l) / 6.0;
        out[5] = c + 1.1 * (h - l) / 4.0;       out[6] = c - 1.1 * (h - l) / 4.0;
        out[7] = c + 1.1 * (h - l) / 2.0;       out[8] = c - 1.1 * (h - l) / 2.0;
        const double R5 = l != 0.0 ? (h / l) * c : na<double>();
        out[9] = R5;
        out[10] = is_na(R5) ? na<double>() : c - (R5 - c);
        return;
    }
    case PivotLevelsType::Woodie:
    case PivotLevelsType::DM:
        return;
    }
}

} // namespace

PivotLevelsType pivot_levels_type(const std::string& name) {
    if (name == "Traditional") return PivotLevelsType::Traditional;
    if (name == "Fibonacci") return PivotLevelsType::Fibonacci;
    if (name == "Woodie") return PivotLevelsType::Woodie;
    if (name == "Classic") return PivotLevelsType::Classic;
    if (name == "DM") return PivotLevelsType::DM;
    if (name == "Camarilla") return PivotLevelsType::Camarilla;
    throw std::invalid_argument("pivot point levels: unknown type '" + name + "'");
}

void PivotPointLevels::Period::enter(double o, double h, double l, double c) {
    if (is_na(open) && !is_na(o)) open = o;
    if (!is_na(h) && (is_na(high) || h > high)) high = h;
    if (!is_na(l) && (is_na(low) || l < low)) low = l;
    if (!is_na(c)) close = c;
}

PivotPointLevels::Levels PivotPointLevels::no_levels() {
    Levels levels;
    for (double& v : levels.v) v = na<double>();
    return levels;
}

std::vector<double> PivotPointLevels::compute(PivotLevelsType type, bool anchor, bool developing,
                                              double open, double high, double low, double close) {
    if (developing && type == PivotLevelsType::Woodie) {
        throw std::runtime_error(
            "pivot point levels: the Woodie type has no developing levels (a Woodie "
            "period's levels need the open of the period after it)");
    }
    saved_period_ = period_;
    saved_held_ = held_;
    if (anchor) {
        // This bar closes the period in progress and opens the next one: the
        // held levels are the closed period's, with this bar's open as the
        // open of the period they are for.
        pivot_levels_of(type, period_.open, period_.high, period_.low, period_.close, open,
                        held_.v);
        period_ = Period{};
    }
    period_.enter(open, high, low, close);
    if (!developing) return std::vector<double>(held_.v, held_.v + 11);
    std::vector<double> out(11);
    pivot_levels_of(type, period_.open, period_.high, period_.low, period_.close, na<double>(),
                    out.data());
    return out;
}

std::vector<double> PivotPointLevels::recompute(PivotLevelsType type, bool anchor, bool developing,
                                                double open, double high, double low,
                                                double close) {
    period_ = saved_period_;
    held_ = saved_held_;
    return compute(type, anchor, developing, open, high, low, close);
}

std::vector<double> PivotPointLevels::compute(const std::string& type, bool anchor, bool developing,
                                              double open, double high, double low, double close) {
    return compute(pivot_levels_type(type), anchor, developing, open, high, low, close);
}

std::vector<double> PivotPointLevels::recompute(const std::string& type, bool anchor,
                                                bool developing, double open, double high,
                                                double low, double close) {
    return recompute(pivot_levels_type(type), anchor, developing, open, high, low, close);
}

} // namespace ta
} // namespace pineforge
