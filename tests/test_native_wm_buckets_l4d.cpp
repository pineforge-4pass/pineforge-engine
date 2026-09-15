// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// request.security(syminfo.tickerid, "W" | "M", x) on an intraday chart with
// TradingView's own daily bars installed (strategy_set_native_security_feed
// "D"): the W/M values are the NATIVE DAILY bars aggregated per period -- o
// the first session's daily open, h / l the daily extremes, c the last
// session's daily close, v the sum -- never the chart's intraday prints, and
// the period completes on its actual last chart bar.
//
// Oracle: the lab tv tapes of the wm-security-buckets pin (ledger note
// log-20260905t022917z-007fd19a, 2026-09-05; scratchpad r6/pins/out-wm-{w,m}-
// f15-{jul,nov}-par{0,1} and out-wm-w-es15-aug-par{0,1}, 1473/1473 qty-encoded
// reads equal to the native-1D-built period, 0 to the 15m-built one), replayed
// here over the registry feeds those tapes were read against
// (test_native_wm_buckets_data.hpp):
//   (a) values = native-1D-built (NYSE:F week 2025-07-28 c 10.82, the 15m
//       print 10.81; week 2025-11-17 o = h = 13.1751, the 15m 13.14 / 13.155;
//       CME_MINI:ES1! week 2025-08-11 c 6471.5 = Friday's settlement, the 15m
//       print 6467.25);
//   (b) lookahead_off x / x[1] / time advance ON the period's last chart bar
//       -- Fri 15:45 ET, the half-day Fri 2025-11-28 at 12:45 (13:00 close),
//       ES Fri 15:45 CT -- never on the next period's first bar; lookahead_on
//       x = the period's FINAL native values from its first chart bar, x[1] =
//       the previous period;
//   (c) a period already in progress at the deep-backtest range start does
//       not exist (na under both modes; the KI-55 range-start gate).
// Plus the control without a native feed (values = the intraday aggregate,
// byte-identical to the aggregator's own arithmetic) and the aggregator-level
// actual-last-bar rule (TimeframeAggregator::feed(bar, next_input_ms)).

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include "test_native_wm_buckets_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace pineforge;

#ifndef PINEFORGE_HAS_NATIVE_SECURITY_FEED_V1
#error "native W/M bucket test requires the native security feed feature probe"
#endif

namespace {

int checks = 0;
int failures = 0;

#define CHECK(cond, tag)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(cond)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, (tag));    \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

constexpr int64_t kMinute = 60000;
constexpr int64_t kQuarter = 15 * kMinute;

// Unix ms of a UTC civil date-time (Howard Hinnant's days_from_civil).
int64_t utc_ms(int y, int m, int d, int h = 0, int mi = 0) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (static_cast<int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000;
}
// Fixed offsets: the tapes never straddle a DST edge.
int64_t edt(int y, int m, int d, int h, int mi) { return utc_ms(y, m, d, h + 4, mi); }
int64_t est(int y, int m, int d, int h, int mi) { return utc_ms(y, m, d, h + 5, mi); }
int64_t cdt(int y, int m, int d, int h, int mi) { return utc_ms(y, m, d, h + 5, mi); }

bool same(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::abs(a - b) < 1e-9;
}

struct Ohlc {
    double o, h, l, c;
};
const Ohlc kNa{na<double>(), na<double>(), na<double>(), na<double>()};

// What the strategy body reads on one chart bar for one security site: x
// (the current slot), x[1] (the previous slot) and time(x).
struct Read {
    Ohlc x0 = kNa;
    Ohlc x1 = kNa;
    int64_t t0 = 0;
    bool complete0 = false;
};

// Mirrors the generated security series: a dispatch opens a new history
// slot exactly when the engine says so (security_series_slot_is_new) and
// otherwise rewrites the current one; the chart body reads the slots.
class WmProbe final : public pineforge::source::PineStrategyHost {
public:
    struct Site {
        std::string tf;
        bool lookahead_on;
    };
    std::vector<Site> sites;
    struct Series {
        std::vector<Bar> slots;
        std::vector<bool> complete;
    };
    std::vector<Series> series;
    std::map<int64_t, std::vector<Read>> rows;  // chart ts -> per site

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        series.assign(sites.size(), Series{});
        for (std::size_t i = 0; i < sites.size(); ++i) {
            register_security_eval(static_cast<int>(i), sites[i].tf, input_tf_,
                                   sites[i].lookahead_on, false);
        }
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        Series& s = series[static_cast<std::size_t>(sec_id)];
        if (s.slots.empty() || security_series_slot_is_new(sec_id)) {
            s.slots.push_back(bar);
            s.complete.push_back(is_complete);
        } else {
            s.slots.back() = bar;
            s.complete.back() = is_complete;
        }
    }

