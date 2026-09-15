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

// request.security(syminfo.tickerid, "D", x) on CME_MINI:ES1! 15m with
// TradingView's own daily bars installed (strategy_set_native_security_feed
// "D"): the daily PERIOD is the native bar's span -- the chart bars from one
// native stamp up to the bar before the next -- and its values are the native
// bar's own.
//
// Oracle: the eight lab tv tapes of the es-daily-timing pin (ledger note
// log-20260905t031053z-f283208c, 2026-09-05; scratchpad r6/pins/out-esd-{aug,
// sep,jul,nov}-par{0,1}, decoded in esd-<win>-table.txt against the registry
// feeds 766e8149d7e1 (15m) / 8da771d4bb79 (native 1D) replayed here from
// test_native_daily_holiday_data.hpp and test_native_wm_buckets_data.hpp):
//   (a) weekday: lookahead_off advances ON the 15:45 CT bar (the bar closing
//       at the 16:00 session close), never on the next session's 17:00 open;
//       Friday's 15:45 bar likewise, the Sunday 17:00 reopen changes nothing;
//   (b) early close: the shortened session's last bar (Thu 07-03 12:00 CT,
//       Fri 11-28 12:00 CT);
//   (c) a CME holiday session that pauses at 12:00 CT and reopens at 17:00
//       the same day (Labor Day Mon 09-01, Thanksgiving Thu 11-27,
//       Independence Day Fri 07-04) is NOT a period: TradingView has no daily
//       bar stamped at it and folds it into the NEXT trade date's daily bar --
//       no advance on the 11:45 pause bar, none at the 17:00 reopen, the
//       merged bar advances on the next session's last bar stamped with the
//       holiday session's open, and its o/h/l/c/v are TradingView's own (the
//       Jul-7 bar's o 6307.75 is the SUNDAY open and its h 6315 excludes the
//       holiday session's 6322.75; the volume covers both sessions);
//   (d) the registry 15m feed's hole Thu 11-27 20:45 -> Fri 11-28 07:15 CT
//       lies inside the merged Thanksgiving bar and must not complete it;
//   (e) values = the native bar: close = the settlement (differs from the
//       15m last print on every day), volume = the daily volume, time = the
//       daily stamp; x[1] = the previous native day, advancing on the same
//       bar; lookahead_on leaks the day's FINAL native values from the
//       session's first 17:00 bar and, on a holiday session's first bar,
//       the merged next-trade-date bar, held through the pause and reopen;
//   (f) the day in progress at the range start is absent under both modes
//       (KI-55), including on its completion bar.
// The W sites pin the engine's derivation for the merged day's week (the
// holiday session belongs to the next trade date, so to its week -- the
// Thu 07-03 17:00 stamp is Monday 07-07's week); that grouping is the
// engine's rule, not a TradingView pin. The control without a native feed
// keeps today's aggregator (the holiday session is its own session-day
// bucket, values the intraday aggregate).

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include "test_native_daily_holiday_data.hpp"
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
#error "native daily holiday test requires the native security feed feature probe"
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
// Fixed offsets: America/Chicago is CDT (UTC-5) through the Jul/Aug/Sep
// windows and CST (UTC-6) through the Nov/Dec one; no tape straddles the
// 2025-11-02 fall-back.
int64_t cdt(int y, int m, int d, int h, int mi) { return utc_ms(y, m, d, h + 5, mi); }
int64_t cst(int y, int m, int d, int h, int mi) { return utc_ms(y, m, d, h + 6, mi); }

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
// (the current slot), x[1] (the previous slot), time(x) and volume(x).
struct Read {
    Ohlc x0 = kNa;
    Ohlc x1 = kNa;
    int64_t t0 = 0;
    double v0 = na<double>();
    bool complete0 = false;
};

