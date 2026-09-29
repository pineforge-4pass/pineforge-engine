// A daily bar the installed daily feed dates is labelled by the feed's stamp
// (R5 lane XAU-CAL, owner-approved 2026-09-29).
//
// OANDA stamps its XAUUSD daily bars at 17:00 ET, in the break of the
// 1800-1700 session, and TradingView dates every daily bar and its orders by
// that stamp: 22:00 UTC before the 2026-03-08 switch to daylight time, 21:00
// UTC after it, on the daily chart with the bar magnifier as without it (lab
// tv xc-dmag-xau1d, xc-dmag-dip-xau1d and their -off twins, OANDA:XAUUSD 1D,
// 2026-02-16 .. 04-01, tests/fixtures/daily_magnifier). A run that aggregates
// an intraday input into "D" script bars resolves each bar's calendar interval
// from the session, whose day opens at 18:00 ET, and labelled it there.
//
// The rule (NativeExecutionConsumer::prepare_day_labels): with a daily feed
// installed -- the public feed store's (BacktestEngine::set_native_security_feed)
// or a declared series' authoritative bars -- each stamp labels the D bar
// holding the session instant it covers: the stamp itself inside a session,
// else the next session's open. Only the label moves; the bar's span, its
// closes, its successor and its OHLCV stay the calendar's aggregate. A bar no
// stamp covers keeps the calendar's label, a bar two stamps cover takes the
// earlier, and a stamp at the calendar's own open (OANDA:EURUSD's 1700-1700)
// changes nothing. No feed, a calendar input, or a script timeframe other than
// one day: nothing moves. NSE's Muhurat session of 2025-10-21 opened at 13:45
// IST and its daily bar is stamped 13:45 (lab tv w14-htf-d-nifty15), inside
// the session: that bar reads 13:45. A label never passes an input bar the run
// holds, and a lookup by a moved label is its bar's own: the bar's open reads
// the interval and input slot its close does, with the bar magnifier as
// without it (NativeExecutionConsumer::day_label_origin).
//
// Kernel-only (native_host.hpp). Synthetic prices on the real clocks.

#include <pineforge/bar.hpp>
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "init";

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL [%s] line %d: %s\n", scenario, __LINE__, #expr); \
        }                                                                      \
    } while (0)

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kHour = 60 * kMinute;

// Unix ms of a civil date-time at a fixed UTC offset (hours, minutes).
std::int64_t civil_ms(int y, int m, int d, int h, int mi, int off_h, int off_m = 0) {
    y -= (m <= 2);
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5
        + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return (static_cast<std::int64_t>(days) * 86400 + h * 3600 + mi * 60) * 1000
        - (static_cast<std::int64_t>(off_h) * 3600 + off_m * 60) * 1000LL;
}

// New York wall time in March 2026: EST before Sunday 03-08 02:00, EDT after.
std::int64_t ny_ms(int d, int h, int mi = 0) {
    const bool edt = d > 8 || (d == 8 && h >= 3);
    return civil_ms(2026, 3, d, h, mi, edt ? -4 : -5);
}

struct Delivered {
    std::int64_t label = 0;
    native_calendar::NativeInterval interval{};
    Bar bar{};
    native_calendar::NativeInterval input{};
    int sub_count = 0;
};

class DayHost final : public NativeStrategyHost {
public:
    std::vector<Delivered> bars;
    std::vector<Delivered> opens;
    void on_native_bar_open(const Bar& bar, const NativeDecisionContext& context) override {
        opens.push_back({context.script_bar_open_ms, context.script_interval, bar,
                         context.input_interval, context.sub_count});
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        bars.push_back({context.script_bar_open_ms, context.script_interval, bar,
                        context.input_interval, context.sub_count});
    }
};