    void on_source_bar(const Bar& bar) override {
        std::vector<Read> reads(sites.size());
        for (std::size_t i = 0; i < sites.size(); ++i) {
            const Series& s = series[i];
            Read& r = reads[i];
            if (!s.slots.empty()) {
                const Bar& b = s.slots.back();
                r.x0 = Ohlc{b.open, b.high, b.low, b.close};
                r.t0 = b.timestamp;
                r.complete0 = s.complete.back();
            }
            if (s.slots.size() >= 2) {
                const Bar& b = s.slots[s.slots.size() - 2];
                r.x1 = Ohlc{b.open, b.high, b.low, b.close};
            }
        }
        rows[bar.timestamp] = std::move(reads);
    }

    const Read& at(int64_t ts, std::size_t site) const {
        static const Read none;
        const auto it = rows.find(ts);
        if (it == rows.end()) return none;
        return it->second[site];
    }
    bool has_row(int64_t ts) const { return rows.count(ts) != 0; }
};

template <std::size_t N>
std::vector<Bar> vec(const Bar (&arr)[N]) {
    return std::vector<Bar>(arr, arr + N);
}

void install_daily(WmProbe& probe, const std::vector<Bar>& daily,
                   const char* tf = "D") {
    const int rc = strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), tf,
        reinterpret_cast<const pf_bar_t*>(daily.data()),
        static_cast<int>(daily.size()));
    CHECK(rc == 0, "native daily feed installs");
}

