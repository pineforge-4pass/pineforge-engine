// The kernel/adapter twin of tests/test_native_instrument_feed.cpp (lane
// XSYM-D): the same synthetic charts and foreign feeds, once through a bare
// kernel host declaring InstrumentFeed series, once through the Pine source
// host whose request sites of another symbol are registered with the symbol
// key overload of register_security_eval, in the shape codegen emits
// (_eval_security_N writes _req_sec_N, the body reads it, clear_security
// blanks it). Every chart bar's value is compared field by field:
//   * the requested bar the chart bar sees -- its time, its time_close, its
//     close and the requested context's bar_index -- is the bar the kernel's
//     native_series_bar() answers for the same chart bar;
//   * the payload runs over EVERY feed bar, in order: a [1] read inside it is
//     the context's previous bar, not the previous chart bar's;
//   * gaps_on reads na exactly where the kernel's series is empty;
//   * inside the payload syminfo.* is the symbol's facts, and the chart's
//     again in the body.
// Then the adapter's own rules: a same-symbol site beside a foreign one (on
// the kernel's drive and on the adapter's), ignore_invalid_symbol, the
// fail-closed refusals (no feed, an invalid symbol, an aggregated chart), the
// stream refusal and the broker hash.

#include "native_instrument_feed_fixture.hpp"

#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <map>
#include <string>
#include <vector>

namespace {
using namespace instrument_fixture;

struct SiteSpec {
    int sec_id = 0;
    std::string symbol;     // empty: a same-symbol site
    std::string tf;
    bool lookahead = false;
    bool gaps = false;
    bool ignore_invalid = false;
};

// One chart bar's reading of one site, as the generated body sees it.
struct Row {
    double time = na<double>();
    double time_close = na<double>();
    double close = na<double>();
    double bar_index = na<double>();
    double close1 = na<double>();       // close[1] inside the payload
    double time1 = na<double>();        // time[1] inside the payload
    double time_close1 = na<double>();  // time_close[1] inside the payload
    double column = na<double>();       // the fp_delta column of the context bar
};

// The generated shape: one _req_sec value set per site, history series per
// site for the [1] reads, clear_security blanking the values and clearing the
// history exactly as codegen's clear_security does.
class ForeignShape final : public source::PineStrategyHost {
public:
    std::vector<SiteSpec> sites;
    std::map<int, Row> value;
    std::map<int, Series<double>> hist_close, hist_time, hist_time_close;
    std::vector<std::map<int, Row>> rows;
    // What the payload saw of syminfo, and what the body sees after it.
    std::map<int, std::string> payload_tickerid, payload_type, payload_timezone;
    std::map<int, double> payload_mintick;
    std::vector<std::string> body_tickerid;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        value.clear();
        hist_close.clear();
        hist_time.clear();
        hist_time_close.clear();
        for (const auto& site : sites) {
            if (site.symbol.empty()) {
                register_security_eval(site.sec_id, site.tf, input_tf_, site.lookahead, site.gaps);
            } else {
                register_security_eval(site.sec_id, site.symbol, site.tf, input_tf_,
                                       site.lookahead, site.gaps, site.ignore_invalid);
            }
            value[site.sec_id] = Row{};
        }
    }
    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        Row& row = value[sec_id];
        row.time = static_cast<double>(bar.timestamp);
        row.time_close = static_cast<double>(time_close());
        row.close = bar.close;
        row.bar_index = pine_bar_index();
        row.close1 = hist_close[sec_id][0];
        row.time1 = hist_time[sec_id][0];
        row.time_close1 = hist_time_close[sec_id][0];
        row.column = security_column_value(sec_id, "fp_delta");
        payload_tickerid[sec_id] = syminfo_.tickerid;
        payload_type[sec_id] = syminfo_.type;
        payload_timezone[sec_id] = syminfo_.timezone;
        payload_mintick[sec_id] = syminfo_.mintick;
        if (is_complete) {
            hist_close[sec_id].push(bar.close);
            hist_time[sec_id].push(static_cast<double>(bar.timestamp));
            hist_time_close[sec_id].push(static_cast<double>(time_close()));
        }
    }
    void clear_security(int sec_id) override {
        value[sec_id] = Row{};
        hist_close[sec_id].clear();
        hist_time[sec_id].clear();
        hist_time_close[sec_id].clear();
    }
    void on_source_bar(const Bar&) override {
        rows.push_back(value);
        body_tickerid.push_back(syminfo_.tickerid);
    }
};

