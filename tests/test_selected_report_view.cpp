// Pure report view: the selected window's equity curve (an owned array) and the
// observation count the equity statistics take, under the documented
// selected-window wire contract (v1.3).
//
// Every expectation is written from that contract and from the per-field contracts
// of pf_equity_stats_t (include/pineforge/pineforge.h), by hand arithmetic
// shown where it is used -- none is read back from the code under test. The
// one exception is the ordinary overload, which is held bit for bit to
// tests/equity_stats_reference.hpp, the walk as it stood before the
// observation-count overload was added.
//
// Rows:
//   anchor_alone                         M = 0 is the anchor alone; null observations are valid
//   observations_follow_the_anchor       one and two observations, then many: position i + 1
//   first_bar_at_the_start_or_after_a_gap  an observation stamped T beside the anchor, or later
//   copied_byte_for_byte                 -0.0, NaN payloads, infinities, denormals, odd equity
//   owned_array_moves_and_releases       move, release into a ReportC, free_report: one address
//   refusals                             negative M, null with M > 0, M beyond any array
//   two_observations_by_hand             100 -> 110 -> 104.5, M = 2, every field by hand
//   span_is_anchored_at_the_window_start the same returns whether the first bar is at T or later
//   empty_and_one_observation            M = 0 is the empty convention; M = 1 has one return
//   undefined_domains                    non-positive equity, zero deviation, empty span
//   exposure_is_over_observations        bars in market / M, never / (M + 1)
//   ordinary_overload_unchanged          old entry and observation_count = n against the reference walk
// Source-free: no engine run and no strategy, so it also runs in the kernel-only profile.
#include <pineforge/engine.hpp>
#include <pineforge/metrics.hpp>
#include <pineforge/run_failure.hpp>

#include "../src/selected_report_view.hpp"
#include "equity_stats_reference.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace metrics = pineforge::metrics;
namespace report = pineforge::report;
namespace ref = equity_stats_reference;

int failures = 0;
long long checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

using Points = std::vector<pf_equity_point_t>;
using Curve = report::SelectedEquityCurve;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::int64_t kMinTime = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMaxTime = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kT = 1'700'000'000'000LL;   // 2023-11-14 22:13:20 UTC
constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;
// 365.25 days: the year length the statistics divide a calendar span by.
constexpr std::int64_t kYear = 31'557'600'000LL;
constexpr std::int64_t kHalfYear = kYear / 2;      // 182.625 days

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

bool within(double got, double want, double tolerance) {
    return std::isfinite(got) && std::fabs(got - want) <= tolerance;
}

bool same_bytes(const pf_equity_point_t* a, const pf_equity_point_t* b, std::size_t count) {
    return count == 0 || std::memcmp(a, b, count * sizeof(pf_equity_point_t)) == 0;
}

// Point 0 is the window anchor: T, the initial capital as given, no open profit.
bool is_anchor(const pf_equity_point_t& point, std::int64_t t, double capital) {
    return point.time_ms == t && same_bits(point.equity, capital)
        && same_bits(point.open_profit, 0.0);
}

bool has_anchor(const Curve& curve, std::int64_t t, double capital) {
    return curve.points != nullptr && curve.count >= 1 && is_anchor(curve.points[0], t, capital);
}

void set_field_bits(double* field, std::uint64_t bits) { std::memcpy(field, &bits, sizeof bits); }

// The helper, then the statistics a selected caller asks for: n = M + 1 curve
// points (the curve's own count), observation_count = M.
pf_equity_stats_t selected_stats(std::int64_t t, double capital, const Points& observations,
                                 std::int64_t in_market, double net_profit,
                                 double first_open = kNaN, double last_close = kNaN) {
    const std::int64_t m = static_cast<std::int64_t>(observations.size());
    const Curve curve = report::build_selected_equity_curve(
        t, capital, observations.empty() ? nullptr : observations.data(), m);
    return metrics::compute_equity_stats(curve.points.get(), curve.count, capital, "UTC",
                                         first_open, last_close, in_market, net_profit, m);
}

// ── build_selected_equity_curve ──────────────────────────────────────────

void anchor_alone() {
    std::printf("selected curve: M = 0 is the anchor alone\n");
    for (const std::int64_t t : {kT, std::int64_t{0}, std::int64_t{-1}, kMinTime, kMaxTime}) {
        const Curve curve = report::build_selected_equity_curve(t, 100000.0, nullptr, 0);
        CHECK(curve.count == 1);
        CHECK(has_anchor(curve, t, 100000.0));
    }
    // No number is judged: any capital is taken as given, bit for bit.
    for (const double capital : {0.0, -5.0, 1e-300, 1e300, kInf, -kInf, kNaN}) {
        const Curve curve = report::build_selected_equity_curve(kT, capital, nullptr, 0);
        CHECK(curve.count == 1);
        CHECK(has_anchor(curve, kT, capital));
    }
    // With M = 0 the observations pointer is never read, null or not.
    const pf_equity_point_t unread[] = {{kT + kHour, 999.0, 99.0}};
    const Curve beside = report::build_selected_equity_curve(kT, 100.0, unread, 0);
    CHECK(beside.count == 1);
    CHECK(has_anchor(beside, kT, 100.0));
}