NativeRunSpec spec_for(const char* key, const char* input_tf, const char* script_tf,
                       const char* session, const char* timezone) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = input_tf;
    spec.script_tf = script_tf;
    spec.ticker = "XAUUSD";
    spec.tickerid = "TEST:XAUUSD";
    spec.type = "cfd";
    spec.currency = "USD";
    spec.basecurrency = "XAU";
    spec.description = "day labels";
    spec.volumetype = "base";
    spec.timezone = timezone;
    spec.session = session;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.001;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

Bar bar_at(std::int64_t ts, double base) {
    Bar b{};
    b.timestamp = ts;
    b.open = base;
    b.high = base + 3.0;
    b.low = base - 2.0;
    b.close = base + 1.0;
    b.volume = 10.0;
    return b;
}

// The XAUUSD week of 2026-03-02 .. 03-13 in hourly bars, 1800-1700 New York:
// each trading date's session runs from the previous day's 18:00 to its own
// 17:00, Sunday's opening Monday's. The DST switch falls on Sunday 03-08.
struct Week {
    std::vector<Bar> hourly;
    std::vector<std::int64_t> opens;    // each session's 18:00 ET open
    std::vector<std::int64_t> stamps;   // OANDA's 17:00 ET stamp before it
};

Week xau_week() {
    Week w;
    // Session opens: Sun 03-01 .. Thu 03-05, Sun 03-08 .. Thu 03-12.
    const int open_days[] = {1, 2, 3, 4, 5, 8, 9, 10, 11, 12};
    double base = 5000.0;
    for (int d : open_days) {
        const std::int64_t open = ny_ms(d, 18);
        w.opens.push_back(open);
        w.stamps.push_back(ny_ms(d, 17));
        for (int k = 0; k < 23; ++k) {
            w.hourly.push_back(bar_at(open + k * kHour, base));
            base += 1.0;
        }
    }
    return w;
}

std::vector<Bar> daily_bars(const std::vector<std::int64_t>& stamps) {
    std::vector<Bar> out;
    double base = 4000.0;   // values of their own: only the stamps are read
    for (std::int64_t s : stamps) out.push_back(bar_at(s, base += 5.0));
    return out;
}

struct Run {
    std::vector<Delivered> bars;
    std::vector<Delivered> opens;
};

// `magnifier`: the input is also the run's retained intrabar path, as a
// daily chart backtested with the bar magnifier runs on its finer feed.
Run run_host(const NativeRunSpec& spec, const std::vector<Bar>& input,
             const std::vector<Bar>* daily_feed, bool magnifier = false) {
    DayHost host;
    NativeRunSpec run_spec = spec;
    if (magnifier) {
        IntrabarPath::lower_tf path;
        path.bars = input;
        path.tf = spec.input_tf;
        path.sample_eligibility = IntrabarPath::SampleEligibility::ContinuousSegments;
        run_spec.intrabar.value = path;
    }
    CHECK(host.configure_native(run_spec).status == NativeSetupStatus::Applied);
    if (daily_feed) {
        CHECK(host.set_native_security_feed("D", daily_feed->data(),
                                            static_cast<int>(daily_feed->size())));
    }
    host.run(input.data(), static_cast<int>(input.size()), spec.input_tf, spec.script_tf,
             magnifier, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) std::printf("  error: %s\n", host.last_error().c_str());
    return {host.bars, host.opens};
}

bool same_values(const Bar& a, const Bar& b) {
    return a.open == b.open && a.high == b.high && a.low == b.low && a.close == b.close
        && a.volume == b.volume;
}

bool same_interval(const native_calendar::NativeInterval& a,
                   const native_calendar::NativeInterval& b) {
    return a.open_ms == b.open_ms && a.eligible_open_ms == b.eligible_open_ms
        && a.last_traded_close_ms == b.last_traded_close_ms
        && a.next_period_open_ms == b.next_period_open_ms
        && a.next_input_open_ms == b.next_input_open_ms;
}

