// LAB-o1s4 E3: arithmetic gate and negative controls; NO engine dependency.
#include "cohort_exposure_oracle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
namespace ce = cohort_exposure_test;

// Candidate storage/selection is deliberately different from the oracle:
// sort by (position, incarnation), deduplicate positions, THEN filter.
// No candidate calls oracle(), model_edit(), or another candidate's exposure.
class StoredCandidate : public ce::ExposureCandidate {
public:
    void insert(std::size_t p, std::uint64_t i, double v) override {
        for (auto& row : rows_) if (row.incarnation == i) row.value = v;
        rows_.push_back({p, i, v});
    }
    void set(std::uint64_t i, double v) override {
        for (auto& row : rows_) if (row.incarnation == i) row.value = v;
    }
    void erase(std::uint64_t i) override {
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                                  [i](const ce::Row& r) { return r.incarnation == i; }),
                    rows_.end());
    }
    void clear() override { rows_.clear(); }

protected:
    std::vector<ce::Row> rows_;
    std::vector<ce::Row> selected() const {
        auto ordered = rows_;
        std::sort(ordered.begin(), ordered.end(), [](const ce::Row& a, const ce::Row& b) {
            return a.position < b.position
                || (a.position == b.position && a.incarnation < b.incarnation);
        });
        ordered.erase(std::unique(ordered.begin(), ordered.end(),
                                  [](const ce::Row& a, const ce::Row& b) {
                                      return a.position == b.position;
                                  }), ordered.end());
        return ordered;
    }
    std::vector<double> values() const {
        std::vector<double> result;
        for (const auto& row : selected())
            if (std::isfinite(row.value) && row.value > 0.0) result.push_back(row.value);
        return result;
    }
    static double clamp(double x) { return std::isfinite(x) && x > 0.0 ? x : 0.0; }
};

class TrueLeftFold final : public StoredCandidate {
public:
    double exposure() override {
        double accumulated = 0.0;
        for (const auto& row : selected())
            if (row.value > 0.0 && std::isfinite(row.value))
                accumulated = accumulated + row.value;
        return clamp(accumulated);
    }
};

class DeltaScalar final : public StoredCandidate {
public:
    void insert(std::size_t p, std::uint64_t i, double v) override {
        change([&] { StoredCandidate::insert(p, i, v); });
    }
    void set(std::uint64_t i, double v) override {
        change([&] { StoredCandidate::set(i, v); });
    }
    void erase(std::uint64_t i) override {
        change([&] { StoredCandidate::erase(i); });
    }
    void clear() override { StoredCandidate::clear(); total_ = 0.0; }
    double exposure() override { return clamp(total_); }

private:
    double total_ = 0.0;
    std::map<std::size_t, double> contributions() const {
        std::map<std::size_t, double> result;
        for (const auto& row : selected())
            result.emplace(row.position, std::isfinite(row.value) && row.value > 0.0
                                             ? row.value : 0.0);
        return result;
    }
    template<class Action> void change(Action action) {
        const auto before = contributions();
        action();
        auto after = contributions();
        for (const auto& row : before) after.try_emplace(row.first, 0.0);
        for (const auto& row : after) {
            const auto old = before.find(row.first);
            const double previous = old == before.end() ? 0.0 : old->second;
            if (ce::bits(previous) == ce::bits(row.second)) continue;
            total_ -= previous;
            total_ += row.second;
        }
    }
};

// Ordinary perfectly balanced BST rebuilt at the median; its cached aggregate
// is exactly sum(left)+value+sum(right). Rebuilding only chooses a tree shape,
// it does not change the wrong subtree-scalar arithmetic being tested.
class BalancedSumTree final : public StoredCandidate {
public:
    double exposure() override {
        const auto operands = values();
        return clamp(sum(operands, 0, operands.size()));
    }
private:
    static double sum(const std::vector<double>& v, std::size_t first, std::size_t last) {
        if (first == last) return 0.0;
        const auto middle = first + (last - first) / 2;
        return (sum(v, first, middle) + v[middle]) + sum(v, middle + 1, last);
    }
};

class Pairwise final : public StoredCandidate {
public:
    double exposure() override {
        const auto operands = values();
        return clamp(sum(operands, 0, operands.size()));
    }
private:
    static double sum(const std::vector<double>& v, std::size_t first, std::size_t last) {
        if (first == last) return 0.0;
        if (last - first == 1) return v[first];
        const auto middle = first + (last - first) / 2;
        return sum(v, first, middle) + sum(v, middle, last);
    }
};

