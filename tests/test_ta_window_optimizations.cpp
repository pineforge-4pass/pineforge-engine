// Differential and allocation checks for the TA window implementations.
//
// Every reference below is deliberately local to this test and follows the
// former straightforward implementation. The production result is compared
// as a double bit pattern; any NaN is compared as NA.

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <array>
#include <cfenv>
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

constexpr int kSeededBars = 4096;
constexpr int kGapPeriod = 13;

std::array<double, kSeededBars> seeded_series() {
    // xorshift32, seed 0x6d2b79f5, shifts (13, 17, 5), unsigned wraparound.
    // Use the high 16 bits modulo this small alphabet; both zero signs have
    // extra weight to put them at the median rank and make them Mode winners.
    // Every thirteenth bar is NA, independent of the PRNG.
    const std::array<double, 22> alphabet = {
        -4.0, -3.0, -2.0, -1.0, -0.0, 0.0, -0.0, 0.0, -0.0, 0.0, 1.0,
        2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0,
    };
    // On libstdc++, sort preserves the three zeros' order in this five-value
    // prefix, whereas nth_element selects +0 at rank 2 instead of -0.
    // Its first three values also give Mode a winning first key of -0.
    const std::array<double, 5> prefix = {1.0, -0.0, 0.0, -0.0, 1.0};
    std::array<double, kSeededBars> series{};
    std::uint32_t state = 0x6d2b79f5u;
    for (int bar = 0; bar < kSeededBars; ++bar) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        series[bar] = alphabet[(state >> 16) % alphabet.size()];
        if (bar % kGapPeriod == kGapPeriod - 1) series[bar] = na<double>();
        if (bar < static_cast<int>(prefix.size())) series[bar] = prefix[bar];
    }
    // Surround the first 17*17 scheduled gaps with every ordered finite pair.
    // At W=3 the NA driver compares [left, NA, right], including distinct
    // keys sharing a map bucket. This exercises NA-first tie repair without
    // depending on chance encounters in the PRNG or hardcoding a hash value.
    static_assert(kSeededBars > 17 * 17 * kGapPeriod, "finite pair coverage fits the series");
    int pair = 0;
    for (int left = -4; left <= 12; ++left) {
        for (int right = -4; right <= 12; ++right) {
            const int gap = kGapPeriod * (++pair) - 1;
            series[gap - 1] = static_cast<double>(left);
            series[gap + 1] = static_cast<double>(right);
        }
    }
    return series;
}

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

// Coverage probes use only the reference window. They do not supply expected
// outputs: those always come from the unchanged former algorithms above.
bool median_selection_changes_zero(const RefMedian& reference, double sorted) {
    if (static_cast<int>(reference.buffer_.size()) < reference.length_
        || is_na(sorted) || sorted != 0.0) {
        return false;
    }
    std::vector<double> selected(reference.buffer_.begin(), reference.buffer_.end());
    if (std::any_of(selected.begin(), selected.end(),
                    [](double value) { return is_na(value); })) {
        return false;
    }
    const int n = static_cast<int>(selected.size());
    if (n % 2 == 1) {
        std::nth_element(selected.begin(), selected.begin() + n / 2, selected.end());
        return !exact(selected[n / 2], sorted);
    }
    std::nth_element(selected.begin(), selected.begin() + n / 2 - 1, selected.end());
    const double lower = selected[n / 2 - 1];
    std::nth_element(selected.begin(), selected.begin() + n / 2, selected.end());
    return !exact((lower + selected[n / 2]) / 2.0, sorted);
}

bool mode_na_tie_repair_is_observable(const RefMode& reference) {
    if (static_cast<int>(reference.buffer_.size()) < reference.length_) return false;
    std::unordered_map<double, int> counts;
    for (double value : reference.buffer_) ++counts[value];
    if (counts.empty() || !is_na(counts.begin()->first)) return false;
    bool has_finite = false;
    for (const auto& entry : counts) {
        if (entry.second != 1) return false;
        if (!is_na(entry.first)) has_finite = true;
    }
    // The first iterated key is NA and all counts tie at one. Removing the
    // is_na(mode_value) tie clause leaves NA instead of the finite minimum.
    return has_finite;
}

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

void test_median_seeded() {
    const auto series = seeded_series();
    int selection_witnesses = 0;
    for (int length = 3; length <= 21; ++length) {
        ta::Median actual(length);
        RefMedian reference(length);
        int compared = 0;
        int negative_zeros = 0;
        int positive_zeros = 0;
        for (int bar = 0; bar < kSeededBars; ++bar) {
            const double expected = reference.compute(series[bar]);
            check_value(actual.compute(series[bar]), expected,
                        "Median seeded signed zero", length, bar);
            ++compared;
            if (!is_na(expected) && expected == 0.0) {
                if (std::signbit(expected)) ++negative_zeros;
                else ++positive_zeros;
            }
            if (!is_na(series[bar]) && median_selection_changes_zero(reference, expected)) {
                ++selection_witnesses;
            }
        }
        check_count(compared, kSeededBars, "Median seeded signed zero", length);
        check_true(negative_zeros > 0 && positive_zeros > 0,
                   "Median seeded reference produces both zero signs");
    }
    check_true(selection_witnesses > 0,
               "Median seeded compares zeros reordered by unguarded selection");
}

