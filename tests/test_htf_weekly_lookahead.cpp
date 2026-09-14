// A coarser-than-chart request.security under lookahead_on on a daily chart
// leaks the period's FINAL values from the period's first chart bar, on both
// feed paths (round 7, family I: hungpixi macd-enhanced-mtf on the
// BINANCE:BTCUSDT and OANDA:XAUUSD 1D lanes; lab tv
// macd1d-hungpixi-{btc,xau}-sense-par{0,1}, 2026-09-05).
//
// The script reads request.security(syminfo.tickerid, "W", f_count(),
// lookahead = barmerge.lookahead_on) beside two "30" requests, so the lane
// runs split-feed: every evaluator, the weekly one included, is fed the 1m
// auxiliary slice. TradingView's per-bar values (the tapes' qty-encoded hw /
// wi) pin three facts the engine got wrong:
//
//   1. the leaked value is the week's FINAL f_count on EVERY chart bar of the
//      week, from Monday -- the engine's historical lookahead projection
//      implements exactly that but was never prepared on the split-feed path
//      (run() built either the auxiliary chart ranges or the projections),
//      so the weekly series was the progressive partial week;
//   2. the week in progress at the deep-backtest range start is na and the
//      first FULL week is weekly bar 0 (KI-55 range-start cut at the range
//      start itself: 2025-04-07 on BTC, the Sunday-17:00-ET-stamped
//      2025-04-06 bar on XAUUSD); the UDF reads 0, not na, while its EMAs
//      warm up, and the weekly MACD line seeds from those bars;
//   3. one requested bar per week: the engine's boundary-emission test
//      (bcf4436) compared a completed bucket's label with bar_label_ms(input)
//      -- the DAY stamp for a calendar W bucket -- so every weekly completion
//      on its last daily bar looked like a boundary and committed a phantom
//      copy of the week (the f_count carry decayed twice per week, hist
//      ties compared the week with its own copy).
//
// Data: tests/test_htf_weekly_lookahead_data.hpp -- the lanes' native 1D
// bars from the registry and TradingView's per-week hw / wi.

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/timeframe.hpp>

#include "test_htf_weekly_lookahead_data.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "the weekly lookahead test requires the auxiliary feed V1 probe"
#endif

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n  %s\n",        \
                         __FILE__, __LINE__, #cond, (msg));                   \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

namespace {

constexpr int64_t kDay = 86400000LL;
constexpr int64_t kHour = 3600000LL;
constexpr int64_t kRangeStart = 1743465600000LL;   // 2025-04-01 00:00Z, both lanes

template <std::size_t N>
constexpr int count(const Bar (&)[N]) { return static_cast<int>(N); }

// Pine helper series as the codegen keeps them inside a request.security UDF:
// push on a new requested-context slot, update otherwise, [k] the k-th
// previous slot (na before it exists).
struct Series {
    std::vector<double> v;
    void put(bool fresh, double x) {
        if (fresh || v.empty()) v.push_back(x); else v.back() = x;
    }
    double operator[](std::size_t k) const {
        return k < v.size() ? v[v.size() - 1 - k] : na<double>();
    }
};

// Pine comparisons as the codegen emits them (na -> false, 1e-10 equality).
bool peq(double a, double b) {
    return !std::isnan(a) && !std::isnan(b)
        && (a == b || std::fabs(a - b) <= 1e-10);
}
bool pgt(double a, double b) { return !std::isnan(a) && !std::isnan(b) && a > b && !peq(a, b); }
bool plt(double a, double b) { return pgt(b, a); }
bool pge(double a, double b) { return !std::isnan(a) && !std::isnan(b) && (a >= b || peq(a, b)); }
bool ple(double a, double b) { return pge(b, a); }
double nz(double x) { return std::isnan(x) ? 0.0 : x; }

bool within(double v, double expected, double tol) {
    return !std::isnan(v) && std::fabs(v - expected) <= tol;
}

// Four "W" lookahead_on sites, evaluated as the generated code evaluates them
// (compute on a new requested-context slot, recompute otherwise):
//   sec 0: f_count()               (the hungpixi UDF: MACD 12/26/9 arms + carry)
//   sec 1: ta.ema(close, 12) - ta.ema(close, 26)
//   sec 2: the requested-context bar ordinal (one per new slot)
//   sec 3: close
class WeeklyProbe final : public pineforge::source::PineStrategyHost {
public:
    ta::EMA e12{12}, e26{26}, e9{9}, m12{12}, m26{26};
    Series indi, sig, hist, cc;
    double exposed[4] = {na<double>(), na<double>(), na<double>(), na<double>()};
    int slots[4] = {0, 0, 0, 0};
    int dispatches[4] = {0, 0, 0, 0};

