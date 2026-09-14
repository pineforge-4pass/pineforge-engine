// request.security(syminfo.tickerid, "D" | "W", x) on OANDA:XAUUSD 15m -- a cfd
// quote stream on the wrapped 1800-1700 America/New_York session, whose
// holiday early closes TradingView's session template does NOT know:
//   (a) lookahead_off advances ON the chart bar whose close reaches the
//       period's NOMINAL close, 17:00 ET: the 16:45 bar of every regular day
//       (D) and Friday's 16:45 bar (W, Fri 2026-02-13 with cW 5042.74);
//   (b) when no chart bar reaches it -- Fri 2025-07-04 (Independence Day,
//       last bar 12:45 ET), Mon 2026-02-16 (Presidents' Day, last bar 14:15
//       ET) -- the value surfaces on the NEXT session's FIRST bar, 18:00 ET,
//       never on the shortened session's last bar: the D stamped Thu 07-03
//       17:00 (c 3336.615) on Sun 07-06 18:00, the D stamped Sun 02-15 17:00
//       (c 4992.005) on Mon 02-16 18:00, the W stamped Sun 06-29 17:00 (c =
//       the 07-04 early close) on Sun 07-06 18:00;
//   (c) values = the registry's native 1D bar = the 15m session's aggregate
//       (OANDA prints no settlement), time = the 17:00 ET stamp one hour
//       before the session trades, x[1] = the previous period on the same
//       bar;
//   (d) lookahead_on: the period's FINAL values from its first 18:00 bar,
//       holding the day in progress through the early close;
//   (e) the period in progress at the deep-backtest range start is absent
//       (KI-55, as on ES / F).
// Oracle: the eight qty-encoded lab tv tapes of the oanda-d-timing pin
// (ledger note log-20260905t034240z-30be11fe, 2026-09-05; scratchpad
// r6/pins/out-oanda-{jul,julw,feb,febw}-par{0,1}, decoded into
// oanda-<win>-table.txt; the tables print 6 significant digits, the tapes
// carry the half-pips the registry bars hold), replayed here over the
// registry feeds those tapes were decoded against
// (test_oanda_lazy_close_data.hpp).
//
// CONTRAST: on exchange sessions -- NYSE:F (stock, the 12:45 ET half-day
// bar) and CME_MINI:ES1! (futures, the 11:45 CT early-close bar) --
// TradingView's session template carries the early close and the shortened
// session's last chart bar completes the period (test_native_wm_buckets,
// test_native_daily_holiday; engine f725bc3's next-input-bar rule). The gate
// is the symbol kind, not the session shape: BacktestEngine::
// session_template_knows_early_close (syminfo.type, false for forex / cfd /
// crypto) -> TimeframeAggregator::set_early_close_completes. Engine f725bc3
// applied the rule to every declared session and closed the 07-04 day on
// the 12:45 bar and the 02-16 day on the 14:15 bar -- the mukhlisilahi
// universal-backtest-pro XAUUSD@15 regression (two trades one bar early).
//
// THE 1D CHART (the split-feed path: native daily bars, the 15m slice
// advancing request.security). What TradingView compares is the CALLING
// chart bar's time_close against the period's nominal close, and the daily
// bar of an early-close day keeps its nominal 17:00 ET time_close: on the
// OANDA:XAUUSD 1D chart (lab tv oanda1d-{jul,feb,novm,decm}-par{0,1},
// 2026-09-05, decoded into oanda1d-<win>-table.txt beside each tape)
//   (f) "W" lookahead_off advances on FRIDAY's daily bar -- the 07-03-stamped
//       bar of the 07-04 early close included (tW Sun 06-29 17:00, cW
//       3336.61 = that early close, cW[1] 3274.18; its time_close 70417) --
//       exactly as on the regular Fridays (06-13 3432.84, 06-20 3368.75,
//       06-27 3274.18, 07-11 3355.66, 02-13 5042.74, 02-20, 02-27, 03-06);
//   (g) "D" is the chart bar itself on every daily bar, the early-close days
//       07-04 (c0 3336.61) and 02-16 (c0 4992.01) included, [1] = the
//       previous daily bar;
//   (h) "M" completes on the month's last daily bar (Nov 2025 on the
//       11-27-stamped Fri 11-28 bar, tM Sun 11-02 17:00, cM 4215.82; Dec on
//       the 12-30-stamped Wed 12-31 bar, cM 4322.61), the month in progress
//       at the range start absent; "240" reads the last 240 bucket the day's
//       data reaches (13:00 ET on a regular day, 09:00 on 07-04).
// So an OTC period the next input bar leaves completes on the slice's last
// bar iff the calling chart bar's nominal close reaches the period's
// (TimeframeAggregator::feed(bar, next_input_ms, calling_close_ms), set per
// native chart bar by feed_aux_security_for_chart_bar); on the 15m chart the
// calling bar is the input bar and nothing changes.

#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include "test_oanda_lazy_close_data.hpp"
#include "test_native_wm_buckets_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace pineforge;

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
constexpr int64_t kHour = 60 * kMinute;

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
// Fixed offsets: neither window straddles a DST edge.
int64_t edt(int y, int m, int d, int h, int mi) { return utc_ms(y, m, d, h + 4, mi); }
int64_t est(int y, int m, int d, int h, int mi) { return utc_ms(y, m, d, h + 5, mi); }

bool same(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::abs(a - b) < 1e-9;
}

struct Ohlc {
    double o, h, l, c;
};
const Ohlc kNa{na<double>(), na<double>(), na<double>(), na<double>()};

bool same(const Ohlc& a, const Ohlc& b) {
    return same(a.o, b.o) && same(a.h, b.h) && same(a.l, b.l) && same(a.c, b.c);
}

