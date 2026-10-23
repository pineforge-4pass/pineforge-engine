#pragma once

// LAB-o1s4 E3. Pure binary64 contract, independent of engine/index code.
// Edit-only lane: compilation and executed red/green evidence belong to CI.
#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cohort_exposure_test {

static_assert(sizeof(double) == sizeof(std::uint64_t)
                  && std::numeric_limits<double>::is_iec559
                  && std::numeric_limits<double>::digits == 53
                  && std::numeric_limits<double>::max_exponent == 1024,
              "cohort exposure specifies IEEE binary64");

inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

inline void require_round_to_nearest() {
    require(std::fegetround() == FE_TONEAREST, "exposure requires round-to-nearest/even");
}

inline std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

inline double from_bits(std::uint64_t word) {
    double result = 0.0;
    std::memcpy(&result, &word, sizeof result);
    return result;
}

struct Row {
    std::size_t position;
    std::uint64_t incarnation;
    double value;
};

// Transcription of pine_adapter.cpp:5314-5347, without the cohort lookup.
// WorkingMap visits ascending incarnation keys. Stable incarnation ordering
// here reproduces that even when the input vector has a different order.
// Selection precedes filtering: a NaN at the winning tie suppresses the other
// rows at that position. Equal positions never advance the fold twice.
// This function shares NO selection, filtering, accumulation or mutation code
// with the candidates. Repeated occurrences at distinct positions DO count.
inline double oracle(const std::vector<Row>& rows) {
    auto canonical = rows;
    std::stable_sort(canonical.begin(), canonical.end(), [](const Row& a, const Row& b) {
        return a.incarnation < b.incarnation;
    });
    double total = 0.0;
    bool have_previous = false;
    std::size_t previous = 0;
    for (;;) {
        bool have_next = false;
        std::size_t next = 0;
        double units = 0.0;
        for (const auto& row : canonical) {
            if (have_previous && row.position <= previous) continue;
            if (!have_next || row.position < next) {
                have_next = true;
                next = row.position;
                units = row.value;
            }
        }
        if (!have_next) break;
        if (std::isfinite(units) && units > 0.0) total += units;
        have_previous = true;
        previous = next;
    }
    return std::isfinite(total) && total > 0.0 ? total : 0.0;
}

// insert appends one position occurrence and installs the incarnation's value
// at ALL its occurrences. set changes a present incarnation (absent is a no-op).
// erase removes all occurrences of an incarnation; reinsert supplies positions
// explicitly. clear removes everything. All queries must return current facts.
// A store-backed implementation can adapt to this interface without changing
// the corpus/driver. Engine-only orphan and retained-roster states are tested
// separately because this arithmetic interface has no origin-without-units API.
class ExposureCandidate {
public:
    virtual ~ExposureCandidate() = default;
    virtual void insert(std::size_t position, std::uint64_t incarnation, double value) = 0;
    virtual void set(std::uint64_t incarnation, double value) = 0;
    virtual void erase(std::uint64_t incarnation) = 0;
    virtual void clear() = 0;
    virtual double exposure() = 0;
};

enum class Edit { insert, set, erase, clear };
struct Mutation {
    Edit edit;
    std::size_t position = 0;
    std::uint64_t incarnation = 0;
    double value = 0.0;
};
inline Mutation insert(std::size_t p, std::uint64_t i, double v) {
    return {Edit::insert, p, i, v};
}
inline Mutation set(std::uint64_t i, double v) { return {Edit::set, 0, i, v}; }
inline Mutation erase(std::uint64_t i) { return {Edit::erase, 0, i, 0.0}; }
inline Mutation clear() { return {Edit::clear, 0, 0, 0.0}; }

struct BitPin {
    std::size_t step; // Number of mutations already applied; zero is empty.
    std::uint64_t expected;
};
struct Case {
    std::string name;
    std::vector<Mutation> mutations;
    std::vector<BitPin> pins;
};
struct HandVector {
    const char* name;
    std::vector<double> operands;
    std::uint64_t expected_bits;
};