    struct Read {
        int64_t ts = 0;
        double v[4] = {0, 0, 0, 0};
    };
    std::vector<Read> reads;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (int s = 0; s < 4; ++s) {
            register_security_eval(s, "W", input_tf_, /*lookahead_on=*/true, false);
        }
    }

    void evaluate_security(int sec_id, const Bar& bar, bool) override {
        const bool fresh = security_series_slot_is_new(sec_id);
        ++dispatches[sec_id];
        if (fresh) ++slots[sec_id];
        const double c = bar.close;
        switch (sec_id) {
            case 0: {
                const double in = (fresh ? e12.compute(c) : e12.recompute(c))
                                - (fresh ? e26.compute(c) : e26.recompute(c));
                indi.put(fresh, in);
                sig.put(fresh, fresh ? e9.compute(indi[0]) : e9.recompute(indi[0]));
                hist.put(fresh, indi[0] - sig[0]);
                double score = 0.0;
                if (pgt(indi[0], indi[1])) {
                    score = pgt(hist[0], hist[1]) ? 10.0 : peq(hist[0], hist[1]) ? 8.0 : 6.0;
                }
                if (plt(indi[0], indi[1])) {
                    score += plt(hist[0], hist[1]) ? -10.0 : peq(hist[0], hist[1]) ? -8.0 : -6.0;
                }
                if (peq(indi[0], indi[1])) {
                    score += pgt(hist[0], hist[1]) ? 2.0 : plt(hist[0], hist[1]) ? -2.0 : 0.0;
                }
                double x = 0.0;
                if (pge(indi[0], sig[0]) && plt(indi[1], sig[1])) x = 10.0;
                else if (ple(indi[0], sig[0]) && pgt(indi[1], sig[1])) x = -10.0;
                // ``countcross := countcross + nz(countcross[1]) * 0.6``: the
                // declaration pushes the slot, the reassignment updates it,
                // so [1] is the previous requested bar's carry.
                cc.put(fresh, x);
                cc.put(false, cc[0] + nz(cc[1]) * 0.6);
                exposed[0] = score + cc[0];
                break;
            }
            case 1:
                exposed[1] = (fresh ? m12.compute(c) : m12.recompute(c))
                           - (fresh ? m26.compute(c) : m26.recompute(c));
                break;
            case 2:
                exposed[2] = slots[2];
                break;
            default:
                exposed[3] = c;
                break;
        }
    }

    void on_source_bar(const Bar& bar) override {
        Read r;
        r.ts = bar.timestamp;
        for (int s = 0; s < 4; ++s) r.v[s] = exposed[s];
        reads.push_back(r);
    }
};

// Hourly auxiliary bars derived from the daily bars: 24 (23 for the 1800-1700
// session, from the stamp's next hour) sub-bars whose aggregate is the daily
// bar exactly -- the split-feed path's evaluators are fed these.
std::vector<Bar> hourly_from(const Bar* days, int n, int first_hour, int hours) {
    std::vector<Bar> out;
    out.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(hours));
    for (int i = 0; i < n; ++i) {
        const Bar& d = days[i];
        for (int h = 0; h < hours; ++h) {
            Bar b;
            b.timestamp = d.timestamp + static_cast<int64_t>(first_hour + h) * kHour;
            b.open = d.open + (d.close - d.open) * h / hours;
            b.close = h + 1 == hours ? d.close
                                     : d.open + (d.close - d.open) * (h + 1) / hours;
            b.high = std::max(b.open, b.close);
            b.low = std::min(b.open, b.close);
            if (h == hours / 4) b.high = d.high;
            if (h == 3 * hours / 4) b.low = d.low;
            b.volume = d.volume / hours;
            out.push_back(b);
        }
    }
    return out;
}

void configure(WeeklyProbe& probe, bool projection, bool range_start) {
    if (range_start) {
        probe.set_syminfo_metadata("security_range_start_na_warmup",
                                   static_cast<double>(kRangeStart));
    }
    probe.set_syminfo_metadata("historical_security_lookahead_projection",
                               projection ? 1.0 : 0.0);
}