// Mirrors the generated security series: a dispatch opens a new history
// slot exactly when the engine says so (security_series_slot_is_new) and
// otherwise rewrites the current one; the chart body reads the slots.
class DProbe final : public pineforge::source::PineStrategyHost {
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
                r.v0 = b.volume;
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

void install_daily(DProbe& probe, const std::vector<Bar>& daily) {
    const int rc = strategy_set_native_security_feed(
        static_cast<pf_strategy_t>(&probe), "D",
        reinterpret_cast<const pf_bar_t*>(daily.data()),
        static_cast<int>(daily.size()));
    CHECK(rc == 0, "native daily feed installs");
}

// CME_MINI:ES1!: America/Chicago, the 1700-1600 session; the campaign's
// historical semantics (KI-55 range start, finite-batch lookahead_on
// projection).
void run_es15(DProbe& probe, const std::vector<Bar>& chart,
              int64_t range_start_ms) {
    probe.set_syminfo_timezone("America/Chicago");
    probe.set_syminfo_session("1700-1600");
    // CME_MINI:ES1! is an exchange-listed future: TradingView's session
    // template knows its early closes, so the no-feed control completes a
    // holiday session on its pause bar (test_oanda_lazy_close pins the OTC
    // contrast).
    probe.set_syminfo_type("futures");
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

// Every chart bar with ts in [from, to] reads `x0` (and `x1`, `t0`) on `site`.
void check_span(const DProbe& p, std::size_t site, int64_t from, int64_t to,
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

// The chart's own aggregate of [from, to]: the control oracle.
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

enum Site : std::size_t { kDOff = 0, kDOn = 1, kWOff = 2, kWOn = 3 };
const std::vector<DProbe::Site> kSites = {
    {"D", false}, {"D", true}, {"W", false}, {"W", true}};

// ---- Labor Day 2025: Sun 08-31 17:00 -> Mon 09-01 11:45 merges into Tue --

// Native daily bars, named by their stamp (the session open, 17:00 CT).
const Ohlc kD0827{6485.0, 6523.0, 6471.0, 6517.5};
const Ohlc kD0828{6516.0, 6518.0, 6455.5, 6472.75};
const Ohlc kD0831{6478.75, 6491.5, 6371.75, 6425.5};   // Sun 17:00 .. Tue 15:45
const Ohlc kD0902{6448.0, 6464.25, 6425.5, 6457.25};
const Ohlc kD0903{6460.0, 6516.75, 6454.5, 6510.75};
// The week of 09-01 from the native days stamped 08-31 .. 09-04.
const Ohlc kW0901{6478.75, 6541.75, 6371.75, 6489.75};

void test_labor_day_merges_into_tuesday() {
    DProbe probe;
    probe.sites = kSites;
    install_daily(probe, vec(esd_data::kEs1DSep));
    // TV's range 2025-08-27 .. 2025-09-04 (UTC): the Tue 08-26 17:00 session
    // is in progress at the range start.
    run_es15(probe, vec(esd_data::kEs15Sep), utc_ms(2025, 8, 27));
    CHECK(probe.rows.size() == 537, "537 chart bars (the tape's)");
    CHECK(probe.native_security_misses() == 0, "every bucket found its native bar");
    // D off 4 (08-27, 08-28, 08-31, 09-02) + D on 5 (those + the trailing
    // 09-03 partial) + W on 1 (the trailing week of 09-01) + W off 0.
    CHECK(probe.native_security_substitutions() == 10, "10 substitutions");

    // (f) the day in progress at the range start is absent, including on
    // its completion bar Wed 08-27 15:45.
    check_span(probe, kDOff, cdt(2025, 8, 26, 19, 0), cdt(2025, 8, 28, 15, 30),
               kNa, kNa, 0, "D off na through Thu 08-28 15:30");
    CHECK(probe.has_row(cdt(2025, 8, 27, 15, 45)), "08-27 15:45 is a chart bar");
    // (a) weekday: the 08-27 bar advances on Thu 08-28 15:45 CT.
    check_span(probe, kDOff, cdt(2025, 8, 28, 15, 45), cdt(2025, 8, 29, 15, 30),
               kD0827, kNa, cdt(2025, 8, 27, 17, 0),
               "D off 08-27 from Thu 15:45 CT");
    CHECK(probe.at(cdt(2025, 8, 28, 15, 45), kDOff).complete0, "D off publishes complete");
    CHECK(same(probe.at(cdt(2025, 8, 28, 15, 45), kDOff).v0, 1170397.0),
          "D off volume = the native daily volume");
    CHECK(!same(probe.at(cdt(2025, 8, 28, 15, 45), kDOff).x0.c, 6516.25),
          "D off close is the settlement 6517.5, not the 15m print 6516.25");
    // (a) Friday's 15:45 advances; the Sunday 17:00 reopen changes nothing;
    // (c) the Labor-Day holiday session's pause bar (Mon 11:45) and its
    // 17:00 reopen change nothing either.
    check_span(probe, kDOff, cdt(2025, 8, 29, 15, 45), cdt(2025, 9, 2, 15, 30),
               kD0828, kD0827, cdt(2025, 8, 28, 17, 0),
               "D off 08-28 from Fri 15:45 through Tue 15:30");
    CHECK(probe.has_row(cdt(2025, 8, 31, 17, 0)), "Sun 17:00 reopen is a chart bar");
    CHECK(probe.has_row(cdt(2025, 9, 1, 11, 45)), "Mon 11:45 pause bar is a chart bar");
    CHECK(!probe.has_row(cdt(2025, 9, 1, 12, 0)), "the holiday session pauses at 12:00");
    CHECK(probe.has_row(cdt(2025, 9, 1, 17, 0)), "Mon 17:00 reopen is a chart bar");
    check_ohlc(probe.at(cdt(2025, 9, 1, 11, 45), kDOff).x0, kD0828,
               "no advance on the holiday session's pause bar");
    check_ohlc(probe.at(cdt(2025, 9, 1, 17, 0), kDOff).x0, kD0828,
               "no advance on the same-day 17:00 reopen");
    // (c) the merged bar advances on Tue 09-02 15:45, stamped Sun 17:00,
    // with TradingView's own values (h from the holiday session, l from
    // Mon-Tue, c = Tuesday's settlement, v = both sessions).
    check_span(probe, kDOff, cdt(2025, 9, 2, 15, 45), cdt(2025, 9, 3, 15, 30),
               kD0831, kD0828, cdt(2025, 8, 31, 17, 0),
               "D off merged 08-31 bar from Tue 15:45, [1] = 08-28");
    CHECK(same(probe.at(cdt(2025, 9, 2, 15, 45), kDOff).v0, 1802584.0),
          "merged bar volume = both sessions (native)");
    check_span(probe, kDOff, cdt(2025, 9, 3, 15, 45), cdt(2025, 9, 3, 19, 0),
               kD0902, kD0831, cdt(2025, 9, 2, 17, 0),
               "D off 09-02 from Wed 15:45, [1] = the merged bar");
    {
        const auto& slots = probe.series[kDOff].slots;
        CHECK(slots.size() == 4, "four completed days");
    }

    // (e) lookahead_on: the day's FINAL values from the session's first
    // 17:00 bar; the merged bar from the holiday session's first bar (Sun
    // 17:00), held through the pause and the Mon 17:00 reopen.
    check_span(probe, kDOn, cdt(2025, 8, 26, 19, 0), cdt(2025, 8, 27, 15, 45),
               kNa, kNa, 0, "D on na through the partial first day");
    check_span(probe, kDOn, cdt(2025, 8, 27, 17, 0), cdt(2025, 8, 28, 15, 45),
               kD0827, kNa, cdt(2025, 8, 27, 17, 0), "D on 08-27 from Wed 17:00");
    check_span(probe, kDOn, cdt(2025, 8, 28, 17, 0), cdt(2025, 8, 29, 15, 45),
               kD0828, kD0827, cdt(2025, 8, 28, 17, 0), "D on 08-28 from Thu 17:00");
    check_span(probe, kDOn, cdt(2025, 8, 31, 17, 0), cdt(2025, 9, 2, 15, 45),
               kD0831, kD0828, cdt(2025, 8, 31, 17, 0),
               "D on merged bar from Sun 17:00 through Tue 15:45");
    check_span(probe, kDOn, cdt(2025, 9, 2, 17, 0), cdt(2025, 9, 3, 15, 45),
               kD0902, kD0831, cdt(2025, 9, 2, 17, 0), "D on 09-02 from Tue 17:00");
    check_span(probe, kDOn, cdt(2025, 9, 3, 17, 0), cdt(2025, 9, 3, 19, 0),
               kD0903, kD0902, cdt(2025, 9, 3, 17, 0),
               "D on trailing 09-03 (whole native day) from Wed 17:00");

    // W: the week of 08-25 opened before the range start (absent); the week
    // of 09-01 opens on the Sunday stamp and never completes on the tape --
    // lookahead_on leaks it from Sun 17:00.
    check_span(probe, kWOff, cdt(2025, 8, 26, 19, 0), cdt(2025, 9, 3, 19, 0),
               kNa, kNa, 0, "W off na for the whole tape");
    check_span(probe, kWOn, cdt(2025, 8, 26, 19, 0), cdt(2025, 8, 29, 15, 45),
               kNa, kNa, 0, "W on na through the partial first week");
    check_span(probe, kWOn, cdt(2025, 8, 31, 17, 0), cdt(2025, 9, 3, 19, 0),
               kW0901, kNa, cdt(2025, 8, 31, 17, 0),
               "W on week of 09-01 from Sun 17:00 (native days 08-31 .. 09-04)");
}

// ---- Thanksgiving 2025: Wed 17:00 -> Thu 11:45, Thu 17:00 -> Fri 12:00 ----

const Ohlc kD1125{6781.0, 6846.75, 6778.25, 6828.0};
const Ohlc kD1126{6830.25, 6863.75, 6824.25, 6859.5};  // Wed 17:00 .. Fri 12:00
const Ohlc kD1130{6854.75, 6864.5, 6802.0, 6826.75};
const Ohlc kD1201{6829.0, 6863.5, 6812.25, 6840.25};
const Ohlc kD1202{6843.0, 6873.25, 6817.5, 6862.0};
const Ohlc kW1201{6854.75, 6905.0, 6802.0, 6878.25};   // native days 11-30 .. 12-04

void test_thanksgiving_merges_into_the_half_day() {
    DProbe probe;
    probe.sites = kSites;
    install_daily(probe, vec(esd_data::kEs1DNov));
    run_es15(probe, vec(esd_data::kEs15Nov), utc_ms(2025, 11, 25));
    CHECK(probe.rows.size() == 479, "479 chart bars (the tape's)");
    CHECK(probe.native_security_misses() == 0, "every bucket found its native bar");
    // D off 4 (11-25, 11-26, 11-30, 12-01) + D on 5 + W on 1.
    CHECK(probe.native_security_substitutions() == 10, "10 substitutions");

    // (d) the registry hole: no bars Thu 20:45 -> Fri 07:15.
    CHECK(probe.has_row(cst(2025, 11, 27, 20, 30)), "Thu 20:30 traded");
    CHECK(!probe.has_row(cst(2025, 11, 27, 20, 45)), "Thu 20:45 is missing");
    CHECK(!probe.has_row(cst(2025, 11, 28, 7, 15)), "Fri 07:15 is missing");
    CHECK(probe.has_row(cst(2025, 11, 28, 7, 30)), "Fri 07:30 traded");

    check_span(probe, kDOff, cst(2025, 11, 24, 18, 0), cst(2025, 11, 26, 15, 30),
               kNa, kNa, 0, "D off na through Wed 11-26 15:30");
    // (a) the 11-25 bar on Wed 15:45, then held through the holiday
    // session's pause (Thu 11:45), its 17:00 reopen, the registry hole and
    // the Friday morning -- (c) + (d): none of them completes the day.
    check_span(probe, kDOff, cst(2025, 11, 26, 15, 45), cst(2025, 11, 28, 11, 45),
               kD1125, kNa, cst(2025, 11, 25, 17, 0),
               "D off 11-25 from Wed 15:45 through Fri 11:45");
    check_ohlc(probe.at(cst(2025, 11, 27, 11, 45), kDOff).x0, kD1125,
               "no advance on the Thanksgiving pause bar");
    check_ohlc(probe.at(cst(2025, 11, 27, 17, 0), kDOff).x0, kD1125,
               "no advance on the Thu 17:00 reopen");
    check_ohlc(probe.at(cst(2025, 11, 27, 20, 30), kDOff).x0, kD1125,
               "no advance on the last bar before the registry hole");
    check_ohlc(probe.at(cst(2025, 11, 28, 7, 30), kDOff).x0, kD1125,
               "no advance on the first bar after the registry hole");
    // (b) + (c): the merged bar (Wed 17:00 -> Fri 12:15, one daily bar)
    // advances on the half-day's 12:00 bar, stamped Wed 17:00.
    check_span(probe, kDOff, cst(2025, 11, 28, 12, 0), cst(2025, 12, 1, 15, 30),
               kD1126, kD1125, cst(2025, 11, 26, 17, 0),
               "D off merged 11-26 bar from Fri 12:00, [1] = 11-25");
    CHECK(same(probe.at(cst(2025, 11, 28, 12, 0), kDOff).v0, 460053.0),
          "merged bar volume (native)");
    CHECK(!same(probe.at(cst(2025, 11, 28, 12, 0), kDOff).x0.c, 6857.25),
          "close is the settlement 6859.5, not the 15m print 6857.25");
    check_span(probe, kDOff, cst(2025, 12, 1, 15, 45), cst(2025, 12, 2, 15, 30),
               kD1130, kD1126, cst(2025, 11, 30, 17, 0),
               "D off 11-30 from Mon 15:45, [1] = the merged bar");
    check_span(probe, kDOff, cst(2025, 12, 2, 15, 45), cst(2025, 12, 2, 18, 0),
               kD1201, kD1130, cst(2025, 12, 1, 17, 0),
               "D off 12-01 from Tue 15:45 (the trailing Tue 17:00 day is open)");

    // lookahead_on: the merged bar from Wed 17:00, held through the pause,
    // the reopen and the hole.
    check_span(probe, kDOn, cst(2025, 11, 24, 18, 0), cst(2025, 11, 25, 15, 45),
               kNa, kNa, 0, "D on na through the partial first day");
    check_span(probe, kDOn, cst(2025, 11, 25, 17, 0), cst(2025, 11, 26, 15, 45),
               kD1125, kNa, cst(2025, 11, 25, 17, 0), "D on 11-25 from Tue 17:00");
    check_span(probe, kDOn, cst(2025, 11, 26, 17, 0), cst(2025, 11, 28, 12, 0),
               kD1126, kD1125, cst(2025, 11, 26, 17, 0),
               "D on merged bar from Wed 17:00 through Fri 12:00");
    check_span(probe, kDOn, cst(2025, 11, 30, 17, 0), cst(2025, 12, 1, 15, 45),
               kD1130, kD1126, cst(2025, 11, 30, 17, 0), "D on 11-30 from Sun 17:00");
    check_span(probe, kDOn, cst(2025, 12, 1, 17, 0), cst(2025, 12, 2, 15, 45),
               kD1201, kD1130, cst(2025, 12, 1, 17, 0), "D on 12-01 from Mon 17:00");
    check_span(probe, kDOn, cst(2025, 12, 2, 17, 0), cst(2025, 12, 2, 18, 0),
               kD1202, kD1201, cst(2025, 12, 2, 17, 0),
               "D on trailing 12-02 (whole native day)");

    // W: the Thanksgiving week opened Sun 11-23 (absent); the week of 12-01
    // from Sun 11-30 17:00 under lookahead_on.
    check_span(probe, kWOff, cst(2025, 11, 24, 18, 0), cst(2025, 12, 2, 18, 0),
               kNa, kNa, 0, "W off na for the whole tape");
    check_span(probe, kWOn, cst(2025, 11, 24, 18, 0), cst(2025, 11, 28, 12, 0),
               kNa, kNa, 0, "W on na through the Thanksgiving week");
    check_span(probe, kWOn, cst(2025, 11, 30, 17, 0), cst(2025, 12, 2, 18, 0),
               kW1201, kNa, cst(2025, 11, 30, 17, 0),
               "W on week of 12-01 from Sun 17:00");
}

// ---- Independence Day 2025: Thu 07-03 12:00 early close, then Thu 17:00 ->
// ---- Fri 11:45 holiday session merged with Sun 17:00 -> Mon 15:45 ---------

const Ohlc kD0701{6247.75, 6279.5, 6235.5, 6275.0};
const Ohlc kD0702{6276.5, 6333.25, 6270.5, 6324.25};   // Wed 17:00 .. Thu 12:00
const Ohlc kD0703{6307.75, 6315.0, 6246.25, 6276.0};   // TradingView's own: o = Sunday's
const Ohlc kD0707{6262.5, 6289.0, 6254.5, 6272.0};
const Ohlc kD0708{6272.0, 6315.25, 6260.0, 6307.25};
// The week of 07-07 = native days stamped 07-03 (Monday's) .. 07-10.
const Ohlc kW0707{6307.75, 6335.5, 6246.25, 6300.0};

void test_independence_day_merges_into_monday() {
    DProbe probe;
    probe.sites = kSites;
    install_daily(probe, vec(esd_data::kEs1DJul));
    run_es15(probe, vec(esd_data::kEs15Jul), utc_ms(2025, 7, 1));
    CHECK(probe.rows.size() == 522, "522 chart bars (the tape's)");
    CHECK(probe.native_security_misses() == 0, "every bucket found its native bar");
    // D off 4 (07-01, 07-02, 07-03, 07-07) + D on 5 + W on 1.
    CHECK(probe.native_security_substitutions() == 10, "10 substitutions");

    check_span(probe, kDOff, cdt(2025, 6, 30, 19, 0), cdt(2025, 7, 2, 15, 30),
               kNa, kNa, 0, "D off na through Wed 07-02 15:30");
    check_span(probe, kDOff, cdt(2025, 7, 2, 15, 45), cdt(2025, 7, 3, 11, 45),
               kD0701, kNa, cdt(2025, 7, 1, 17, 0), "D off 07-01 from Wed 15:45");
    // (b) the early close: the 07-02 bar advances on Thu 12:00 (closing
    // 12:15), then holds through the holiday session (Thu 17:00 -> Fri
    // 11:45) and the Sunday 17:00 reopen.
    CHECK(!probe.has_row(cdt(2025, 7, 3, 12, 15)), "Thu 07-03 closes at 12:15");
    check_span(probe, kDOff, cdt(2025, 7, 3, 12, 0), cdt(2025, 7, 7, 15, 30),
               kD0702, kD0701, cdt(2025, 7, 2, 17, 0),
               "D off 07-02 from Thu 12:00 through Mon 15:30");
    CHECK(same(probe.at(cdt(2025, 7, 3, 12, 0), kDOff).v0, 750998.0),
          "early-close day volume (native)");
    check_ohlc(probe.at(cdt(2025, 7, 4, 11, 45), kDOff).x0, kD0702,
               "no advance on the Independence-Day pause bar");
    check_ohlc(probe.at(cdt(2025, 7, 6, 17, 0), kDOff).x0, kD0702,
               "no advance on the Sunday 17:00 reopen");
    // (c) the merged bar advances on Mon 07-07 15:45, stamped Thu 17:00,
    // carrying TradingView's own values: o = the Sunday open, h 6315 below
    // the holiday session's 6322.75 -- not the chart aggregate.
    check_span(probe, kDOff, cdt(2025, 7, 7, 15, 45), cdt(2025, 7, 8, 15, 30),
               kD0703, kD0702, cdt(2025, 7, 3, 17, 0),
               "D off merged 07-03 bar from Mon 15:45, [1] = 07-02");
    CHECK(same(probe.at(cdt(2025, 7, 7, 15, 45), kDOff).v0, 1376613.0),
          "merged bar volume (native)");
    {
        const std::vector<Bar> chart = vec(esd_data::kEs15Jul);
        const Ohlc merged_15m = aggregate(chart, cdt(2025, 7, 3, 17, 0),
                                          cdt(2025, 7, 7, 15, 45));
        CHECK(same(merged_15m.o, 6320.75) && same(merged_15m.h, 6322.75),
              "the chart aggregate of the merged span opens 6320.75 / high 6322.75");
        CHECK(!same(probe.at(cdt(2025, 7, 7, 15, 45), kDOff).x0.h, merged_15m.h),
              "the merged bar is the native bar, not the chart aggregate");
    }
    check_span(probe, kDOff, cdt(2025, 7, 8, 15, 45), cdt(2025, 7, 8, 19, 0),
               kD0707, kD0703, cdt(2025, 7, 7, 17, 0),
               "D off 07-07 from Tue 15:45, [1] = the merged bar");

    // lookahead_on: the merged bar leaks from the holiday session's first
    // bar (Thu 17:00) and holds through the pause and the Sunday reopen.
    check_span(probe, kDOn, cdt(2025, 6, 30, 19, 0), cdt(2025, 7, 1, 15, 45),
               kNa, kNa, 0, "D on na through the partial first day");
    check_span(probe, kDOn, cdt(2025, 7, 1, 17, 0), cdt(2025, 7, 2, 15, 45),
               kD0701, kNa, cdt(2025, 7, 1, 17, 0), "D on 07-01 from Tue 17:00");
    check_span(probe, kDOn, cdt(2025, 7, 2, 17, 0), cdt(2025, 7, 3, 12, 0),
               kD0702, kD0701, cdt(2025, 7, 2, 17, 0), "D on 07-02 from Wed 17:00");
    check_span(probe, kDOn, cdt(2025, 7, 3, 17, 0), cdt(2025, 7, 7, 15, 45),
               kD0703, kD0702, cdt(2025, 7, 3, 17, 0),
               "D on merged bar from Thu 17:00 through Mon 15:45");
    check_span(probe, kDOn, cdt(2025, 7, 7, 17, 0), cdt(2025, 7, 8, 15, 45),
               kD0707, kD0703, cdt(2025, 7, 7, 17, 0), "D on 07-07 from Mon 17:00");
    check_span(probe, kDOn, cdt(2025, 7, 8, 17, 0), cdt(2025, 7, 8, 19, 0),
               kD0708, kD0707, cdt(2025, 7, 8, 17, 0),
               "D on trailing 07-08 (whole native day)");

    // W (the engine's derivation): the merged bar is Monday 07-07's, so the
    // week of 06-30 ends on the early close Thu 12:00 (absent here: it
    // opened Sun 06-29, before the range start) and the week of 07-07 opens
    // on the Thu 07-03 17:00 stamp -- lookahead_on leaks it from there.
    check_span(probe, kWOff, cdt(2025, 6, 30, 19, 0), cdt(2025, 7, 8, 19, 0),
               kNa, kNa, 0, "W off na for the whole tape");
    check_span(probe, kWOn, cdt(2025, 6, 30, 19, 0), cdt(2025, 7, 3, 12, 0),
               kNa, kNa, 0, "W on na through the week of 06-30 (ends Thu 12:00)");
    check_span(probe, kWOn, cdt(2025, 7, 3, 17, 0), cdt(2025, 7, 8, 19, 0),
               kW0707, kNa, cdt(2025, 7, 3, 17, 0),
               "W on week of 07-07 from the Thu 17:00 stamp");
}

// ---- weekdays and Friday -> Sunday (the esd-aug tape, wm_data feeds) -------

void test_weekdays_and_friday_advance_on_the_15_45_bar() {
    DProbe probe;
    probe.sites = {{"D", false}, {"D", true}};
    install_daily(probe, vec(wm_data::kEs1DAug));
    run_es15(probe, vec(wm_data::kEs15Aug), utc_ms(2025, 8, 6));
    CHECK(probe.rows.size() == 728, "728 chart bars");
    CHECK(probe.native_security_misses() == 0, "every bucket found its native bar");
    // D off 7 (08-06 .. 08-14, Friday's on the chart's last bar) + D on 7.
    CHECK(probe.native_security_substitutions() == 14, "14 substitutions");

    struct Day {
        int m, d;            // stamp date (the 17:00 CT open)
        int nm, nd;          // the session's last chart bar's date (15:45 CT)
        Ohlc bar;
        double last_15m_close;
    };
    const Day days[] = {
        {8, 6, 8, 7, {6371.0, 6426.75, 6334.5, 6366.5}, 6373.75},
        {8, 7, 8, 8, {6372.5, 6425.75, 6369.25, 6413.5}, 6425.25},
        {8, 10, 8, 11, {6422.75, 6431.5, 6387.5, 6399.75}, 6396.5},
        {8, 11, 8, 12, {6396.0, 6470.0, 6391.25, 6468.5}, 6468.5},
        {8, 12, 8, 13, {6468.0, 6502.5, 6461.0, 6488.75}, 6484.0},
        {8, 13, 8, 14, {6485.0, 6496.0, 6453.25, 6490.5}, 6489.75},
        {8, 14, 8, 15, {6489.25, 6508.75, 6461.5, 6471.5}, 6467.25},
    };
    // (f) the 08-05 session in progress at the range start is absent, on
    // its completion bar Wed 08-06 15:45 too.
    check_span(probe, kDOff, cdt(2025, 8, 5, 19, 0), cdt(2025, 8, 7, 15, 30),
               kNa, kNa, 0, "D off na through Thu 08-07 15:30");
    CHECK(probe.has_row(cdt(2025, 8, 6, 15, 45)), "08-06 15:45 is a chart bar");
    check_span(probe, kDOn, cdt(2025, 8, 5, 19, 0), cdt(2025, 8, 6, 15, 45),
               kNa, kNa, 0, "D on na through the partial first day");
    const Ohlc* prev = &kNa;
    for (std::size_t i = 0; i < sizeof(days) / sizeof(days[0]); ++i) {
        const Day& d = days[i];
        const int64_t stamp = cdt(2025, d.m, d.d, 17, 0);
        const int64_t last = cdt(2025, d.nm, d.nd, 15, 45);
        // (a) lookahead_off: na / the previous day until 15:30, the day on
        // its 15:45 bar; the next session's 17:00 open (Sunday's included)
        // changes nothing until the next 15:45.
        const int64_t hold_to = (i + 1 < sizeof(days) / sizeof(days[0]))
            ? cdt(2025, days[i + 1].nm, days[i + 1].nd, 15, 30) : last;
        check_span(probe, kDOff, last, hold_to, d.bar, *prev, stamp,
                   "D off advances on the 15:45 CT bar and holds");
        // The settlement differs from the 15m last print on six of the
        // seven days (08-12's happen to coincide at 6468.5).
        if (!same(d.bar.c, d.last_15m_close)) {
            CHECK(!same(probe.at(last, kDOff).x0.c, d.last_15m_close),
                  "D off close is the settlement, not the 15m last print");
        }
        // (e) lookahead_on: the whole day from its first 17:00 bar.
        check_span(probe, kDOn, stamp, last, d.bar, *prev, stamp,
                   "D on the whole day from its 17:00 CT open");
        prev = &d.bar;
    }
    // The Friday -> Sunday reopen: Sun 08-10 17:00 still reads Friday's bar
    // under lookahead_off and Monday's under lookahead_on.
    check_ohlc(probe.at(cdt(2025, 8, 10, 17, 0), kDOff).x0, days[1].bar,
               "D off holds Friday on the Sunday 17:00 reopen");
    check_ohlc(probe.at(cdt(2025, 8, 10, 17, 0), kDOn).x0, days[2].bar,
               "D on flips to Monday's bar on the Sunday 17:00 reopen");
    // Friday's bar on the chart's last bar (no next bar; the close reaches
    // the session's 16:00 close).
    CHECK(probe.at(cdt(2025, 8, 15, 15, 45), kDOff).complete0,
          "Friday completes on the chart's last bar");
}

// ---- control: no native feed keeps today's aggregator ----------------------

void test_control_without_native_feed_splits_the_holiday_session() {
    DProbe probe;
    probe.sites = {{"D", false}, {"D", true}};
    const std::vector<Bar> chart = vec(esd_data::kEs15Sep);
    run_es15(probe, chart, utc_ms(2025, 8, 27));
    CHECK(probe.native_security_substitutions() == 0, "nothing substituted");
    CHECK(probe.native_security_misses() == 0, "nothing missed");
    // The nominal session calendar: the holiday session (Sun 17:00 -> Mon
    // 11:45) is its own session-day bucket, completed on the pause bar when
    // the Mon 17:00 reopen opens the next session-day; values are the chart
    // aggregates (the 15m last print, the 15m volume).
    const Ohlc d0827 = aggregate(chart, cdt(2025, 8, 27, 17, 0), cdt(2025, 8, 28, 15, 45));
    const Ohlc d0828 = aggregate(chart, cdt(2025, 8, 28, 17, 0), cdt(2025, 8, 29, 15, 45));
    const Ohlc holiday = aggregate(chart, cdt(2025, 8, 31, 17, 0), cdt(2025, 9, 1, 11, 45));
    const Ohlc d0901 = aggregate(chart, cdt(2025, 9, 1, 17, 0), cdt(2025, 9, 2, 15, 45));
    check_ohlc(holiday, Ohlc{6478.75, 6491.5, 6459.5, 6483.0}, "15m-built holiday session");
    check_ohlc(d0901, Ohlc{6480.75, 6482.25, 6371.75, 6447.25}, "15m-built Mon 17:00 -> Tue 15:45");
    check_span(probe, kDOff, cdt(2025, 8, 28, 15, 45), cdt(2025, 8, 29, 15, 30),
               d0827, kNa, cdt(2025, 8, 27, 17, 0), "control D off 08-27 from Thu 15:45");
    check_span(probe, kDOff, cdt(2025, 8, 29, 15, 45), cdt(2025, 9, 1, 11, 30),
               d0828, d0827, cdt(2025, 8, 28, 17, 0), "control D off 08-28 from Fri 15:45");
    check_span(probe, kDOff, cdt(2025, 9, 1, 11, 45), cdt(2025, 9, 2, 15, 30),
               holiday, d0828, cdt(2025, 8, 31, 17, 0),
               "control: the holiday session completes on its pause bar");
    check_span(probe, kDOff, cdt(2025, 9, 2, 15, 45), cdt(2025, 9, 3, 15, 30),
               d0901, holiday, cdt(2025, 9, 1, 17, 0),
               "control: Mon 17:00 opens its own session-day bucket");
    CHECK(same(probe.at(cdt(2025, 9, 2, 15, 45), kDOff).v0, 1615511.0),
          "control volume = the 15m sum");
    // lookahead_on control: the holiday session and the Mon 17:00 session
    // are two projected buckets.
    check_span(probe, kDOn, cdt(2025, 8, 31, 17, 0), cdt(2025, 9, 1, 11, 45),
               holiday, d0828, cdt(2025, 8, 31, 17, 0), "control D on holiday session");
    check_span(probe, kDOn, cdt(2025, 9, 1, 17, 0), cdt(2025, 9, 2, 15, 45),
               d0901, holiday, cdt(2025, 9, 1, 17, 0), "control D on Mon 17:00 session");
}

// ---- aggregator: the native period partition ------------------------------

void test_aggregator_native_periods() {
    const std::vector<Bar> chart = vec(esd_data::kEs15Sep);
    const std::vector<Bar> daily = vec(esd_data::kEs1DSep);
    std::vector<int64_t> stamps;
    std::vector<int64_t> trade_instants;
    {
        std::size_t j = 0;
        for (std::size_t k = 0; k < daily.size(); ++k) {
            const int64_t stamp = daily[k].timestamp;
            const int64_t next = k + 1 < daily.size()
                ? daily[k + 1].timestamp : INT64_MAX;
            int64_t last = stamp;
            while (j < chart.size() && chart[j].timestamp < next) {
                if (chart[j].timestamp >= stamp) last = chart[j].timestamp;
                ++j;
            }
            stamps.push_back(stamp);
            trade_instants.push_back(last);
        }
    }
    auto completions = [&](TimeframeAggregator& agg) {
        std::vector<int64_t> on;
        for (std::size_t i = 0; i < chart.size(); ++i) {
            const int64_t next = i + 1 < chart.size() ? chart[i + 1].timestamp : 0;
            if (agg.feed(chart[i], next).is_complete) on.push_back(chart[i].timestamp);
        }
        return on;
    };
    // Installed on a calendar D aggregator: the merged Labor-Day period.
    {
        TimeframeAggregator agg("D", "15", "America/Chicago", "1700-1600");
        CHECK(!agg.has_native_periods(), "no periods by default");
        agg.set_native_periods(stamps, trade_instants, CalendarPeriod::DAY);
        CHECK(agg.has_native_periods(), "periods installed");
        CHECK(agg.bar_label_ms(cdt(2025, 9, 1, 18, 0)) == cdt(2025, 8, 31, 17, 0),
              "Mon 18:00 is labelled by the Sunday stamp");
        CHECK(agg.bucket_open_ms(cdt(2025, 9, 2, 15, 45)) == cdt(2025, 8, 31, 17, 0),
              "Tue 15:45 opens on the Sunday stamp");
        CHECK(!agg.period_changes(cdt(2025, 9, 1, 11, 45), cdt(2025, 9, 1, 17, 0)),
              "the pause and the reopen are one period");
        CHECK(agg.period_changes(cdt(2025, 9, 2, 15, 45), cdt(2025, 9, 2, 17, 0)),
              "Tue 15:45 and Tue 17:00 are two periods");
        CHECK(agg.bucket_open_ms(cdt(2025, 8, 12, 12, 0)) == cdt(2025, 8, 11, 17, 0),
              "before the first stamp the nominal session-day key stands");
        const auto on = completions(agg);
        const std::vector<int64_t> want = {
            cdt(2025, 8, 27, 15, 45), cdt(2025, 8, 28, 15, 45), cdt(2025, 8, 29, 15, 45),
            cdt(2025, 9, 2, 15, 45), cdt(2025, 9, 3, 15, 45)};
        CHECK(on == want, "D completes on each session's last bar, not on the pause bar");
        CHECK(agg.last_completed().timestamp == cdt(2025, 9, 2, 17, 0),
              "the last completed bucket is labelled by its stamp");
    }
    // Without periods: today's rule splits the holiday session on its pause.
    {
        TimeframeAggregator agg("D", "15", "America/Chicago", "1700-1600");
        const auto on = completions(agg);
        CHECK(std::find(on.begin(), on.end(), cdt(2025, 9, 1, 11, 45)) != on.end(),
              "the nominal calendar completes the holiday session on Mon 11:45");
        CHECK(on.size() == 6, "six nominal session-days complete");
    }
    // A W aggregator groups the stamps by their trade date's week.
    {
        TimeframeAggregator agg("W", "15", "America/Chicago", "1700-1600");
        agg.set_native_periods(stamps, trade_instants, CalendarPeriod::DAY);
        CHECK(agg.bucket_open_ms(cdt(2025, 9, 3, 12, 0)) == cdt(2025, 8, 31, 17, 0),
              "the week of 09-01 opens on the Sunday stamp");
        CHECK(agg.bucket_open_ms(cdt(2025, 8, 29, 15, 45)) == cdt(2025, 8, 24, 17, 0),
              "the week of 08-25 opens on its Sunday stamp");
        CHECK(agg.bar_label_ms(cdt(2025, 9, 3, 12, 0)) == cdt(2025, 9, 2, 17, 0),
              "bar_label_ms is the day stamp");
        const auto on = completions(agg);
        CHECK(on.size() == 1 && on[0] == cdt(2025, 8, 29, 15, 45),
              "the week of 08-25 completes on Fri 15:45");
    }
    // The Independence-Day merge: the Thu 07-03 stamp is Monday's week.
    {
        const std::vector<Bar> jul = vec(esd_data::kEs15Jul);
        const std::vector<Bar> jul_daily = vec(esd_data::kEs1DJul);
        std::vector<int64_t> st;
        std::vector<int64_t> ti;
        std::size_t j = 0;
        for (std::size_t k = 0; k < jul_daily.size(); ++k) {
            const int64_t stamp = jul_daily[k].timestamp;
            const int64_t next = k + 1 < jul_daily.size()
                ? jul_daily[k + 1].timestamp : INT64_MAX;
            int64_t last = stamp;
            while (j < jul.size() && jul[j].timestamp < next) {
                if (jul[j].timestamp >= stamp) last = jul[j].timestamp;
                ++j;
            }
            st.push_back(stamp);
            ti.push_back(last);
        }
        TimeframeAggregator agg("W", "15", "America/Chicago", "1700-1600");
        agg.set_native_periods(st, ti, CalendarPeriod::DAY);
        CHECK(agg.bucket_open_ms(cdt(2025, 7, 4, 10, 0)) == cdt(2025, 7, 3, 17, 0),
              "the holiday session's bars open the week of 07-07");
        CHECK(agg.bucket_open_ms(cdt(2025, 7, 3, 12, 0)) == cdt(2025, 6, 29, 17, 0),
              "Thu 12:00 is still the week of 06-30");
        CHECK(agg.period_changes(cdt(2025, 7, 3, 12, 0), cdt(2025, 7, 3, 17, 0)),
              "the week of 06-30 ends on the early close");
        CHECK(!agg.period_changes(cdt(2025, 7, 4, 11, 45), cdt(2025, 7, 6, 17, 0)),
              "the holiday session and the Sunday reopen are one week");
    }
    // The partition covers the feed: past the last stamp's nominal period
    // the nominal calendar stands (a feed ending on the Sunday stamp does
    // not carry the Labor-Day merge; a chart day without a native bar is
    // its own session-day bucket -- test_native_security_feed pins the
    // aggregate it then keeps).
    {
        std::vector<int64_t> st;
        std::vector<int64_t> ti;
        for (std::size_t k = 0; k < stamps.size(); ++k) {
            if (stamps[k] > cdt(2025, 8, 31, 17, 0)) break;
            st.push_back(stamps[k]);
            ti.push_back(trade_instants[k]);
        }
        TimeframeAggregator agg("D", "15", "America/Chicago", "1700-1600");
        agg.set_native_periods(st, ti, CalendarPeriod::DAY);
        CHECK(agg.bucket_open_ms(cdt(2025, 9, 1, 11, 45)) == cdt(2025, 8, 31, 17, 0),
              "the last stamp holds its own session-day");
        CHECK(agg.bucket_open_ms(cdt(2025, 9, 1, 17, 0)) == cdt(2025, 9, 1, 17, 0),
              "past the last stamp's session close the nominal day stands");
        CHECK(agg.bar_label_ms(cdt(2025, 9, 3, 12, 0)) == cdt(2025, 9, 2, 17, 0),
              "nominal labels past the feed");
        CHECK(agg.period_changes(cdt(2025, 9, 1, 11, 45), cdt(2025, 9, 1, 17, 0)),
              "without the next stamp the merge is not asserted");
    }
    // RATIO / PASSTHROUGH and malformed installs are inert.
    {
        TimeframeAggregator ratio("60", "15", "America/Chicago", "1700-1600");
        ratio.set_native_periods(stamps, trade_instants, CalendarPeriod::DAY);
        CHECK(!ratio.has_native_periods(), "a ratio aggregator ignores the stamps");
        TimeframeAggregator agg("D", "15", "America/Chicago", "1700-1600");
        agg.set_native_periods(stamps, std::vector<int64_t>(stamps.size() - 1, 0), CalendarPeriod::DAY);
        CHECK(!agg.has_native_periods(), "mismatched sizes install nothing");
        std::vector<int64_t> unsorted = stamps;
        std::swap(unsorted[0], unsorted[1]);
        agg.set_native_periods(unsorted, trade_instants, CalendarPeriod::DAY);
        CHECK(!agg.has_native_periods(), "non-increasing stamps install nothing");
    }
}

}  // namespace

int main() {
    test_labor_day_merges_into_tuesday();
    test_thanksgiving_merges_into_the_half_day();
    test_independence_day_merges_into_monday();
    test_weekdays_and_friday_advance_on_the_15_45_bar();
    test_control_without_native_feed_splits_the_holiday_session();
    test_aggregator_native_periods();
    std::printf("test_native_daily_holiday: %d checks, %d failures\n", checks, failures);
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