void test_mode_seeded() {
    const auto series = seeded_series();
    for (int length = 3; length <= 21; ++length) {
        ta::Mode actual(length);
        RefMode reference(length);
        int compared = 0;
        int negative_zeros = 0;
        int positive_zeros = 0;
        for (int bar = 0; bar < kSeededBars; ++bar) {
            const double expected = reference.compute(series[bar]);
            check_value(actual.compute(series[bar]), expected,
                        "Mode seeded zero winner", length, bar);
            ++compared;
            if (!is_na(expected) && expected == 0.0) {
                if (std::signbit(expected)) ++negative_zeros;
                else ++positive_zeros;
            }
        }
        check_count(compared, kSeededBars, "Mode seeded zero winner", length);
        check_true(negative_zeros > 0 && positive_zeros > 0,
                   "Mode seeded reference produces both zero signs");
    }
}

void test_mode_seeded_na_fallback() {
    const auto series = seeded_series();
    int repair_witnesses = 0;
    for (int length = 3; length <= 21; ++length) {
        ta::Mode actual(length);
        RefMode reference(length);
        int compared = 0;
        int injected = 0;
        int fallback_compared = 0;
        int expected_fallback = 0;
        int ready_bars = 0;
        for (int bar = 0; bar < kSeededBars; ++bar) {
            const bool gap = is_na(series[bar]);
            // compute(NA) skips the window. To reach the exceptional fallback
            // state, first advance this bar, then inject NA with recompute.
            const double src = gap ? 1.0 : series[bar];
            const double expected = reference.compute(src);
            check_value(actual.compute(src), expected,
                        "Mode seeded NA fallback", length, bar);
            ++compared;
            if (bar >= length - 1) {
                ++ready_bars;
                const bool has_na = std::any_of(
                    reference.buffer_.begin(), reference.buffer_.end(),
                    [](double value) { return is_na(value); });
                if (has_na) ++fallback_compared;
                // An injection at g survives a subsequent compute at b
                // exactly when b - length < g < b. Gaps are 12, 25, 38, ...
                const int gap_count = bar / kGapPeriod
                    - std::max(0, (bar - length + 1) / kGapPeriod);
                if (gap_count > 0) ++expected_fallback;
                if (mode_na_tie_repair_is_observable(reference)) ++repair_witnesses;
            }
            if (gap) {
                check_value(actual.recompute(na<double>()),
                            reference.recompute(na<double>()),
                            "Mode seeded inject NA", length, bar);
                ++injected;
            }
        }
        check_count(compared, kSeededBars, "Mode seeded NA fallback", length);
        check_count(injected, kSeededBars / kGapPeriod, "Mode seeded inject NA", length);
        check_count(ready_bars, kSeededBars - length + 1,
                    "Mode seeded ready windows", length);
        check_count(fallback_compared, expected_fallback,
                    "Mode seeded retained NA windows", length);
    }
    check_true(repair_witnesses > 0,
               "Mode seeded compares NA-first tied maps with a finite winner");
}

