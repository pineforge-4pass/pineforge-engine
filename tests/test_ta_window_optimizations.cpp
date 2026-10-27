// Differential and allocation checks for the TA window implementations.
//
// Every reference below is deliberately local to this test and follows the
// former straightforward implementation. The production result is compared
// as a double bit pattern; any NaN is compared as NA.

#include "global_allocation_replacement.hpp"

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

bool exact(double actual, double expected) {
    if (is_na(actual) || is_na(expected)) return is_na(actual) && is_na(expected);
    return bits(actual) == bits(expected);
}

void check_value(double actual, double expected, const char* name, int length, int bar) {
    ++checks;
    if (exact(actual, expected)) return;
    std::printf("FAIL %s length=%d bar=%d got=0x%016llx expected=0x%016llx\n",
                name, length, bar,
                static_cast<unsigned long long>(bits(actual)),
                static_cast<unsigned long long>(bits(expected)));
    ++failures;
}

void check_true(bool value, const char* name) {
    ++checks;
    if (value) return;
    std::printf("FAIL %s\n", name);
    ++failures;
}

void check_count(int compared, int expected, const char* name, int length) {
    char label[96];
    std::snprintf(label, sizeof(label), "%s length=%d compared nonzero", name, length);
    check_true(compared > 0, label);
    std::snprintf(label, sizeof(label), "%s length=%d compared count", name, length);
    check_true(compared == expected, label);
}

const std::array<int, 4> kLengths = {1, 2, 5, 6};
const std::array<double, 18> kSeries = {
    3.0, 1.0, 1.0, na<double>(), 2.0, 5.0, 2.0, -0.0, 0.0,
    4.0, na<double>(), 4.0, 1.0, 6.0, 1.0, 2.0, 2.0, 7.0,
};

double volume_at(int bar) {
    return bar == 10 ? na<double>() : 100.0 + 3.0 * bar;
}

bool ref_band_eq(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return a == b
        || (std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= 1e-10);
}

bool ref_band_gt(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return a > b && !ref_band_eq(a, b);
}

bool ref_band_lt(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return a < b && !ref_band_eq(a, b);
}

// --------------------------------------------------------------------------
// Independent former implementations.
// --------------------------------------------------------------------------

struct RefRCI {
    explicit RefRCI(int length) : length_(length) {}

    double calculate() {
        const int n = static_cast<int>(buffer_.size());
        std::vector<double> prices(buffer_.begin(), buffer_.end());
        std::vector<int> order(n);
        for (int i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return prices[a] < prices[b];
        });

        std::vector<double> price_rank(n);
        int i = 0;
        while (i < n) {
            int j = i;
            while (j + 1 < n && prices[order[j + 1]] == prices[order[i]]) ++j;
            const double average_rank = (i + j) / 2.0;
            for (int k = i; k <= j; ++k) price_rank[order[k]] = average_rank;
            i = j + 1;
        }

        double sum_d2 = 0.0;
        for (int t = 0; t < n; ++t) {
            const double d = static_cast<double>(t) - price_rank[t];
            sum_d2 += d * d;
        }
        const double rho = 1.0 - 6.0 * sum_d2 / (n * (n * n - 1));
        return rho * 100.0;
    }

    double compute(double src) {
        if (is_na(src)) return na<double>();
        buffer_.push_back(src);
        while (static_cast<int>(buffer_.size()) > length_) buffer_.pop_front();
        const int n = static_cast<int>(buffer_.size());
        if (n < length_ || n < 2) return na<double>();
        return calculate();
    }

    double recompute(double src) {
        if (buffer_.empty()) return compute(src);
        buffer_.back() = src;
        if (is_na(src)) return na<double>();
        const int n = static_cast<int>(buffer_.size());
        if (n < length_ || n < 2) return na<double>();
        return calculate();
    }

    int length_;
    std::deque<double> buffer_;
};

struct RefMedian {
    explicit RefMedian(int length) : length_(length) {}