void run_btc(WeeklyProbe& probe, bool projection, bool aux) {
    using namespace htf_weekly_lookahead_data;
    probe.set_syminfo_timezone("UTC");
    probe.set_syminfo_session("24x7");
    probe.set_syminfo_type("crypto");
    configure(probe, projection, true);
    std::vector<Bar> hourly;
    if (aux) {
        hourly = hourly_from(kBtc1d, count(kBtc1d), 0, 24);
        CHECK(probe.set_aux_security_feed(hourly.data(),
                                          static_cast<int>(hourly.size()), "60"),
              "the hourly auxiliary feed installs");
    }
    probe.run(kBtc1d, count(kBtc1d), "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
    CHECK(probe.reads.size() == static_cast<std::size_t>(count(kBtc1d)),
          "one chart body per daily bar");
}

void run_xau(WeeklyProbe& probe, bool projection, bool aux) {
    using namespace htf_weekly_lookahead_data;
    probe.set_syminfo_timezone("America/New_York");
    probe.set_syminfo_session("1800-1700");
    probe.set_syminfo_type("cfd");
    configure(probe, projection, true);
    std::vector<Bar> hourly;
    if (aux) {
        // The 17:00 ET stamp precedes the 18:00 session open by an hour: the
        // slice runs 18:00 .. 16:00 ET, 23 hourly bars.
        hourly = hourly_from(kXau1d, count(kXau1d), 1, 23);
        CHECK(probe.set_aux_security_feed(hourly.data(),
                                          static_cast<int>(hourly.size()), "60"),
              "the hourly auxiliary feed installs");
    }
    probe.run(kXau1d, count(kXau1d), "1D", "1D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
    CHECK(probe.reads.size() == static_cast<std::size_t>(count(kXau1d)),
          "one chart body per daily bar");
}

// The tape's hw is qty-encoded to 1e-3 and wi to 1e-2.
constexpr double kHwTol = 5e-4;
constexpr double kWiTol = 6e-3;

// Every chart bar of TradingView's weekly bar k reads that week's final
// values; the bars before weekly bar 0 read na.
template <std::size_t NW>
void check_tv_weeks(const WeeklyProbe& probe, const Bar* days, int n,
                    const htf_weekly_lookahead_data::TvWeek (&weeks)[NW],
                    const char* lane) {
    const int64_t first = weeks[0].first_bar_ms;
    std::size_t k = 0;
    int checked = 0;
    for (int i = 0; i < n; ++i) {
        const auto& r = probe.reads[static_cast<std::size_t>(i)];
        CHECK(r.ts == days[i].timestamp, "chart bodies follow the daily bars");
        if (r.ts < first) {
            CHECK(std::isnan(r.v[0]) && std::isnan(r.v[1]) && std::isnan(r.v[2])
                      && std::isnan(r.v[3]),
                  "the week in progress at the range start reads na (no weekly bar yet)");
            continue;
        }
        while (k + 1 < NW && weeks[k + 1].first_bar_ms <= r.ts) ++k;
        const auto& w = weeks[k];
        char msg[256];
        std::snprintf(msg, sizeof msg,
                      "%s week %zu (bar %d): f_count %.4f vs TV %.4f, macd %.3f vs TV %.3f, slot %.0f",
                      lane, k, i, r.v[0], w.hw, r.v[1], w.wi, r.v[2]);
        CHECK(within(r.v[2], static_cast<double>(k + 1), 0.0), msg);
        CHECK(within(r.v[0], w.hw, kHwTol), msg);
        if (std::isnan(w.wi)) {
            CHECK(std::isnan(r.v[1]), msg);
        } else {
            CHECK(within(r.v[1], w.wi, kWiTol), msg);
        }
        ++checked;
    }
    CHECK(k + 1 == NW, "every TradingView week was reached");
    CHECK(checked >= n - 15, "every daily bar from the first full week on was checked");
}

// ---- 1: the leak is the week's final value on every bar of the week ---------

void test_btc_projection_reads_tv_weeks() {
    using namespace htf_weekly_lookahead_data;
    WeeklyProbe probe;
    run_btc(probe, /*projection=*/true, /*aux=*/false);
    check_tv_weeks(probe, kBtc1d, count(kBtc1d), kBtcTvWeeks, "BTC");
    // The leaked close is the week's LAST daily close from its first bar.
    for (std::size_t k = 0; k + 1 < sizeof(kBtcTvWeeks) / sizeof(kBtcTvWeeks[0]); ++k) {
        const int64_t start = kBtcTvWeeks[k].first_bar_ms;
        const int64_t next = kBtcTvWeeks[k + 1].first_bar_ms;
        double last_close = na<double>();
        for (int i = 0; i < count(kBtc1d); ++i) {
            if (kBtc1d[i].timestamp >= start && kBtc1d[i].timestamp < next) {
                last_close = kBtc1d[i].close;
            }
        }
        for (const auto& r : probe.reads) {
            if (r.ts >= start && r.ts < next) {
                CHECK(within(r.v[3], last_close, 0.0), "weekly close = the week's last daily close, from Monday");
            }
        }
    }
    // One projection per week: the evaluator ran once per weekly bar.
    CHECK(probe.dispatches[0] == static_cast<int>(sizeof(kBtcTvWeeks) / sizeof(kBtcTvWeeks[0])),
          "one dispatch per projected week");
}

void test_xau_projection_reads_tv_weeks() {
    using namespace htf_weekly_lookahead_data;
    WeeklyProbe probe;
    run_xau(probe, /*projection=*/true, /*aux=*/false);
    check_tv_weeks(probe, kXau1d, count(kXau1d), kXauTvWeeks, "XAUUSD");
    CHECK(probe.dispatches[0] == static_cast<int>(sizeof(kXauTvWeeks) / sizeof(kXauTvWeeks[0])),
          "one dispatch per projected week (the Sunday-17:00-ET-stamped bar opens it)");
}

// ---- 2: the split-feed path projects the same weeks -------------------------

void test_split_feed_projection_matches_the_chart_path() {
    using namespace htf_weekly_lookahead_data;
    WeeklyProbe chart, split;
    run_btc(chart, true, false);
    run_btc(split, true, true);
    for (std::size_t i = 0; i < chart.reads.size(); ++i) {
        const auto& a = chart.reads[i];
        const auto& b = split.reads[i];
        CHECK(a.ts == b.ts, "same chart bars");
        for (int s = 0; s < 4; ++s) {
            CHECK((std::isnan(a.v[s]) && std::isnan(b.v[s])) || within(b.v[s], a.v[s], 1e-9),
                  "the split-feed weekly read equals the chart-path read on every daily bar");
        }
    }
    CHECK(split.dispatches[0] == chart.dispatches[0],
          "one dispatch per projected week on the split-feed path too (the week's first auxiliary bar)");
    check_tv_weeks(split, kBtc1d, count(kBtc1d), kBtcTvWeeks, "BTC split-feed");

    WeeklyProbe xchart, xsplit;
    run_xau(xchart, true, false);
    run_xau(xsplit, true, true);
    for (std::size_t i = 0; i < xchart.reads.size(); ++i) {
        const auto& a = xchart.reads[i];
        const auto& b = xsplit.reads[i];
        for (int s = 0; s < 4; ++s) {
            CHECK((std::isnan(a.v[s]) && std::isnan(b.v[s])) || within(b.v[s], a.v[s], 1e-9),
                  "XAUUSD: the split-feed weekly read equals the chart-path read (the 18:00 ET slice opens the 17:00-stamped bar's week)");
        }
    }
    CHECK(xsplit.dispatches[0] == xchart.dispatches[0], "XAUUSD: one dispatch per projected week");
    check_tv_weeks(xsplit, kXau1d, count(kXau1d), kXauTvWeeks, "XAUUSD split-feed");
}

// ---- 3: without the projection, still exactly one requested bar per week ----

template <std::size_t NW>
void check_one_slot_per_week(const WeeklyProbe& probe,
                             const htf_weekly_lookahead_data::TvWeek (&weeks)[NW]) {
    std::size_t k = 0;
    for (const auto& r : probe.reads) {
        if (r.ts < weeks[0].first_bar_ms) continue;
        while (k + 1 < NW && weeks[k + 1].first_bar_ms <= r.ts) ++k;
        char msg[160];
        std::snprintf(msg, sizeof msg, "week %zu: requested bar ordinal %.0f (a phantom copy of the completed week would add one)", k, r.v[2]);
        CHECK(within(r.v[2], static_cast<double>(k + 1), 0.0), msg);
    }
}

void test_progressive_weeks_have_no_phantom_slot() {
    using namespace htf_weekly_lookahead_data;
    // Chart path: the weekly bucket completes on its last daily bar (the next
    // input's timestamp is known) -- an eager completion, not a boundary one.
    WeeklyProbe chart;
    run_btc(chart, /*projection=*/false, /*aux=*/false);
    check_one_slot_per_week(chart, kBtcTvWeeks);
    int fed = 0;
    for (int i = 0; i < count(kBtc1d); ++i) {
        if (kBtc1d[i].timestamp >= kBtcTvWeeks[0].first_bar_ms) ++fed;
    }
    CHECK(chart.dispatches[0] == fed, "one dispatch per retained daily input (partial peeks + completions), none for a phantom");
    // Split-feed path: the same completion on the week's last hourly bar.
    WeeklyProbe split;
    run_btc(split, false, true);
    check_one_slot_per_week(split, kBtcTvWeeks);
    CHECK(split.dispatches[0] == fed * 24, "one dispatch per retained hourly input, none for a phantom");
    // OANDA: the week completes on the Friday session's last bar.
    WeeklyProbe xau;
    run_xau(xau, false, false);
    check_one_slot_per_week(xau, kXauTvWeeks);
    WeeklyProbe xsplit;
    run_xau(xsplit, false, true);
    check_one_slot_per_week(xsplit, kXauTvWeeks);
}

}  // namespace

int main() {
    test_btc_projection_reads_tv_weeks();
    test_xau_projection_reads_tv_weeks();
    test_split_feed_projection_matches_the_chart_path();
    test_progressive_weeks_have_no_phantom_slot();
    std::printf("test_htf_weekly_lookahead: OK\n");
    return 0;
}