// Twelve independently derived E3 vectors. E1 can merge further vectors here;
// these literals are analytic binary64 answers, never copied from execution.
inline std::vector<HandVector> e1_hand_vectors() {
    return {
        // ulp(1)=2^-52; half an ulp at even significand 2^52 rounds down.
        {"e1_even_half", {1.0, 0x1p-53}, 0x3ff0000000000000ULL},
        // (1+2^-52)+2^-53 is halfway; odd low bit rounds to 1+2*2^-52.
        {"e1_odd_half", {0x1.0000000000001p0, 0x1p-53}, 0x3ff0000000000002ULL},
        // (2-2^-52)+2^-53 is halfway below 2; 2 has the even significand.
        {"e1_binade_carry", {0x1.fffffffffffffp0, 0x1p-53}, 0x4000000000000000ULL},
        // 1 is lost at 2^60 (ulp=256), then 128 ties to that even accumulator.
        {"e1_jump_60", {1.0, 0x1p60, 128.0}, 0x43b0000000000000ULL},
        // Two minimum subnormals are exactly 2*2^-1074 (raw significand 2).
        {"e1_min_subnormals", {0x1p-1074, 0x1p-1074}, 0x0000000000000002ULL},
        // (2^52-1)*2^-1074 + 2^-1074 = 2^-1022, the first normal.
        {"e1_subnormal_to_normal", {0x0.fffffffffffffp-1022, 0x1p-1074},
         0x0010000000000000ULL},
        // 2^-1074 is far below half ulp(1), so it cannot change 1's bits.
        {"e1_subnormal_at_one", {1.0, 0x1p-1074}, 0x3ff0000000000000ULL},
        // 2*(2-2^-52)*2^1023 exceeds binary64 range; +inf clamps to +0.
        {"e1_overflow_clamp", {0x1.fffffffffffffp1023, 0x1.fffffffffffffp1023},
         0x0000000000000000ULL},
        // 1+2^53 ties to 2^53, whose ulp is 2; the last 1 ties down again.
        {"e1_lost_middle", {1.0, 0x1p53, 1.0}, 0x4340000000000000ULL},
        // Both 1s tie down separately; an exact/compensated sum would be +2.
        {"e1_large_first", {0x1p53, 1.0, 1.0}, 0x4340000000000000ULL},
        // 1+1=2 exactly; 2^53+2 is the next binary64 number above 2^53.
        {"e1_small_first", {1.0, 1.0, 0x1p53}, 0x4340000000000001ULL},
        // (2^53-1)+1=2^53 exactly: the operand moves into the next binade.
        {"e1_exact_binade_jump", {0x1.fffffffffffffp52, 1.0}, 0x4340000000000000ULL}
    };
}

inline Case values_case(std::string name, const std::vector<double>& values,
                        std::uint64_t expected) {
    Case result{std::move(name), {}, {}};
    for (std::size_t i = 0; i < values.size(); ++i)
        result.mutations.push_back(insert(i, i + 1, values[i]));
    result.pins.push_back({values.size(), expected});
    return result;
}