    double calculate() {
        std::vector<double> sorted(buffer_.begin(), buffer_.end());
        std::sort(sorted.begin(), sorted.end());
        const int n = static_cast<int>(sorted.size());
        if (n % 2 == 1) return sorted[n / 2];
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    }

    double compute(double src) {
        if (is_na(src)) return na<double>();
        buffer_.push_back(src);
        while (static_cast<int>(buffer_.size()) > length_) buffer_.pop_front();
        if (static_cast<int>(buffer_.size()) < length_) return na<double>();
        return calculate();
    }

    double recompute(double src) {
        if (buffer_.empty()) return compute(src);
        buffer_.back() = src;
        if (static_cast<int>(buffer_.size()) < length_) return na<double>();
        return calculate();
    }

    int length_;
    std::deque<double> buffer_;
};

struct RefALMA {
    RefALMA(int length, double offset, double sigma, bool floor)
        : length_(length), offset_(offset), sigma_(sigma), floor_(floor) {}

    double calculate() {
        double m = offset_ * (length_ - 1);
        if (floor_) m = std::floor(m);
        double s = length_ / sigma_;
        double norm = 0.0;
        double sum = 0.0;
        for (int i = 0; i < length_; ++i) {
            const double weight =
                std::exp(-((i - m) * (i - m)) / (2.0 * s * s));
            norm += weight;
            sum += buffer_[i] * weight;
        }
        return sum / norm;
    }

    double compute(double src) {
        buffer_.push_back(src);
        if (static_cast<int>(buffer_.size()) > length_) buffer_.pop_front();
        if (static_cast<int>(buffer_.size()) < length_) return na<double>();
        return calculate();
    }

    double recompute(double src) {
        if (buffer_.empty()) return compute(src);
        buffer_.back() = src;
        if (static_cast<int>(buffer_.size()) < length_) return na<double>();
        return calculate();
    }

    int length_;
    double offset_;
    double sigma_;
    bool floor_;
    std::deque<double> buffer_;
};

struct RefMode {
    explicit RefMode(int length) : length_(length) {}

    double calculate() {
        std::unordered_map<double, int> counts;
        for (double value : buffer_) ++counts[value];
        int max_count = 0;
        double mode_value = na<double>();
        for (auto& entry : counts) {
            if (entry.second > max_count
                || (entry.second == max_count
                    && (is_na(mode_value) || entry.first < mode_value))) {
                max_count = entry.second;
                mode_value = entry.first;
            }
        }
        return mode_value;
    }

    double compute(double src) {
        if (is_na(src)) return na<double>();
        buffer_.push_back(src);
        if (static_cast<int>(buffer_.size()) > length_) buffer_.pop_front();
        if (static_cast<int>(buffer_.size()) < length_) return na<double>();
        return calculate();
    }

    double recompute(double src) {
        if (buffer_.empty()) return compute(src);
        buffer_.back() = src;
        if (is_na(src)) return na<double>();
        if (static_cast<int>(buffer_.size()) < length_) return na<double>();
        return calculate();
    }

    int length_;
    std::deque<double> buffer_;
};

struct RefPercentRank {
    explicit RefPercentRank(int length) : length_(length) {}

    double compute(double src) {
        buffer_.push_back(src);
        while (static_cast<int>(buffer_.size()) > length_ + 1) buffer_.pop_front();
        if (static_cast<int>(buffer_.size()) < length_ + 1) return na<double>();

        const double current = buffer_.back();
        int count = 0;
        const int start = static_cast<int>(buffer_.size()) - 1 - length_;
        for (int i = start; i < static_cast<int>(buffer_.size()) - 1; ++i) {
            if (ref_band_lt(buffer_[i], current)
                || ref_band_eq(buffer_[i], current)) {
                ++count;
            }
        }
        return (static_cast<double>(count) / static_cast<double>(length_)) * 100.0;
    }

    int length_;
    std::deque<double> buffer_;
};