void run15(WmProbe& probe, const std::vector<Bar>& chart, const char* tz,
           const char* session, const char* type, int64_t range_start_ms) {
    probe.set_syminfo_timezone(tz);
    probe.set_syminfo_session(session);
    // The lane's syminfo.type: an exchange-listed kind, whose TradingView
    // session template knows the early closes, so a half-day's last chart
    // bar completes the period (test_oanda_lazy_close pins the OTC case).
    probe.set_syminfo_type(type);
    // The campaign's historical semantics: TV's deep-backtest range start
    // (KI-55) and the finite-batch lookahead_on projection.
    probe.set_syminfo_metadata("security_range_start_na_warmup",
                               static_cast<double>(range_start_ms));
    probe.set_syminfo_metadata("historical_security_lookahead_projection", 1.0);
    probe.run(chart.data(), static_cast<int>(chart.size()), "15", "15",
              false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
}

void check_ohlc(const Ohlc& got, const Ohlc& want, const char* tag) {
    const bool ok = same(got.o, want.o) && same(got.h, want.h)
        && same(got.l, want.l) && same(got.c, want.c);
    if (!ok) {
        std::printf("    %s: got o %.6g h %.6g l %.6g c %.6g, want o %.6g h %.6g l %.6g c %.6g\n",
                    tag, got.o, got.h, got.l, got.c, want.o, want.h, want.l, want.c);
    }
    CHECK(ok, tag);
}

// Every chart bar with ts in [from, to] reads `x0` (and `x1`) on `site`.
void check_span(const WmProbe& p, std::size_t site, int64_t from, int64_t to,
                const Ohlc& x0, const Ohlc& x1, int64_t t0, const char* tag) {
    int seen = 0;
    for (const auto& kv : p.rows) {
        if (kv.first < from || kv.first > to) continue;
        ++seen;
        const Read& r = kv.second[site];
        check_ohlc(r.x0, x0, tag);
        check_ohlc(r.x1, x1, tag);
        if (t0 != 0) CHECK(r.t0 == t0, tag);
    }
    CHECK(seen > 0, tag);
}

// The chart's own aggregate of a session-day / week / month: the control
// oracle (what the aggregator computes without a native feed).
Ohlc aggregate(const std::vector<Bar>& bars, int64_t from, int64_t to) {
    Ohlc out = kNa;
    bool first = true;
    for (const Bar& b : bars) {
        if (b.timestamp < from || b.timestamp > to) continue;
        if (first) {
            out = Ohlc{b.open, b.high, b.low, b.close};
            first = false;
        } else {
            out.h = std::max(out.h, b.high);
            out.l = std::min(out.l, b.low);
            out.c = b.close;
        }
    }
    return out;
}

// ---- NYSE:F, July: W and M under both modes -------------------------------

const Ohlc kWeek0728{11.48, 11.49, 10.68, 10.82};   // 07-28 .. 08-01 native
const Ohlc kWeek0804{10.89, 11.385, 10.86, 11.32};  // 08-04 .. 08-08 native
const Ohlc kAug2025{10.92, 11.99, 10.68, 11.77};    // the whole of August

void test_f_july_weekly_and_monthly() {
    WmProbe probe;
    probe.sites = {{"W", false}, {"W", true}, {"M", false}, {"M", true}};
    install_daily(probe, vec(wm_data::kF1DJul));
    // TV's chart range 2025-07-23 .. 2025-08-09: the week of 07-21 and July
    // are already in progress at the range start.
    run15(probe, vec(wm_data::kF15Jul), "America/New_York", "0930-1600", "stock",
          utc_ms(2025, 7, 23));
    CHECK(probe.rows.size() == 338, "338 chart bars");
    CHECK(probe.native_security_misses() == 0, "every bucket found its period");
    // W off: 07-28 + 08-04 completions; W on: 07-28 (complete) + 08-04 (the
    // trailing partial); M on: August (trailing partial); M off: nothing
    // completes on the tape.
    CHECK(probe.native_security_substitutions() == 5, "5 substitutions");

    // (c) + (b) lookahead_off W: na until the week of 07-28 completes on Fri
    // 08-01 15:45, then held; 08-04's week on Fri 08-08 15:45 with [1] =
    // 07-28's.
    check_span(probe, 0, edt(2025, 7, 23, 9, 30), edt(2025, 8, 1, 15, 30),
               kNa, kNa, 0, "W off na before the first whole week completes");
    check_span(probe, 0, edt(2025, 8, 1, 15, 45), edt(2025, 8, 8, 15, 30),
               kWeek0728, kNa, edt(2025, 7, 28, 9, 30),
               "W off week 07-28 from Fri 08-01 15:45");
    check_span(probe, 0, edt(2025, 8, 8, 15, 45), edt(2025, 8, 8, 15, 45),
               kWeek0804, kWeek0728, edt(2025, 8, 4, 9, 30),
               "W off week 08-04 on Fri 08-08 15:45, [1] = 07-28");
    CHECK(probe.at(edt(2025, 8, 1, 15, 45), 0).complete0, "W off publishes complete");
    // (a) the value is the native period: the 15m-built week reads h 11.48
    // l 10.685 c 10.81.
    CHECK(!same(probe.at(edt(2025, 8, 1, 15, 45), 0).x0.c, 10.81),
          "W off close is the native 10.82, not the 15m 10.81");
    // v = the sum of the daily volumes (07-28 .. 08-01).
    {
        const auto& slots = probe.series[0].slots;
        CHECK(slots.size() == 2, "two completed weeks");
        if (slots.size() == 2) {
            CHECK(same(slots[0].volume,
                       54173647.0 + 58371483.0 + 79866858.0 + 101090884.0
                           + 73541489.0),
                  "W volume = sum of the daily volumes");
        }
    }

    // lookahead_on W: the FINAL native week from Monday 09:30 (the leak),
    // [1] = the previous week; the week in progress at the range start is
    // absent.
    check_span(probe, 1, edt(2025, 7, 23, 9, 30), edt(2025, 7, 25, 15, 45),
               kNa, kNa, 0, "W on na through the partial first week");
    check_span(probe, 1, edt(2025, 7, 28, 9, 30), edt(2025, 8, 1, 15, 45),
               kWeek0728, kNa, edt(2025, 7, 28, 9, 30),
               "W on week 07-28 from Mon 07-28 09:30");
    check_span(probe, 1, edt(2025, 8, 4, 9, 30), edt(2025, 8, 8, 15, 45),
               kWeek0804, kWeek0728, edt(2025, 8, 4, 9, 30),
               "W on week 08-04 from Mon 08-04 09:30, [1] = 07-28");

    // M off: July is absent and August never completes on the tape.
    check_span(probe, 2, edt(2025, 7, 23, 9, 30), edt(2025, 8, 8, 15, 45),
               kNa, kNa, 0, "M off na for the whole tape");
    // M on: July absent, August's FINAL native month from 08-01 09:30 --
    // its high 11.99 and close 11.77 print after the chart ends 08-08.
    check_span(probe, 3, edt(2025, 7, 23, 9, 30), edt(2025, 7, 31, 15, 45),
               kNa, kNa, 0, "M on na through July");
    check_span(probe, 3, edt(2025, 8, 1, 9, 30), edt(2025, 8, 8, 15, 45),
               kAug2025, kNa, edt(2025, 8, 1, 9, 30),
               "M on August from 08-01 09:30");
}

// ---- NYSE:F, November: the holiday week / half-day and D -------------------

const Ohlc kWeek1117{13.1751, 13.1751, 12.38, 12.83};  // sub-penny official open
const Ohlc kWeek1124{12.84, 13.34, 12.825, 13.28};     // 11-27 closed, 11-28 half-day
const Ohlc kWeek1201{13.195, 13.385, 12.87, 13.03};
const Ohlc kDec2025{13.195, 13.99, 12.87, 13.12};
const Ohlc kDay1126{13.17, 13.26, 13.12, 13.19};
const Ohlc kDay1128{13.205, 13.34, 13.18, 13.28};

void test_f_november_half_day_and_holiday() {
    WmProbe probe;
    probe.sites = {{"W", false}, {"W", true}, {"M", false}, {"M", true},
                   {"D", false}};
    install_daily(probe, vec(wm_data::kF1DNov), "1D");
    run15(probe, vec(wm_data::kF15Nov), "America/New_York", "0930-1600", "stock",
          utc_ms(2025, 11, 12));
    CHECK(probe.rows.size() == 430, "430 chart bars");
    CHECK(probe.native_security_misses() == 0, "every bucket found its period");
    // W off 3 + W on 3 + M on 1 (December, trailing) + D off 17 sessions.
    CHECK(probe.native_security_substitutions() == 24, "24 substitutions");

    // Week 11-17 (o = h = 13.1751, the official open above every 15m print)
    // completes on Fri 11-21 15:45.
    check_span(probe, 0, est(2025, 11, 12, 9, 30), est(2025, 11, 21, 15, 30),
               kNa, kNa, 0, "W off na before 11-21 15:45");
    check_span(probe, 0, est(2025, 11, 21, 15, 45), est(2025, 11, 28, 12, 30),
               kWeek1117, kNa, est(2025, 11, 17, 9, 30),
               "W off week 11-17 from Fri 11-21 15:45");
    // (b) the holiday week: Thu 11-27 closed, Fri 11-28 closes 13:00 -- the
    // week completes on the 12:45 bar, not on Mon 12-01 09:30.
    check_span(probe, 0, est(2025, 11, 28, 12, 45), est(2025, 12, 5, 15, 30),
               kWeek1124, kWeek1117, est(2025, 11, 24, 9, 30),
               "W off week 11-24 from the half-day's 12:45 bar");
    check_span(probe, 0, est(2025, 12, 5, 15, 45), est(2025, 12, 5, 15, 45),
               kWeek1201, kWeek1124, est(2025, 12, 1, 9, 30),
               "W off week 12-01 on Fri 12-05 15:45");
    CHECK(!probe.has_row(est(2025, 11, 27, 9, 30)), "Thanksgiving holds no bars");
    CHECK(!probe.has_row(est(2025, 11, 28, 13, 0)), "the half-day ends 13:00");

    // lookahead_on W.
    check_span(probe, 1, est(2025, 11, 12, 9, 30), est(2025, 11, 14, 15, 45),
               kNa, kNa, 0, "W on na through the partial first week");
    check_span(probe, 1, est(2025, 11, 17, 9, 30), est(2025, 11, 21, 15, 45),
               kWeek1117, kNa, est(2025, 11, 17, 9, 30), "W on week 11-17");
    check_span(probe, 1, est(2025, 11, 24, 9, 30), est(2025, 11, 28, 12, 45),
               kWeek1124, kWeek1117, est(2025, 11, 24, 9, 30), "W on week 11-24");
    check_span(probe, 1, est(2025, 12, 1, 9, 30), est(2025, 12, 5, 15, 45),
               kWeek1201, kWeek1124, est(2025, 12, 1, 9, 30),
               "W on week 12-01 (trailing, whole native week)");

    // M: November (opened 11-03) is absent; December from 12-01 09:30 under
    // lookahead_on with its final values, never under lookahead_off.
    check_span(probe, 2, est(2025, 11, 12, 9, 30), est(2025, 12, 5, 15, 45),
               kNa, kNa, 0, "M off na for the whole tape");
    check_span(probe, 3, est(2025, 11, 12, 9, 30), est(2025, 11, 28, 12, 45),
               kNa, kNa, 0, "M on na through November");
    check_span(probe, 3, est(2025, 12, 1, 9, 30), est(2025, 12, 5, 15, 45),
               kDec2025, kNa, est(2025, 12, 1, 9, 30), "M on December");

    // D on the half-day: the 11-28 daily bar is published on its 12:45 bar
    // (13:00 close), the 11-26 bar still reads on 12:30.
    check_ohlc(probe.at(est(2025, 11, 28, 12, 30), 4).x0, kDay1126,
               "D off reads 11-26 on 11-28 12:30");
    check_ohlc(probe.at(est(2025, 11, 28, 12, 45), 4).x0, kDay1128,
               "D off reads 11-28 on the half-day's 12:45 bar");
    check_ohlc(probe.at(est(2025, 11, 28, 12, 45), 4).x1, kDay1126,
               "D off [1] = 11-26 on the half-day's last bar");
    check_ohlc(probe.at(est(2025, 12, 1, 9, 30), 4).x0, kDay1128,
               "D off holds 11-28 on Mon 09:30");
}

// ---- CME_MINI:ES1!: the overnight session, W and D -------------------------

const Ohlc kEsWeek0811{6422.75, 6508.75, 6387.5, 6471.5};  // c = Fri settlement
const Ohlc kEsThu0814{6485.0, 6496.0, 6453.25, 6490.5};     // stamped Wed 17:00 CT
const Ohlc kEsWed0813{6468.0, 6502.5, 6461.0, 6488.75};

void test_es_overnight_session_week() {
    WmProbe probe;
    probe.sites = {{"W", false}, {"W", true}, {"D", false}};
    install_daily(probe, vec(wm_data::kEs1DAug));
    // TV's range 2025-08-06 .. 2025-08-16: the week of 08-04 (opened Sun
    // 08-03 17:00 CT) is in progress at the range start.
    run15(probe, vec(wm_data::kEs15Aug), "America/Chicago", "1700-1600", "futures",
          utc_ms(2025, 8, 6));
    CHECK(probe.rows.size() == 728, "728 chart bars");
    CHECK(probe.native_security_misses() == 0, "every bucket found its period");
    // W off 1 + W on 1 + D off: the seven session-days completed on the tape
    // (the first session's bucket opened before the range start; the last
    // one, Friday's, completes on the chart's last bar -- its 15:45 CT bar
    // closing at the 16:00 session close, as TradingView's esd-aug tape
    // reads it there: t0 = Thu 08-14 17:00, c 6471.5).
    CHECK(probe.native_security_substitutions() == 9, "9 substitutions");

    // W off: the week completes on Fri 15:45 CT (16:00 close) with Friday's
    // settlement 6471.5, not the 15m print 6467.25.
    check_span(probe, 0, cdt(2025, 8, 5, 19, 0), cdt(2025, 8, 15, 15, 30),
               kNa, kNa, 0, "W off na before Fri 08-15 15:45 CT");
    check_span(probe, 0, cdt(2025, 8, 15, 15, 45), cdt(2025, 8, 15, 15, 45),
               kEsWeek0811, kNa, cdt(2025, 8, 10, 17, 0),
               "W off week 08-11 on Fri 15:45 CT, dated Sun 17:00 CT");
    // W on: absent through the partial week, the final week from Sun 17:00.
    check_span(probe, 1, cdt(2025, 8, 5, 19, 0), cdt(2025, 8, 8, 15, 45),
               kNa, kNa, 0, "W on na through the partial first week");
    check_span(probe, 1, cdt(2025, 8, 10, 17, 0), cdt(2025, 8, 15, 15, 45),
               kEsWeek0811, kNa, cdt(2025, 8, 10, 17, 0),
               "W on week 08-11 from Sun 17:00 CT");
    // D off: a session-day completes on its 15:45 CT bar (the bar closing at
    // the 16:00 session close), not on the next session's 17:00 open.
    check_ohlc(probe.at(cdt(2025, 8, 14, 15, 30), 2).x0, kEsWed0813,
               "D off reads Wednesday on Thu 15:30 CT");
    check_ohlc(probe.at(cdt(2025, 8, 14, 15, 45), 2).x0, kEsThu0814,
               "D off reads Thursday on Thu 15:45 CT");
    check_ohlc(probe.at(cdt(2025, 8, 14, 17, 0), 2).x0, kEsThu0814,
               "D off holds Thursday on the 17:00 CT open");
    // The chart's last bar is Friday's 15:45 CT bar: under the native
    // partition the day completes there (no next bar to compare, but its
    // close reaches the session-day's 16:00 close) -- the esd-aug tape reads
    // Friday's settlement 6471.5 on it (test_native_daily_holiday pins the
    // whole window).
    check_ohlc(probe.at(cdt(2025, 8, 15, 15, 30), 2).x0, kEsThu0814,
               "D off still reads Thursday on Fri 15:30 CT");
    check_ohlc(probe.at(cdt(2025, 8, 15, 15, 45), 2).x0,
               Ohlc{6489.25, 6508.75, 6461.5, 6471.5},
               "D off reads Friday on the chart's last bar, Fri 15:45 CT");
    CHECK(probe.at(cdt(2025, 8, 15, 15, 45), 2).t0 == cdt(2025, 8, 14, 17, 0),
          "Friday's bar is stamped Thu 17:00 CT");
}

// ---- control: no native feed keeps the intraday aggregate -------------------

void test_control_without_native_feed_is_the_intraday_aggregate() {
    WmProbe probe;
    probe.sites = {{"W", false}, {"W", true}, {"D", false}};
    const std::vector<Bar> chart = vec(wm_data::kF15Nov);
    run15(probe, chart, "America/New_York", "0930-1600", "stock", utc_ms(2025, 11, 12));
    CHECK(probe.native_security_substitutions() == 0, "nothing substituted");
    CHECK(probe.native_security_misses() == 0, "nothing missed");

    const Ohlc w1117 = aggregate(chart, est(2025, 11, 17, 9, 30), est(2025, 11, 21, 15, 45));
    const Ohlc w1124 = aggregate(chart, est(2025, 11, 24, 9, 30), est(2025, 11, 28, 12, 45));
    const Ohlc w1201 = aggregate(chart, est(2025, 12, 1, 9, 30), est(2025, 12, 5, 15, 45));
    // The pin's 15m-built values for the same weeks.
    check_ohlc(w1117, Ohlc{13.14, 13.155, 12.385, 12.845}, "15m-built week 11-17");
    check_ohlc(w1124, Ohlc{12.855, 13.34, 12.825, 13.255}, "15m-built week 11-24");

    check_span(probe, 0, est(2025, 11, 12, 9, 30), est(2025, 11, 21, 15, 30),
               kNa, kNa, 0, "control W off na before 11-21 15:45");
    check_span(probe, 0, est(2025, 11, 21, 15, 45), est(2025, 11, 28, 12, 30),
               w1117, kNa, est(2025, 11, 17, 9, 30), "control W off week 11-17");
    // The actual-last-bar completion does not depend on the native feed:
    // the holiday week still completes on the half-day's 12:45 bar.
    check_span(probe, 0, est(2025, 11, 28, 12, 45), est(2025, 12, 5, 15, 30),
               w1124, w1117, est(2025, 11, 24, 9, 30), "control W off week 11-24");
    check_span(probe, 0, est(2025, 12, 5, 15, 45), est(2025, 12, 5, 15, 45),
               w1201, w1124, est(2025, 12, 1, 9, 30), "control W off week 12-01");
    check_span(probe, 1, est(2025, 11, 17, 9, 30), est(2025, 11, 21, 15, 45),
               w1117, kNa, est(2025, 11, 17, 9, 30), "control W on week 11-17");
    check_span(probe, 1, est(2025, 11, 24, 9, 30), est(2025, 11, 28, 12, 45),
               w1124, w1117, est(2025, 11, 24, 9, 30), "control W on week 11-24");
    check_span(probe, 1, est(2025, 12, 1, 9, 30), est(2025, 12, 5, 15, 45),
               w1201, w1124, est(2025, 12, 1, 9, 30), "control W on week 12-01");

    const Ohlc d1128 = aggregate(chart, est(2025, 11, 28, 9, 30), est(2025, 11, 28, 12, 45));
    const Ohlc d1126 = aggregate(chart, est(2025, 11, 26, 9, 30), est(2025, 11, 26, 15, 45));
    check_ohlc(probe.at(est(2025, 11, 28, 12, 30), 2).x0, d1126, "control D 11-26 on 12:30");
    check_ohlc(probe.at(est(2025, 11, 28, 12, 45), 2).x0, d1128, "control D 11-28 on 12:45");
}

// ---- aggregator: the actual-last-bar rule ----------------------------------

// One RTH 15m session of `bars` bars from 09:30 ET on the given EDT date.
void push_session(std::vector<Bar>& out, int y, int m, int d, int bars,
                  double base) {
    for (int k = 0; k < bars; ++k) {
        const double v = base + k;
        out.push_back({v, v + 1.0, v - 1.0, v, 1.0, edt(y, m, d, 9, 30) + k * kQuarter});
    }
}

void test_aggregator_completes_on_the_actual_last_bar() {
    // Independence Day 2025: Thu 07-03 closes 13:00 (14 bars), Fri 07-04 is
    // a holiday, so the week's last chart bar is Thu 12:45.
    std::vector<Bar> feed;
    push_session(feed, 2025, 6, 30, 26, 100.0);
    push_session(feed, 2025, 7, 1, 26, 200.0);
    push_session(feed, 2025, 7, 2, 26, 300.0);
    push_session(feed, 2025, 7, 3, 14, 400.0);
    push_session(feed, 2025, 7, 7, 26, 500.0);

    auto completion_ts = [&](const char* tf, bool with_next) {
        TimeframeAggregator agg(tf, "15", "America/New_York", "0930-1600");
        std::vector<int64_t> completed_on;
        for (std::size_t i = 0; i < feed.size(); ++i) {
            const int64_t next = (with_next && i + 1 < feed.size())
                ? feed[i + 1].timestamp : 0;
            const AggregatedBar ab = with_next ? agg.feed(feed[i], next)
                                               : agg.feed(feed[i]);
            if (ab.is_complete) completed_on.push_back(feed[i].timestamp);
        }
        return completed_on;
    };

    // W: with the next bar known the week completes on Thu 07-03 12:45;
    // without it (the stream) it still completes lazily on Mon 07-07 09:30.
    {
        const auto on = completion_ts("W", true);
        CHECK(on.size() == 1 && on[0] == edt(2025, 7, 3, 12, 45),
              "W completes on the half-day Thursday's last bar");
        const auto lazy = completion_ts("W", false);
        CHECK(lazy.size() == 1 && lazy[0] == edt(2025, 7, 7, 9, 30),
              "W without the hint completes on Monday's first bar");
    }
    // D: full sessions on their 15:45 bar as before, the half-day on 12:45.
    {
        const auto on = completion_ts("D", true);
        CHECK(on.size() == 5, "five completed sessions");
        if (on.size() == 5) {
            CHECK(on[0] == edt(2025, 6, 30, 15, 45), "D full session 06-30 on 15:45");
            CHECK(on[3] == edt(2025, 7, 3, 12, 45), "D half-day 07-03 on 12:45");
            CHECK(on[4] == edt(2025, 7, 7, 15, 45), "D full session 07-07 on 15:45");
        }
        const auto lazy = completion_ts("D", false);
        CHECK(lazy.size() == 5 && lazy[3] == edt(2025, 7, 7, 9, 30),
              "D without the hint completes the half-day on Monday 09:30");
    }
    // M: the month whose last session is a half-day (June 2025 ends on a
    // full Monday here; use the week feed's own month change 07-03 -> 07-07
    // as a no-op check: July does not complete on the tape).
    {
        const auto on = completion_ts("M", true);
        CHECK(on.size() == 1 && on[0] == edt(2025, 6, 30, 15, 45),
              "M June completes on its last session's last bar");
    }
    // 24x7 UTC: a hole before midnight is not a close -- the next-bar hint
    // must not complete the day early (bit-identical to the hint-less form).
    {
        std::vector<Bar> utc;
        for (int k = 0; k < 95; ++k) {  // 00:00 .. 23:30, the 23:45 bar missing
            const double v = 10.0 + k;
            utc.push_back({v, v + 1.0, v - 1.0, v, 1.0,
                           utc_ms(2025, 7, 1) + k * kQuarter});
        }
        utc.push_back({200.0, 201.0, 199.0, 200.0, 1.0, utc_ms(2025, 7, 2)});
        TimeframeAggregator hinted("D", "15", "UTC", "");
        TimeframeAggregator plain("D", "15", "UTC", "");
        for (std::size_t i = 0; i < utc.size(); ++i) {
            const int64_t next = i + 1 < utc.size() ? utc[i + 1].timestamp : 0;
            const AggregatedBar a = hinted.feed(utc[i], next);
            const AggregatedBar b = plain.feed(utc[i]);
            CHECK(a.is_complete == b.is_complete, "24x7 hint is inert");
            if (a.is_complete) {
                CHECK(utc[i].timestamp == utc_ms(2025, 7, 2),
                      "24x7 day with a hole still completes on the next bar");
            }
        }
    }
}

}  // namespace

int main() {
    test_f_july_weekly_and_monthly();
    test_f_november_half_day_and_holiday();
    test_es_overnight_session_week();
    test_control_without_native_feed_is_the_intraday_aggregate();
    test_aggregator_completes_on_the_actual_last_bar();
    std::printf("test_native_wm_buckets: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
