#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr std::size_t kMaxBarsBack = 15000;
constexpr std::size_t kBufferCapacity = kMaxBarsBack + 1;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

void require_same(double actual, double expected, const char* message) {
    if (is_na(expected)) {
        require(is_na(actual), message);
    } else {
        require(!is_na(actual) && bits(actual) == bits(expected), message);
    }
}

double model_percentile(const std::vector<double>& appended, std::size_t length,
                        double percentage) {
    if (appended.size() < length) return na<double>();

    std::vector<double> ranked;
    ranked.reserve(length);
    const std::size_t first = appended.size() - length;
    for (std::size_t i = first; i != appended.size(); ++i) {
        const double value = appended[i];
        const auto at = std::find_if(ranked.begin(), ranked.end(),
                                     [&](double held) { return held > value; });
        ranked.insert(at, value);
    }

    const double rank = std::ceil(percentage / 100.0 * ranked.size());
    if (std::isnan(rank)) return na<double>();
    const double bounded = std::min(std::max(rank, 1.0),
                                    static_cast<double>(ranked.size()));
    return ranked[static_cast<std::size_t>(bounded) - 1];
}

double finite_source(int bar) {
    return static_cast<double>((bar * 37) % 211 - 105) / 8.0
        + static_cast<double>(bar % 7) / 100.0;
}

double source_with_na(int bar) {
    if (bar % 19 == 0) return na<double>();
    return finite_source(bar);
}

void loop_lengths() {
    source::SeriesPercentileNearestRank actual;
    std::vector<double> expected_history;
    int first_value_bar = -1;

    for (int bar = 0; bar != 1441; ++bar) {
        const double src = finite_source(bar);
        double expected_sum = 0.0;
        double actual_sum = 0.0;
        bool complete = true;
        bool actual_complete = true;
        for (std::size_t length : {2u, 3u, 4u}) {
            expected_history.push_back(src);
            const double expected = model_percentile(expected_history, length, 50.0);
            const double value = actual.compute(src, static_cast<double>(length), 50.0);
            require_same(value, expected, "series-length loop matches the append model");
            if (is_na(expected)) {
                complete = false;
            } else {
                expected_sum += expected;
            }
            if (is_na(value)) {
                actual_complete = false;
            } else {
                actual_sum += value;
            }
        }
        const double expected = complete ? expected_sum : na<double>();
        const double value = actual_complete ? actual_sum : na<double>();
        if (!is_na(expected) && first_value_bar < 0) first_value_bar = bar;
        require_same(value, expected, "loop accumulation matches the append model");
        require(first_value_bar < 0 || bar >= first_value_bar,
                "loop output does not become valid before its modeled warm-up");
    }
    require(first_value_bar == 1, "loop first value is on bar 1");
}

void constant_lengths_and_percentages() {
    for (double length : {1.0, 2.0, 5.0, 11.0}) {
        for (double percentage : {0.0, 33.3, 50.0, 100.0}) {
            {
                source::SeriesPercentileNearestRank actual;
                ta::PercentileNearestRank constant(static_cast<int>(length));
                std::vector<double> expected_history;
                for (int bar = 0; bar != 113; ++bar) {
                    const double src = finite_source(bar);
                    expected_history.push_back(src);
                    const double expected = model_percentile(
                        expected_history, static_cast<std::size_t>(length), percentage);
                    const double actual_value = actual.compute(src, length, percentage);
                    const double constant_value = constant.compute(src, percentage);
                    require_same(actual_value, expected,
                                 "constant length matches the finite replay model");
                    require_same(constant_value, expected,
                                 "constant-length TA matches the finite replay model");
                }
            }
            {
                source::SeriesPercentileNearestRank actual;
                ta::PercentileNearestRank constant(static_cast<int>(length));
                for (int bar = 0; bar != 113; ++bar) {
                    const double src = source_with_na(bar);
                    require_same(actual.compute(src, length, percentage),
                                 constant.compute(src, percentage),
                                 "constant length with na matches constant-length TA");
                }
            }
        }
    }
}

void length_change_replays_na_history() {
    source::SeriesPercentileNearestRank actual;
    ta::PercentileNearestRank constant(3);
    std::vector<double> expected_history;

    for (int bar = 0; bar != 4; ++bar) {
        const double src = source_with_na(bar);
        expected_history.push_back(src);
        require_same(actual.compute(src, 3.0, 50.0),
                     constant.compute(src, 50.0),
                     "constant segment with na matches a fresh constant window");
    }

    for (double length : {5.0, 5.0, 3.0, 3.0}) {
        const double src = source_with_na(static_cast<int>(expected_history.size()));
        expected_history.push_back(src);
        const double expected = model_percentile(
            expected_history, static_cast<std::size_t>(length), 50.0);
        require_same(actual.compute(src, length, 50.0), expected,
                     "length change replays the retained na history");
    }
}