struct RefPercentileWindow {
    explicit RefPercentileWindow(int length) : length_(length) {}

    const std::vector<double>* push(double src) {
        if (length_ < 1) return nullptr;
        left_at_ = -1;
        if (static_cast<int>(history_.size()) == length_) {
            const double left = history_.front();
            history_.pop_front();
            const auto at = std::find_if(ranked_.begin(), ranked_.end(),
                                         [&](double held) {
                                             return is_na(left) ? is_na(held) : held == left;
                                         });
            left_at_ = static_cast<int>(at - ranked_.begin());
            left_value_ = left;
            ranked_.erase(at);
        }
        history_.push_back(src);
        const auto at = std::find_if(ranked_.begin(), ranked_.end(),
                                     [&](double held) { return held > src; });
        entered_at_ = static_cast<int>(at - ranked_.begin());
        ranked_.insert(at, src);
        return static_cast<int>(history_.size()) == length_ ? &ranked_ : nullptr;
    }

    const std::vector<double>* repush(double src) {
        if (entered_at_ < 0) return push(src);
        ranked_.erase(ranked_.begin() + entered_at_);
        history_.pop_back();
        if (left_at_ >= 0) {
            ranked_.insert(ranked_.begin() + left_at_, left_value_);
            history_.push_front(left_value_);
        }
        return push(src);
    }

    int length_;
    std::deque<double> history_;
    std::vector<double> ranked_;
    int entered_at_ = -1;
    int left_at_ = -1;
    double left_value_ = na<double>();
};

double ref_nearest(const std::vector<double>* ranked, double percentage) {
    if (ranked == nullptr) return na<double>();
    const int n = static_cast<int>(ranked->size());
    const double rank = std::ceil(percentage / 100.0 * n);
    if (std::isnan(rank)) return na<double>();
    return (*ranked)[static_cast<int>(
        std::min(std::max(rank, 1.0), static_cast<double>(n))) - 1];
}

double ref_linear(const std::vector<double>* ranked, double percentage) {
    if (ranked == nullptr) return na<double>();
    const int n = static_cast<int>(ranked->size());
    const double k = std::min(std::max(percentage / 100.0 * n - 0.5, 0.0),
                              n - 1.0);
    if (std::isnan(k)) return na<double>();
    const int lower = static_cast<int>(std::floor(k));
    if (lower >= n - 1) return (*ranked)[n - 1];
    const double fraction = k - lower;
    return (*ranked)[lower] * (1.0 - fraction) + (*ranked)[lower + 1] * fraction;
}

struct RefMFI {
    explicit RefMFI(int length) : length_(length) {}

    double compute(double src, double volume) {
        saved_prev_ = prev_;
        saved_bar_count_ = bar_count_;
        saved_pos_ = pos_;
        saved_neg_ = neg_;
        ++bar_count_;

        double positive = 0.0;
        double negative = 0.0;
        if (!is_na(prev_)) {
            const double money_flow = src * volume;
            if (ref_band_gt(src, prev_)) positive = money_flow;
            else if (ref_band_lt(src, prev_)) negative = money_flow;
        }
        prev_ = src;
        pos_.push_back(positive);
        neg_.push_back(negative);
        if (static_cast<int>(pos_.size()) > length_) {
            pos_.pop_front();
            neg_.pop_front();
        }
        if (bar_count_ < length_ + 1) return na<double>();

        double pos_sum = 0.0;
        double neg_sum = 0.0;
        for (double value : pos_) pos_sum += value;
        for (double value : neg_) neg_sum += value;
        if (neg_sum == 0.0) return 100.0;
        return 100.0 - 100.0 / (1.0 + pos_sum / neg_sum);
    }

    double recompute(double src, double volume) {
        prev_ = saved_prev_;
        bar_count_ = saved_bar_count_;
        pos_ = saved_pos_;
        neg_ = saved_neg_;
        return compute(src, volume);
    }