// The bar a dated run delivers is the plain run's but for its label: the
// script interval's open is the stamp, every other instant of the span and
// the input interval are the calendar's, and the bar's open (the bar-open
// hook) reads the same interval as its close.
void check_dated_like_plain(const Run& plain, const Run& dated,
                            const std::vector<std::int64_t>& labels) {
    CHECK(plain.bars.size() == labels.size());
    CHECK(dated.bars.size() == labels.size());
    CHECK(plain.opens.size() == labels.size());
    CHECK(dated.opens.size() == labels.size());
    if (plain.bars.size() != labels.size() || dated.bars.size() != labels.size()
        || plain.opens.size() != labels.size() || dated.opens.size() != labels.size()) {
        return;
    }
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const Delivered* d[] = {&dated.bars[i], &dated.opens[i]};
        const Delivered* p[] = {&plain.bars[i], &plain.opens[i]};
        for (int k = 0; k < 2; ++k) {
            CHECK(d[k]->label == labels[i]);
            CHECK(d[k]->interval.open_ms == labels[i]);
            native_calendar::NativeInterval span = d[k]->interval;
            span.open_ms = p[k]->interval.open_ms;
            CHECK(same_interval(span, p[k]->interval));
            CHECK(same_interval(d[k]->input, p[k]->input));
            CHECK(d[k]->sub_count == p[k]->sub_count);
        }
        CHECK(same_interval(dated.opens[i].interval, dated.bars[i].interval));
        CHECK(same_values(dated.bars[i].bar, plain.bars[i].bar));
    }
}

// ---- 1. OANDA's break stamps date the aggregated days ----------------------
void test_break_stamps_label_the_day() {
    scenario = "break stamps";
    const Week w = xau_week();
    const auto daily = daily_bars(w.stamps);
    const NativeRunSpec spec = spec_for("xc-day-labels", "60", "1D", "1800-1700",
                                        "America/New_York");
    const Run plain = run_host(spec, w.hourly, nullptr);
    const Run dated = run_host(spec, w.hourly, &daily);
    CHECK(plain.bars.size() == w.opens.size());
    CHECK(dated.bars.size() == w.opens.size());
    if (plain.bars.size() != w.opens.size() || dated.bars.size() != w.opens.size()) return;
    for (std::size_t i = 0; i < w.opens.size(); ++i) {
        // Without the feed: the calendar's 18:00 ET open.
        CHECK(plain.bars[i].label == w.opens[i]);
        // With it: the stamp -- 22:00 UTC in EST, 21:00 UTC in EDT.
        CHECK(dated.bars[i].label == w.stamps[i]);
        CHECK(dated.bars[i].interval.open_ms == w.stamps[i]);
        // Only the label moves: the span, the closes and the values are the
        // calendar's aggregate.
        CHECK(dated.bars[i].interval.eligible_open_ms == plain.bars[i].interval.eligible_open_ms);
        CHECK(dated.bars[i].interval.last_traded_close_ms
              == plain.bars[i].interval.last_traded_close_ms);
        CHECK(dated.bars[i].interval.next_period_open_ms
              == plain.bars[i].interval.next_period_open_ms);
        CHECK(same_values(dated.bars[i].bar, plain.bars[i].bar));
    }
    CHECK(dated.bars.front().label == civil_ms(2026, 3, 1, 22, 0, 0));   // Sun 17:00 EST
    CHECK(dated.bars[5].label == civil_ms(2026, 3, 8, 21, 0, 0));        // Sun 17:00 EDT
    // A lookup by the label is the bar's own: its open reads the interval
    // and the input slot its close does, not the day before the stamp.
    check_dated_like_plain(plain, dated, w.stamps);
}

// ---- 2. a declared series' authoritative daily bars date them too ----------
void test_declared_daily_series() {
    scenario = "declared daily series";
    const Week w = xau_week();
    NativeRunSpec spec = spec_for("xc-day-labels-series", "60", "1D", "1800-1700",
                                  "America/New_York");
    NativeTimeframeSubscription day;
    day.tf = "D";
    day.authoritative_bars = daily_bars(w.stamps);
    spec.subscriptions.push_back(day);
    const Run host = run_host(spec, w.hourly, nullptr);
    CHECK(host.bars.size() == w.stamps.size());
    for (std::size_t i = 0; i < host.bars.size() && i < w.stamps.size(); ++i)
        CHECK(host.bars[i].label == w.stamps[i]);
}