void prime_chart(ForeignShape& pine, const NativeRunSpec& spec) {
    pine.set_syminfo_timezone(spec.timezone);
    pine.set_syminfo_session(spec.session);
    pine.set_syminfo_type(spec.type);
    pine.set_syminfo_string("tickerid", spec.tickerid);
    pine.set_syminfo_string("ticker", spec.ticker);
}

bool install(ForeignShape& pine, const NativeInstrumentFeed& feed) {
    const bool ok = pine.set_symbol_feed(feed.instrument, feed.tf, feed.bars.data(),
                                         feed.close_ms.data(),
                                         static_cast<int>(feed.bars.size()));
    for (const auto& column : feed.columns) {
        if (!pine.set_symbol_feed_column(feed.instrument, feed.tf, column.name,
                                         column.values.data(),
                                         static_cast<int>(column.values.size())))
            return false;
    }
    return ok;
}

bool run_pine(ForeignShape& pine, const NativeRunSpec& spec, const std::vector<Bar>& chart) {
    pine.run(chart.data(), static_cast<int>(chart.size()), spec.input_tf, spec.script_tf, false,
             4, MagnifierDistribution::ENDPOINTS);
    if (!pine.last_error().empty()) std::printf("  pine error: %s\n", pine.last_error().c_str());
    return pine.last_error().empty();
}

bool same_value(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return a == b;
}

// Row j of the adapter against the kernel's reading of chart bar j.
void check_rows(const ForeignShape& pine, int sec_id, const InstrumentHost& kernel,
                std::size_t series, const NativeInstrumentFeed& feed, bool with_history) {
    CHECK(pine.rows.size() == kernel.reads.size());
    if (pine.rows.size() != kernel.reads.size()) return;
    int mismatches = 0;
    for (std::size_t j = 0; j < pine.rows.size(); ++j) {
        const Row& got = pine.rows[j].at(sec_id);
        const auto& visible = kernel.reads[j].visible[series];
        Row want;
        if (visible) {
            const auto i = static_cast<std::size_t>(kernel.reads[j].index[series]);
            want.time = static_cast<double>(feed.bars[i].timestamp);
            want.time_close = static_cast<double>(feed.close_ms[i]);
            want.close = feed.bars[i].close;
            want.bar_index = static_cast<double>(i);
            if (with_history && i > 0) {
                want.close1 = feed.bars[i - 1].close;
                want.time1 = static_cast<double>(feed.bars[i - 1].timestamp);
                want.time_close1 = static_cast<double>(feed.close_ms[i - 1]);
            }
            if (!feed.columns.empty()) want.column = feed.columns[0].values[i];
        }
        const bool equal = same_value(got.time, want.time)
            && same_value(got.time_close, want.time_close) && same_value(got.close, want.close)
            && same_value(got.bar_index, want.bar_index)
            && (!with_history
                || (same_value(got.close1, want.close1) && same_value(got.time1, want.time1)
                    && same_value(got.time_close1, want.time_close1)))
            && (feed.columns.empty() || same_value(got.column, want.column));
        if (!equal && mismatches++ < 3) {
            std::printf("  chart bar %zu sec %d: got t %.0f tc %.0f c %.6g bi %.0f c1 %.6g; "
                        "want t %.0f tc %.0f c %.6g bi %.0f c1 %.6g\n",
                        j, sec_id, got.time, got.time_close, got.close, got.bar_index,
                        got.close1, want.time, want.time_close, want.close, want.bar_index,
                        want.close1);
        }
        CHECK(equal);
    }
}

// ---- 1. a foreign session that differs from the chart's -------------------