void observations_follow_the_anchor() {
    std::printf("selected curve: observation i lands at position i + 1\n");
    const Points one = {{kT + kMinute, 101.5, 1.5}};
    const Curve c1 = report::build_selected_equity_curve(kT, 100.0, one.data(), 1);
    CHECK(c1.count == 2);
    CHECK(has_anchor(c1, kT, 100.0));
    CHECK(same_bytes(&c1.points[1], &one[0], 1));

    const Points two = {{kT + kMinute, 110.0, 10.0}, {kT + 2 * kMinute, 104.5, 4.5}};
    const Curve c2 = report::build_selected_equity_curve(kT, 100.0, two.data(), 2);
    CHECK(c2.count == 3);
    CHECK(has_anchor(c2, kT, 100.0));
    CHECK(same_bytes(&c2.points[1], &two[0], 1));
    CHECK(same_bytes(&c2.points[2], &two[1], 1));

    for (const std::int64_t m : {3, 7, 64, 1000, 100000}) {
        Points observations(static_cast<std::size_t>(m));
        for (std::int64_t i = 0; i < m; ++i) {
            observations[static_cast<std::size_t>(i)] = {
                kT + (i + 1) * kMinute, 100.0 + static_cast<double>(i),
                static_cast<double>(i % 7) - 3.0};
        }
        const Curve curve = report::build_selected_equity_curve(kT, 100.0, observations.data(), m);
        CHECK(curve.count == m + 1);
        CHECK(has_anchor(curve, kT, 100.0));
        CHECK(same_bytes(curve.points.get() + 1, observations.data(), observations.size()));
    }
}

void first_bar_at_the_start_or_after_a_gap() {
    std::printf("selected curve: first observation at T, or after a gap, adds and drops no point\n");
    // The window's first bar opens exactly at T: two points share the stamp T.
    const Points at_start = {{kT, 110.0, 10.0}, {kT + kMinute, 104.5, 4.5}};
    const Curve c0 = report::build_selected_equity_curve(kT, 100.0, at_start.data(), 2);
    CHECK(c0.count == 3);
    CHECK(has_anchor(c0, kT, 100.0));
    CHECK(c0.points[1].time_ms == kT);
    CHECK(same_bytes(c0.points.get() + 1, at_start.data(), 2));

    // The first bar comes three days after T (a window that opens in a gap in
    // the feed): nothing is filled in at the gap.
    const Points after_gap = {{kT + 3 * kDay, 110.0, 10.0}, {kT + 3 * kDay + kMinute, 104.5, 4.5}};
    const Curve c1 = report::build_selected_equity_curve(kT, 100.0, after_gap.data(), 2);
    CHECK(c1.count == 3);
    CHECK(has_anchor(c1, kT, 100.0));
    CHECK(c1.points[1].time_ms == kT + 3 * kDay);
    CHECK(same_bytes(c1.points.get() + 1, after_gap.data(), 2));
}

void copied_byte_for_byte() {
    std::printf("selected curve: observations are copied byte for byte, none filtered or recomputed\n");
    constexpr std::size_t kM = 7;
    pf_equity_point_t odd[kM];
    std::memset(odd, 0, sizeof odd);
    // Ascending stamps from the smallest to the largest: only the values are odd.
    const std::int64_t times[kM] = {kMinTime, kMinTime + 1, -1, 0, kT, kMaxTime - 1, kMaxTime};
    for (std::size_t i = 0; i < kM; ++i) odd[i].time_ms = times[i];
    set_field_bits(&odd[0].equity, 0x8000000000000000ULL);        // -0.0
    set_field_bits(&odd[0].open_profit, 0x8000000000000000ULL);
    set_field_bits(&odd[1].equity, 0x7ff0000000000001ULL);        // signalling NaN
    set_field_bits(&odd[1].open_profit, 0xfff8000000000abcULL);   // negative quiet NaN with payload
    set_field_bits(&odd[2].equity, 0x7ff0000000000000ULL);        // +inf
    set_field_bits(&odd[2].open_profit, 0xfff0000000000000ULL);   // -inf
    set_field_bits(&odd[3].equity, 0x0000000000000001ULL);        // smallest denormal
    set_field_bits(&odd[3].open_profit, 0x8000000000000001ULL);
    odd[4].equity = std::numeric_limits<double>::max();
    odd[4].open_profit = -std::numeric_limits<double>::max();
    odd[5].equity = 123.456;      // not initial capital + open profit: nothing is recomputed
    odd[5].open_profit = 999.0;
    odd[6].equity = 0.0;
    odd[6].open_profit = 0.0;

    const std::int64_t m = static_cast<std::int64_t>(kM);
    const Curve curve = report::build_selected_equity_curve(kMinTime, 5.0, odd, m);
    CHECK(curve.count == m + 1);
    CHECK(has_anchor(curve, kMinTime, 5.0));
    CHECK(std::memcmp(curve.points.get() + 1, odd, sizeof odd) == 0);

    // Owned: a later change to the caller's array does not reach the curve, and
    // a second call from the same bytes answers the same bytes (no state).
    pf_equity_point_t before[kM];
    std::memcpy(before, odd, sizeof odd);
    odd[0].equity = 42.0;
    odd[kM - 1].time_ms = 7;
    CHECK(std::memcmp(curve.points.get() + 1, before, sizeof before) == 0);
    const Curve again = report::build_selected_equity_curve(kMinTime, 5.0, before, m);
    CHECK(again.count == curve.count);
    CHECK(same_bytes(again.points.get(), curve.points.get(),
                     static_cast<std::size_t>(curve.count)));
    CHECK(again.points.get() != curve.points.get());
}