// ---- 3. a stamp at the calendar's own open changes nothing ----------------
void test_stamps_at_the_open() {
    scenario = "stamps at the open";
    // OANDA:EURUSD: 1700-1700, whose day opens at the 17:00 ET stamp.
    const Week w = xau_week();
    std::vector<Bar> hourly;
    for (Bar b : w.hourly) {
        hourly.push_back(b);
    }
    // One more hour per day, 17:00-18:00 ET, which 1700-1700 trades.
    std::vector<Bar> fx;
    for (std::size_t i = 0; i < w.stamps.size(); ++i) {
        fx.push_back(bar_at(w.stamps[i], 11.0));
        for (std::size_t k = 0; k < 23; ++k) fx.push_back(hourly[i * 23 + k]);
    }
    const auto daily = daily_bars(w.stamps);
    const NativeRunSpec spec = spec_for("xc-day-labels-fx", "60", "1D", "1700-1700",
                                        "America/New_York");
    const Run plain = run_host(spec, fx, nullptr);
    const Run dated = run_host(spec, fx, &daily);
    CHECK(plain.bars.size() == dated.bars.size());
    CHECK(plain.bars.size() == w.stamps.size());
    for (std::size_t i = 0; i < plain.bars.size() && i < dated.bars.size(); ++i) {
        CHECK(plain.bars[i].label == w.stamps[i]);
        CHECK(dated.bars[i].label == plain.bars[i].label);
        CHECK(same_values(dated.bars[i].bar, plain.bars[i].bar));
    }
}

// ---- 4. a bar no stamp covers; a bar two stamps cover; a stamp after inputs
void test_missing_and_doubled_stamps() {
    scenario = "missing and doubled stamps";
    const Week w = xau_week();
    std::vector<std::int64_t> stamps;
    for (std::size_t i = 0; i < w.stamps.size(); ++i) {
        if (i == 2) continue;                          // Wednesday 03-04's bar: no stamp
        if (i == 7) {                                  // Wednesday 03-11's bar: its one stamp
            stamps.push_back(ny_ms(11, 3));            // after its first inputs
            continue;
        }
        stamps.push_back(w.stamps[i]);
        if (i == 6) stamps.push_back(ny_ms(10, 3));    // Tuesday 03-10's bar: a second stamp
    }
    const auto daily = daily_bars(stamps);
    const NativeRunSpec spec = spec_for("xc-day-labels-gaps", "60", "1D", "1800-1700",
                                        "America/New_York");
    const Run host = run_host(spec, w.hourly, &daily);
    CHECK(host.bars.size() == w.opens.size());
    for (std::size_t i = 0; i < host.bars.size() && i < w.opens.size(); ++i) {
        // The uncovered bar and the one whose only stamp follows its first
        // inputs keep the calendar's label; the doubled one takes the earlier
        // stamp; every other bar its own stamp.
        CHECK(host.bars[i].label == (i == 2 || i == 7 ? w.opens[i] : w.stamps[i]));
    }
}

