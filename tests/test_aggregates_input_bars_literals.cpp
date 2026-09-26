// R5 lane PERF-P5611, item P6, then R5 lane H-THIN (audit X15): whether a
// Pine run's chart aggregates input bars.
//
// Until H-THIN the source layer answered this itself from the spec's two
// literals (source::detail::aggregates_input_bars in pine_strategy_host.cpp:
// equal literals without a parse, tf_ratio for every other pair; before
// PERF-P5611 tf_ratio for all). The kernel resolves the same pair when a spec
// is configured (native_calendar::compatibility, the pairing its aggregator
// runs on), so H-THIN deleted the source copy: the host asks
// NativeStrategyHost::native_aggregates_input_bars(), which answers
// native_calendar::pairing_aggregates of that pairing. This row judges the
// kernel's answer against the source layer's former one -- tf_ratio, verbatim
// -- over every pair of a literal population that
// native_calendar::parse_timeframe accepts, as configure_native requires of a
// run's literals: every suffix at the counts where the unit arithmetic
// changes shape, the largest count of each unit whose tf_to_seconds still fits
// an int, and seeded random counts; plus the undetected and spec-less views.
//
// Pairs configure_native refuses are counted and skipped: no run holds them,
// and the former copy's answer for them was never read. On every admitted pair
// the two agree but two classes, each counted here exactly:
//   - byte-identical monthly literals ("M" on "M", "3M" on "3M"): tf_to_seconds
//     is negative, tf_ratio answers -1 and the former copy said "aggregates";
//     the kernel pairs them Passthrough, a chart-timeframe run;
//   - one-bucket pairings the kernel gathers but tf_ratio rounds to 0 or 1
//     ("1D" on "D", "1440" on "D", "999" on "D", "60S" on "1", "9999" on "W"):
//     a multiple of one, or an input under a script bar less than twice it.
// The one Pine reader left (PineStrategyHost::on_native_applied) re-dates a
// row to its chart bar's open on a chart the kernel buckets, because the
// kernel dates a bucket's close point at the bucket's close. A chart-timeframe
// run already dates every modeled point at that open, so the first class moves
// no row; the second class's rows were dated at the bucket's close -- for "1D"
// on "D" the next day's open -- and are now dated at their own bar's open, as a
// chart-timeframe run and TradingView date them (the lane report measures both
// classes on a POOC run).
//
// Source-bound: the host query binds the adapter's consumer (the include below
// is what keeps this TU out of the kernel-only profile).
#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

int failures = 0;
long long checks = 0;

// The source layer's body before PERF-P5611 (P5611 kept its answers).
bool tf_ratio_answer(const NativeStateView& state) {
    if (!state.spec || state.spec->timeframe_undetected) return false;
    const int ratio = tf_ratio(state.spec->input_tf, state.spec->script_tf);
    return ratio > 1 || ratio == -1;
}

long long monthly_passthrough = 0;
long long one_bucket = 0;
long long refused_pairs = 0;

// The pairing configure_native resolves for these literals, and whether it
// admits a run on it (native_run_spec.cpp: Passthrough, a same-unit or fixed
// multiple, a fixed or calendar feed under a calendar script bar).
bool admitted(const native_calendar::TimeframeCompatibility& pairing) {
    using P = native_calendar::TimeframePairing;
    switch (pairing.pairing) {
    case P::Passthrough: case P::SameUnitMultiple: case P::FixedDivisible:
    case P::FixedToCalendar: case P::CalendarToCalendar: return true;
    default: return false;
    }
}

// The kernel's answer for a configured spec with these literals (none for an
// undetected timeframe); nullopt when configure_native refuses the pair, so no
// run can hold it.
std::optional<bool> kernel_answer(const NativeStateView& state) {
    if (!state.spec || state.spec->timeframe_undetected) return false;
    const auto input = native_calendar::parse_timeframe(state.spec->input_tf);
    const auto script = native_calendar::parse_timeframe(state.spec->script_tf);
    if (!input || !script) return std::nullopt;
    const auto pairing = native_calendar::compatibility(*input, *script);
    if (!admitted(pairing)) return std::nullopt;
    return native_calendar::pairing_aggregates(pairing);
}

void check_pair(const std::string& input, const std::string& script, bool undetected) {
    static NativeRunSpec spec;
    spec.input_tf = input;
    spec.script_tf = script;
    spec.timeframe_undetected = undetected;
    NativeStateView state;
    state.spec = &spec;
    ++checks;
    const bool former = tf_ratio_answer(state);
    const std::optional<bool> answer = kernel_answer(state);
    if (!answer) {
        ++refused_pairs;
        return;
    }
    const bool got = *answer;
    const bool monthly_equal = !undetected && input == script && !script.empty()
        && script.back() == 'M';
    if (monthly_equal) {
        // tf_ratio -1, a Passthrough pairing.
        ++monthly_passthrough;
        if (!(former && !got) && ++failures <= 20) {
            std::fprintf(stderr, "FAIL monthly literal \"%s\" expected former 1 kernel 0, got %d %d\n",
                         input.c_str(), former, got);
        }
        return;
    }
    if (got && !former) {
        // A bucket tf_ratio rounds to one bar or none.
        const int ratio = tf_ratio(input, script);
        ++one_bucket;
        if (!(ratio == 0 || ratio == 1) && ++failures <= 20) {
            std::fprintf(stderr, "FAIL input=\"%s\" script=\"%s\" kernel 1 former 0 at tf_ratio %d\n",
                         input.c_str(), script.c_str(), ratio);
        }
        return;
    }
    if (former != got && ++failures <= 20) {
        std::fprintf(stderr, "FAIL input=\"%s\" script=\"%s\" undetected=%d former %d kernel %d\n",
                     input.c_str(), script.c_str(), undetected ? 1 : 0, former, got);
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
    if (kernel_answer(bare).value_or(true)) {
        std::fprintf(stderr, "FAIL a view without a spec aggregates\n");
        ++failures;
    }
    // The host query itself: a host with no spec answers false.
    ++checks;
    {
        struct Bare final : NativeStrategyHost {
            void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
        } host;
        if (host.native_aggregates_input_bars()) {
            std::fprintf(stderr, "FAIL an unconfigured host aggregates\n");
            ++failures;
        }
    }
    if (monthly_passthrough < 3 || one_bucket < 3) {
        std::fprintf(stderr, "FAIL a class was not reached: monthly %lld one-bucket %lld\n",
                     monthly_passthrough, one_bucket);
        ++failures;
    }
    if (failures == 0) {
        std::printf("test_aggregates_input_bars_literals: ok (%lld pairs, %lld of them refused "
                    "by configure_native, the rest against tf_ratio, %zu literals; %lld "
                    "byte-identical monthly pairs tf_ratio called aggregated, %lld one-bucket "
                    "pairs it did not)\n", checks, refused_pairs, literals.size(),
                    monthly_passthrough, one_bucket);
    }
    return failures == 0 ? 0 : 1;
}