// The curve is one array whose owner the report takes over: the same address
// through a move and through release() into ReportC::equity_curve, intact once
// every owner object is gone, and deleted by the report's own free_report -- the
// delete[] that only a new[] array survives (the sanitizer runs would flag a
// vector's buffer, or any other allocator, there).
void owned_array_moves_and_releases() {
    std::printf("selected curve: one owned array moves, releases into a report, is freed by it\n");
    const Points observations = {{kT + kMinute, 110.0, 10.0}, {kT + 2 * kMinute, 104.5, 4.5}};
    for (const std::int64_t m : {std::int64_t{0}, std::int64_t{2}}) {
        pineforge::ReportC rep{};
        const pf_equity_point_t* array = nullptr;
        {
            Curve built = report::build_selected_equity_curve(
                kT, 100.0, m > 0 ? observations.data() : nullptr, m);
            CHECK(built.count == m + 1);
            array = built.points.get();
            CHECK(array != nullptr);

            // A move hands the same array over; the source owns nothing afterwards.
            Curve moved = std::move(built);
            CHECK(built.points == nullptr);
            CHECK(moved.points.get() == array);
            CHECK(moved.count == m + 1);

            // The report takes it by release(): same address, no copy, owner empty.
            rep.equity_curve = moved.points.release();
            rep.equity_curve_len = moved.count;
            CHECK(moved.points == nullptr);
            CHECK(rep.equity_curve == array);
        }
        // Both owner objects are gone; the array lives on in the report, whole.
        CHECK(rep.equity_curve == array);
        CHECK(rep.equity_curve_len == m + 1);
        CHECK(is_anchor(rep.equity_curve[0], kT, 100.0));
        CHECK(same_bytes(rep.equity_curve + 1, observations.data(), static_cast<std::size_t>(m)));

        pineforge::BacktestEngine::free_report(&rep);
        CHECK(rep.equity_curve == nullptr);
        CHECK(rep.equity_curve_len == 0);
    }
}

enum class Outcome { built, engine_invariant, other };

// How the call ends: built, refused the way the engine refuses its own faults
// (a logic_error that carries engine_invariant in the failure channel's own
// record and classifies as it), or anything else.
Outcome outcome_of(std::int64_t count, const pf_equity_point_t* observations) {
    try {
        (void)report::build_selected_equity_curve(kT, 100.0, observations, count);
    } catch (const std::logic_error& error) {
        const pineforge::RunFailureInfo* carried =
            dynamic_cast<const pineforge::RunFailureInfo*>(&error);
        const bool carries_code = carried != nullptr
            && carried->run_failure_code() == pineforge::RunFailureCode::engine_invariant;
        const bool classified = pineforge::classify_run_failure(error).code
            == pineforge::RunFailureCode::engine_invariant;
        return carries_code && classified ? Outcome::engine_invariant : Outcome::other;
    } catch (...) {
        return Outcome::other;
    }
    return Outcome::built;
}

void refusals() {
    std::printf("selected curve: a call the engine got wrong is engine_invariant\n");
    const pf_equity_point_t one[] = {{kT + kMinute, 101.0, 0.0}};
    CHECK(outcome_of(-1, one) == Outcome::engine_invariant);
    CHECK(outcome_of(-1, nullptr) == Outcome::engine_invariant);
    CHECK(outcome_of(std::numeric_limits<std::int64_t>::min(), one) == Outcome::engine_invariant);
    CHECK(outcome_of(1, nullptr) == Outcome::engine_invariant);
    CHECK(outcome_of(1000, nullptr) == Outcome::engine_invariant);
    // M + 1 would overflow int64_t, and no array holds that many points; the
    // single point behind the pointer is never read.
    CHECK(outcome_of(std::numeric_limits<std::int64_t>::max(), one) == Outcome::engine_invariant);
    // M + 1 is still an int64_t, but the bytes of that many points are not a size_t.
    CHECK(outcome_of(std::numeric_limits<std::int64_t>::max() - 1, one)
          == Outcome::engine_invariant);
    // The first M whose M + 1 points no size_t can size: floor(SIZE_MAX / sizeof(point))
    // points still have a byte size, one more has none. Refused before any
    // allocation, so the answer is the coded refusal and not std::bad_alloc.
    const std::int64_t first_unsizable = static_cast<std::int64_t>(
        std::numeric_limits<std::size_t>::max() / sizeof(pf_equity_point_t));
    CHECK(outcome_of(first_unsizable, one) == Outcome::engine_invariant);
    // The legal neighbours are not refused.
    CHECK(outcome_of(0, nullptr) == Outcome::built);
    CHECK(outcome_of(0, one) == Outcome::built);
    CHECK(outcome_of(1, one) == Outcome::built);
}