// ---- 5. a late open inside the session (NSE's Muhurat day) ----------------
void test_late_open_stamp() {
    scenario = "late open";
    // NSE:NIFTY, 0915-1530 Asia/Kolkata, 15-minute input: Mon 2025-10-20 and
    // Thu 10-23 regular, Tue 10-21 the Muhurat session 13:45 .. 14:45 only.
    const auto ist = [](int d, int h, int mi) { return civil_ms(2025, 10, d, h, mi, 5, 30); };
    std::vector<Bar> input;
    std::vector<std::int64_t> stamps;
    double base = 25000.0;
    const struct { int d, h, mi, n; } days[] = {{20, 9, 15, 25}, {21, 13, 45, 4}, {23, 9, 15, 25}};
    for (const auto& day : days) {
        stamps.push_back(ist(day.d, day.h, day.mi));
        for (int k = 0; k < day.n; ++k)
            input.push_back(bar_at(ist(day.d, day.h, day.mi) + k * 15 * kMinute, base += 1.0));
    }
    const auto daily = daily_bars(stamps);
    NativeRunSpec spec = spec_for("xc-day-labels-nse", "15", "1D", "0915-1530", "Asia/Kolkata");
    const Run plain = run_host(spec, input, nullptr);
    const Run dated = run_host(spec, input, &daily);
    CHECK(plain.bars.size() == 3);
    CHECK(dated.bars.size() == 3);
    if (plain.bars.size() != 3 || dated.bars.size() != 3) return;
    CHECK(plain.bars[1].label == ist(21, 9, 15));    // the calendar's day open
    CHECK(dated.bars[0].label == ist(20, 9, 15));
    CHECK(dated.bars[1].label == ist(21, 13, 45));   // the venue's stamp
    CHECK(dated.bars[2].label == ist(23, 9, 15));
    for (std::size_t i = 0; i < 3; ++i) CHECK(same_values(dated.bars[i].bar, plain.bars[i].bar));
}

// ---- 6. nothing moves off the rule's ground -------------------------------
void test_off_the_rule() {
    scenario = "off the rule";
    const Week w = xau_week();
    const auto daily = daily_bars(w.stamps);
    // A daily input is the venue's bars themselves: the feed moves nothing.
    {
        std::vector<Bar> days;
        for (std::size_t i = 0; i < w.stamps.size(); ++i) days.push_back(bar_at(w.stamps[i], 10.0 + i));
        // OANDA's break stamps are raw labels the calendar holds no slot for:
        // a daily feed host runs FeedTolerant, as the Pine adapter does.
        NativeRunSpec spec = spec_for("xc-day-labels-daily-input", "1D", "1D", "1800-1700",
                                      "America/New_York");
        spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        const Run plain = run_host(spec, days, nullptr);
        const Run dated = run_host(spec, days, &daily);
        CHECK(plain.bars.size() == dated.bars.size());
        for (std::size_t i = 0; i < plain.bars.size() && i < dated.bars.size(); ++i)
            CHECK(dated.bars[i].label == plain.bars[i].label);
    }
    // A weekly script bar is no daily bar: its labels stay the calendar's.
    {
        const NativeRunSpec spec = spec_for("xc-day-labels-weekly", "60", "W", "1800-1700",
                                            "America/New_York");
        const Run plain = run_host(spec, w.hourly, nullptr);
        const Run dated = run_host(spec, w.hourly, &daily);
        CHECK(!plain.bars.empty());
        CHECK(plain.bars.size() == dated.bars.size());
        for (std::size_t i = 0; i < plain.bars.size() && i < dated.bars.size(); ++i)
            CHECK(dated.bars[i].label == plain.bars[i].label);
    }
    // An intraday script bar neither.
    {
        const NativeRunSpec spec = spec_for("xc-day-labels-240", "60", "240", "1800-1700",
                                            "America/New_York");
        const Run plain = run_host(spec, w.hourly, nullptr);
        const Run dated = run_host(spec, w.hourly, &daily);
        CHECK(!plain.bars.empty());
        CHECK(plain.bars.size() == dated.bars.size());
        for (std::size_t i = 0; i < plain.bars.size() && i < dated.bars.size(); ++i)
            CHECK(dated.bars[i].label == plain.bars[i].label);
    }
}

