#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Unit vectors use independent arithmetic, finite TV captures where cited,
// and a full-feed comparison with real fixed SMA for numeric alignment.
// All compute sequences below represent ordinary successive bar updates.
namespace {

using pineforge::na;
using pineforge::source::SeriesSma;
using pineforge::source::ta_number;

void expect(double actual, double expected) {
    if (std::isnan(expected)) {
        assert(std::isnan(actual));
    } else {
        assert(!std::isnan(actual));
        assert(actual == expected);
    }
}

struct Row {
    double source;
    double length;
    double expected;
};

void changing_lengths() {
    // Literal suffix means, independently enumerated. In particular the
    // windows of 12, 14 and 16 reach behind earlier shrinks and warm-up.
    const Row rows[] = {
        {6, 3, na<double>()},
        {18, 2, 12},
        {-6, 1, -6},
        {30, 4, 12},
        {12, 2, 21},
        {42, 5, 19.2},                 // 96 / 5
        {-12, 3, 14},
        {54, 7, 19.714285714285715},    // 138 / 7
        // expectation corrected: lengths 0/-2/na -> valid warm-up length 20,
        // because invalid lengths now stop before recording an observation.
        {0, 20, na<double>()},
        {66, 20, na<double>()},
        {24, 20, na<double>()},
        {78, 4, 42},                   // (0 + 66 + 24 + 78) / 4
        {-24, 12, 23.5},               // 282 / 12
        {90, 1, 90},
        {36, 14, 29.142857142857142},   // 408 / 14
        {102, 16, 32.25},              // 516 / 16
    };
    SeriesSma actual;
    std::size_t selected = 0;
    for (const auto& row : rows) {
        expect(actual.compute(row.source, row.length), row.expected);
        ++selected;
    }
    assert(selected > 0 && selected == 16);
}

void invalid_length_transport_and_recovery() {
    using pineforge::RunFailureCode;
    struct Invalid {
        double length;
        RunFailureCode code;
        const char* text;
        const char* argument;
    };
    const Invalid rows[] = {
        {0, RunFailureCode::pine_invalid_argument, "It must be > 0.", "must_be_positive"},
        {-3, RunFailureCode::pine_invalid_argument, "(-3)", "must_be_positive"},
        {0.9, RunFailureCode::pine_invalid_argument, "(0)", "must_be_positive"},
        {ta_number(na<int>()), RunFailureCode::pine_invalid_argument,
         "It must not be na", "must_not_be_na"},
        {ta_number(na<std::int64_t>()), RunFailureCode::pine_invalid_argument,
         "It must not be na", "must_not_be_na"},
        {std::numeric_limits<double>::infinity(), RunFailureCode::pine_invalid_argument,
         "It must not be na", "must_not_be_na"},
        {-std::numeric_limits<double>::infinity(), RunFailureCode::pine_invalid_argument,
         "It must not be na", "must_not_be_na"},
        {std::numeric_limits<double>::max(), RunFailureCode::pine_runtime_limit,
         "The historical buffer's limit is 15000 bars.", "max_bars_back"},
        {-std::numeric_limits<double>::max(), RunFailureCode::pine_invalid_argument,
         "It must be > 0.", "must_be_positive"},
        {15002, RunFailureCode::pine_runtime_limit, "(15001 bars back)", "max_bars_back"},
    };
    // expectation corrected: silent NA/retention -> coded family errors,
    // because CORE requires validation before any invalid logical update.
    for (bool again : {false, true}) {
        for (double source : {999.0, na<double>()}) {
            SeriesSma actual;
            expect(actual.compute(4, 1), 4);
            expect(actual.compute(12, 2), 8);
            expect(actual.compute(20, 3), 12);
            std::size_t checked = 0;
            for (const auto& row : rows) {
                bool caught = false;
                try {
                    if (again) actual.recompute(source, row.length);
                    else actual.compute(source, row.length);
                } catch (const pineforge::coded<std::runtime_error>& error) {
                    caught = true;
                    assert(error.run_failure_code() == row.code);
                    assert(std::string(error.what()).find(row.text) != std::string::npos);
                    const auto& args = error.run_failure().args;
                    assert(args && args->find(row.argument) != std::string::npos);
                    if (row.code == RunFailureCode::pine_invalid_argument) {
                        assert(std::string(error.what()).find("'sma' function") != std::string::npos);
                        assert(std::string(error.what()).find("'length' argument") != std::string::npos);
                    }
                }
                assert(caught);
                // Neither recording nor the pre-update checkpoint changed.
                expect(actual.recompute(20, 3), 12); // [4, 12, 20]
                ++checked;
            }
            assert(checked == 10);
            expect(actual.recompute(6, 2.9), 9); // [12, 6], truncation retained
            expect(actual.compute(0, 1), 0);    // numeric zero is not NA
        }
    }
}

void replace_current_update() {
    SeriesSma actual;
    expect(actual.recompute(na<double>(), 1), na<double>()); // first logical NA
    expect(actual.recompute(10, 3), na<double>()); // first update, not 3 bars
    expect(actual.recompute(20, 1), 20);
    expect(actual.compute(8, 2), 14);
    // expectation corrected: invalid N=0 -> valid warm-up N=4.
    expect(actual.compute(50, 4), na<double>());
    expect(actual.recompute(14, 3), 14); // [20, 8, 14]
    expect(actual.recompute(-4, 2), 2);
    expect(actual.recompute(-4, 5), na<double>()); // still only 3 updates
    // expectation corrected: invalid N=0 -> valid warm-up N=5.
    expect(actual.recompute(100, 5), na<double>());
    expect(actual.recompute(2, 3), 10);  // [20, 8, 2]
    expect(actual.compute(10, 4), 10);
    expect(actual.compute(30, 5), 14);
    // expectation corrected: invalid NA length -> valid warm-up N=6.
    expect(actual.recompute(40, 6), na<double>());
    expect(actual.compute(0, 6), 13.333333333333334); // 80 / 6

    // Repeated replacements always start from the same six-sample prefix.
    expect(actual.compute(na<double>(), 2), 20); // [40, 0]
    expect(actual.recompute(12, 2), 6);          // [0, 12], NA -> value
    expect(actual.recompute(na<double>(), 3), 16.666666666666668); // [10, 40, 0]
    expect(actual.recompute(18, 3), 19.333333333333332); // [40, 0, 18]
    expect(actual.recompute(na<double>(), 1), 0); // value -> NA
    expect(actual.compute(na<double>(), 4), 13); // [2, 10, 40, 0]
    expect(actual.recompute(24, 4), 18.5);       // [10, 40, 0, 24]
    expect(actual.recompute(24, 8), na<double>()); // only seven retained
    // Equal source and length still represent distinct compute calls.
    expect(actual.compute(24, 3), 16);           // [0, 24, 24]
    expect(actual.compute(24, 3), 24);
}

void checkpoint_copy_and_restore() {
    SeriesSma actual;
    expect(actual.compute(4, 1), 4);
    expect(actual.compute(12, 2), 8);
    expect(actual.compute(20, 1), 20);
    auto checkpoint = actual;
    auto other = checkpoint;

    expect(actual.recompute(44, 3), 20);
    expect(other.recompute(-4, 3), 4);
    expect(checkpoint.recompute(20, 3), 12);
    expect(actual.compute(100, 4), 40);
    expect(other.compute(4, 4), 4);

    actual = checkpoint;               // restore both history and update count
    expect(actual.compute(28, 4), 16);
    expect(checkpoint.compute(36, 4), 18);
    expect(other.compute(8, 5), 4.8);     // original independent branch
    expect(actual.recompute(28, 5), na<double>()); // restored count was 3

    expect(actual.compute(na<double>(), 3), 20); // [12, 20, 28]
    auto na_checkpoint = actual;                // count and prefix are both 4
    auto na_branch = na_checkpoint;
    expect(actual.recompute(44, 3), 30.666666666666668); // [20, 28, 44]
    expect(actual.compute(52, 6), 26.666666666666668); // sum 160, prefix now 5
    expect(na_branch.recompute(na<double>(), 2), 24); // original [20, 28]
    actual = na_checkpoint;                     // restore pre-NA prefix too
    expect(actual.recompute(36, 5), 20);         // [4, 12, 20, 28, 36]
    expect(na_checkpoint.recompute(na<double>(), 4), 16);
    auto value_checkpoint = actual;
    expect(actual.recompute(na<double>(), 4), 16);
    expect(na_branch.recompute(52, 5), 23.2);    // independent [4, 12, 20, 28, 52]
    actual = value_checkpoint;
    expect(actual.recompute(44, 5), 21.6);       // replace 36, do not append
    expect(value_checkpoint.recompute(36, 5), 20);
}

void source_na_membership() {
    // expectation corrected: NA-slot propagation -> last-N non-NA membership,
    // because the finite clock_na_true TV packet skips source NA (see below).
    // These small integer suffix means are independently enumerated.
    const Row rows[] = {
        {na<double>(), 1, na<double>()},
        {6, 2, na<double>()},
        {18, 1, 18},
        {na<double>(), 2, 12}, // expectation corrected: na -> 12, (6 + 18) / 2
        {30, 3, 18},          // expectation corrected: na -> 18, (6 + 18 + 30) / 3
        {42, 2, 36},
        {-6, 3, 22},
        {0, 1, 0},
    };
    SeriesSma actual;
    std::size_t selected = 0;
    for (const auto& row : rows) {
        expect(actual.compute(row.source, row.length), row.expected);
        ++selected;
    }
    assert(selected > 0 && selected == 8);
    // expectation corrected: na -> -6, because replacing 0 with NA removes it.
    expect(actual.recompute(na<double>(), 1), -6);
    expect(actual.recompute(12, 3), 16); // [42, -6, 12], same pre-update prefix
    // expectation corrected: invalid N=0 -> valid warm-up N=7.
    expect(actual.compute(na<double>(), 7), na<double>());
    // The warm-up NA update adds no sample.
    expect(actual.compute(18, 2), 15); // [12, 18]
    expect(actual.recompute(30, 1), 30);
    expect(actual.compute(6, 2), 18);
}

void tv_clock_na_prefix() {
    // Exact field projection from the finite TradingView carrier packet.
    // Values projected from a TradingView capture of this script.
    // Case clock_na_true; each row is values.source_at_call.floatHex (null
    // becomes NA), values.dense_length_at_call, values.dense_value.floatHex
    // (null becomes NA). Comments identify the original bar and time.
    // Full prefix 0..16: warm-up is retained, including the NA at bar 4.
    // Bar 15 is the first dense NA-source mismatch in the prior native replay;
    // bar 16 grows back to N=8. Expectations come from TV, not native output.
    const Row rows[] = {
        {0x1.4bd675c28f5c3p+15, 8, na<double>()}, // bar 0, 1704067200000
        {0x1.4ceb1eb851eb8p+15, 8, na<double>()}, // bar 1, 1704070800000
        {0x1.4caa333333333p+15, 8, na<double>()}, // bar 2, 1704074400000
        {0x1.4ab4fae147ae1p+15, 8, na<double>()}, // bar 3, 1704078000000
        {na<double>(), 8, na<double>()},        // bar 4, 1704081600000
        {0x1.49f4051eb851fp+15, 8, na<double>()}, // bar 5, 1704085200000
        {0x1.4b396147ae148p+15, 8, na<double>()}, // bar 6, 1704088800000
        {0x1.4bf8eb851eb85p+15, 8, na<double>()}, // bar 7, 1704092400000
        {0x1.4c6bfae147ae1p+15, 2, 0x1.4c32733333333p+15}, // bar 8, 1704096000000
        {0x1.4d336147ae148p+15, 2, 0x1.4ccfae147ae14p+15}, // bar 9, 1704099600000
        {0x1.4d86333333333p+15, 2, 0x1.4d5cca3d70a3ep+15}, // bar 10, 1704103200000
        {0x1.4d84666666666p+15, 2, 0x1.4d854ccccccccp+15}, // bar 11, 1704106800000
        {0x1.4d30c28f5c28fp+15, 2, 0x1.4d5a947ae147ap+15}, // bar 12, 1704110400000
        {0x1.4db7147ae147bp+15, 2, 0x1.4d73eb851eb85p+15}, // bar 13, 1704114000000
        {0x1.4ce87ae147ae1p+15, 2, 0x1.4d4fc7ae147aep+15}, // bar 14, 1704117600000
        {na<double>(), 2, 0x1.4d4fc7ae147aep+15},          // bar 15, 1704121200000
        {0x1.4dca4cccccccdp+15, 8, 0x1.4d48928f5c28fp+15}, // bar 16, 1704124800000
    };
    SeriesSma actual;
    std::size_t selected = 0;
    for (const auto& row : rows) {
        expect(actual.compute(row.source, row.length), row.expected);
        ++selected;
    }
    assert(selected > 0 && selected == 17);
}

void precision_and_order() {
    // Exact integer sums: [2^53, 1, -2^53] sums to 1, and adding 5,-3
    // sums to 3. Independent rational means distinguish lost low addends.
    // They specify the chosen compensated path, not measured TV bit order.
    const Row rows[] = {
        {0x1p53, 1, 0x1p53},
        {1, 2, 0x1p52},
        {-0x1p53, 3, 0.33333333333333331},
        {5, 2, -4503599627370493.5},
        {-3, 5, 0.6},
    };
    SeriesSma actual;
    std::size_t selected = 0;
    for (const auto& row : rows) {
        expect(actual.compute(row.source, row.length), row.expected);
        ++selected;
    }
    assert(selected > 0 && selected == 5);
    expect(actual.recompute(3, 5), 1.8); // exact sum 9
    expect(actual.recompute(-5, 2), 0);
    expect(actual.compute(9, 6), 1.6666666666666667); // exact sum 10
}

void retained_history_and_storage() {
    // Independent closed form: the last C of the first m odd integers have
    // mean 2*m-C. More than two full rings must preserve that suffix.
    constexpr std::size_t capacity =
        pineforge::source::SeriesWindowExtreme::kMaxBarsBack + 1;
    constexpr std::size_t updates = 2 * capacity + 1;
    const std::array<std::size_t, 3> capacities{
        pineforge::WindowCacheCapacity::kKeepResident, 0, 1};
    std::size_t selected = 0;
    for (const auto pages : capacities) {
        SeriesSma actual(pineforge::WindowCacheCapacity{pages});
        std::uint64_t resident_at_full = 0;
        std::size_t checked = 0;
        for (std::size_t i = 0; i != updates; ++i) {
            const double source = static_cast<double>(2 * i + 1);
            expect(actual.compute(source, 1), source);
            const auto m = i + 1;
            if (m == capacity || m == capacity + 1 || m == updates) {
                const double full_mean = static_cast<double>(2 * m - capacity);
                expect(actual.recompute(source, capacity), full_mean);
                if (m > capacity) {
                    auto checkpoint = actual;
                    const double previous_mean = static_cast<double>(2 * (m - 1) - capacity);
                    expect(actual.recompute(na<double>(), capacity), previous_mean);
                    auto na_checkpoint = actual; // includes the restored evicted oldest
                    expect(actual.recompute(na<double>(), 2), static_cast<double>(2 * m - 4));
                    // Replacing the latest odd value with source+2 adds 2/C.
                    expect(actual.recompute(source + 2, capacity),
                           (capacity * full_mean + 2) / capacity);
                    expect(checkpoint.recompute(source, capacity), full_mean);
                    actual = na_checkpoint;
                    expect(actual.recompute(na<double>(), capacity), previous_mean);
                    expect(actual.recompute(source, capacity), full_mean);
                    expect(checkpoint.recompute(na<double>(), capacity), previous_mean);
                    // A fresh logical NA at a full ring must also retain the
                    // oldest value for a later finite replacement and rollback.
                    expect(actual.compute(na<double>(), capacity), full_mean);
                    auto logical_na = actual;
                    expect(actual.recompute(source + 2, capacity), full_mean + 2);
                    expect(actual.recompute(na<double>(), capacity), full_mean);
                    actual = logical_na;
                    expect(actual.recompute(source + 2, capacity), full_mean + 2);
                    expect(actual.recompute(na<double>(), capacity), full_mean);
                }
                pineforge::OwnedStorage observation;
                actual.observe_owned_storage(observation);
                const auto usage = observation.usage();
                assert(usage.complete());
                // expectation corrected: O(bars) lower bound -> one fixed ring,
                // because retained history is capped at kMaxBarsBack+1.
                assert(usage.logical_payload_bytes == capacity * sizeof(double));
                if (m == capacity) resident_at_full = usage.resident_bytes();
                assert(usage.resident_bytes() <= resident_at_full);
                if (pages != pineforge::WindowCacheCapacity::kKeepResident)
                    assert(usage.backing_bytes == capacity * sizeof(double));
            }
            ++checked;
        }
        assert(checked == updates && checked > 2 * capacity);
        ++selected;
    }
    assert(selected > 0 && selected == 3);
}

void constant_length_corpus_comparison() {
    // Existing checked-in BTCUSDT 60m corpus-feed copy, FULL 1441-row series:
    // tests/fixtures/qty_step_mincontract/btc60/feed/bars.csv (header + 1441).
    // This fixed-N=20 exercise of the VARIABLE adapter is not coverage of a
    // generated constant-length script or proof of its codegen routing.
    // The 1.13.0 variable-length path temporarily uses Neumaier arithmetic;
    // measure its difference from real fixed SMA across every numeric row.
    // No alignment/parity verdict or expected difference count/max is pinned.
    const std::string file = __FILE__;
    const auto slash = file.find_last_of("/\\");
    assert(slash != std::string::npos);
    std::ifstream input(file.substr(0, slash) +
        "/fixtures/qty_step_mincontract/btc60/feed/bars.csv");
    assert(input.is_open());
    std::string line;
    assert(static_cast<bool>(std::getline(input, line)));
    assert(line == "timestamp,open,high,low,close,volume");
    struct Source {
        std::int64_t time;
        double close;
    };
    std::vector<Source> sources;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string cell;
        assert(static_cast<bool>(std::getline(row, cell, ',')));
        std::size_t used = 0;
        const auto time = std::stoll(cell, &used);
        assert(used == cell.size());
        for (int field = 1; field != 5; ++field)
            assert(static_cast<bool>(std::getline(row, cell, ',')));
        const double close = std::stod(cell, &used);
        assert(used == cell.size() && std::isfinite(close));
        sources.push_back({time, close});
    }
    assert(input.eof() && sources.size() == 1441);