// ── compute_equity_stats(..., observation_count) ─────────────────────────

void two_observations_by_hand() {
    std::printf("statistics: 100 -> 110 -> 104.5 over one year, M = 2, by hand\n");
    // The anchor stands at T with equity 100. Observation 1, half a year on, is
    // 110: the position is closed and +10 is realized. Observation 2, a year on,
    // is 104.5: a position is open and 5.5 under water (100 + 10 - 5.5).
    //
    //   returns, anchor first   110 / 100 - 1 = +0.10, then 104.5 / 110 - 1 = -0.05
    //   span                    (T + 1 year) - T is one year exactly, so M / span is
    //                           2 observations a year: per-period risk-free
    //                           0.02 / 2 = 0.01, annualizer sqrt(2)
    //   mean, deviation         (0.10 - 0.05) / 2 = 0.025; deviations +-0.075;
    //                           sample variance (N - 1 = 1) = 2 * 0.075^2 = 0.01125
    //   sharpe_bar              (0.025 - 0.01) / sqrt(0.01125) * sqrt(2)
    //                           = 0.015 * sqrt(2 / 0.01125) = 0.015 * 40 / 3 = 0.2
    //   sortino_bar             only -0.05 is below 0.01: (-0.06)^2 / 2 = 0.0018
    //                           = 0.015 * sqrt(2 / 0.0018) = 0.015 * 100 / 3 = 0.5
    //   cagr                    (104.5 / 100)^(1 / 1) - 1 = 4.5 %
    //   drawdown                peak 110, low 104.5: 5.5, and 5.5 / 110 = 5 %;
    //                           no run-up (a new peak resets the trough)
    //   calmar, recovery        4.5 / 5 = 0.9; net profit 10 / 5.5
    //   months                  Nov 2023, May 2024, Nov 2024: one bucket each, so the
    //                           monthly returns are the same +0.10, -0.05, against
    //                           0.02 / 12 and sqrt(12)
    //   exposure                1 observation in market of M = 2: 50 %
    //                           (an anchor-counted denominator would say 1 / 3)
    const Points observations = {{kT + kHalfYear, 110.0, 0.0}, {kT + kYear, 104.5, -5.5}};
    const std::int64_t in_market = 1;
    const double net_profit = 10.0;
    const Curve curve = report::build_selected_equity_curve(kT, 100.0, observations.data(), 2);
    CHECK(curve.count == 3);
    const pf_equity_stats_t s = metrics::compute_equity_stats(
        curve.points.get(), curve.count, 100.0, "UTC", 50.0, 55.0, in_market, net_profit, 2);

    CHECK(within(s.sharpe_bar, 0.2, 1e-12));
    CHECK(within(s.sortino_bar, 0.5, 1e-12));
    CHECK(within(s.cagr, 4.5, 1e-9));
    CHECK(within(s.max_equity_drawdown, 5.5, 1e-12));
    CHECK(within(s.max_equity_drawdown_pct, 5.0, 1e-12));
    CHECK(s.max_equity_runup == 0.0);
    CHECK(s.max_equity_runup_pct == 0.0);
    CHECK(within(s.calmar, 0.9, 1e-9));
    CHECK(within(s.recovery_factor, 10.0 / 5.5, 1e-12));
    CHECK(s.time_in_market_pct == 50.0);
    CHECK(s.open_pl == -5.5);

    const double rf_month = 0.02 / 12.0;
    const double sample_deviation = std::sqrt(0.01125);
    const double downside = std::sqrt((0.05 + rf_month) * (0.05 + rf_month) / 2.0);
    CHECK(within(s.sharpe_monthly, (0.025 - rf_month) / sample_deviation * std::sqrt(12.0), 1e-12));
    CHECK(within(s.sortino_monthly, (0.025 - rf_month) / downside * std::sqrt(12.0), 1e-12));

    // The buy-and-hold inputs are the caller's (the first selected bar's open, the
    // last close): 55 / 50 - 1 = 10 %, and 10 on a capital of 100.
    CHECK(within(s.buy_hold_return_pct, 10.0, 1e-9));
    CHECK(within(s.buy_hold_return, 10.0, 1e-9));

    // The ordinary entry still divides by the n = 3 points it is given, and that
    // is the only statistic the observation count moves.
    const pf_equity_stats_t anchor_counted = metrics::compute_equity_stats(
        curve.points.get(), curve.count, 100.0, "UTC", 50.0, 55.0, in_market, net_profit);
    CHECK(within(anchor_counted.time_in_market_pct, 100.0 / 3.0, 1e-12));
    pf_equity_stats_t rest = s;
    rest.time_in_market_pct = anchor_counted.time_in_market_pct;
    CHECK(ref::first_difference(rest, anchor_counted) == nullptr);
}