void changing_lengths() {
    source::SeriesPercentileNearestRank actual;
    std::vector<double> expected_history;
    const std::vector<double> lengths{2.0, 5.0, 3.0, 8.0, 4.0, 7.0, 2.0};
    for (int bar = 0; bar != 97; ++bar) {
        const double src = source_with_na(bar + 1);
        const double length = lengths[static_cast<std::size_t>(bar) % lengths.size()];
        expected_history.push_back(src);
        const double expected = model_percentile(
            expected_history, static_cast<std::size_t>(length), 63.0);
        require_same(actual.compute(src, length, 63.0), expected,
                     "changing length reads the last appended values");
    }
}

template<class Call>
void expect_failure(Call&& call, RunFailureCode code, const char* message,
                    const char* failure_message) {
    bool failed = false;
    try {
        call();
    } catch (const std::runtime_error& error) {
        const auto* info = dynamic_cast<const RunFailureInfo*>(&error);
        require(info != nullptr && info->run_failure_code() == code,
                "series percentile failure has the expected typed code");
        require(std::string(error.what()) == failure_message, message);
        failed = true;
    }
    require(failed, message);
}

void errors() {
    source::SeriesPercentileNearestRank actual;
    expect_failure(
        [&] { (void)actual.compute(1.0, na<double>(), 50.0); },
        RunFailureCode::pine_invalid_argument,
        "na length is refused",
        "Invalid value of the 'length' argument (0) in the 'percentile_nearest_rank' function. It must be > 0.");
    expect_failure(
        [&] { (void)actual.compute(1.0, 0.0, 50.0); },
        RunFailureCode::pine_invalid_argument,
        "zero length is refused",
        "Invalid value of the 'length' argument (0) in the 'percentile_nearest_rank' function. It must be > 0.");
    expect_failure(
        [&] { (void)actual.compute(1.0, -3.0, 50.0); },
        RunFailureCode::pine_invalid_argument,
        "negative length is refused",
        "Invalid value of the 'length' argument (-3) in the 'percentile_nearest_rank' function. It must be > 0.");

    require(is_na(actual.compute(1.0, 15001.0, 50.0)),
            "a 15001-value window does not exceed the historical limit");
    expect_failure(
        [&] { (void)actual.compute(1.0, 15002.0, 50.0); },
        RunFailureCode::pine_runtime_limit,
        "a 15002-value window is refused",
        "The script attempts to reference historical data that is too far from the current bar (15001 bars back). The historical buffer's limit is 15000 bars.");
}

void recompute_replaces_last_append() {
    // `actual` appends 19 values and then re-executes the last call with a
    // replacement source; `fresh` is fed the same history in which the 19th
    // value is the replacement from the start. The re-executed call must
    // answer what the fresh site answers for that history.
    source::SeriesPercentileNearestRank actual;
    source::SeriesPercentileNearestRank fresh;
    const double replacement = finite_source(91);
    for (int bar = 0; bar != 19; ++bar) {
        const double src = finite_source(bar + 4);
        const double length = static_cast<double>(2 + bar % 5);
        const double fresh_src = bar == 18 ? replacement : src;
        const double fresh_value = fresh.compute(fresh_src, length, 50.0);
        if (bar != 18) {
            require_same(actual.compute(src, length, 50.0), fresh_value,
                         "recompute setup has matching history");
        } else {
            (void)actual.compute(src, length, 50.0);
            require_same(actual.recompute(replacement, length, 50.0), fresh_value,
                         "recompute replaces the previous append");
        }
    }

    require_same(actual.compute(finite_source(92), 2.0, 0.0),
                 fresh.compute(finite_source(92), 2.0, 0.0),
                 "the next append follows the recomputed history");
}

OwnedStorageUsage observed(const source::SeriesPercentileNearestRank& value) {
    OwnedStorage out;
    value.observe_owned_storage(out);
    return out.usage();
}

void memory_is_bounded() {
    source::SeriesPercentileNearestRank actual;
    for (int i = 0; i != static_cast<int>(kBufferCapacity); ++i)
        (void)actual.compute(finite_source(i), 1.0, 50.0);
    const OwnedStorageUsage at_capacity = observed(actual);

    for (int i = static_cast<int>(kBufferCapacity); i != 100000; ++i)
        (void)actual.compute(finite_source(i), 1.0, 50.0);
    const OwnedStorageUsage after_more_appends = observed(actual);

    require(at_capacity.complete() && after_more_appends.complete(),
            "series percentile storage observation is complete");
    require(at_capacity.resident_bytes() == after_more_appends.resident_bytes()
                && at_capacity.logical_payload_bytes
                    == after_more_appends.logical_payload_bytes
                && at_capacity.resident_payload_bytes
                    == after_more_appends.resident_payload_bytes
                && at_capacity.resident_metadata_bytes
                    == after_more_appends.resident_metadata_bytes
                && at_capacity.backing_bytes == after_more_appends.backing_bytes,
            "series percentile buffer stays at kMaxBarsBack plus one values");
    require(after_more_appends.resident_payload_bytes
                >= kBufferCapacity * sizeof(double),
            "series percentile storage retains the bounded append ring");
}

}  // namespace

int main() {
    loop_lengths();
    constant_lengths_and_percentages();
    length_change_replays_na_history();
    changing_lengths();
    errors();
    recompute_replaces_last_append();
    memory_is_bounded();
    return 0;
}