    static_assert(sizeof(double) == sizeof(std::uint64_t));
    static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53 &&
                  std::numeric_limits<double>::max_exponent == 1024);
    const auto ordered_key = [](double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof value);
        constexpr auto sign = std::uint64_t{1} << 63;
        // Reverse negative encodings, then place nonnegative encodings above
        // them. The unsigned distance includes adjacent -0/+0 encodings.
        return (bits & sign) ? ~bits : (bits ^ sign);
    };
    struct Difference {
        std::size_t row = 0; // zero-based data row, excluding the CSV header
        std::int64_t time = 0;
        double source = 0;
        double variable = 0;
        double fixed = 0;
        std::uint64_t ulp = 0;
    };
    Difference first, maximum;
    SeriesSma actual;
    pineforge::ta::SMA fixed(20);
    std::size_t numeric = 0, warmup_na = 0, differences = 0;
    for (std::size_t i = 0; i != sources.size(); ++i) {
        const auto& source = sources[i];
        const double value = actual.compute(source.close, 20);
        const double expected = fixed.compute(source.close);
        if (std::isnan(expected)) {
            assert(std::isnan(value));
            ++warmup_na;
        } else {
            assert(std::isfinite(value) && std::isfinite(expected));
            const auto value_key = ordered_key(value);
            const auto fixed_key = ordered_key(expected);
            const auto ulp = value_key >= fixed_key
                ? value_key - fixed_key : fixed_key - value_key;
            if (ulp != 0) {
                const Difference difference{i, source.time, source.close, value, expected, ulp};
                if (differences == 0) first = difference;
                if (ulp > maximum.ulp) maximum = difference; // earliest maximum on ties
                ++differences;
            }
            ++numeric;
        }
    }
    assert(numeric == 1441 - 20 + 1 && numeric > 0);
    assert(warmup_na == 19 && numeric + warmup_na == sources.size());

    const auto require_output = [](int result) {
        if (result < 0)
            throw std::runtime_error("ARITHMETIC_DIFFERENCE stdout write failed");
    };
    require_output(std::printf(
        "ARITHMETIC_DIFFERENCE scope=variable_adapter_fixed_N_exercise "
        "generated_constant_script_coverage=false length=20 rows=%zu numeric=%zu "
        "warmup_na=%zu differences=%zu max_ulp=%" PRIu64 " row_ids=zero_based\n",
        sources.size(), numeric, warmup_na, differences, maximum.ulp));
    const auto emit_difference = [&](const char* kind, const Difference& difference) {
        require_output(std::printf(
            "ARITHMETIC_DIFFERENCE kind=%s row=%zu csv_line=%zu time=%" PRId64
            " source=%a variable=%a fixed=%a ulp=%" PRIu64 "\n",
            kind, difference.row, difference.row + 2, difference.time,
            difference.source, difference.variable, difference.fixed, difference.ulp));
    };
    if (differences != 0) {
        emit_difference("first", first);
        emit_difference("maximum", maximum);
    } else {
        require_output(std::printf(
            "ARITHMETIC_DIFFERENCE kind=first row=none\n"
            "ARITHMETIC_DIFFERENCE kind=maximum row=none\n"));
    }
    if (std::fflush(stdout) != 0 || std::ferror(stdout))
        throw std::runtime_error("ARITHMETIC_DIFFERENCE stdout flush failed");
}

}  // namespace

int main() {
    changing_lengths();
    invalid_length_transport_and_recovery();
    replace_current_update();
    checkpoint_copy_and_restore();
    source_na_membership();
    tv_clock_na_prefix();
    precision_and_order();
    retained_history_and_storage();
    constant_length_corpus_comparison();
}