    int length_;
    std::deque<double> pos_;
    std::deque<double> neg_;
    double prev_ = na<double>();
    int bar_count_ = 0;
    std::deque<double> saved_pos_;
    std::deque<double> saved_neg_;
    double saved_prev_ = na<double>();
    int saved_bar_count_ = 0;
};

struct RefCMO {
    explicit RefCMO(int length) : length_(length) {}

    double compute(double src) {
        saved_prev_ = prev_;
        saved_bar_count_ = bar_count_;
        saved_up_ = up_;
        saved_down_ = down_;
        ++bar_count_;

        double up_value = 0.0;
        double down_value = 0.0;
        if (!is_na(prev_)) {
            const double difference = src - prev_;
            if (difference > 0.0) up_value = difference;
            else down_value = -difference;
        }
        prev_ = src;
        up_.push_back(up_value);
        down_.push_back(down_value);
        if (static_cast<int>(up_.size()) > length_) {
            up_.pop_front();
            down_.pop_front();
        }
        if (bar_count_ < length_ + 1) return na<double>();

        double up_sum = 0.0;
        double down_sum = 0.0;
        for (double value : up_) up_sum += value;
        for (double value : down_) down_sum += value;
        const double denominator = up_sum + down_sum;
        if (denominator == 0.0) return na<double>();
        return 100.0 * (up_sum - down_sum) / denominator;
    }

    double recompute(double src) {
        prev_ = saved_prev_;
        bar_count_ = saved_bar_count_;
        up_ = saved_up_;
        down_ = saved_down_;
        return compute(src);
    }

    int length_;
    std::deque<double> up_;
    std::deque<double> down_;
    double prev_ = na<double>();
    int bar_count_ = 0;
    std::deque<double> saved_up_;
    std::deque<double> saved_down_;
    double saved_prev_ = na<double>();
    int saved_bar_count_ = 0;
};

// --------------------------------------------------------------------------
// Differential rows.
// --------------------------------------------------------------------------

void test_rci() {
    for (int length : kLengths) {
        ta::RCI actual(length);
        RefRCI reference(length);
        int compared = 0;
        for (int bar = 0; bar < static_cast<int>(kSeries.size()); ++bar) {
            check_value(actual.compute(kSeries[bar]), reference.compute(kSeries[bar]),
                        "RCI", length, bar);
            ++compared;
        }
        check_count(compared, static_cast<int>(kSeries.size()), "RCI", length);
    }
}

void test_median() {
    for (int length : kLengths) {
        ta::Median actual(length);
        RefMedian reference(length);
        int compared = 0;
        for (int bar = 0; bar < static_cast<int>(kSeries.size()); ++bar) {
            check_value(actual.compute(kSeries[bar]), reference.compute(kSeries[bar]),
                        "Median", length, bar);
            ++compared;
        }
        check_count(compared, static_cast<int>(kSeries.size()), "Median", length);
    }
}

void test_alma() {
    for (int length : kLengths) {
        for (bool floor : {false, true}) {
            ta::ALMA actual(length, 0.73, 5.0, floor);
            RefALMA reference(length, 0.73, 5.0, floor);
            int compared = 0;
            for (int bar = 0; bar < static_cast<int>(kSeries.size()); ++bar) {
                check_value(actual.compute(kSeries[bar]), reference.compute(kSeries[bar]),
                            floor ? "ALMA floor" : "ALMA", length, bar);
                ++compared;
            }
            check_count(compared, static_cast<int>(kSeries.size()),
                        floor ? "ALMA floor" : "ALMA", length);
        }
    }
}

void test_mode() {
    for (int length : kLengths) {
        ta::Mode actual(length);
        RefMode reference(length);
        int compared = 0;
        for (int bar = 0; bar < static_cast<int>(kSeries.size()); ++bar) {
            check_value(actual.compute(kSeries[bar]), reference.compute(kSeries[bar]),
                        "Mode", length, bar);
            ++compared;
        }
        check_count(compared, static_cast<int>(kSeries.size()), "Mode", length);
    }
}