void span_is_anchored_at_the_window_start() {
    std::printf("statistics: the span starts at T whether the first bar is at T or after a gap\n");
    // Same returns (+10 %, then -5 %) and the same last stamp (T + 1 year); only
    // the first observation's stamp differs. The calendar span runs from the
    // anchor, so both are one year, 2 observations a year: the same 0.2 and 0.5
    // as above. A span taken from the first observation would give the second
    // curve half a year and 4 a year instead.
    const Points at_start = {{kT, 110.0, 0.0}, {kT + kYear, 104.5, -5.5}};
    const Points after_gap = {{kT + kHalfYear, 110.0, 0.0}, {kT + kYear, 104.5, -5.5}};
    const pf_equity_stats_t a = selected_stats(kT, 100.0, at_start, 1, 10.0);
    const pf_equity_stats_t b = selected_stats(kT, 100.0, after_gap, 1, 10.0);
    CHECK(within(a.sharpe_bar, 0.2, 1e-12));
    CHECK(within(b.sharpe_bar, 0.2, 1e-12));
    CHECK(within(a.sortino_bar, 0.5, 1e-12));
    CHECK(within(b.sortino_bar, 0.5, 1e-12));
    CHECK(within(a.cagr, 4.5, 1e-9));
    CHECK(within(b.cagr, 4.5, 1e-9));
    // Months: with the first bar at T the anchor and that bar share November
    // 2023, the bucket keeps its last point (110), and one monthly return
    // (110 -> 104.5) is left: undefined. After the gap there are three buckets.
    CHECK(std::isnan(a.sharpe_monthly));
    CHECK(std::isnan(a.sortino_monthly));
    CHECK(std::isfinite(b.sharpe_monthly));
    CHECK(std::isfinite(b.sortino_monthly));
}

void empty_and_one_observation() {
    std::printf("statistics: M = 0 follows the empty convention, M = 1 has one return\n");
    // M = 0: the anchor alone is not an observation. Everything answers as an
    // empty ordinary curve does: no exposure, no open profit, nothing to walk.
    const Curve anchor = report::build_selected_equity_curve(kT, 100.0, nullptr, 0);
    const pf_equity_stats_t none = metrics::compute_equity_stats(
        anchor.points.get(), anchor.count, 100.0, "UTC", 50.0, 55.0, 0, 0.0, 0);
    const pf_equity_stats_t empty = metrics::compute_equity_stats(
        nullptr, 0, 100.0, "UTC", 50.0, 55.0, 0, 0.0);
    CHECK(ref::first_difference(none, empty) == nullptr);
    CHECK(std::isnan(none.time_in_market_pct));
    CHECK(none.open_pl == 0.0);
    CHECK(none.max_equity_drawdown == 0.0);
    CHECK(none.max_equity_runup == 0.0);
    CHECK(std::isnan(none.cagr));
    CHECK(std::isnan(none.calmar));
    CHECK(std::isnan(none.recovery_factor));
    CHECK(std::isnan(none.sharpe_monthly));
    CHECK(std::isnan(none.sortino_monthly));
    CHECK(std::isnan(none.sharpe_bar));
    CHECK(std::isnan(none.sortino_bar));
    // The buy-and-hold inputs are the caller's, as for any curve ...
    CHECK(within(none.buy_hold_return_pct, 10.0, 1e-9));
    // ... and with no first selected bar the caller has no open to give.
    const pf_equity_stats_t no_open = selected_stats(kT, 100.0, Points(), 0, 0.0);
    CHECK(std::isnan(no_open.buy_hold_return));
    CHECK(std::isnan(no_open.buy_hold_return_pct));
    // Bars in market with no observation is not a division by zero.
    const pf_equity_stats_t odd_count = metrics::compute_equity_stats(
        anchor.points.get(), anchor.count, 100.0, "UTC", kNaN, kNaN, 3, 0.0, 0);
    CHECK(std::isnan(odd_count.time_in_market_pct));

    // M = 1: anchor 100 -> one observation of 110 a year on, position open (+10).
    // One return, +10 %: cagr is 10 %, and with fewer than 2 returns, and fewer
    // than the 3 points the per-bar walk needs, no Sharpe or Sortino exists.
    // No drawdown, so no calmar and no recovery factor.
    const Points one = {{kT + kYear, 110.0, 10.0}};
    const pf_equity_stats_t s = selected_stats(kT, 100.0, one, 1, 0.0);
    CHECK(within(s.cagr, 10.0, 1e-9));
    CHECK(s.max_equity_drawdown == 0.0);
    CHECK(std::isnan(s.calmar));
    CHECK(std::isnan(s.recovery_factor));
    CHECK(std::isnan(s.sharpe_monthly));
    CHECK(std::isnan(s.sortino_monthly));
    CHECK(std::isnan(s.sharpe_bar));
    CHECK(std::isnan(s.sortino_bar));
    CHECK(s.open_pl == 10.0);
    CHECK(s.time_in_market_pct == 100.0);     // 1 of 1 observation, not 1 of 2 points
    CHECK(selected_stats(kT, 100.0, one, 0, 0.0).time_in_market_pct == 0.0);
    // The first bar at T: no calendar span, no cagr.
    const Points at_start = {{kT, 110.0, 10.0}};
    CHECK(std::isnan(selected_stats(kT, 100.0, at_start, 1, 0.0).cagr));
}