void twin_foreign_session() {
    scenario = "twin: F-15-like chart <- DXY-like 23h feed";
    const std::vector<std::int64_t> days = {monday(), monday() + kDay};
    const std::vector<Bar> chart = nyse_quarter_chart(days);
    NativeInstrumentFeed feed = dxy_like_quarter_feed(monday(), monday() + 2 * kDay);
    NativeInstrumentColumn delta{"fp_delta", {}};
    for (std::size_t i = 0; i < feed.bars.size(); ++i)
        delta.values.push_back(3.0 * static_cast<double>(i) - 50.0);
    feed.columns.push_back(delta);
    NativeRunSpec spec = nyse_spec("twin-session", "15");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));
    InstrumentHost kernel;
    if (!run_batch(kernel, spec, chart)) return;

    ForeignShape pine;
    prime_chart(pine, spec);
    pine.sites = {{0, "SYN:DXY", "15", false, false, false}};
    CHECK(install(pine, feed));
    CHECK(pine.set_symbol_facts("SYN:DXY", "canonical", "SYN:DXY"));
    CHECK(pine.set_symbol_facts("SYN:DXY", "valid", "true"));
    CHECK(pine.set_symbol_facts("SYN:DXY", "type", "index"));
    CHECK(pine.set_symbol_facts("SYN:DXY", "timezone", "America/New_York"));
    CHECK(pine.set_symbol_facts("SYN:DXY", "session", "regular"));
    CHECK(pine.set_symbol_facts("SYN:DXY", "currency", "USD"));
    CHECK(pine.set_symbol_facts("SYN:DXY", "mintick", "0.001"));
    if (!run_pine(pine, spec, chart)) return;
    check_rows(pine, 0, kernel, 0, feed, true);
    // The report's pin through the adapter: the first chart bar reads the
    // context's bar 54, closed with the chart bar at 13:45Z.
    CHECK(!pine.rows.empty() && pine.rows[0].at(0).bar_index == 54.0);
    CHECK(!pine.rows.empty()
          && pine.rows[0].at(0).time_close
              == static_cast<double>(monday() + 13 * kHour + 45 * kMinute));
    // syminfo inside the payload is the symbol's; in the body, the chart's.
    CHECK(pine.payload_tickerid[0] == "SYN:DXY");
    CHECK(pine.payload_type[0] == "index");
    CHECK(pine.payload_timezone[0] == "America/New_York");
    CHECK(same(pine.payload_mintick[0], 0.001));
    for (const auto& tickerid : pine.body_tickerid) CHECK(tickerid == "TEST:F");
    // The kernel series the adapter declared reads the same bars.
    const auto view = pine.native_state();
    CHECK(view.spec != nullptr && view.spec->instrument_feeds.size() == 1);
    CHECK(view.spec != nullptr && view.spec->subscriptions.size() == 1
          && view.spec->subscriptions[0].source == NativeSeriesSource::InstrumentFeed
          && view.spec->subscriptions[0].instrument == "SYN:DXY");
}

// ---- 2. a daily foreign close after the chart's close ---------------------

void twin_daily_late() {
    scenario = "twin: F-1D-like chart <- US10Y-like daily feed";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 10);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    const NativeInstrumentFeed feed = us10y_like_daily_feed(days);
    NativeRunSpec spec = nyse_spec("twin-late", "D");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));
    InstrumentHost kernel;
    if (!run_batch(kernel, spec, chart)) return;

    ForeignShape pine;
    prime_chart(pine, spec);
    // Registered under Pine's bare "D"; installed under the manifest's "1D".
    pine.sites = {{0, "SYN:US10Y", "D", false, false, false}};
    NativeInstrumentFeed as_manifest = feed;
    as_manifest.tf = "1D";
    CHECK(install(pine, as_manifest));
    if (!run_pine(pine, spec, chart)) return;
    check_rows(pine, 0, kernel, 0, feed, true);
    CHECK(!pine.rows.empty() && std::isnan(pine.rows[0].at(0).close));
}

// ---- 3. the weekend under gaps off and on ---------------------------------

void twin_weekend() {
    scenario = "twin: 24x7 daily chart <- weekday-only daily feed, gaps off and on";
    const std::vector<Bar> chart = continuous_daily_chart(monday(), 14);
    const NativeInstrumentFeed feed = weekday_daily_feed(weekdays_from(monday(), 10));
    NativeRunSpec spec = continuous_spec("twin-weekend", "D");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed, false, false));
    spec.subscriptions.push_back(instrument_series(feed, false, true));
    InstrumentHost kernel(2);
    if (!run_batch(kernel, spec, chart)) return;

    ForeignShape pine;
    prime_chart(pine, spec);
    pine.sites = {{0, "SYN:DXY", "D", false, false, false}, {1, "SYN:DXY", "D", false, true, false}};
    CHECK(install(pine, feed));
    if (!run_pine(pine, spec, chart)) return;
    check_rows(pine, 0, kernel, 0, feed, true);
    // gaps_on: the value only (codegen's clear_security clears the site's
    // history with it, so its [1] reads are the generated code's, not a
    // kernel fact).
    check_rows(pine, 1, kernel, 1, feed, false);
}

// ---- 4. a finer foreign feed under a daily chart, lookahead and [1] -------