void test_alma_cached_weights() {
    std::fenv_t saved_environment{};
    const int saved = std::fegetenv(&saved_environment);
    check_true(saved == 0, "ALMA cache witness can save floating environment");
    if (saved != 0) return;

    for (bool floor : {false, true}) {
        // W=5, offset=0.5, sigma=100 gives exponents -800, -200, 0, -200,
        // -800. The tails underflow to zero; the other cached weights and
        // their sums/products with 1.0 are normal or exactly zero.
        // A volatile parameter keeps the reference exp calls observable.
        volatile double sigma = 100.0;
        ta::ALMA actual(5, 0.5, sigma, floor);
        RefALMA reference(5, 0.5, sigma, floor);
        for (int bar = 0; bar < 5; ++bar) {
            actual.compute(1.0);
            reference.compute(1.0);
        }
        int compared = 0;
        for (int again = 0; again < 2; ++again) {
            check_true(std::feclearexcept(FE_ALL_EXCEPT) == 0,
                       "ALMA cache witness can clear reference flags");
            const double expected = again == 0
                ? reference.compute(1.0) : reference.recompute(1.0);
            const bool reference_underflow = std::fetestexcept(FE_UNDERFLOW) != 0;
            check_true(reference_underflow,
                       "ALMA former per-call exp raises underflow (positive control)");
            check_true(std::feclearexcept(FE_ALL_EXCEPT) == 0,
                       "ALMA cache witness can clear production flags");
            const double value = again == 0
                ? actual.compute(1.0) : actual.recompute(1.0);
            const bool actual_underflow = std::fetestexcept(FE_UNDERFLOW) != 0;
            check_value(value, expected, "ALMA cached weights value", 5, again);
            check_true(!actual_underflow, again == 0
                ? "ALMA cached weights compute does not repeat exp underflow"
                : "ALMA cached weights recompute does not repeat exp underflow");
            ++compared;
        }
        check_count(compared, 2, "ALMA cached weights", 5);
    }
    check_true(std::fesetenv(&saved_environment) == 0,
               "ALMA cache witness restores floating environment");
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
// Allocation witness. These windows still use std::deque<double>: warmed
// compute calls can allocate a node as the ends move. Count that separately
// from temporary vector/map/node allocations, over 384 calls at W=5 and W=50.
// libstdc++ nodes are 512 bytes (64 doubles); libc++ nodes are 4096 bytes
// (512 doubles). Both obey the conservative ceil(calls/64)+1 node bound here.
// At these lengths the former vectors (4W/8W bytes) cannot masquerade as a
// deque node. All measured inputs are finite, so Mode's NA fallback is outside
// this claim. No reference or assertion runs inside the measured span; only
// the indicator and the nonallocating size callback do.
// --------------------------------------------------------------------------

#if defined(_LIBCPP_VERSION)
constexpr std::size_t kDequeNodeBytes = 4096;
#else
constexpr std::size_t kDequeNodeBytes = 512;
#endif
// Warm through four node widths, including libc++'s initial pointer-map
// growth, so the measured allocations are steady-state node turnover.
constexpr int kAllocationWarmup = 4 * kDequeNodeBytes / sizeof(double);
constexpr int kAllocationCalls = 384;
constexpr std::size_t kDequeNodeBound = (kAllocationCalls + 63) / 64 + 1;

struct AllocationCounts {
    std::size_t deque_nodes = 0;
    std::size_t other = 0;
    int calls = 0;
};

AllocationCounts* active_allocation_counts = nullptr;

void count_window_allocation(std::size_t size) {
    if (size == kDequeNodeBytes) ++active_allocation_counts->deque_nodes;
    else ++active_allocation_counts->other;
}

template <class Indicator>
AllocationCounts allocations_after_warmup(int length) {
    Indicator indicator(length);
    volatile double sink = 0.0;
    for (int i = 0; i < kAllocationWarmup; ++i) sink = indicator.compute(1.25 + i);
    AllocationCounts counts;
    active_allocation_counts = &counts;
    global_allocation::on_recorded = count_window_allocation;
    global_allocation::recording = true;
    for (int i = 0; i < kAllocationCalls; ++i) {
        sink = indicator.compute(1000.0 + i);
        ++counts.calls;
    }
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    active_allocation_counts = nullptr;
    (void)sink;
    return counts;
}

template <class Indicator, class Reference>
void check_window_allocations(const char* name, int length) {
    const auto counts = allocations_after_warmup<Indicator>(length);
    char label[160];
    std::snprintf(label, sizeof(label), "%s allocation span", name);
    check_count(counts.calls, kAllocationCalls, label, length);
    std::snprintf(label, sizeof(label),
                  "%s length=%d temporary allocations=%zu expected=0",
                  name, length, counts.other);
    check_true(counts.other == 0, label);
    std::snprintf(label, sizeof(label),
                  "%s length=%d deque node allocations=%zu bound=%zu",
                  name, length, counts.deque_nodes, kDequeNodeBound);
    check_true(counts.deque_nodes <= kDequeNodeBound, label);

    // Positive control: the same measurement must see at least one temporary
    // allocation on EVERY former-algorithm call, not just a deque boundary.
    const auto old_counts = allocations_after_warmup<Reference>(length);
    std::snprintf(label, sizeof(label), "%s former allocation span", name);
    check_count(old_counts.calls, kAllocationCalls, label, length);
    std::snprintf(label, sizeof(label),
                  "%s length=%d former temporary allocations=%zu minimum=%d",
                  name, length, old_counts.other, kAllocationCalls);
    check_true(old_counts.other >= static_cast<std::size_t>(kAllocationCalls), label);
}

void test_allocations() {
    for (int length : {5, 50}) {
        check_window_allocations<ta::RCI, RefRCI>("RCI", length);
        check_window_allocations<ta::Median, RefMedian>("Median", length);
        check_window_allocations<ta::Mode, RefMode>("Mode", length);
    }
}

}  // namespace

int main() {
    test_rci();
    test_median();
    test_alma();
    test_mode();
    test_median_seeded();
    test_mode_seeded();
    test_mode_seeded_na_fallback();
    test_alma_cached_weights();
    test_percent_rank();
    test_percentiles();
    test_mfi_cmo();
    test_recompute_paths();
    test_allocations();

    std::printf("test_ta_window_optimizations: checks=%d failures=%d\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