void undefined_domains() {
    std::printf("statistics: the established undefined domains hold for a selected curve\n");
    const double rf_bar = 0.01;        // 0.02 per year over 2 observations a year

    // (a) A non-positive anchor equity (capital 0): no cagr (needs a positive
    // capital), and the return off the anchor is skipped, leaving one return.
    {
        const Points observations = {{kT + kHalfYear, 10.0, 10.0}, {kT + kYear, 20.0, 20.0}};
        const pf_equity_stats_t s = selected_stats(kT, 0.0, observations, 2, 0.0);
        CHECK(std::isnan(s.cagr));
        CHECK(std::isnan(s.sharpe_bar));
        CHECK(std::isnan(s.sortino_bar));
        CHECK(std::isnan(s.sharpe_monthly));
        CHECK(s.max_equity_drawdown == 0.0);
        CHECK(std::isnan(s.recovery_factor));
        CHECK(s.open_pl == 20.0);
    }

    // (b) Equity ends below zero: no cagr, but both returns come off a positive
    // equity, so they are defined: 50 / 100 - 1 = -0.5 and -10 / 50 - 1 = -1.2,
    // mean -0.85, deviations +-0.35, sample variance 0.245. Drawdown from the
    // peak of 100 to -10 is 110, 110 %.
    {
        const Points observations = {{kT + kHalfYear, 50.0, -50.0}, {kT + kYear, -10.0, -110.0}};
        const pf_equity_stats_t s = selected_stats(kT, 100.0, observations, 2, 0.0);
        CHECK(std::isnan(s.cagr));
        CHECK(within(s.sharpe_bar, (-0.85 - rf_bar) / std::sqrt(0.245) * std::sqrt(2.0), 1e-12));
        CHECK(within(s.sortino_bar,
                     (-0.85 - rf_bar) / std::sqrt((0.51 * 0.51 + 1.21 * 1.21) / 2.0) * std::sqrt(2.0),
                     1e-12));
        CHECK(std::isfinite(s.sharpe_monthly));
        CHECK(within(s.max_equity_drawdown, 110.0, 1e-9));
        CHECK(within(s.max_equity_drawdown_pct, 110.0, 1e-9));
        CHECK(s.max_equity_runup == 0.0);
        CHECK(std::isnan(s.calmar));         // no cagr to divide
    }

    // (c) Equity touches zero and recovers (100 -> 0 -> 50): cagr is -50 %, the
    // return out of zero is skipped so only -1.0 remains, the run-up from 0 is 50
    // but a zero trough has no percent, and the drawdown is 100 of 100.
    {
        const Points observations = {{kT + kHalfYear, 0.0, -100.0}, {kT + kYear, 50.0, -50.0}};
        const pf_equity_stats_t s = selected_stats(kT, 100.0, observations, 2, 0.0);
        CHECK(within(s.cagr, -50.0, 1e-9));
        CHECK(std::isnan(s.sharpe_bar));
        CHECK(std::isnan(s.sharpe_monthly));
        CHECK(within(s.max_equity_drawdown, 100.0, 1e-12));
        CHECK(within(s.max_equity_drawdown_pct, 100.0, 1e-12));
        CHECK(within(s.max_equity_runup, 50.0, 1e-12));
        CHECK(std::isnan(s.max_equity_runup_pct));
        CHECK(within(s.calmar, -0.5, 1e-9));
    }

    // (d) A peak that never exceeds zero (capital 0, then losses): drawdown and
    // run-up have no percent.
    {
        const Points observations = {{kT + kHalfYear, -5.0, -5.0}, {kT + kYear, -3.0, -3.0}};
        const pf_equity_stats_t s = selected_stats(kT, 0.0, observations, 2, 0.0);
        CHECK(within(s.max_equity_drawdown, 5.0, 1e-12));
        CHECK(std::isnan(s.max_equity_drawdown_pct));
        CHECK(within(s.max_equity_runup, 2.0, 1e-12));
        CHECK(std::isnan(s.max_equity_runup_pct));
        CHECK(std::isnan(s.calmar));
    }

    // (e) Zero deviation: 100 -> 110 -> 121 returns 0.1 twice (110 / 100 and
    // 121 / 110 are the same double), so no Sharpe and, with nothing under the
    // risk-free, no Sortino; cagr is 21 %; no drawdown, so no calmar or recovery.
    {
        const Points observations = {{kT + kHalfYear, 110.0, 0.0}, {kT + kYear, 121.0, 0.0}};
        const pf_equity_stats_t s = selected_stats(kT, 100.0, observations, 0, 0.0);
        CHECK(std::isnan(s.sharpe_bar));
        CHECK(std::isnan(s.sortino_bar));
        CHECK(std::isnan(s.sharpe_monthly));
        CHECK(std::isnan(s.sortino_monthly));
        CHECK(within(s.cagr, 21.0, 1e-9));
        CHECK(std::isnan(s.calmar));
        CHECK(std::isnan(s.recovery_factor));
    }

    // (f) A flat curve: returns 0, 0 have no Sharpe, but they sit 0.01 under the
    // per-period risk-free, so the Sortino is (0 - 0.01) / 0.01 * sqrt(2) = -sqrt(2);
    // monthly the same against 0.02 / 12 and sqrt(12).
    {
        const Points observations = {{kT + kHalfYear, 100.0, 0.0}, {kT + kYear, 100.0, 0.0}};
        const pf_equity_stats_t s = selected_stats(kT, 100.0, observations, 0, 0.0);
        CHECK(std::isnan(s.sharpe_bar));
        CHECK(within(s.sortino_bar, -std::sqrt(2.0), 1e-12));
        CHECK(std::isnan(s.sharpe_monthly));
        CHECK(within(s.sortino_monthly, -std::sqrt(12.0), 1e-12));
        CHECK(within(s.cagr, 0.0, 1e-9));
    }

    // (g) No calendar span (every stamp T): no cagr, no per-bar Sharpe or
    // Sortino even with 3 points, and one month bucket. The drawdown stands.
    {
        const Points observations = {{kT, 110.0, 0.0}, {kT, 104.5, -5.5}};
        const pf_equity_stats_t s = selected_stats(kT, 100.0, observations, 1, 10.0);
        CHECK(std::isnan(s.cagr));
        CHECK(std::isnan(s.calmar));
        CHECK(std::isnan(s.sharpe_bar));
        CHECK(std::isnan(s.sortino_bar));
        CHECK(std::isnan(s.sharpe_monthly));
        CHECK(within(s.max_equity_drawdown, 5.5, 1e-12));
        CHECK(within(s.recovery_factor, 10.0 / 5.5, 1e-12));
    }
}