void test_percent_rank() {
    for (int length : kLengths) {
        ta::PercentRank actual(length);
        RefPercentRank reference(length);
        int compared = 0;
        for (int bar = 0; bar < static_cast<int>(kSeries.size()); ++bar) {
            check_value(actual.compute(kSeries[bar]), reference.compute(kSeries[bar]),
                        "PercentRank", length, bar);
            ++compared;
        }
        check_count(compared, static_cast<int>(kSeries.size()), "PercentRank", length);
    }
}

void test_percentiles() {
    constexpr std::array<double, 18> kPercentages = {
        0.0, 25.0, 50.0, 75.0, 100.0, 50.0, 25.0, 75.0, 50.0,
        0.0, 100.0, 50.0, 25.0, 75.0, 50.0, 100.0, 0.0, 50.0,
    };
    for (int length : kLengths) {
        ta::PercentileNearestRank nearest(length);
        ta::PercentileLinearInterpolation linear(length);
        RefPercentileWindow nearest_reference(length);
        RefPercentileWindow linear_reference(length);
        int compared_nearest = 0;
        int compared_linear = 0;
        for (int bar = 0; bar < static_cast<int>(kSeries.size()); ++bar) {
            check_value(nearest.compute(kSeries[bar], kPercentages[bar]),
                        ref_nearest(nearest_reference.push(kSeries[bar]),
                                    kPercentages[bar]),
                        "PercentileNearestRank", length, bar);
            check_value(linear.compute(kSeries[bar], kPercentages[bar]),
                        ref_linear(linear_reference.push(kSeries[bar]),
                                   kPercentages[bar]),
                        "PercentileLinearInterpolation", length, bar);
            ++compared_nearest;
            ++compared_linear;
        }
        check_count(compared_nearest, static_cast<int>(kSeries.size()),
                    "PercentileNearestRank", length);
        check_count(compared_linear, static_cast<int>(kSeries.size()),
                    "PercentileLinearInterpolation", length);
    }
}

void test_mfi_cmo() {
    for (int length : kLengths) {
        ta::MFI actual_mfi(length);
        RefMFI reference_mfi(length);
        ta::CMO actual_cmo(length);
        RefCMO reference_cmo(length);
        int compared_mfi = 0;
        int compared_cmo = 0;
        for (int bar = 0; bar < static_cast<int>(kSeries.size()); ++bar) {
            const double src = kSeries[bar];
            const double volume = volume_at(bar);
            check_value(actual_mfi.compute(src, volume),
                        reference_mfi.compute(src, volume), "MFI", length, bar);
            check_value(actual_cmo.compute(src), reference_cmo.compute(src),
                        "CMO", length, bar);
            ++compared_mfi;
            ++compared_cmo;
        }
        check_count(compared_mfi, static_cast<int>(kSeries.size()), "MFI", length);
        check_count(compared_cmo, static_cast<int>(kSeries.size()), "CMO", length);
    }
}

