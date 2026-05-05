/*
 * ta_oscillators.cpp — oscillators / momentum / state: RSI, Stoch, CCI, MFI, CMO, TSI, WPR, COG, RCI, Mom, ROC, Change, Cross, Crossover, Crossunder, Rising, Falling
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


// --- RSI ---

RSI::RSI(int length)
    : rma_up(length), rma_down(length), prev_src(na<double>()), bar_count(0) {}

double RSI::compute(double src) {
    saved_prev_src_ = prev_src;
    saved_bar_count_ = bar_count;
    rma_up.save();
    rma_down.save();

    if (is_na(src)) {
        return na<double>();
    }

    if (bar_count == 0) {
        prev_src = src;
        bar_count++;
        return na<double>();
    }

    double change = src - prev_src;
    double up = std::max(change, 0.0);
    double down = std::max(-change, 0.0);

    double rma_up_val = rma_up.compute(up);
    double rma_down_val = rma_down.compute(down);

    prev_src = src;
    bar_count++;

    if (is_na(rma_up_val) || is_na(rma_down_val)) {
        return na<double>();
    }

    if (rma_down_val == 0.0) {
        return 100.0;
    }

    double rs = rma_up_val / rma_down_val;
    double rsi = 100.0 - 100.0 / (1.0 + rs);
    return rsi;
}

// --- Crossover ---
// Pine parity (TradingView / PineTS): prev1 <= prev2 && current1 > current2
// Ref: pinets/src/namespaces/ta/methods/crossover.ts

Crossover::Crossover()
    : prev_a(na<double>()), prev_b(na<double>()) {}

bool Crossover::compute(double a, double b) {
    saved_prev_a_ = prev_a;
    saved_prev_b_ = prev_b;
    if (is_na(a) || is_na(b) || is_na(prev_a) || is_na(prev_b)) {
        prev_a = a;
        prev_b = b;
        return false;
    }

    bool result = (a > b) && (prev_a <= prev_b);
    prev_a = a;
    prev_b = b;
    return result;
}

// --- Crossunder ---
// Pine parity: prev1 >= prev2 && current1 < current2
// Ref: pinets/src/namespaces/ta/methods/crossunder.ts

Crossunder::Crossunder()
    : prev_a(na<double>()), prev_b(na<double>()) {}

bool Crossunder::compute(double a, double b) {
    saved_prev_a_ = prev_a;
    saved_prev_b_ = prev_b;
    if (is_na(a) || is_na(b) || is_na(prev_a) || is_na(prev_b)) {
        prev_a = a;
        prev_b = b;
        return false;
    }

    bool result = (a < b) && (prev_a >= prev_b);
    prev_a = a;
    prev_b = b;
    return result;
}

// --- Stoch ---

Stoch::Stoch(int length)
    : highest(length), lowest(length), length(length) {}

double Stoch::compute(double src, double high, double low) {
    double hi = highest.compute(high);
    double lo = lowest.compute(low);

    if (is_na(hi) || is_na(lo) || is_na(src)) {
        return na<double>();
    }

    double range = hi - lo;
    if (range == 0.0) {
        return 50.0;  // Avoid division by zero; midpoint when flat
    }

    return (src - lo) / range * 100.0;
}

// --- Change ---

Change::Change(int max_length) : max_length_(max_length) {}

double Change::compute(double src, int length) {
    history.push_back(src);

    // Keep only enough history: we need at most max_length_ + 1 entries
    int keep = std::max(max_length_, length) + 1;
    while ((int)history.size() > keep) {
        history.pop_front();
    }

    if ((int)history.size() <= length) {
        return na<double>();
    }

    int idx = (int)history.size() - 1 - length;
    double prev = history[idx];
    if (is_na(src) || is_na(prev)) {
        return na<double>();
    }

    return src - prev;
}

// --- Cross ---

Cross::Cross()
    : prev_a(na<double>()), prev_b(na<double>()) {}

bool Cross::compute(double a, double b) {
    saved_prev_a_ = prev_a;
    saved_prev_b_ = prev_b;
    if (is_na(a) || is_na(b) || is_na(prev_a) || is_na(prev_b)) {
        prev_a = a;
        prev_b = b;
        return false;
    }

    // Cross in either direction
    bool result = ((a > b) && (prev_a <= prev_b)) ||
                  ((a < b) && (prev_a >= prev_b));
    prev_a = a;
    prev_b = b;
    return result;
}

// --- Mom (Momentum) ---

Mom::Mom(int length) : length_(length) {}

double Mom::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer_.push_back(src);
    while ((int)buffer_.size() > length_ + 1) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < length_ + 1) {
        return na<double>();
    }

    return src - buffer_.front();
}

// --- ROC (Rate of Change) ---

ROC::ROC(int length) : length_(length) {}

double ROC::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }

    buffer_.push_back(src);
    while ((int)buffer_.size() > length_ + 1) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < length_ + 1) {
        return na<double>();
    }

    double prev = buffer_.front();
    if (prev == 0.0) {
        return na<double>();
    }

    return (src - prev) / prev * 100.0;
}

// --- Rising ---

Rising::Rising(int length) : length_(length) {}

double Rising::compute(double src) {
    if (is_na(src)) {
        return 0.0;
    }

    buffer_.push_back(src);
    while ((int)buffer_.size() > length_ + 1) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < length_ + 1) {
        return 0.0;
    }

    // Check if each consecutive pair is rising
    for (int i = 1; i < (int)buffer_.size(); i++) {
        if (buffer_[i] <= buffer_[i - 1]) {
            return 0.0;
        }
    }
    return 1.0;
}

// --- Falling ---

Falling::Falling(int length) : length_(length) {}

double Falling::compute(double src) {
    if (is_na(src)) {
        return 0.0;
    }

    buffer_.push_back(src);
    while ((int)buffer_.size() > length_ + 1) {
        buffer_.pop_front();
    }

    if ((int)buffer_.size() < length_ + 1) {
        return 0.0;
    }

    // Check if each consecutive pair is falling
    for (int i = 1; i < (int)buffer_.size(); i++) {
        if (buffer_[i] >= buffer_[i - 1]) {
            return 0.0;
        }
    }
    return 1.0;
}

// --- CCI (Commodity Channel Index) ---

CCI::CCI(int length) : length_(length) {}

double CCI::compute(double src) {
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

    // Calculate SMA
    double sum = 0.0;
    for (double v : buffer_) {
        sum += v;
    }
    double mean = sum / length_;

    // Calculate mean deviation
    double md_sum = 0.0;
    for (double v : buffer_) {
        md_sum += std::abs(v - mean);
    }
    double mean_dev = md_sum / length_;

    if (mean_dev == 0.0) {
        return 0.0;
    }

    return (src - mean) / (0.015 * mean_dev);
}

// --- RCI: Spearman rank correlation (time order vs price ranks) × 100 ---

RCI::RCI(int length) : length_(length) {}

double RCI::compute(double src) {
    if (is_na(src)) {
        return na<double>();
    }
    buffer_.push_back(src);
    while ((int)buffer_.size() > length_) {
        buffer_.pop_front();
    }
    const int n = (int)buffer_.size();
    if (n < length_ || n < 2) {
        return na<double>();
    }

    std::vector<double> prices(buffer_.begin(), buffer_.end());
    std::vector<int> order(n);
    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return prices[a] < prices[b];
    });

    std::vector<double> price_rank(n);
    int i = 0;
    while (i < n) {
        int j = i;
        while (j + 1 < n && prices[order[j + 1]] == prices[order[i]]) {
            j++;
        }
        double avg_rank = (i + j) / 2.0;
        for (int k = i; k <= j; k++) {
            price_rank[order[k]] = avg_rank;
        }
        i = j + 1;
    }

    double sum_d2 = 0.0;
    for (int t = 0; t < n; t++) {
        double d = (double)t - price_rank[t];
        sum_d2 += d * d;
    }
    double rho = 1.0 - 6.0 * sum_d2 / (n * (n * n - 1));
    return rho * 100.0;
}

// ============================================================================
// MFI (Money Flow Index)
// ============================================================================

MFI::MFI(int length) : length_(length), prev_src_(na<double>()), bar_count_(0) {}

double MFI::compute(double src, double vol) {
    saved_prev_src_ = prev_src_;
    saved_bar_count_ = bar_count_;
    saved_pos_buffer_ = pos_buffer_;
    saved_neg_buffer_ = neg_buffer_;
    bar_count_++;
    double pos = 0, neg = 0;
    if (!is_na(prev_src_)) {
        double mf = src * vol;
        if (src > prev_src_) pos = mf;
        else if (src < prev_src_) neg = mf;
    }
    prev_src_ = src;
    pos_buffer_.push_back(pos);
    neg_buffer_.push_back(neg);
    if ((int)pos_buffer_.size() > length_) { pos_buffer_.pop_front(); neg_buffer_.pop_front(); }
    if (bar_count_ < length_ + 1) return na<double>();
    double pos_sum = 0, neg_sum = 0;
    for (auto v : pos_buffer_) pos_sum += v;
    for (auto v : neg_buffer_) neg_sum += v;
    if (neg_sum == 0) return 100.0;
    return 100.0 - 100.0 / (1.0 + pos_sum / neg_sum);
}

// ============================================================================
// CMO (Chande Momentum Oscillator)
// ============================================================================

CMO::CMO(int length) : length_(length), prev_src_(na<double>()), bar_count_(0) {}

double CMO::compute(double src) {
    saved_prev_src_ = prev_src_;
    saved_bar_count_ = bar_count_;
    saved_up_buffer_ = up_buffer_;
    saved_down_buffer_ = down_buffer_;
    bar_count_++;
    double up = 0, down = 0;
    if (!is_na(prev_src_)) {
        double diff = src - prev_src_;
        if (diff > 0) up = diff;
        else down = -diff;
    }
    prev_src_ = src;
    up_buffer_.push_back(up);
    down_buffer_.push_back(down);
    if ((int)up_buffer_.size() > length_) { up_buffer_.pop_front(); down_buffer_.pop_front(); }
    if (bar_count_ < length_ + 1) return na<double>();
    double up_sum = 0, down_sum = 0;
    for (auto v : up_buffer_) up_sum += v;
    for (auto v : down_buffer_) down_sum += v;
    double denom = up_sum + down_sum;
    if (denom == 0) return 0.0;
    return 100.0 * (up_sum - down_sum) / denom;
}

// ============================================================================
// TSI (True Strength Index)
// ============================================================================

TSI::TSI(int short_length, int long_length)
    : ema_long_(long_length), ema_short_(short_length),
      ema_abs_long_(long_length), ema_abs_short_(short_length),
      prev_src_(na<double>()), bar_count_(0) {}

double TSI::compute(double src) {
    saved_prev_src_ = prev_src_;
    saved_bar_count_ = bar_count_;
    ema_long_.save();
    ema_short_.save();
    ema_abs_long_.save();
    ema_abs_short_.save();
    bar_count_++;
    if (is_na(prev_src_)) { prev_src_ = src; return na<double>(); }
    double pc = src - prev_src_;
    prev_src_ = src;
    double ds = ema_short_.compute(ema_long_.compute(pc));
    double ads = ema_abs_short_.compute(ema_abs_long_.compute(std::abs(pc)));
    if (ads == 0) return 0.0;
    return 100.0 * ds / ads;
}

// ============================================================================
// WPR (Williams %R)
// ============================================================================

WPR::WPR(int length) : length_(length), highest_(length), lowest_(length) {}

double WPR::compute(double close, double high, double low) {
    double hh = highest_.compute(high);
    double ll = lowest_.compute(low);
    if (is_na(hh) || is_na(ll) || hh == ll) return na<double>();
    return (hh - close) / (hh - ll) * -100.0;
}

// ============================================================================
// COG (Center of Gravity)
// ============================================================================

COG::COG(int length) : length_(length) {}

double COG::compute(double src) {
    buffer_.push_back(src);
    if ((int)buffer_.size() > length_) buffer_.pop_front();
    if ((int)buffer_.size() < length_) return na<double>();
    double num = 0, den = 0;
    for (int i = 0; i < (int)buffer_.size(); i++) {
        num += buffer_[i] * (i + 1);
        den += buffer_[i];
    }
    if (den == 0) return 0.0;
    return -num / den;
}

// --- RSI ---
double RSI::recompute(double src) {
    prev_src = saved_prev_src_;
    bar_count = saved_bar_count_;
    rma_up.restore();
    rma_down.restore();
    return compute(src);
}

// --- Crossover ---
bool Crossover::recompute(double a, double b) {
    prev_a = saved_prev_a_;
    prev_b = saved_prev_b_;
    return compute(a, b);
}

// --- Crossunder ---
bool Crossunder::recompute(double a, double b) {
    prev_a = saved_prev_a_;
    prev_b = saved_prev_b_;
    return compute(a, b);
}

// --- Cross ---
bool Cross::recompute(double a, double b) {
    prev_a = saved_prev_a_;
    prev_b = saved_prev_b_;
    return compute(a, b);
}

// --- Stoch ---
double Stoch::recompute(double src, double high, double low) {
    double hi = highest.recompute(high);
    double lo = lowest.recompute(low);

    if (is_na(hi) || is_na(lo) || is_na(src)) {
        return na<double>();
    }

    double range = hi - lo;
    if (range == 0.0) return 50.0;
    return (src - lo) / range * 100.0;
}

// --- Change ---
double Change::recompute(double src, int length) {
    if (history.empty()) return compute(src, length);
    history.back() = src;

    if ((int)history.size() <= length) {
        return na<double>();
    }

    int idx = (int)history.size() - 1 - length;
    double prev = history[idx];
    if (is_na(src) || is_na(prev)) {
        return na<double>();
    }

    return src - prev;
}

// --- Mom ---
double Mom::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_ + 1) return na<double>();
    return src - buffer_.front();
}

// --- ROC ---
double ROC::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_ + 1) return na<double>();

    double prev = buffer_.front();
    if (prev == 0.0) return na<double>();
    return (src - prev) / prev * 100.0;
}

// --- Rising ---
double Rising::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return 0.0;
    if ((int)buffer_.size() < length_ + 1) return 0.0;

    for (int i = 1; i < (int)buffer_.size(); i++) {
        if (buffer_[i] <= buffer_[i - 1]) return 0.0;
    }
    return 1.0;
}

// --- Falling ---
double Falling::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return 0.0;
    if ((int)buffer_.size() < length_ + 1) return 0.0;

    for (int i = 1; i < (int)buffer_.size(); i++) {
        if (buffer_[i] >= buffer_[i - 1]) return 0.0;
    }
    return 1.0;
}

// --- CCI ---
double CCI::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    if ((int)buffer_.size() < length_) return na<double>();

    double sum = 0.0;
    for (double v : buffer_) sum += v;
    double mean = sum / length_;

    double md_sum = 0.0;
    for (double v : buffer_) md_sum += std::abs(v - mean);
    double mean_dev = md_sum / length_;

    if (mean_dev == 0.0) return 0.0;
    return (src - mean) / (0.015 * mean_dev);
}

// --- RCI ---
double RCI::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;

    if (is_na(src)) return na<double>();
    const int n = (int)buffer_.size();
    if (n < length_ || n < 2) return na<double>();

    std::vector<double> prices(buffer_.begin(), buffer_.end());
    std::vector<int> order(n);
    for (int i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return prices[a] < prices[b];
    });

    std::vector<double> price_rank(n);
    int i = 0;
    while (i < n) {
        int j = i;
        while (j + 1 < n && prices[order[j + 1]] == prices[order[i]]) j++;
        double avg_rank = (i + j) / 2.0;
        for (int k = i; k <= j; k++) price_rank[order[k]] = avg_rank;
        i = j + 1;
    }

    double sum_d2 = 0.0;
    for (int t = 0; t < n; t++) {
        double d = (double)t - price_rank[t];
        sum_d2 += d * d;
    }
    double rho = 1.0 - 6.0 * sum_d2 / (n * (n * n - 1));
    return rho * 100.0;
}

// --- MFI ---
double MFI::recompute(double src, double vol) {
    prev_src_ = saved_prev_src_;
    bar_count_ = saved_bar_count_;
    pos_buffer_ = saved_pos_buffer_;
    neg_buffer_ = saved_neg_buffer_;
    return compute(src, vol);
}

// --- CMO ---
double CMO::recompute(double src) {
    prev_src_ = saved_prev_src_;
    bar_count_ = saved_bar_count_;
    up_buffer_ = saved_up_buffer_;
    down_buffer_ = saved_down_buffer_;
    return compute(src);
}

// --- TSI ---
double TSI::recompute(double src) {
    prev_src_ = saved_prev_src_;
    bar_count_ = saved_bar_count_;
    ema_long_.restore();
    ema_short_.restore();
    ema_abs_long_.restore();
    ema_abs_short_.restore();

    return compute(src);
}

// --- WPR ---
double WPR::recompute(double close, double high, double low) {
    double hh = highest_.recompute(high);
    double ll = lowest_.recompute(low);
    if (is_na(hh) || is_na(ll) || hh == ll) return na<double>();
    return (hh - close) / (hh - ll) * -100.0;
}

// --- COG ---
double COG::recompute(double src) {
    if (buffer_.empty()) return compute(src);
    buffer_.back() = src;
    if ((int)buffer_.size() < length_) return na<double>();
    double num = 0, den = 0;
    for (int i = 0; i < (int)buffer_.size(); i++) {
        num += buffer_[i] * (i + 1);
        den += buffer_[i];
    }
    if (den == 0) return 0.0;
    return -num / den;
}

} // namespace ta
} // namespace pineforge