void exposure_is_over_observations() {
    std::printf("statistics: exposure is bars in market / M, never / (M + 1)\n");
    struct Case {
        std::int64_t in_market;
        std::int64_t m;
        double percent;
    };
    const Case cases[] = {{0, 2, 0.0},  {1, 2, 50.0}, {2, 2, 100.0},
                          {1, 1, 100.0}, {3, 4, 75.0}, {1, 8, 12.5}};
    for (const Case& c : cases) {
        Points observations;
        for (std::int64_t i = 0; i < c.m; ++i)
            observations.push_back({kT + (i + 1) * kHour, 100.0 + static_cast<double>(i), 0.0});
        const pf_equity_stats_t s = selected_stats(kT, 100.0, observations, c.in_market, 0.0);
        CHECK(s.time_in_market_pct == c.percent);
    }
    // The count is the only thing that moves it: the same three points, counted
    // as 0 or fewer observations, have no exposure at all.
    const Points three = {{kT, 100.0, 0.0}, {kT + kHour, 101.0, 1.0}, {kT + 2 * kHour, 102.0, 2.0}};
    const pf_equity_stats_t zero = metrics::compute_equity_stats(
        three.data(), 3, 100.0, "UTC", kNaN, kNaN, 2, 0.0, 0);
    const pf_equity_stats_t negative = metrics::compute_equity_stats(
        three.data(), 3, 100.0, "UTC", kNaN, kNaN, 2, 0.0, -1);
    CHECK(std::isnan(zero.time_in_market_pct));
    CHECK(std::isnan(negative.time_in_market_pct));
}

// ── The ordinary overload, against the reference walk ────────────────────

struct Walk {
    Points points;
    double first_open = 0.0;
    double last_close = 0.0;
    std::int64_t in_market = 0;
    double net_profit = 0.0;
};