class Kahan final : public StoredCandidate {
public:
    double exposure() override {
        double sum = 0.0, error = 0.0;
        for (double x : values()) {
            const double corrected = x - error;
            const double next = sum + corrected;
            error = (next - sum) - corrected;
            sum = next;
        }
        return clamp(sum);
    }
};

class Neumaier final : public StoredCandidate {
public:
    double exposure() override {
        double sum = 0.0, correction = 0.0;
        for (double x : values()) {
            const double next = sum + x;
            correction += std::abs(sum) >= std::abs(x) ? (sum - next) + x : (x - next) + sum;
            sum = next;
        }
        return clamp(sum + correction);
    }
};

class LongDouble final : public StoredCandidate {
public:
    double exposure() override {
        long double sum = 0.0L;
        for (double x : values()) sum += static_cast<long double>(x);
        return clamp(static_cast<double>(sum));
    }
};

// Actual exact positive integer accumulator, scaled by 2^-1074, followed by
// ONE ties-to-even rounding. 36 limbs cover 2304 bits; even 257 maximum finite
// inputs need only 2107 bits. This is a negative control, not an engine helper.
// No double/long-double additions are used in its accumulation/rounding.
class ExactRoundedOnce final : public StoredCandidate {
public:
    double exposure() override {
        std::array<std::uint64_t, 36> limbs{};
        const auto add_bit = [&](unsigned bit) {
            auto limb = bit / 64;
            std::uint64_t increment = std::uint64_t{1} << (bit % 64);
            for (;;) {
                ce::require(limb < limbs.size(), "exact negative-control accumulator fits");
                const auto before = limbs[limb];
                limbs[limb] += increment;
                if (limbs[limb] >= before) break;
                ++limb;
                increment = 1;
            }
        };
        for (double x : values()) {
            const auto word = ce::bits(x);
            const auto exponent = static_cast<unsigned>((word >> 52) & 0x7ffU);
            auto significand = word & 0x000fffffffffffffULL;
            if (exponent) significand |= 0x0010000000000000ULL;
            unsigned bit = exponent ? exponent - 1 : 0;
            for (; significand; significand >>= 1, ++bit)
                if (significand & 1U) add_bit(bit);
        }
        const auto has_bit = [&](unsigned bit) {
            return (limbs[bit / 64] >> (bit % 64)) & 1U;
        };
        int top = static_cast<int>(limbs.size() * 64) - 1;
        while (top >= 0 && !has_bit(static_cast<unsigned>(top))) --top;
        if (top < 0) return 0.0;
        if (top < 52) return ce::from_bits(limbs[0]); // Exact subnormal.
        const auto cut = static_cast<unsigned>(top - 52);
        std::uint64_t significand = 0;
        for (unsigned bit = 0; bit <= 52; ++bit)
            if (has_bit(cut + bit)) significand |= std::uint64_t{1} << bit;
        bool sticky = false;
        for (unsigned bit = 0; bit + 1 < cut; ++bit)
            sticky = sticky || has_bit(bit);
        if (cut && has_bit(cut - 1) && (sticky || (significand & 1U))) {
            ++significand;
            if (significand == 0x0020000000000000ULL) {
                significand >>= 1;
                ++top;
            }
        }
        if (top >= 2098) return 0.0; // Rounded exponent 2047 -> +inf -> clamp.
        const auto exponent = static_cast<std::uint64_t>(top - 51);
        return ce::from_bits((exponent << 52) | (significand & 0x000fffffffffffffULL));
    }
};

class IncarnationOrder final : public StoredCandidate {
public:
    double exposure() override {
        auto ordered = selected();
        std::stable_sort(ordered.begin(), ordered.end(), [](const ce::Row& a, const ce::Row& b) {
            return a.incarnation < b.incarnation;
        });
        double sum = 0.0;
        for (const auto& row : ordered)
            if (std::isfinite(row.value) && row.value > 0.0) sum += row.value;
        return clamp(sum);
    }
};

class EpsilonPositive final : public StoredCandidate {
public:
    double exposure() override {
        double sum = 0.0;
        for (const auto& row : selected())
            if (std::isfinite(row.value) && row.value > 1e-10) sum += row.value;
        return clamp(sum);
    }
};

class NoFinalClamp final : public StoredCandidate {
public:
    double exposure() override {
        double sum = 0.0;
        for (double x : values()) sum += x;
        return sum;
    }
};