void twin_lookahead_offset() {
    scenario = "twin: AAPL-1D-like chart <- VIX-240-like feed, lookahead on, [1]";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 8);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    const NativeInstrumentFeed feed = vix_like_240_feed(days);
    NativeRunSpec spec = nyse_spec("twin-lookahead", "D");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed, true, false));
    InstrumentHost kernel;
    if (!run_batch(kernel, spec, chart)) return;

    ForeignShape pine;
    prime_chart(pine, spec);
    pine.sites = {{0, "SYN:VIX", "240", true, false, false}};
    CHECK(install(pine, feed));
    if (!run_pine(pine, spec, chart)) return;
    check_rows(pine, 0, kernel, 0, feed, true);
    // The report's pin through the adapter: [time[1], time_close[1], close[1],
    // bar_index] on the first chart bar is [11:15Z, 13:26Z, bar 1's close, 2].
    if (!pine.rows.empty()) {
        const Row& first = pine.rows[0].at(0);
        CHECK(first.bar_index == 2.0);
        CHECK(first.time1 == static_cast<double>(monday() + 11 * kHour + 15 * kMinute));
        CHECK(first.time_close1 == static_cast<double>(monday() + 13 * kHour + 26 * kMinute));
        CHECK(same(first.close1, feed.bars[1].close));
    }
}

// ---- 5. beside a same-symbol site, on either drive -------------------------

void twin_beside_same_symbol() {
    scenario = "twin: a foreign site beside a same-symbol site";
    const std::vector<std::int64_t> days = {monday()};
    const std::vector<Bar> chart = nyse_quarter_chart(days);
    const NativeInstrumentFeed feed = dxy_like_quarter_feed(monday(), monday() + kDay);
    NativeRunSpec spec = nyse_spec("twin-beside", "15");
    spec.instrument_feeds.push_back(feed);
    spec.subscriptions.push_back(instrument_series(feed));
    InstrumentHost kernel;
    if (!run_batch(kernel, spec, chart)) return;

    // The same-symbol "60" site alone: the reference for its values.
    ForeignShape alone;
    prime_chart(alone, spec);
    alone.sites = {{0, "", "60", false, false, false}};
    if (!run_pine(alone, spec, chart)) return;

    // Same-symbol first (sec_id 0, dense): the kernel steps both.
    {
        ForeignShape pine;
        prime_chart(pine, spec);
        pine.sites = {{0, "", "60", false, false, false}, {1, "SYN:DXY", "15", false, false, false}};
        CHECK(install(pine, feed));
        if (!run_pine(pine, spec, chart)) return;
        check_rows(pine, 1, kernel, 0, feed, true);
        CHECK(pine.rows.size() == alone.rows.size());
        for (std::size_t j = 0; j < pine.rows.size() && j < alone.rows.size(); ++j) {
            CHECK(same_value(pine.rows[j].at(0).close, alone.rows[j].at(0).close));
            CHECK(same_value(pine.rows[j].at(0).time, alone.rows[j].at(0).time));
        }
        const auto view = pine.native_state();
        CHECK(view.spec != nullptr && view.spec->subscriptions.size() == 2);
    }
    // Foreign first (sec_id 0): the same-symbol site is not dense from 0, so it
    // stays on the adapter's own drive while the foreign one is the kernel's.
    {
        ForeignShape pine;
        prime_chart(pine, spec);
        pine.sites = {{0, "SYN:DXY", "15", false, false, false}, {1, "", "60", false, false, false}};
        CHECK(install(pine, feed));
        if (!run_pine(pine, spec, chart)) return;
        check_rows(pine, 0, kernel, 0, feed, true);
        for (std::size_t j = 0; j < pine.rows.size() && j < alone.rows.size(); ++j) {
            CHECK(same_value(pine.rows[j].at(1).close, alone.rows[j].at(0).close));
        }
        const auto view = pine.native_state();
        CHECK(view.spec != nullptr && view.spec->subscriptions.size() == 1
              && view.spec->subscriptions[0].source == NativeSeriesSource::InstrumentFeed);
    }
}

// ---- 6. ignore_invalid_symbol and the fail-closed refusals -----------------