// SplitMix64: unsigned arithmetic/shift widths fully specify the stream.
// No standard-library distributions or platform-dependent seed conversion.
struct Generator {
    std::uint64_t state = 0x4f31533445ULL;
    std::uint64_t next() {
        auto z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    double finite() {
        auto word = next();
        // Map exponent 2047 to 2046 without losing payload/sign variety.
        if ((word & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
            word ^= 0x0010000000000000ULL;
        return from_bits(word);
    }
    double quantity() {
        switch (next() % 8) {
        case 0: return 0.001;
        case 1: return 0.01;
        case 2: return 1.0 / 3.0;
        case 3: return 0.125 * static_cast<double>(next() % 129);
        case 4: return from_bits(1 + next() % 0x0010000000000000ULL);
        default: return finite();
        }
    }
};

inline std::vector<Case> adversarial_corpus() {
    std::vector<Case> cases;
    for (const auto& hand : e1_hand_vectors())
        cases.push_back(values_case(hand.name, hand.operands, hand.expected_bits));

    // In units of 2^-56, binary64 .1/.2/.3 have integer numerators
    // A=7205759403792794, B=14411518807585588, C=21617278211378380.
    // R(A+B)=21617278211378384; R(A+C)=28823037615171176.
    // Adding the third operand then gives 43234556422756764, halfway
    // between ...760 (odd) and ...768 (even): choose ...768, bits ...334.
    // B+C=36028797018963968 exactly; +A gives ...762, nearer ...760:
    // bits ...333. Commuting the first pair covers all six orders.
    cases.push_back(values_case("decimal_abc", {0.1, 0.2, 0.3}, 0x3fe3333333333334ULL));
    cases.push_back(values_case("decimal_acb", {0.1, 0.3, 0.2}, 0x3fe3333333333334ULL));
    cases.push_back(values_case("decimal_bac", {0.2, 0.1, 0.3}, 0x3fe3333333333334ULL));
    cases.push_back(values_case("decimal_bca", {0.2, 0.3, 0.1}, 0x3fe3333333333333ULL));
    cases.push_back(values_case("decimal_cab", {0.3, 0.1, 0.2}, 0x3fe3333333333334ULL));
    cases.push_back(values_case("decimal_cba", {0.3, 0.2, 0.1}, 0x3fe3333333333333ULL));

    // A median-root tree of five leaves computes its last two 1s together,
    // whereas all four sequential 1s tie down at the even 2^53 accumulator.
    cases.push_back(values_case("balanced_right_subtree", {0x1p53, 1, 1, 1, 1},
                                0x4340000000000000ULL));
    const std::array<std::vector<double>, 4> triples{{
        {0.1, 0.2, 0.3}, {1, 0x1p53, 1}, {0x1p53, 1, 1}, {1, 1, 0x1p53}
    }};
    const std::array<std::uint64_t, 4> initial{{
        0x3fe3333333333334ULL, 0x4340000000000000ULL,
        0x4340000000000000ULL, 0x4340000000000001ULL
    }};
    // Deleting the middle leaves [.1,.3], [1,1], [2^53,1], [1,2^53].
    const std::array<std::uint64_t, 4> deleted{{
        0x3fd999999999999aULL, 0x4000000000000000ULL,
        0x4340000000000000ULL, 0x4340000000000000ULL
    }};
    // Middle=.5: (.1+.5)+.3 = R(binary64(.6)+binary64(.3)) =
    // 0x1.cccccccccccccp-1 (halfway chooses even); 1+.5+1=2.5;
    // [2^53,.5,1] loses both small adds; [1,.5,2^53] rounds to 2^53+2.
    const std::array<std::uint64_t, 4> half_replaced{{
        0x3fecccccccccccccULL, 0x4004000000000000ULL,
        0x4340000000000000ULL, 0x4340000000000001ULL
    }};
    // Moving the middle to the end produces [.1,.3,.2], [1,1,2^53],
    // [2^53,1,1], [1,2^53,1]; the decimal/2^53 derivations above apply.
    const std::array<std::uint64_t, 4> moved{{
        0x3fe3333333333334ULL, 0x4340000000000001ULL,
        0x4340000000000000ULL, 0x4340000000000000ULL
    }};
    for (std::size_t i = 0; i < triples.size(); ++i) {
        auto c = values_case("middle_delete_" + std::to_string(i), triples[i], initial[i]);
        c.mutations.push_back(erase(2));
        // For [1,2^53,1], deleting the middle leaves exactly 2; a scalar
        // delta instead computes (rounded 2^53)-2^53=0.
        c.mutations.push_back(insert(1, 2, triples[i][1])); // Same retained position.
        c.mutations.push_back(set(2, 0.5));
        c.mutations.push_back(set(2, triples[i][1]));
        c.mutations.push_back(erase(2));
        c.mutations.push_back(insert(40, 2, triples[i][1])); // New position.
        c.pins.insert(c.pins.end(), {{4, deleted[i]}, {5, initial[i]}, {6, half_replaced[i]},
                                     {7, initial[i]}, {8, deleted[i]}, {9, moved[i]}});
        cases.push_back(std::move(c));
    }

    const double nan1 = from_bits(0x7ff8000000000001ULL);
    const double nan2 = from_bits(0x7ff8abcdef012345ULL);
    const double nan3 = from_bits(0xfff8000000000042ULL);
    cases.push_back({"position_tie_filter_before_add",
        {insert(4, 30, 0x1p53), insert(4, 10, nan1), insert(9, 20, 0.1),
         erase(10), set(30, nan2), insert(4, 5, 1.0), erase(5)},
        // Lowest incarnation 10 wins but is filtered: only .1 remains.
        {{3, 0x3fb999999999999aULL}, {4, 0x4340000000000000ULL}}});
    cases.push_back({"incarnation_order_reversed",
        {insert(0, 30, 0.1), insert(1, 20, 0.2), insert(2, 10, 0.3)},
        {{3, 0x3fe3333333333334ULL}}});
    cases.push_back({"repeated_incarnation",
        {insert(0, 40, 0.1), insert(1, 50, 0.2), insert(2, 40, 0.1),
         set(40, 0.3), erase(40), insert(8, 40, 0.1), insert(10, 40, 0.1)},
        // (.1+.2)+.1 rounds to .4; (.3+.2)+.3 rounds to .8.
        {{3, 0x3fd999999999999aULL}, {4, 0x3fe999999999999aULL}}});
    cases.push_back(values_case("filtered_payloads",
        {nan1, nan2, nan3, from_bits(0x7ff0000000000000ULL),
         from_bits(0xfff0000000000000ULL), -1.0, -0x1p-1074, 0.0,
         from_bits(0x8000000000000000ULL), 0x1p-1074},
        0x0000000000000001ULL)); // Only the positive minimum subnormal survives.
    // Doubling a binary64 value is exact here and only increments its exponent.
    // .001=0x1.0624dd2f1a9fcp-10, .01=0x1.47ae147ae147bp-7.
    cases.push_back(values_case("quantity_step_001", {0.001, 0.001},
                                0x3f60624dd2f1a9fcULL));
    cases.push_back(values_case("quantity_step_01", {0.01, 0.01},
                                0x3f947ae147ae147bULL));
    // 3*binary64(1/3)=1-2^-54 exactly, halfway below 1; ties choose 1.
    cases.push_back(values_case("quantity_thirds", {1.0 / 3, 1.0 / 3, 1.0 / 3},
                                0x3ff0000000000000ULL));
    // Tiny operands disappear at 2^900; the last equal large operand doubles it.
    cases.push_back(values_case("huge_and_tiny", {0x1p900, 0x1p-900, 1, 0x1p900},
                                0x7840000000000000ULL));
    // 257 exact dyadic steps: 257/1024 = 0x1.01p-2.
    cases.push_back(values_case("many_equal", std::vector<double>(257, 0x1p-10),
                                0x3fd0100000000000ULL));
    cases.push_back({"lose_all_and_regain",
        {insert(0, 1, 0.1), insert(1, 2, 0.2), erase(1), erase(2),
         insert(99, 1, 2), clear(), insert(120, 3, 2), insert(101, 4, 4)},
        {{4, 0x0000000000000000ULL}, {6, 0x0000000000000000ULL},
         {8, 0x4018000000000000ULL}}}); // Empty gives +0; 4+2=6 exactly.
    cases.push_back({"max_position",
        {insert(std::numeric_limits<std::size_t>::max(), 1, 1.0)},
        {{1, 0x3ff0000000000000ULL}}}); // A position is not an end sentinel.

    Generator rng;
    for (std::size_t n : {0U, 1U, 2U, 3U, 7U, 17U, 33U, 65U, 257U}) {
        Case c{"seeded_U" + std::to_string(n), {}, {}};
        std::vector<std::size_t> positions;
        std::vector<std::uint64_t> incarnations;
        for (std::size_t i = 0; i < n; ++i) positions.push_back(3 * i + 1);
        for (std::size_t i = n; i > 1; --i)
            std::swap(positions[i - 1], positions[static_cast<std::size_t>(rng.next() % i)]);
        for (std::size_t i = 0; i < n; ++i) {
            c.mutations.push_back(insert(positions[i], i + 1, rng.quantity()));
            incarnations.push_back(i + 1);
        }
        const std::size_t edits = n <= 17 ? 48 : 12;
        for (std::size_t step = 0; step < edits; ++step) {
            const auto choice = rng.next();
            const auto incarnation = incarnations.empty() ? std::uint64_t{1}
                : incarnations[static_cast<std::size_t>(choice % incarnations.size())];
            if (step % 4 == 0 || step % 4 == 3)
                c.mutations.push_back(set(incarnation, rng.quantity()));
            else if (step % 4 == 1)
                c.mutations.push_back(erase(incarnation));
            else {
                // Sequence RNG calls explicitly: C++17 does not order
                // different function arguments relative to each other.
                const auto position =
                    static_cast<std::size_t>(rng.next() % (4 * (n + edits) + 1));
                const auto value = rng.quantity();
                c.mutations.push_back(insert(position, 1000 + step, value));
                incarnations.push_back(1000 + step);
            }
        }
        c.mutations.push_back(clear());
        c.mutations.push_back(insert(n + 100, 9001, rng.quantity()));
        cases.push_back(std::move(c));
    }
    return cases;
}

// Test-side canonical edits, independent of every candidate's storage.
inline void model_edit(std::vector<Row>& model, const Mutation& m) {
    if (m.edit == Edit::clear) {
        model.clear();
    } else if (m.edit == Edit::erase) {
        std::vector<Row> retained;
        for (const auto& row : model)
            if (row.incarnation != m.incarnation) retained.push_back(row);
        model.swap(retained);
    } else {
        for (auto& row : model)
            if (row.incarnation == m.incarnation) row.value = m.value;
        if (m.edit == Edit::insert) model.push_back({m.position, m.incarnation, m.value});
    }
}

struct Checkpoint { Mutation mutation; std::uint64_t expected; };
struct PreparedCase { std::string name; std::vector<Checkpoint> checkpoints; };
using PreparedCorpus = std::vector<PreparedCase>;

// Compute the quadratic oracle ONCE per checkpoint, outside candidate/engine
// measurements. Every analytic pin checks the oracle too; neither side can
// silently share a mistaken literal. All comparison operands are raw bits.
inline PreparedCorpus prepare(const std::vector<Case>& cases) {
    PreparedCorpus result;
    for (const auto& c : cases) {
        PreparedCase prepared{c.name, {}};
        std::vector<Row> model;
        for (const auto& pin : c.pins)
            require(pin.step <= c.mutations.size(), "pin names an existing checkpoint");
        const auto verify_pin = [&](std::size_t step, std::uint64_t actual) {
            for (const auto& pin : c.pins) {
                if (pin.step != step || pin.expected == actual) continue;
                std::fprintf(stderr, "%s step=%zu oracle=%016llx hand=%016llx\n",
                             c.name.c_str(), step, static_cast<unsigned long long>(actual),
                             static_cast<unsigned long long>(pin.expected));
                throw std::runtime_error("analytic exposure pin disagrees with oracle");
            }
        };
        verify_pin(0, bits(oracle(model)));
        for (std::size_t i = 0; i < c.mutations.size(); ++i) {
            model_edit(model, c.mutations[i]);
            const auto expected = bits(oracle(model));
            verify_pin(i + 1, expected);
            prepared.checkpoints.push_back({c.mutations[i], expected});
        }
        result.push_back(std::move(prepared));
    }
    return result;
}

struct Mismatch {
    std::string case_name;
    std::size_t step;
    std::uint64_t expected, actual;
};
struct Verdict {
    std::size_t comparisons = 0, mismatches = 0;
    std::vector<Mismatch> rejected_cases; // First mismatch in EACH named case.
};

inline Verdict exercise_candidate(ExposureCandidate& candidate, const PreparedCorpus& corpus) {
    Verdict result;
    for (const auto& c : corpus) {
        candidate.clear();
        bool rejected = false;
        const auto compare = [&](std::size_t step, std::uint64_t expected) {
            const auto actual = bits(candidate.exposure()); // Always executes.
            ++result.comparisons;
            if (actual == expected) return;
            ++result.mismatches;
            if (!rejected) result.rejected_cases.push_back({c.name, step, expected, actual});
            rejected = true;
        };
        compare(0, 0x0000000000000000ULL);
        for (std::size_t i = 0; i < c.checkpoints.size(); ++i) {
            const auto& point = c.checkpoints[i];
            const auto& m = point.mutation;
            switch (m.edit) {
            case Edit::insert: candidate.insert(m.position, m.incarnation, m.value); break;
            case Edit::set: candidate.set(m.incarnation, m.value); break;
            case Edit::erase: candidate.erase(m.incarnation); break;
            case Edit::clear: candidate.clear(); break;
            }
            compare(i + 1, point.expected);
        }
    }
    return result;
}

} // namespace cohort_exposure_test