void print_mismatch(const char* candidate, const ce::Mismatch& mismatch) {
    std::printf("candidate=%s rejected_by=%s step=%zu oracle=%016llx candidate=%016llx\n",
                candidate, mismatch.case_name.c_str(), mismatch.step,
                static_cast<unsigned long long>(mismatch.expected),
                static_cast<unsigned long long>(mismatch.actual));
}

template<class Candidate>
void must_reject(const char* name, const char* separating_case, const ce::PreparedCorpus& corpus) {
    Candidate candidate;
    const auto verdict = ce::exercise_candidate(candidate, corpus);
    for (const auto& failure : verdict.rejected_cases) {
        if (failure.case_name != separating_case) continue;
        print_mismatch(name, failure);
        ce::require(verdict.mismatches > 0, "negative control has an actual bit mismatch");
        return;
    }
    for (const auto& failure : verdict.rejected_cases) print_mismatch(name, failure);
    std::fprintf(stderr, "FALSE SUCCESS: %s was not rejected by %s; long-double digits=%d\n",
                 name, separating_case, std::numeric_limits<long double>::digits);
    throw std::runtime_error("corpus failed to reject a required negative control");
}

void check_bit_transport() {
    for (const auto word : {0x0000000000000000ULL, 0x8000000000000000ULL,
                           0x7ff8000000000001ULL, 0x7ff8abcdef012345ULL,
                           0xfff8000000000042ULL})
        ce::require(ce::bits(ce::from_bits(word)) == word, "memcpy preserves zeros/NaN payloads");
    ce::require(ce::bits(0.0) != ce::bits(-0.0), "bit comparator distinguishes zero signs");
}

void check_exact_negative_control() {
    ExactRoundedOnce exact;
    exact.insert(0, 1, 0x1p53);
    exact.insert(1, 2, 1.0);
    exact.insert(2, 3, 1.0);
    // Exact sum 2^53+2 is representable, unlike the left-fold result 2^53.
    ce::require(ce::bits(exact.exposure()) == 0x4340000000000001ULL,
                "exact negative control really retains the two unit operands");
    exact.clear();
    exact.insert(0, 1, 0x0.fffffffffffffp-1022);
    exact.insert(1, 2, 0x1p-1074);
    ce::require(ce::bits(exact.exposure()) == 0x0010000000000000ULL,
                "exact negative control crosses subnormal/normal boundary");
    exact.clear();
    exact.insert(0, 1, 0x1.fffffffffffffp1023);
    exact.insert(1, 2, 0x1.fffffffffffffp1023);
    ce::require(ce::bits(exact.exposure()) == 0x0000000000000000ULL,
                "exact negative control applies overflow clamp");
}
} // namespace

int main() {
    try {
        ce::require_round_to_nearest();
        check_bit_transport();
        check_exact_negative_control();
        ce::require(ce::e1_hand_vectors().size() >= 12, "twelve E3 analytic vectors are present");
        const auto cases = ce::adversarial_corpus();
        const auto corpus = ce::prepare(cases);
        TrueLeftFold correct;
        const auto accepted = ce::exercise_candidate(correct, corpus);
        for (const auto& failure : accepted.rejected_cases) print_mismatch("TRUE-left-fold", failure);
        ce::require(accepted.mismatches == 0, "independent true left fold accepts EVERY checkpoint");
        must_reject<DeltaScalar>("delta-scalar", "middle_delete_1", corpus);
        must_reject<BalancedSumTree>("balanced-sum-tree", "balanced_right_subtree", corpus);
        must_reject<Pairwise>("pairwise", "e1_large_first", corpus);
        must_reject<Kahan>("Kahan", "e1_large_first", corpus);
        must_reject<Neumaier>("Neumaier", "e1_large_first", corpus);
        // If long double is binary64 on a host, it is not a different
        // arithmetic algorithm there. Missing rejection is a FAILURE, never a
        // skip/fabricated pass; the supervisor's x86-64 hosts provide >53 bits.
        must_reject<LongDouble>("long-double", "e1_large_first", corpus);
        must_reject<ExactRoundedOnce>("exact-rounded-once", "e1_large_first", corpus);
        must_reject<IncarnationOrder>("incarnation-order", "incarnation_order_reversed", corpus);
        must_reject<EpsilonPositive>("epsilon-positive", "e1_min_subnormals", corpus);
        must_reject<NoFinalClamp>("no-final-clamp", "e1_overflow_clamp", corpus);
        std::printf("cohort exposure arithmetic: cases=%zu comparisons=%zu; "
                    "true fold accepted; ten wrong candidates rejected\n",
                    corpus.size(), accepted.comparisons);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cohort exposure arithmetic: %s\n", error.what());
        return 1;
    }
}