// What the strategy body reads on one chart bar for one security site: x
// (the current slot), x[1] (the previous slot), time(x), volume(x).
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
class LazyProbe final : public pineforge::source::PineStrategyHost {
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

// The chart from its first bar stamped at or after `from`.
std::vector<Bar> from(const std::vector<Bar>& chart, int64_t from_ts) {
    std::vector<Bar> out;
    for (const Bar& b : chart) {
        if (b.timestamp >= from_ts) out.push_back(b);
    }
    return out;
}

// The bars stamped in [first, last].
std::vector<Bar> between(const std::vector<Bar>& bars, int64_t first, int64_t last) {
    std::vector<Bar> out;
    for (const Bar& b : bars) {
        if (b.timestamp >= first && b.timestamp <= last) out.push_back(b);
    }
    return out;
}

void run15(LazyProbe& probe, const std::vector<Bar>& chart, const char* tz,
           const char* session, const char* type, int64_t range_start_ms) {
    probe.set_syminfo_timezone(tz);
    probe.set_syminfo_session(session);
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

// OANDA:XAUUSD, the lane as the campaign declares it: cfd on 1800-1700 ET.
void run_xau(LazyProbe& probe, const std::vector<Bar>& chart,
             int64_t range_start_ms, const char* type = "cfd") {
    run15(probe, chart, "America/New_York", "1800-1700", type, range_start_ms);
}

// The OANDA:XAUUSD 1D lane: native daily bars (stamped 17:00 ET) as the
// chart, the 15m feed as the auxiliary request.security slice -- the
// split-feed path every @1D lane runs.
void run_xau_daily(LazyProbe& probe, const std::vector<Bar>& daily,
                   const std::vector<Bar>& aux15, int64_t range_start_ms,
                   const char* type = "cfd") {
    probe.set_syminfo_timezone("America/New_York");
    probe.set_syminfo_session("1800-1700");
    probe.set_syminfo_type(type);
    probe.set_syminfo_metadata("security_range_start_na_warmup",
                               static_cast<double>(range_start_ms));
    probe.set_syminfo_metadata("historical_security_lookahead_projection", 1.0);
    CHECK(probe.set_aux_security_feed(aux15.data(), static_cast<int>(aux15.size()), "15"),
          "the 15m auxiliary feed installs");
    probe.run(daily.data(), static_cast<int>(daily.size()), "1D", "1D",
              false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(probe.last_error().empty(), probe.last_error().c_str());
}

void check_ohlc(const Ohlc& got, const Ohlc& want, const char* tag) {
    const bool ok = same(got, want);
    if (!ok) {
        std::printf("    %s: got o %.6f h %.6f l %.6f c %.6f, want o %.6f h %.6f l %.6f c %.6f\n",
                    tag, got.o, got.h, got.l, got.c, want.o, want.h, want.l, want.c);
    }
    CHECK(ok, tag);
}

// Every chart bar with ts in [from, to] reads `x0` (and `x1`) on `site`.
void check_span(const LazyProbe& p, std::size_t site, int64_t from, int64_t to,
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

// The native 1D bar carrying `stamp` (the 17:00 ET roll).
Bar daily_bar(const std::vector<Bar>& daily, int64_t stamp) {
    for (const Bar& b : daily) {
        if (b.timestamp == stamp) return b;
    }
    std::printf("    no 1D bar stamped %lld\n", static_cast<long long>(stamp));
    ++failures;
    return Bar{};
}
Ohlc daily(const std::vector<Bar>& d, int64_t stamp) {
    const Bar b = daily_bar(d, stamp);
    return Ohlc{b.open, b.high, b.low, b.close};
}
// The native 1D bars stamped in [first, last] aggregated: TradingView's W.
Ohlc weekly(const std::vector<Bar>& daily, int64_t first, int64_t last) {
    Ohlc out = kNa;
    bool open = false;
    for (const Bar& b : daily) {
        if (b.timestamp < first || b.timestamp > last) continue;
        if (!open) {
            out = Ohlc{b.open, b.high, b.low, b.close};
            open = true;
        } else {
            out.h = std::max(out.h, b.high);
            out.l = std::min(out.l, b.low);
            out.c = b.close;
        }
    }
    return out;
}

// The chart's own aggregate of [from, to] (o/h/l/c and the volume sum).
Bar aggregate(const std::vector<Bar>& bars, int64_t from, int64_t to) {
    Bar out{};
    out.open = out.high = out.low = out.close = out.volume = na<double>();
    bool first = true;
    for (const Bar& b : bars) {
        if (b.timestamp < from || b.timestamp > to) continue;
        if (first) {
            out = b;
            first = false;
        } else {
            out.high = std::max(out.high, b.high);
            out.low = std::min(out.low, b.low);
            out.close = b.close;
            out.volume += b.volume;
        }
    }
    return out;
}

enum Site : std::size_t { kDOff = 0, kDOn = 1, kWOff = 2, kWOn = 3 };

// ---- July 2025: Independence Day, the 12:45 ET early close ----------------

// The tape's chart: Sun 06-29 18:00 .. Tue 07-08 20:00 ET, TV range from
// Sat 2025-06-28 (nothing in progress at the range start).
void test_july_independence_day() {
    LazyProbe probe;
    probe.sites = {{"D", false}, {"D", true}, {"W", false}, {"W", true}};
    const std::vector<Bar> chart = vec(oanda_data::kXau15Jul);
    const std::vector<Bar> d1 = vec(oanda_data::kXau1DJul);
    run_xau(probe, chart, utc_ms(2025, 6, 28));
    CHECK(probe.rows.size() == 637, "637 chart bars");
    CHECK(probe.has_row(edt(2025, 7, 4, 12, 45)), "the 07-04 session ends 12:45 ET");
    CHECK(!probe.has_row(edt(2025, 7, 4, 13, 0)), "no bar after 12:45 on 07-04");
    CHECK(probe.has_row(edt(2025, 7, 6, 18, 0)), "Sun 07-06 reopens 18:00 ET");

    // The pinned values: each day's D = the registry 1D bar (the 15m run's
    // aggregate, half-pips included: the table's 3309.64 is 3309.645).
    const Ohlc d0629 = daily(d1, edt(2025, 6, 29, 17, 0));
    const Ohlc d0630 = daily(d1, edt(2025, 6, 30, 17, 0));
    const Ohlc d0701 = daily(d1, edt(2025, 7, 1, 17, 0));
    const Ohlc d0702 = daily(d1, edt(2025, 7, 2, 17, 0));
    const Ohlc d0703 = daily(d1, edt(2025, 7, 3, 17, 0));  // the 07-04 trade date
    const Ohlc d0706 = daily(d1, edt(2025, 7, 6, 17, 0));
    const Ohlc d0707 = daily(d1, edt(2025, 7, 7, 17, 0));
    check_ohlc(d0629, Ohlc{3271.64, 3309.645, 3244.415, 3303.15}, "1D 06-29 = the table's 3271.64/3309.64/3244.41/3303.15");
    check_ohlc(d0703, Ohlc{3324.24, 3345.175, 3323.66, 3336.615}, "1D 07-03 = the early-close day, c 3336.61(5)");
    check_ohlc(d0706, Ohlc{3341.57, 3342.075, 3296.28, 3336.44}, "1D 07-06");

    // (a) D off advances on the 16:45 bar of every regular day, [1] = the
    // previous day on the same bar, time = the 17:00 ET stamp.
    check_span(probe, kDOff, edt(2025, 6, 29, 18, 0), edt(2025, 6, 30, 16, 30),
               kNa, kNa, 0, "D off na before the first day completes");
    check_span(probe, kDOff, edt(2025, 6, 30, 16, 45), edt(2025, 7, 1, 16, 30),
               d0629, kNa, edt(2025, 6, 29, 17, 0), "D off 06-29 from Mon 16:45");
    check_span(probe, kDOff, edt(2025, 7, 1, 16, 45), edt(2025, 7, 2, 16, 30),
               d0630, d0629, edt(2025, 6, 30, 17, 0), "D off 06-30 from Tue 16:45");
    check_span(probe, kDOff, edt(2025, 7, 2, 16, 45), edt(2025, 7, 3, 16, 30),
               d0701, d0630, edt(2025, 7, 1, 17, 0), "D off 07-01 from Wed 16:45");
    // (b) the early close: Thu 07-03 16:45 completes 07-02's day as usual;
    // the 07-03-stamped day (trading Thu 18:00 -> Fri 12:45) is NOT
    // completed on its last bar 12:45 -- the 12:45 bar still reads 07-02 --
    // and surfaces on Sun 07-06 18:00, the next session's first bar.
    check_span(probe, kDOff, edt(2025, 7, 3, 16, 45), edt(2025, 7, 4, 12, 45),
               d0702, d0701, edt(2025, 7, 2, 17, 0),
               "D off 07-02 from Thu 16:45 THROUGH the 07-04 12:45 early close");
    CHECK(probe.at(edt(2025, 7, 4, 12, 45), kDOff).complete0, "07-02's day stays the published one on 12:45");
    check_span(probe, kDOff, edt(2025, 7, 6, 18, 0), edt(2025, 7, 7, 16, 30),
               d0703, d0702, edt(2025, 7, 3, 17, 0),
               "D off 07-03 (the early-close day) from Sun 07-06 18:00");
    CHECK(probe.at(edt(2025, 7, 6, 18, 0), kDOff).complete0, "the early-close day publishes complete on Sun 18:00");
    CHECK(same(probe.at(edt(2025, 7, 6, 18, 0), kDOff).v0, 221059.0),
          "D off volume = the 1D bar's 221059 (the 15m sum of the shortened session)");
    check_span(probe, kDOff, edt(2025, 7, 7, 16, 45), edt(2025, 7, 8, 16, 30),
               d0706, d0703, edt(2025, 7, 6, 17, 0), "D off 07-06 from Mon 16:45, [1] = the early-close day");
    check_span(probe, kDOff, edt(2025, 7, 8, 16, 45), edt(2025, 7, 8, 20, 0),
               d0707, d0706, edt(2025, 7, 7, 17, 0), "D off 07-07 from Tue 16:45");

    // (d) D on: the day's final values from its first 18:00 bar; the
    // shortened day holds through 12:45 and flips on Sun 18:00. (time(x) is
    // not asserted on the lookahead_on sites: the finite-batch projection
    // labels a bucket by its first chart bar, 18:00 ET, where the tape
    // reads the 17:00 stamp -- a residual older than this change and
    // outside it; the lookahead_off sites carry the stamp.)
    check_span(probe, kDOn, edt(2025, 6, 29, 18, 0), edt(2025, 6, 30, 16, 45),
               d0629, kNa, 0, "D on 06-29 from Sun 18:00");
    check_span(probe, kDOn, edt(2025, 6, 30, 18, 0), edt(2025, 7, 1, 16, 45),
               d0630, d0629, 0, "D on 06-30 from Mon 18:00");
    check_span(probe, kDOn, edt(2025, 7, 2, 18, 0), edt(2025, 7, 3, 16, 45),
               d0702, d0701, 0, "D on 07-02 from Wed 18:00");
    check_span(probe, kDOn, edt(2025, 7, 3, 18, 0), edt(2025, 7, 4, 12, 45),
               d0703, d0702, 0,
               "D on 07-03 from Thu 18:00 through the 12:45 early close");
    check_span(probe, kDOn, edt(2025, 7, 6, 18, 0), edt(2025, 7, 7, 16, 45),
               d0706, d0703, 0, "D on 07-06 from Sun 18:00");
    check_span(probe, kDOn, edt(2025, 7, 7, 18, 0), edt(2025, 7, 8, 16, 45),
               d0707, d0706, 0, "D on 07-07 from Mon 18:00");
    // (The trailing partial day, 07-08 18:00 .. 20:00, reads TV's full day
    // 3313.7 on the tape -- data the chart does not hold; not asserted.)

    // W off: the week stamped Sun 06-29 17:00 holds the early close; no bar
    // reaches Fri 17:00, so it completes on Sun 07-06 18:00 -- na through
    // the 07-04 12:45 bar -- with c = the early close 3336.615 (cW 3336.61).
    const Ohlc w0629 = weekly(d1, edt(2025, 6, 29, 17, 0), edt(2025, 7, 3, 17, 0));
    check_ohlc(w0629, Ohlc{3271.64, 3365.92, 3244.415, 3336.615}, "the week 06-29 from the 1D bars");
    check_span(probe, kWOff, edt(2025, 6, 29, 18, 0), edt(2025, 7, 4, 12, 45),
               kNa, kNa, 0, "W off na through the 07-04 12:45 early close");
    check_span(probe, kWOff, edt(2025, 7, 6, 18, 0), edt(2025, 7, 8, 20, 0),
               w0629, kNa, edt(2025, 6, 29, 17, 0),
               "W off week 06-29 from Sun 07-06 18:00, dated Sun 06-29 17:00");
    CHECK(probe.at(edt(2025, 7, 6, 18, 0), kWOff).complete0, "W off publishes complete on Sun 18:00");
    CHECK(same(probe.at(edt(2025, 7, 6, 18, 0), kWOff).v0,
               349557.0 + 417491.0 + 398577.0 + 374883.0 + 221059.0),
          "W off volume = the sum of the daily volumes");
    // W on: the whole week from its Sunday 18:00 bar, through the early close.
    check_span(probe, kWOn, edt(2025, 6, 29, 18, 0), edt(2025, 7, 4, 12, 45),
               w0629, kNa, 0, "W on week 06-29 from Sun 06-29 18:00");
    // (The trailing partial week from 07-06 18:00 reads TV's final 3355.665
    // on the tape -- beyond the chart; not asserted.)
}

// ---- February 2026: Presidents' Day, the 14:15 ET early close -------------

// The tape's chart: Sun 02-08 18:00 .. Tue 02-17 19:00 ET, TV range from
// Sat 2026-02-07.
void test_february_presidents_day() {
    LazyProbe probe;
    probe.sites = {{"D", false}, {"D", true}, {"W", false}, {"W", true}};
    const std::vector<Bar> chart = vec(oanda_data::kXau15Feb);
    const std::vector<Bar> d1 = vec(oanda_data::kXau1DFeb);
    run_xau(probe, chart, utc_ms(2026, 2, 7));
    CHECK(probe.rows.size() == 639, "639 chart bars");
    CHECK(probe.has_row(est(2026, 2, 16, 14, 15)), "the 02-16 session ends 14:15 ET");
    CHECK(!probe.has_row(est(2026, 2, 16, 14, 30)), "no bar after 14:15 on 02-16");
    CHECK(probe.has_row(est(2026, 2, 16, 18, 0)), "Mon 02-16 reopens 18:00 ET");

    const Ohlc d0208 = daily(d1, est(2026, 2, 8, 17, 0));
    const Ohlc d0209 = daily(d1, est(2026, 2, 9, 17, 0));
    const Ohlc d0211 = daily(d1, est(2026, 2, 11, 17, 0));
    const Ohlc d0212 = daily(d1, est(2026, 2, 12, 17, 0));
    const Ohlc d0215 = daily(d1, est(2026, 2, 15, 17, 0));  // the 02-16 trade date
    const Ohlc d0216 = daily(d1, est(2026, 2, 16, 17, 0));
    check_ohlc(d0212, Ohlc{4888.315, 5046.48, 4888.315, 5042.74}, "1D 02-12 = Friday's day, c 5042.74");
    check_ohlc(d0215, Ohlc{5017.145, 5032.565, 4964.865, 4992.005}, "1D 02-15 = the early-close day, c 4992.00(5)");
    check_ohlc(d0216, Ohlc{4997.26, 5001.24, 4842.6, 4877.855}, "1D 02-16");

    // (a) regular days, including the normal Friday 02-13 16:45.
    check_span(probe, kDOff, est(2026, 2, 8, 18, 0), est(2026, 2, 9, 16, 30),
               kNa, kNa, 0, "D off na before the first day completes");
    check_span(probe, kDOff, est(2026, 2, 9, 16, 45), est(2026, 2, 10, 16, 30),
               d0208, kNa, est(2026, 2, 8, 17, 0), "D off 02-08 from Mon 16:45");
    check_span(probe, kDOff, est(2026, 2, 10, 16, 45), est(2026, 2, 11, 16, 30),
               d0209, d0208, est(2026, 2, 9, 17, 0), "D off 02-09 from Tue 16:45");
    // (b) Fri 02-13 16:45 completes Thursday's stamp; the 02-15-stamped day
    // (Sun 18:00 -> Mon 14:15) is NOT completed on its 14:15 bar and
    // surfaces on Mon 02-16 18:00.
    check_span(probe, kDOff, est(2026, 2, 13, 16, 45), est(2026, 2, 16, 14, 15),
               d0212, d0211, est(2026, 2, 12, 17, 0),
               "D off 02-12 from Fri 16:45 THROUGH the 02-16 14:15 early close");
    check_span(probe, kDOff, est(2026, 2, 16, 18, 0), est(2026, 2, 17, 16, 30),
               d0215, d0212, est(2026, 2, 15, 17, 0),
               "D off 02-15 (the early-close day) from Mon 02-16 18:00");
    CHECK(probe.at(est(2026, 2, 16, 18, 0), kDOff).complete0, "the early-close day publishes complete on Mon 18:00");
    CHECK(same(probe.at(est(2026, 2, 16, 18, 0), kDOff).v0, 396800.0),
          "D off volume = the 1D bar's 396800");
    check_span(probe, kDOff, est(2026, 2, 17, 16, 45), est(2026, 2, 17, 19, 0),
               d0216, d0215, est(2026, 2, 16, 17, 0), "D off 02-16 from Tue 16:45, [1] = the early-close day");

    // (d) D on holds the shortened day through 14:15, flips on Mon 18:00.
    check_span(probe, kDOn, est(2026, 2, 12, 18, 0), est(2026, 2, 13, 16, 45),
               d0212, d0211, 0, "D on 02-12 from Thu 18:00");
    check_span(probe, kDOn, est(2026, 2, 15, 18, 0), est(2026, 2, 16, 14, 15),
               d0215, d0212, 0,
               "D on 02-15 from Sun 18:00 through the 14:15 early close");
    check_span(probe, kDOn, est(2026, 2, 16, 18, 0), est(2026, 2, 17, 16, 45),
               d0216, d0215, 0, "D on 02-16 from Mon 18:00");

    // W off: the regular week completes on Fri 02-13 16:45 (cW 5042.74, tW
    // Sun 02-08 17:00) and holds; the week of 02-15 never completes on the
    // tape.
    const Ohlc w0208 = weekly(d1, est(2026, 2, 8, 17, 0), est(2026, 2, 12, 17, 0));
    check_ohlc(w0208, Ohlc{4989.93, 5119.345, 4878.5, 5042.74}, "the week 02-08 from the 1D bars");
    check_span(probe, kWOff, est(2026, 2, 8, 18, 0), est(2026, 2, 13, 16, 30),
               kNa, kNa, 0, "W off na before Fri 02-13 16:45");
    check_span(probe, kWOff, est(2026, 2, 13, 16, 45), est(2026, 2, 17, 19, 0),
               w0208, kNa, est(2026, 2, 8, 17, 0),
               "W off week 02-08 from Fri 02-13 16:45 (the nominal close), held through the 02-16 early close");
    CHECK(probe.at(est(2026, 2, 13, 16, 45), kWOff).complete0, "W off publishes complete on Fri 16:45");
    check_span(probe, kWOn, est(2026, 2, 8, 18, 0), est(2026, 2, 13, 16, 45),
               w0208, kNa, 0, "W on week 02-08 from Sun 02-08 18:00");
}

// ---- (e) the period in progress at the range start is absent --------------

void test_range_start_absent_period() {
    // The jul tape: TV range 2025-07-01 .. 07-09, the chart from Mon 06-30
    // 20:00 ET (07-01 00:00Z): the 06-30-stamped day and the 06-29 week are
    // in progress at the range start.
    {
        LazyProbe probe;
        probe.sites = {{"D", false}, {"D", true}, {"W", false}, {"W", true}};
        const std::vector<Bar> chart = from(vec(oanda_data::kXau15Jul), utc_ms(2025, 7, 1));
        const std::vector<Bar> d1 = vec(oanda_data::kXau1DJul);
        run_xau(probe, chart, utc_ms(2025, 7, 1));
        CHECK(probe.rows.size() == 537, "537 chart bars (the jul tape)");
        const Ohlc d0701 = daily(d1, edt(2025, 7, 1, 17, 0));
        const Ohlc d0702 = daily(d1, edt(2025, 7, 2, 17, 0));
        const Ohlc d0703 = daily(d1, edt(2025, 7, 3, 17, 0));
        check_span(probe, kDOff, edt(2025, 6, 30, 20, 0), edt(2025, 7, 2, 16, 30),
                   kNa, kNa, 0, "jul: D off na through the partial first day's completion bar 07-01 16:45");
        check_span(probe, kDOff, edt(2025, 7, 2, 16, 45), edt(2025, 7, 3, 16, 30),
                   d0701, kNa, edt(2025, 7, 1, 17, 0), "jul: D off 07-01 on Wed 16:45, no [1]");
        check_span(probe, kDOff, edt(2025, 7, 3, 16, 45), edt(2025, 7, 4, 12, 45),
                   d0702, d0701, edt(2025, 7, 2, 17, 0), "jul: D off 07-02 through the early close");
        check_span(probe, kDOff, edt(2025, 7, 6, 18, 0), edt(2025, 7, 7, 16, 30),
                   d0703, d0702, edt(2025, 7, 3, 17, 0), "jul: D off 07-03 from Sun 18:00");
        check_span(probe, kDOn, edt(2025, 6, 30, 20, 0), edt(2025, 7, 1, 16, 45),
                   kNa, kNa, 0, "jul: D on na through the partial first day");
        check_span(probe, kDOn, edt(2025, 7, 1, 18, 0), edt(2025, 7, 2, 16, 45),
                   d0701, kNa, 0, "jul: D on 07-01 from Tue 18:00");
        check_span(probe, kWOff, edt(2025, 6, 30, 20, 0), edt(2025, 7, 8, 20, 0),
                   kNa, kNa, 0, "jul: W off na for the whole tape (the 06-29 week was in progress)");
        check_span(probe, kWOn, edt(2025, 6, 30, 20, 0), edt(2025, 7, 4, 12, 45),
                   kNa, kNa, 0, "jul: W on na through the partial week");
    }
    // The feb tape: TV range 2026-02-12 .. 02-19, the chart from Wed 02-11
    // 19:00 ET (02-12 00:00Z).
    {
        LazyProbe probe;
        probe.sites = {{"D", false}, {"D", true}, {"W", false}, {"W", true}};
        const std::vector<Bar> chart = from(vec(oanda_data::kXau15Feb), utc_ms(2026, 2, 12));
        const std::vector<Bar> d1 = vec(oanda_data::kXau1DFeb);
        run_xau(probe, chart, utc_ms(2026, 2, 12));
        CHECK(probe.rows.size() == 359, "359 chart bars (the feb tape's chart to Tue 02-17 19:00)");
        const Ohlc d0212 = daily(d1, est(2026, 2, 12, 17, 0));
        const Ohlc d0215 = daily(d1, est(2026, 2, 15, 17, 0));
        check_span(probe, kDOff, est(2026, 2, 11, 19, 0), est(2026, 2, 13, 16, 30),
                   kNa, kNa, 0, "feb: D off na through the partial first day's completion bar 02-12 16:45");
        check_span(probe, kDOff, est(2026, 2, 13, 16, 45), est(2026, 2, 16, 14, 15),
                   d0212, kNa, est(2026, 2, 12, 17, 0), "feb: D off 02-12 on Fri 16:45 through the early close, no [1]");
        check_span(probe, kDOff, est(2026, 2, 16, 18, 0), est(2026, 2, 17, 16, 30),
                   d0215, d0212, est(2026, 2, 15, 17, 0), "feb: D off 02-15 from Mon 18:00");
        check_span(probe, kDOn, est(2026, 2, 11, 19, 0), est(2026, 2, 12, 16, 45),
                   kNa, kNa, 0, "feb: D on na through the partial first day");
        check_span(probe, kDOn, est(2026, 2, 12, 18, 0), est(2026, 2, 13, 16, 45),
                   d0212, kNa, 0, "feb: D on 02-12 from Thu 18:00");
        check_span(probe, kWOff, est(2026, 2, 11, 19, 0), est(2026, 2, 17, 19, 0),
                   kNa, kNa, 0, "feb: W off na for the whole tape");
        check_span(probe, kWOn, est(2026, 2, 11, 19, 0), est(2026, 2, 13, 16, 45),
                   kNa, kNa, 0, "feb: W on na through the partial week");
    }
}

// ---- (c) the values: every 15m session = the registry 1D bar --------------

void test_sessions_aggregate_to_the_native_daily_bar() {
    struct Window { const std::vector<Bar> chart, daily; };
    const Window windows[] = {
        {vec(oanda_data::kXau15Jul), vec(oanda_data::kXau1DJul)},
        {vec(oanda_data::kXau15Feb), vec(oanda_data::kXau1DFeb)},
    };
    int sessions = 0;
    for (const Window& w : windows) {
        std::size_t begin = 0;
        for (std::size_t i = 1; i <= w.chart.size(); ++i) {
            const bool gap = i == w.chart.size()
                || w.chart[i].timestamp - w.chart[i - 1].timestamp > kQuarter;
            if (!gap) continue;
            const Bar& first = w.chart[begin];
            const Bar& last = w.chart[i - 1];
            // A whole session (closing 16:45, or the two early closes).
            const bool whole = last.timestamp + kQuarter == first.timestamp + 23 * kHour
                || last.timestamp == edt(2025, 7, 4, 12, 45)
                || last.timestamp == est(2026, 2, 16, 14, 15);
            if (whole) {
                const Bar agg = aggregate(w.chart, first.timestamp, last.timestamp);
                const Bar native = daily_bar(w.daily, first.timestamp - kHour);
                CHECK(same(agg.open, native.open) && same(agg.high, native.high)
                          && same(agg.low, native.low) && same(agg.close, native.close)
                          && same(agg.volume, native.volume),
                      "the session's 15m aggregate is the 1D bar stamped an hour before it");
                ++sessions;
            }
            begin = i;
        }
    }
    CHECK(sessions == 14, "seven whole sessions per window");
}

// ---- the gate is the symbol kind, not the session shape -------------------

void test_exchange_kind_completes_on_the_actual_last_bar() {
    // The same OANDA feed declared as an exchange-listed kind takes engine
    // f725bc3's rule: the 07-03-stamped day and the 06-29 week complete on
    // the 12:45 bar (what TradingView does on NYSE:F / CME_MINI:ES1!, and
    // what it does NOT do on OANDA).
    {
        LazyProbe probe;
        probe.sites = {{"D", false}, {"W", false}};
        const std::vector<Bar> chart = vec(oanda_data::kXau15Jul);
        const std::vector<Bar> d1 = vec(oanda_data::kXau1DJul);
        run_xau(probe, chart, utc_ms(2025, 6, 28), "futures");
        const Ohlc d0701 = daily(d1, edt(2025, 7, 1, 17, 0));
        const Ohlc d0702 = daily(d1, edt(2025, 7, 2, 17, 0));
        const Ohlc d0703 = daily(d1, edt(2025, 7, 3, 17, 0));
        const Ohlc w0629 = weekly(d1, edt(2025, 6, 29, 17, 0), edt(2025, 7, 3, 17, 0));
        check_span(probe, 0, edt(2025, 7, 3, 16, 45), edt(2025, 7, 4, 12, 30),
                   d0702, d0701, edt(2025, 7, 2, 17, 0), "as futures: D off 07-02 until 12:30");
        check_span(probe, 0, edt(2025, 7, 4, 12, 45), edt(2025, 7, 7, 16, 30),
                   d0703, d0702, edt(2025, 7, 3, 17, 0), "as futures: D off 07-03 ON the 12:45 bar");
        check_span(probe, 1, edt(2025, 6, 29, 18, 0), edt(2025, 7, 4, 12, 30),
                   kNa, kNa, 0, "as futures: W off na until 12:30");
        check_span(probe, 1, edt(2025, 7, 4, 12, 45), edt(2025, 7, 8, 20, 0),
                   w0629, kNa, edt(2025, 6, 29, 17, 0), "as futures: W off week 06-29 ON the 12:45 bar");
    }
    // NYSE:F's Thanksgiving week (Thu 11-27 closed, Fri 11-28 13:00 close):
    // as the stock it is, the week completes on the half-day's 12:45 bar
    // (the wm-w-f15-nov pin); declared cfd it would wait for Mon 09:30.
    for (const char* kind : {"stock", "cfd"}) {
        LazyProbe probe;
        probe.sites = {{"W", false}, {"D", false}};
        const std::vector<Bar> chart = vec(wm_data::kF15Nov);
        run15(probe, chart, "America/New_York", "0930-1600", kind, utc_ms(2025, 11, 12));
        const Bar w1124 = aggregate(chart, est(2025, 11, 24, 9, 30), est(2025, 11, 28, 12, 45));
        const Bar w1117 = aggregate(chart, est(2025, 11, 17, 9, 30), est(2025, 11, 21, 15, 45));
        const Bar d1128 = aggregate(chart, est(2025, 11, 28, 9, 30), est(2025, 11, 28, 12, 45));
        const Bar d1126 = aggregate(chart, est(2025, 11, 26, 9, 30), est(2025, 11, 26, 15, 45));
        const Ohlc ow1124{w1124.open, w1124.high, w1124.low, w1124.close};
        const Ohlc ow1117{w1117.open, w1117.high, w1117.low, w1117.close};
        const Ohlc od1128{d1128.open, d1128.high, d1128.low, d1128.close};
        const Ohlc od1126{d1126.open, d1126.high, d1126.low, d1126.close};
        const bool exchange = std::string(kind) == "stock";
        const Read& half_day = probe.at(est(2025, 11, 28, 12, 45), 0);
        const Read& monday = probe.at(est(2025, 12, 1, 9, 30), 0);
        if (exchange) {
            check_ohlc(half_day.x0, ow1124, "F as stock: W 11-24 on the half-day's 12:45 bar");
            check_ohlc(probe.at(est(2025, 11, 28, 12, 45), 1).x0, od1128, "F as stock: D 11-28 on the 12:45 bar");
        } else {
            check_ohlc(half_day.x0, ow1117, "F as cfd: the 12:45 bar still reads week 11-17");
            check_ohlc(probe.at(est(2025, 11, 28, 12, 45), 1).x0, od1126, "F as cfd: the 12:45 bar still reads 11-26");
            check_ohlc(probe.at(est(2025, 12, 1, 9, 30), 1).x0, od1128, "F as cfd: D 11-28 surfaces Mon 09:30");
        }
        check_ohlc(monday.x0, ow1124, "F: week 11-24 read on Mon 12-01 09:30 either way");
        check_ohlc(monday.x1, ow1117, "F: [1] = week 11-17 on Mon 12-01 09:30 either way");
        CHECK(probe.session_template_knows_early_close() == exchange,
              "session_template_knows_early_close follows syminfo.type");
    }
}

// ---- the 1D chart: W on Friday's daily bar, D the bar itself (f, g) ----------

// The oanda1d-jul tape's week of 06-29 on the daily chart: the seven daily
// bars whose sessions the 15m slice holds whole, Sun 06-29 .. Mon 07-07
// stamps (Fri 07-04 = the 12:45 early close).
void test_daily_chart_july() {
    const std::vector<Bar> aux = vec(oanda_data::kXau15Jul);
    const std::vector<Bar> d1 = vec(oanda_data::kXau1DJul);
    const std::vector<Bar> chart = between(d1, edt(2025, 6, 29, 17, 0), edt(2025, 7, 7, 17, 0));
    CHECK(chart.size() == 7, "seven daily bars 06-29 .. 07-07");
    const Ohlc d0629 = daily(d1, edt(2025, 6, 29, 17, 0));
    const Ohlc d0630 = daily(d1, edt(2025, 6, 30, 17, 0));
    const Ohlc d0701 = daily(d1, edt(2025, 7, 1, 17, 0));
    const Ohlc d0702 = daily(d1, edt(2025, 7, 2, 17, 0));
    const Ohlc d0703 = daily(d1, edt(2025, 7, 3, 17, 0));  // Fri 07-04, data to 12:45
    const Ohlc d0706 = daily(d1, edt(2025, 7, 6, 17, 0));
    const Ohlc d0707 = daily(d1, edt(2025, 7, 7, 17, 0));
    const Ohlc w0629 = weekly(d1, edt(2025, 6, 29, 17, 0), edt(2025, 7, 3, 17, 0));
    check_ohlc(w0629, Ohlc{3271.64, 3365.92, 3244.415, 3336.615}, "the week 06-29: c = the 07-04 early close, cW 3336.61");
    for (const char* kind : {"cfd", "futures"}) {
        LazyProbe probe;
        probe.sites = {{"D", false}, {"D", true}, {"W", false}, {"W", true}};
        run_xau_daily(probe, chart, aux, utc_ms(2025, 6, 28), kind);
        CHECK(probe.rows.size() == 7, "seven daily chart bars ran");
        const bool cfd = std::string(kind) == "cfd";
        const std::string tag = std::string("1D as ") + kind + ": ";
        // (g) D off = the chart bar itself on every daily bar, [1] = the
        // previous daily bar, time = the stamp -- the tape's t0 = tb, c0 =
        // the bar's close, c1 = the previous close on 07-03 (70317 3336.61
        // 3326.03) exactly as on 07-02 and 07-06.
        struct Day { int64_t stamp; Ohlc x0, x1; };
        const Day days[] = {
            {edt(2025, 6, 29, 17, 0), d0629, kNa},
            {edt(2025, 6, 30, 17, 0), d0630, d0629},
            {edt(2025, 7, 1, 17, 0), d0701, d0630},
            {edt(2025, 7, 2, 17, 0), d0702, d0701},
            {edt(2025, 7, 3, 17, 0), d0703, d0702},   // the early-close day reads ITSELF
            {edt(2025, 7, 6, 17, 0), d0706, d0703},
            {edt(2025, 7, 7, 17, 0), d0707, d0706},
        };
        for (const Day& d : days) {
            const Read& off = probe.at(d.stamp, kDOff);
            check_ohlc(off.x0, d.x0, (tag + "D off = the daily bar itself").c_str());
            check_ohlc(off.x1, d.x1, (tag + "D off [1] = the previous daily bar").c_str());
            CHECK(off.t0 == d.stamp, (tag + "D off time = the bar's 17:00 ET stamp").c_str());
            CHECK(off.complete0, (tag + "D off publishes complete on its own bar").c_str());
            check_ohlc(probe.at(d.stamp, kDOn).x0, d.x0, (tag + "D on = the daily bar itself").c_str());
        }
        CHECK(same(probe.at(edt(2025, 7, 3, 17, 0), kDOff).v0, 221059.0),
              (tag + "the early-close day's D volume = the 1D bar's 221059").c_str());
        // (f) W off: na on Sun .. Thu, the week on FRIDAY's bar (the
        // 07-04 early close: tW 62917, cW 3336.61), held on the next week's
        // Sun / Mon bars with no [1].
        for (int k = 0; k < 4; ++k) {
            check_ohlc(probe.at(days[k].stamp, kWOff).x0, kNa, (tag + "W off na through Thu 07-03").c_str());
        }
        const Read& friday = probe.at(edt(2025, 7, 3, 17, 0), kWOff);
        check_ohlc(friday.x0, w0629, (tag + "W off week 06-29 ON Fri 07-04's daily bar").c_str());
        check_ohlc(friday.x1, kNa, (tag + "W off [1] na on Fri 07-04").c_str());
        CHECK(friday.t0 == edt(2025, 6, 29, 17, 0), (tag + "W off dated Sun 06-29 17:00").c_str());
        CHECK(friday.complete0, (tag + "W off publishes complete on Friday's bar").c_str());
        CHECK(same(friday.v0, 349557.0 + 417491.0 + 398577.0 + 374883.0 + 221059.0),
              (tag + "W off volume = the sum of the daily volumes").c_str());
        check_ohlc(probe.at(edt(2025, 7, 6, 17, 0), kWOff).x0, w0629, (tag + "W off held on Sun 07-06's bar").c_str());
        check_ohlc(probe.at(edt(2025, 7, 7, 17, 0), kWOff).x0, w0629, (tag + "W off held on Mon 07-07's bar").c_str());
        // W on: the tape leaks the week's FINAL values from its Sunday bar
        // (tWL 62917 cWL 3336.61 from the 06-29 stamp); the split-feed path
        // builds no finite-batch projection (prepare_aux_security_chart_ranges
        // replaces prepare_historical_security_lookahead_projections) and
        // peeks the running week instead -- a residual older than this
        // change and outside it; only Friday's bar, where the two agree, is
        // asserted.
        check_ohlc(probe.at(edt(2025, 7, 3, 17, 0), kWOn).x0, w0629, (tag + "W on = the whole week on Fri 07-04's bar").c_str());
        CHECK(probe.session_template_knows_early_close() == !cfd,
              "session_template_knows_early_close follows syminfo.type");
    }
}

// The oanda1d-feb tape on the daily chart: Sun 02-08 .. Mon 02-16 stamps
// (Fri 02-13 a regular Friday; Mon 02-16 Presidents' Day, data to 14:15).
void test_daily_chart_february() {
    const std::vector<Bar> aux = vec(oanda_data::kXau15Feb);
    const std::vector<Bar> d1 = vec(oanda_data::kXau1DFeb);
    const std::vector<Bar> chart = between(d1, est(2026, 2, 8, 17, 0), est(2026, 2, 16, 17, 0));
    CHECK(chart.size() == 7, "seven daily bars 02-08 .. 02-16");
    LazyProbe probe;
    probe.sites = {{"D", false}, {"D", true}, {"W", false}, {"W", true}};
    run_xau_daily(probe, chart, aux, utc_ms(2026, 2, 7));
    CHECK(probe.rows.size() == 7, "seven daily chart bars ran");
    const Ohlc d0208 = daily(d1, est(2026, 2, 8, 17, 0));
    const Ohlc d0209 = daily(d1, est(2026, 2, 9, 17, 0));
    const Ohlc d0210 = daily(d1, est(2026, 2, 10, 17, 0));
    const Ohlc d0211 = daily(d1, est(2026, 2, 11, 17, 0));
    const Ohlc d0212 = daily(d1, est(2026, 2, 12, 17, 0));
    const Ohlc d0215 = daily(d1, est(2026, 2, 15, 17, 0));  // Mon 02-16, data to 14:15
    const Ohlc d0216 = daily(d1, est(2026, 2, 16, 17, 0));
    const Ohlc w0208 = weekly(d1, est(2026, 2, 8, 17, 0), est(2026, 2, 12, 17, 0));
    check_ohlc(w0208, Ohlc{4989.93, 5119.345, 4878.5, 5042.74}, "the week 02-08, cW 5042.74");
    struct Day { int64_t stamp; Ohlc x0, x1; };
    const Day days[] = {
        {est(2026, 2, 8, 17, 0), d0208, kNa},
        {est(2026, 2, 9, 17, 0), d0209, d0208},
        {est(2026, 2, 10, 17, 0), d0210, d0209},
        {est(2026, 2, 11, 17, 0), d0211, d0210},
        {est(2026, 2, 12, 17, 0), d0212, d0211},
        {est(2026, 2, 15, 17, 0), d0215, d0212},   // the early-close day reads ITSELF (t0 21517 c0 4992.01)
        {est(2026, 2, 16, 17, 0), d0216, d0215},
    };
    for (const Day& d : days) {
        const Read& off = probe.at(d.stamp, kDOff);
        check_ohlc(off.x0, d.x0, "1D feb: D off = the daily bar itself");
        check_ohlc(off.x1, d.x1, "1D feb: D off [1] = the previous daily bar");
        CHECK(off.t0 == d.stamp, "1D feb: D off time = the stamp");
        CHECK(off.complete0, "1D feb: D off publishes complete on its own bar");
        check_ohlc(probe.at(d.stamp, kDOn).x0, d.x0, "1D feb: D on = the daily bar itself");
    }
    CHECK(same(probe.at(est(2026, 2, 15, 17, 0), kDOff).v0, 396800.0),
          "1D feb: the early-close day's D volume = the 1D bar's 396800");
    // W off: na Sun .. Thu, the week on Fri 02-13's bar (tW 20817 cW
    // 5042.74), held through the 02-16 early close and the 02-16 stamp.
    for (int k = 0; k < 4; ++k) {
        check_ohlc(probe.at(days[k].stamp, kWOff).x0, kNa, "1D feb: W off na through Thu 02-12");
    }
    const Read& friday = probe.at(est(2026, 2, 12, 17, 0), kWOff);
    check_ohlc(friday.x0, w0208, "1D feb: W off week 02-08 on Fri 02-13's daily bar");
    CHECK(friday.t0 == est(2026, 2, 8, 17, 0), "1D feb: W off dated Sun 02-08 17:00");
    CHECK(friday.complete0, "1D feb: W off publishes complete on Friday's bar");
    check_ohlc(probe.at(est(2026, 2, 15, 17, 0), kWOff).x0, w0208, "1D feb: W off held on the 02-16 early-close bar");
    check_ohlc(probe.at(est(2026, 2, 16, 17, 0), kWOff).x0, w0208, "1D feb: W off held on the 02-16 stamp");
    // (W on: the running week on the split-feed path, see test_daily_chart_july.)
    check_ohlc(probe.at(est(2026, 2, 12, 17, 0), kWOn).x0, w0208, "1D feb: W on = the whole week on Fri 02-13's bar");
}

// ---- aggregator: the calling chart bar's nominal close (f, g, h) -------------

void test_aggregator_calling_close_hint() {
    const std::vector<Bar> feed = vec(oanda_data::kXau15Jul);
    const std::string tz = "America/New_York", sess = "1800-1700";
    // The daily chart bar's nominal close for a 15m bar = its session-day's
    // close (Fri 17:00 ET for the 07-04 12:45 bar); the 15m chart bar's own
    // end = ts + 15m.
    auto daily_close = [&](int64_t ts) {
        return session_period_last_traded_close_ms(ts, tz, sess, CalendarPeriod::DAY);
    };
    enum Hint { kNone, kOwnEnd, kDailyClose };
    auto completions = [&](const char* tf, bool early, Hint hint, const std::vector<Bar>& bars) {
        TimeframeAggregator agg(tf, "15", tz, sess);
        agg.set_early_close_completes(early);
        std::vector<int64_t> on;
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const int64_t next = i + 1 < bars.size() ? bars[i + 1].timestamp : 0;
            const int64_t close = hint == kNone ? 0
                : hint == kOwnEnd ? bars[i].timestamp + kQuarter
                                  : daily_close(bars[i].timestamp);
            const AggregatedBar ab = hint == kNone ? agg.feed(bars[i], next)
                                                   : agg.feed(bars[i], next, close);
            if (ab.is_complete) on.push_back(bars[i].timestamp);
        }
        return on;
    };
    CHECK(daily_close(edt(2025, 7, 4, 12, 45)) == edt(2025, 7, 4, 17, 0),
          "the 07-04 12:45 bar's daily chart bar closes Fri 17:00 ET (tc 70417)");
    CHECK(daily_close(edt(2025, 7, 3, 16, 45)) == edt(2025, 7, 3, 17, 0),
          "the 07-03 16:45 bar's daily chart bar closes Thu 17:00 ET");
    const std::vector<int64_t> regular = {
        edt(2025, 6, 30, 16, 45), edt(2025, 7, 1, 16, 45), edt(2025, 7, 2, 16, 45),
        edt(2025, 7, 3, 16, 45), 0 /* the early-close day */, edt(2025, 7, 7, 16, 45),
        edt(2025, 7, 8, 16, 45)};
    for (const bool early : {false, true}) {
        for (const Hint hint : {kNone, kOwnEnd, kDailyClose}) {
            const auto on = completions("D", early, hint, feed);
            CHECK(on.size() == 7, "seven completed days");
            if (on.size() != 7) continue;
            for (std::size_t k = 0; k < on.size(); ++k) {
                if (regular[k] != 0) CHECK(on[k] == regular[k], "regular days complete on the 16:45 bar under every hint");
            }
            // cfd: the daily chart bar's close (Fri 17:00) reaches the day's
            // close, so the 12:45 bar completes it -- the 1D chart's rule;
            // the 15m bar's own end (13:00) does not, nor does no hint --
            // the 15m chart's rule. Exchange kinds: 12:45 either way.
            const bool on_12_45 = early || hint == kDailyClose;
            CHECK(on[4] == (on_12_45 ? edt(2025, 7, 4, 12, 45) : edt(2025, 7, 6, 18, 0)),
                  on_12_45 ? "the shortened day completes on its 12:45 bar"
                           : "the shortened day completes on Sun 18:00");
            const auto week = completions("W", early, hint, feed);
            CHECK(week.size() == 1 && week[0] == (on_12_45 ? edt(2025, 7, 4, 12, 45) : edt(2025, 7, 6, 18, 0)),
                  on_12_45 ? "the week completes on the 12:45 bar"
                           : "the week completes on Sun 07-06 18:00");
        }
    }
    // (h) M on the month's last daily bar: June 2025's last session (Sun
    // 06-29 18:00 -> Mon 06-30) with its tail cut at 12:45, a synthetic
    // early close on the month's last day. The daily chart bar's close (Mon
    // 17:00) reaches June's last traded close, so the 12:45 bar completes
    // the month on the 1D chart; on the 15m chart it waits for July's first
    // bar, Mon 18:00. Untouched, the month completes on 06-30 16:45 either
    // way (the nominal rule).
    std::vector<Bar> cut;
    for (const Bar& b : feed) {
        if (b.timestamp > edt(2025, 6, 30, 12, 45) && b.timestamp < edt(2025, 6, 30, 18, 0)) continue;
        cut.push_back(b);
    }
    CHECK(cut.size() == feed.size() - 16, "sixteen 15m bars cut from 06-30 13:00 .. 16:45");
    for (const Hint hint : {kNone, kOwnEnd, kDailyClose}) {
        const auto whole = completions("M", false, hint, feed);
        CHECK(whole.size() == 1 && whole[0] == edt(2025, 6, 30, 16, 45), "June completes on 06-30 16:45 under every hint");
        const auto early = completions("M", false, hint, cut);
        CHECK(early.size() == 1 && early[0] == (hint == kDailyClose ? edt(2025, 6, 30, 12, 45) : edt(2025, 6, 30, 18, 0)),
              hint == kDailyClose ? "cfd, the daily chart bar's close: June completes on the cut 12:45 bar"
                                  : "cfd, the 15m bar: June completes on July's first bar, Mon 18:00");
        const auto exchange = completions("M", true, hint, cut);
        CHECK(exchange.size() == 1 && exchange[0] == edt(2025, 6, 30, 12, 45), "exchange kinds: June completes on the cut 12:45 bar either way");
    }
}

// ---- aggregator: the flag ----------------------------------------------------

void test_aggregator_flag() {
    const std::vector<Bar> feed = vec(oanda_data::kXau15Jul);
    auto completion_ts = [&](const char* tf, bool early_close_completes) {
        TimeframeAggregator agg(tf, "15", "America/New_York", "1800-1700");
        CHECK(agg.early_close_completes(), "the default keeps the next-input-bar rule");
        agg.set_early_close_completes(early_close_completes);
        std::vector<int64_t> completed_on;
        for (std::size_t i = 0; i < feed.size(); ++i) {
            const int64_t next = i + 1 < feed.size() ? feed[i + 1].timestamp : 0;
            const AggregatedBar ab = agg.feed(feed[i], next);
            if (ab.is_complete) completed_on.push_back(feed[i].timestamp);
        }
        return completed_on;
    };
    const std::vector<int64_t> regular = {
        edt(2025, 6, 30, 16, 45), edt(2025, 7, 1, 16, 45), edt(2025, 7, 2, 16, 45),
        edt(2025, 7, 3, 16, 45), 0 /* the early-close day */, edt(2025, 7, 7, 16, 45),
        edt(2025, 7, 8, 16, 45)};
    for (const bool early : {true, false}) {
        const auto on = completion_ts("D", early);
        CHECK(on.size() == 7, "seven completed days");
        if (on.size() != 7) continue;
        for (std::size_t k = 0; k < on.size(); ++k) {
            if (regular[k] != 0) CHECK(on[k] == regular[k], "regular days complete on the 16:45 bar either way");
        }
        CHECK(on[4] == (early ? edt(2025, 7, 4, 12, 45) : edt(2025, 7, 6, 18, 0)),
              early ? "early_close_completes: the shortened day completes on its 12:45 bar"
                    : "!early_close_completes: the shortened day completes on Sun 18:00");
    }
    {
        const auto early = completion_ts("W", true);
        CHECK(early.size() == 1 && early[0] == edt(2025, 7, 4, 12, 45),
              "early_close_completes: the week completes on the 12:45 bar");
        const auto lazy = completion_ts("W", false);
        CHECK(lazy.size() == 1 && lazy[0] == edt(2025, 7, 6, 18, 0),
              "!early_close_completes: the week completes on Sun 07-06 18:00");
    }
    // Without the hint (the stream form) both settings are the lazy rule.
    {
        TimeframeAggregator agg("D", "15", "America/New_York", "1800-1700");
        std::vector<int64_t> completed_on;
        for (const Bar& b : feed) {
            if (agg.feed(b).is_complete) completed_on.push_back(b.timestamp);
        }
        CHECK(completed_on.size() == 7 && completed_on[4] == edt(2025, 7, 6, 18, 0),
              "no hint: the shortened day completes on Sun 18:00");
    }
}

}  // namespace

int main() {
    test_july_independence_day();
    test_february_presidents_day();
    test_range_start_absent_period();
    test_sessions_aggregate_to_the_native_daily_bar();
    test_exchange_kind_completes_on_the_actual_last_bar();
    test_daily_chart_july();
    test_daily_chart_february();
    test_aggregator_calling_close_hint();
    test_aggregator_flag();
    std::printf("test_oanda_lazy_close: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