Walk make_walk(std::uint64_t seed, std::int64_t start, std::int64_t step, std::int64_t n,
               bool gaps) {
    std::uint64_t mix = seed * 0x9e3779b97f4a7c15ull + 1;
    const auto next = [&mix]() {
        mix ^= mix << 13;
        mix ^= mix >> 7;
        mix ^= mix << 17;
        return mix;
    };
    Walk w;
    double equity = 10000.0;
    std::int64_t ts = start;
    for (std::int64_t i = 0; i < n; ++i) {
        equity += (static_cast<double>(next() % 2001) - 1000.0) / 25.0;
        if (next() % 97 == 0) equity = next() % 2 ? 0.0 : -equity;   // touch or cross zero
        const double open_profit = (static_cast<double>(next() % 401) - 200.0) / 8.0;
        w.points.push_back({ts, equity, open_profit});
        ts += step;
        if (gaps && next() % 5 == 0) ts += step * static_cast<std::int64_t>(next() % 40);
    }
    w.first_open = next() % 11 == 0 ? kNaN : 100.0 + static_cast<double>(next() % 1000) / 7.0;
    w.last_close = 100.0 + static_cast<double>(next() % 1000) / 3.0;
    w.in_market = n > 0 ? static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(n + 1)) : 0;
    w.net_profit = (static_cast<double>(next() % 20001) - 10000.0) / 3.0;
    return w;
}

long long walks_compared = 0;

void compare_walk(const Walk& w, double capital, const std::string& tz) {
    ++walks_compared;
    const std::int64_t n = static_cast<std::int64_t>(w.points.size());
    const pf_equity_point_t* data = w.points.empty() ? nullptr : w.points.data();
    const pf_equity_stats_t want = ref::compute_equity_stats(
        data, n, capital, tz, w.first_open, w.last_close, w.in_market, w.net_profit);

    // The old entry, and the overload given the count the old entry gives it.
    const pf_equity_stats_t old_entry = metrics::compute_equity_stats(
        data, n, capital, tz, w.first_open, w.last_close, w.in_market, w.net_profit);
    const pf_equity_stats_t counted_n = metrics::compute_equity_stats(
        data, n, capital, tz, w.first_open, w.last_close, w.in_market, w.net_profit, n);
    CHECK(ref::first_difference(old_entry, want) == nullptr);
    CHECK(ref::first_difference(counted_n, want) == nullptr);

    // The selected shape over the same n points: M = n - 1 observations. Only the
    // exposure reads M; every other field is the reference's, bit for bit.
    if (n >= 1) {
        const std::int64_t m = n - 1;
        pf_equity_stats_t expected = want;
        expected.time_in_market_pct =
            m > 0 ? static_cast<double>(w.in_market) / static_cast<double>(m) * 100.0 : kNaN;
        const pf_equity_stats_t selected = metrics::compute_equity_stats(
            data, n, capital, tz, w.first_open, w.last_close, w.in_market, w.net_profit, m);
        CHECK(ref::first_difference(selected, expected) == nullptr);
    }
}

void ordinary_overload_unchanged() {
    std::printf("statistics: the old entry and observation_count = n equal the reference walk\n");
    const std::int64_t starts[] = {1672531200000LL, kT, 951782400000LL, 4102444800000LL};
    const std::int64_t steps[] = {kMinute, kHour, kDay, 7 * kDay, 30 * kDay + 10 * kHour};
    const std::int64_t lengths[] = {0, 1, 2, 3, 5, 17, 400};
    std::uint64_t seed = 1;
    for (const char* tz : {"", "UTC", "Etc/UTC"}) {
        for (const std::int64_t start : starts) {
            for (const std::int64_t step : steps) {
                for (const std::int64_t n : lengths) {
                    for (const bool gaps : {false, true}) {
                        ++seed;
                        const Walk w = make_walk(seed, start, step, n, gaps);
                        compare_walk(w, 10000.0, tz);
                        compare_walk(w, seed % 5 == 0 ? 0.0 : 25000.0, tz);
                    }
                }
            }
        }
    }
    // A chart timezone keeps its per-point localtime_r walk, which the count does not touch.
    for (const char* tz : {"America/New_York", "Asia/Tokyo"}) {
        for (const std::int64_t step : {kHour, kDay}) {
            compare_walk(make_walk(++seed, 1672531200000LL, step, 3000, true), 10000.0, tz);
        }
    }
    CHECK(walks_compared > 1000);
}

}  // namespace

int main() {
    anchor_alone();
    observations_follow_the_anchor();
    first_bar_at_the_start_or_after_a_gap();
    copied_byte_for_byte();
    owned_array_moves_and_releases();
    refusals();
    two_observations_by_hand();
    span_is_anchored_at_the_window_start();
    empty_and_one_observation();
    undefined_domains();
    exposure_is_over_observations();
    ordinary_overload_unchanged();
    if (failures == 0) {
        std::printf("test_selected_report_view: ok (%lld checks)\n", checks);
        return 0;
    }
    std::fprintf(stderr, "test_selected_report_view: %d of %lld checks FAILED\n", failures, checks);
    return 1;
}