void twin_invalid_and_refusals() {
    scenario = "twin: invalid symbols and fail-closed refusals";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 3);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    const NativeInstrumentFeed feed = us10y_like_daily_feed(days);
    const NativeRunSpec spec = nyse_spec("twin-refusals", "D");
    {
        // An invalid symbol under ignore_invalid_symbol reads na throughout.
        ForeignShape pine;
        prime_chart(pine, spec);
        pine.sites = {{0, "SYN:GONE", "D", false, false, true}};
        CHECK(pine.set_symbol_facts("SYN:GONE", "valid", "false"));
        if (run_pine(pine, spec, chart)) {
            CHECK(pine.rows.size() == chart.size());
            for (const auto& row : pine.rows) CHECK(std::isnan(row.at(0).close));
        }
    }
    {
        // Without it, the run fails closed naming the symbol.
        ForeignShape pine;
        prime_chart(pine, spec);
        pine.sites = {{0, "SYN:GONE", "D", false, false, false}};
        CHECK(pine.set_symbol_facts("SYN:GONE", "valid", "false"));
        pine.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
                 MagnifierDistribution::ENDPOINTS);
        CHECK(pine.last_error() == "request.security: symbol 'SYN:GONE' is invalid");
        CHECK(pine.rows.empty());
    }
    {
        // A registered key with no installed feed fails closed, naming the key
        // and the timeframe -- ignore_invalid_symbol does not excuse missing data.
        ForeignShape pine;
        prime_chart(pine, spec);
        pine.sites = {{0, "SYN:US10Y", "60", false, false, true}};
        CHECK(install(pine, feed));
        pine.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
                 MagnifierDistribution::ENDPOINTS);
        CHECK(pine.last_error()
              == "request.security: no feed is installed for symbol 'SYN:US10Y' at timeframe '60'");
    }
    {
        // A chart aggregated from finer input is refused: the merge is judged
        // per chart bar and the kernel hands bars over per input.
        const std::vector<Bar> quarters = nyse_quarter_chart({monday()});
        ForeignShape pine;
        prime_chart(pine, spec);
        pine.sites = {{0, "SYN:DXY", "15", false, false, false}};
        CHECK(install(pine, dxy_like_quarter_feed(monday(), monday() + kDay)));
        pine.run(quarters.data(), static_cast<int>(quarters.size()), "15", "60", false, 4,
                 MagnifierDistribution::ENDPOINTS);
        CHECK(pine.last_error().find("needs the chart's own bars as input") != std::string::npos);
    }
    {
        // Streams refuse an installed symbol feed at stream_begin.
        ForeignShape pine;
        prime_chart(pine, spec);
        CHECK(install(pine, feed));
        CHECK(!pine.stream_begin(chart.data(), static_cast<int>(chart.size()), "D", "D"));
        CHECK(pine.last_error()
              == "request.security symbol feeds and recorded request series support historical "
                 "runs only");
    }
    {
        // The doors refuse malformed input by name and install nothing.
        ForeignShape pine;
        NativeInstrumentFeed bad = feed;
        bad.close_ms[1] = bad.bars[1].timestamp;
        CHECK(!pine.set_symbol_feed(bad.instrument, bad.tf, bad.bars.data(), bad.close_ms.data(),
                                    static_cast<int>(bad.bars.size())));
        CHECK(pine.last_error().find("strategy_set_symbol_feed: feed refused") == 0);
        CHECK(!pine.set_symbol_feed("SYN:X", "7Q", feed.bars.data(), feed.close_ms.data(), 1));
        CHECK(!pine.set_symbol_feed_column("SYN:NOPE", "D", "x", nullptr, 0));
        CHECK(!pine.set_symbol_facts("SYN:X", "mintick", "-1"));
        CHECK(!pine.set_symbol_facts("SYN:X", "valid", "maybe"));
        CHECK(!pine.set_symbol_facts("SYN:X", "color", "red"));
        CHECK(install(pine, feed));
        std::vector<double> short_column(feed.bars.size() - 1, 1.0);
        CHECK(!pine.set_symbol_feed_column(feed.instrument, feed.tf, "x", short_column.data(),
                                           static_cast<int>(short_column.size())));
    }
}

// ---- 7. the broker hash ----------------------------------------------------

void twin_hash() {
    scenario = "twin: broker hash";
    const std::vector<std::int64_t> days = weekdays_from(monday(), 5);
    const std::vector<Bar> chart = nyse_daily_chart(days);
    const NativeInstrumentFeed feed = us10y_like_daily_feed(days);
    const NativeRunSpec spec = nyse_spec("twin-hash", "D");
    const auto run_hash = [&](const NativeInstrumentFeed& installed, bool with_site) {
        ForeignShape pine;
        prime_chart(pine, spec);
        if (with_site) pine.sites = {{0, "SYN:US10Y", "D", false, false, false}};
        if (!installed.bars.empty()) install(pine, installed);
        run_pine(pine, spec, chart);
        return pine.broker_state_hash();
    };
    const std::uint64_t plain = run_hash(NativeInstrumentFeed{}, false);
    const std::uint64_t with_feed = run_hash(feed, true);
    CHECK(run_hash(feed, true) == with_feed);
    CHECK(with_feed != plain);
    NativeInstrumentFeed other = feed;
    other.close_ms[2] -= kMinute;
    CHECK(run_hash(other, true) != with_feed);
}

}  // namespace

int main() {
    twin_foreign_session();
    twin_daily_late();
    twin_weekend();
    twin_lookahead_offset();
    twin_beside_same_symbol();
    twin_invalid_and_refusals();
    twin_hash();
    std::printf("test_native_instrument_feed_twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