// ---- 7. a reused host drops the labels of the run before ------------------
void test_reused_host() {
    scenario = "reused host";
    const Week w = xau_week();
    const auto daily = daily_bars(w.stamps);
    DayHost host;
    NativeRunSpec spec = spec_for("xc-day-labels-reuse", "60", "1D", "1800-1700",
                                  "America/New_York");
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    CHECK(host.set_native_security_feed("D", daily.data(), static_cast<int>(daily.size())));
    host.run(w.hourly.data(), static_cast<int>(w.hourly.size()), "60", "1D", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.bars.size() == w.stamps.size());
    CHECK(!host.bars.empty() && host.bars.front().label == w.stamps.front());
    // The feed removed (n == 0), the next run is the calendar's again.
    CHECK(host.set_native_security_feed("D", nullptr, 0));
    host.bars.clear();
    spec.identity = {"xc-day-labels-reuse", 2};
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(w.hourly.data(), static_cast<int>(w.hourly.size()), "60", "1D", false, 4,
             MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    CHECK(host.bars.size() == w.opens.size());
    for (std::size_t i = 0; i < host.bars.size() && i < w.opens.size(); ++i)
        CHECK(host.bars[i].label == w.opens[i]);
}

// ---- 8. the bar magnifier walks the dated bar's own sub-bars ---------------
void test_magnified_day() {
    scenario = "magnified day";
    const Week w = xau_week();
    const auto daily = daily_bars(w.stamps);
    const NativeRunSpec spec = spec_for("xc-day-labels-magnified", "60", "1D", "1800-1700",
                                        "America/New_York");
    const Run plain = run_host(spec, w.hourly, nullptr, true);
    const Run dated = run_host(spec, w.hourly, &daily, true);
    // Its open, its close and every sub-bar read the bar's own interval, the
    // calendar's, and walk its 23 hourly bars from the session's open.
    check_dated_like_plain(plain, dated, w.stamps);
    for (const Delivered& bar : dated.bars) CHECK(bar.sub_count == 23);
}

// ---- 9. a label never passes an input bar the run holds --------------------
void test_label_never_passes_an_input() {
    scenario = "label never passes an input";
    const Week w = xau_week();
    const auto daily = daily_bars(w.stamps);
    // An input at Tuesday 03-03 17:30 EST, in the break after Tuesday's
    // session and before Wednesday's opens (a feed that runs FeedTolerant
    // holds it, as the Pine adapter's does): it is Tuesday's, so Wednesday's
    // bar keeps the calendar's 18:00 label rather than the 17:00 stamp before
    // that input; every other bar takes its stamp.
    const std::int64_t late = ny_ms(3, 17, 30);
    std::vector<Bar> input;
    for (const Bar& b : w.hourly) {
        if (b.timestamp > late && (input.empty() || input.back().timestamp < late))
            input.push_back(bar_at(late, 7777.0));
        input.push_back(b);
    }
    NativeRunSpec spec = spec_for("xc-day-labels-break-input", "60", "1D", "1800-1700",
                                  "America/New_York");
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    const Run plain = run_host(spec, input, nullptr);
    const Run dated = run_host(spec, input, &daily);
    CHECK(plain.bars.size() == w.opens.size());
    CHECK(dated.bars.size() == w.opens.size());
    for (std::size_t i = 0; i < dated.bars.size() && i < w.opens.size(); ++i) {
        CHECK(dated.bars[i].label == (i == 2 ? w.opens[i] : w.stamps[i]));
        if (i < plain.bars.size()) CHECK(same_values(dated.bars[i].bar, plain.bars[i].bar));
    }
    // Tuesday's bar holds the break input; no later bar is labelled before it.
    if (dated.bars.size() > 2) {
        CHECK(dated.bars[1].bar.high == 7780.0);
        CHECK(dated.bars[2].label > late);
    }
}

}  // namespace

int main() {
    test_break_stamps_label_the_day();
    test_declared_daily_series();
    test_stamps_at_the_open();
    test_missing_and_doubled_stamps();
    test_late_open_stamp();
    test_off_the_rule();
    test_reused_host();
    test_magnified_day();
    test_label_never_passes_an_input();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
