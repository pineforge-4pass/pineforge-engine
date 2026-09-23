// R5 lane PERF-P5611, item P6. Whether a Pine run's chart aggregates input
// bars is a per-run invariant of its spec, and the host and the scheduler ask
// it in their per-bar callbacks (PERF0-P counted 1.04-1.40 asks a bar and
// 18 ns/bar of timeframe parsing behind them).
//
// Until this lane both callbacks' copies answered it by tf_ratio of the spec's
// two literals: two tf_to_seconds parses per ask. The one definition left
// (source::detail::aggregates_input_bars, src/source/pine_strategy_host.cpp)
// answers equal literals -- every chart that does not aggregate -- without a
// parse, and keeps tf_ratio for every other pair. The answer may not move for
// any spec a run can hold, so this row judges it against the pre-lane body
// verbatim over every pair of a literal population that
// native_calendar::parse_timeframe accepts, as configure_native requires of a
// run's literals: every suffix at the counts where the unit arithmetic
// changes shape, the largest count of each unit whose tf_to_seconds still fits
// an int, and seeded random counts; plus the undetected and spec-less views.
//
// Source-bound: the definition lives in the source layer (the include below is
// what keeps this TU out of the kernel-only profile).
#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pineforge::source::detail {
// src/source/pine_strategy_host.cpp. External linkage only so this row and
// PineScheduler reach the one definition; not part of the installed API.
bool aggregates_input_bars(const NativeStateView& state);
}  // namespace pineforge::source::detail

namespace {
using namespace pineforge;

int failures = 0;
long long checks = 0;

// Both copies' body before this lane.
bool tf_ratio_answer(const NativeStateView& state) {
    if (!state.spec || state.spec->timeframe_undetected) return false;
    const int ratio = tf_ratio(state.spec->input_tf, state.spec->script_tf);
    return ratio > 1 || ratio == -1;
}

void check_pair(const std::string& input, const std::string& script, bool undetected) {
    static NativeRunSpec spec;
    spec.input_tf = input;
    spec.script_tf = script;
    spec.timeframe_undetected = undetected;
    NativeStateView state;
    state.spec = &spec;
    ++checks;
    const bool expected = tf_ratio_answer(state);
    const bool got = source::detail::aggregates_input_bars(state);
    if (expected != got && ++failures <= 20) {
        std::fprintf(stderr, "FAIL input=\"%s\" script=\"%s\" undetected=%d expected %d got %d\n",
                     input.c_str(), script.c_str(), undetected ? 1 : 0, expected, got);
    }
}

// Literals configure_native admits, whose tf_to_seconds fits an int (a larger
// count is not a timeframe any feed has, and its int product is undefined).
std::vector<std::string> literal_population() {
    std::vector<std::string> out = {"D", "W", "M"};
    const long long counts[] = {1, 2, 3, 4, 5, 9, 10, 12, 15, 30, 45, 59, 60, 90, 99, 100,
                                120, 180, 240, 360, 480, 720, 999, 1000, 1440, 3550,
                                9999, 10000, 24855, 99999, 35791394, 2147483647};
    const struct { const char* suffix; long long max_count; } units[] = {
        {"", 35791394}, {"S", 2147483647}, {"D", 24855}, {"W", 3550}, {"M", 2147483647},
    };
    for (const auto& unit : units) {
        for (long long count : counts) {
            if (count > unit.max_count) continue;
            out.push_back(std::to_string(count) + unit.suffix);
        }
    }
    std::uint64_t mix = 0x5deece66dull;
    for (int i = 0; i < 400; ++i) {
        mix ^= mix << 13; mix ^= mix >> 7; mix ^= mix << 17;
        const auto& unit = units[mix % 5];
        const long long cap = unit.max_count < 100000 ? unit.max_count : 100000;
        out.push_back(std::to_string(1 + static_cast<long long>((mix >> 8) % cap)) + unit.suffix);
    }
    std::vector<std::string> admitted;
    for (const auto& literal : out) {
        if (native_calendar::parse_timeframe(literal)) admitted.push_back(literal);
    }
    return admitted;
}

}  // namespace

int main() {
    const auto literals = literal_population();
    if (literals.size() < 400) {
        std::fprintf(stderr, "FAIL population too small: %zu\n", literals.size());
        ++failures;
    }
    for (const auto& input : literals) {
        for (const auto& script : literals) check_pair(input, script, false);
        check_pair(input, input, false);
    }
    // An undetected run holds empty literals; a view without a spec asks nothing.
    check_pair("", "", true);
    check_pair("", "", false);
    NativeStateView bare;
    ++checks;
    if (source::detail::aggregates_input_bars(bare)) {
        std::fprintf(stderr, "FAIL a view without a spec aggregates\n");
        ++failures;
    }
    if (failures == 0) {
        std::printf("test_aggregates_input_bars_literals: ok (%lld pairs against tf_ratio, "
                    "%zu literals)\n", checks, literals.size());
    }
    return failures == 0 ? 0 : 1;
}