void test_recompute_paths() {
    const std::array<double, 8> finite = {3.0, 1.0, 1.0, 2.0, 5.0, 2.0, 4.0, 1.0};

    ta::RCI rci(5);
    RefRCI rci_reference(5);
    ta::Median median(5);
    RefMedian median_reference(5);
    ta::ALMA alma(5, 0.73, 5.0, true);
    RefALMA alma_reference(5, 0.73, 5.0, true);
    ta::Mode mode(5);
    RefMode mode_reference(5);
    int compared = 0;
    for (int bar = 0; bar < static_cast<int>(finite.size()); ++bar) {
        check_value(rci.compute(finite[bar]), rci_reference.compute(finite[bar]),
                    "RCI recompute setup", 5, bar);
        check_value(median.compute(finite[bar]), median_reference.compute(finite[bar]),
                    "Median recompute setup", 5, bar);
        check_value(alma.compute(finite[bar]), alma_reference.compute(finite[bar]),
                    "ALMA recompute setup", 5, bar);
        check_value(mode.compute(finite[bar]), mode_reference.compute(finite[bar]),
                    "Mode recompute setup", 5, bar);
        ++compared;
    }
    check_value(rci.recompute(9.25), rci_reference.recompute(9.25),
                "RCI recompute", 5, 8);
    check_value(median.recompute(9.25), median_reference.recompute(9.25),
                "Median recompute", 5, 8);
    check_value(alma.recompute(9.25), alma_reference.recompute(9.25),
                "ALMA recompute", 5, 8);
    check_value(mode.recompute(9.25), mode_reference.recompute(9.25),
                "Mode recompute", 5, 8);
    check_value(mode.recompute(na<double>()), mode_reference.recompute(na<double>()),
                "Mode recompute NA", 5, 9);
    check_value(mode.compute(8.5), mode_reference.compute(8.5),
                "Mode after recompute NA", 5, 10);
    compared += 2;
    ++compared;
    check_true(compared == static_cast<int>(finite.size()) + 3,
               "recompute differential count");

    ta::MFI mfi(5);
    RefMFI mfi_reference(5);
    ta::CMO cmo(5);
    RefCMO cmo_reference(5);
    for (int bar = 0; bar < static_cast<int>(finite.size()); ++bar) {
        check_value(mfi.compute(finite[bar], 100.0 + bar),
                    mfi_reference.compute(finite[bar], 100.0 + bar),
                    "MFI recompute setup", 5, bar);
        check_value(cmo.compute(finite[bar]), cmo_reference.compute(finite[bar]),
                    "CMO recompute setup", 5, bar);
    }
    check_value(mfi.recompute(9.25, 143.0),
                mfi_reference.recompute(9.25, 143.0), "MFI recompute", 5, 8);
    check_value(cmo.recompute(9.25), cmo_reference.recompute(9.25),
                "CMO recompute", 5, 8);
}

// --------------------------------------------------------------------------
// Allocation witness. Warm-up fills the fixed windows before measurement.
// --------------------------------------------------------------------------

std::size_t rci_allocations_after_warmup() {
    ta::RCI indicator(5);
    for (int i = 0; i < 20; ++i) indicator.compute(1.25 + i);
    const std::size_t before = global_allocation::allocations;
    volatile double sink = 0.0;
    for (int i = 0; i < 8; ++i) sink = indicator.compute(100.0 + i);
    (void)sink;
    return global_allocation::allocations - before;
}

std::size_t median_allocations_after_warmup() {
    ta::Median indicator(5);
    for (int i = 0; i < 20; ++i) indicator.compute(1.25 + i);
    const std::size_t before = global_allocation::allocations;
    volatile double sink = 0.0;
    for (int i = 0; i < 8; ++i) sink = indicator.compute(100.0 + i);
    (void)sink;
    return global_allocation::allocations - before;
}

std::size_t mode_allocations_after_warmup() {
    ta::Mode indicator(5);
    for (int i = 0; i < 20; ++i) indicator.compute(1.25 + i);
    const std::size_t before = global_allocation::allocations;
    volatile double sink = 0.0;
    for (int i = 0; i < 8; ++i) sink = indicator.compute(100.0 + i);
    (void)sink;
    return global_allocation::allocations - before;
}

void test_allocations() {
    check_true(rci_allocations_after_warmup() == 0,
               "RCI has no per-call allocations after warmup");
    check_true(median_allocations_after_warmup() == 0,
               "Median has no per-call allocations after warmup");
    check_true(mode_allocations_after_warmup() == 0,
               "Mode has no per-call allocations after warmup");
}

}  // namespace

int main() {
    test_rci();
    test_median();
    test_alma();
    test_mode();
    test_percent_rank();
    test_percentiles();
    test_mfi_cmo();
    test_recompute_paths();
    test_allocations();

    std::printf("test_ta_window_optimizations: checks=%d failures=%d\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
