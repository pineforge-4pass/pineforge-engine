/*
 * test_issue694_security_site_projection.cpp -- #694 behaviour 5.
 *
 * On historical bars TradingView's
 *
 *     request.security(syminfo.tickerid, "60", close, lookahead = barmerge.lookahead_on)
 *
 * reads, on every chart bar of a requested bar, that requested bar's final
 * value. The registration of a same-symbol request site takes an optional
 * seventh argument that asks for this on that one site. Without it, and on
 * a stream, the site reads the developing value, as before.
 *
 * The daily rows use the reported BTCUSDT hourly feed and TradingView readings.
 * Smaller feeds distinguish evaluator dispatch, history slots and chart reads.
 *
 * The file compiles against a tree without the seventh argument: there the
 * opt-in rows register the site with the six-argument form, the only one
 * that tree has, and fail on the values the engine then produces. The form
 * used is printed on each row. Rows marked "control" hold on both trees.
 *
 * Output: for each row a RUN line, OBSERVED and EXPECTED lines, and a PASS or
 * FAIL line; then one SUMMARY line. Exit status 1 when a row failed.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr std::int64_t kQuarterMs = 900000LL;  // the chart: 15 minutes
constexpr std::int64_t kHourMs = 3600000LL;    // the request: 60 minutes

int rows_run = 0;
int rows_failed = 0;

// Quarter-hour bars from 01:00 UTC. Bar i closes at 10 * (i + 1) and opens at
// the previous close; every bar has its own high, low and volume.
std::vector<Bar> quarter_hour_bars(int count) {
    std::vector<Bar> bars;
    for (int index = 0; index < count; ++index) {
        Bar bar{};
        bar.open = 10.0 * static_cast<double>(std::max(index, 1));
        bar.close = 10.0 * static_cast<double>(index + 1);
        bar.high = bar.close + 1.0;
        bar.low = bar.open - 1.0;
        bar.volume = static_cast<double>(index + 1);
        bar.timestamp = kHourMs + static_cast<std::int64_t>(index) * kQuarterMs;
        bars.push_back(bar);
    }
    return bars;
}

std::int64_t hour_of(const Bar& bar) { return bar.timestamp / kHourMs; }

// The aggregate of the bars [first, last] that share bars[at]'s hour.
Bar hour_aggregate(const std::vector<Bar>& bars, std::size_t at, std::size_t first,
                   std::size_t last) {
    Bar out{};
    bool seeded = false;
    for (std::size_t index = first; index <= last && index < bars.size(); ++index) {
        if (hour_of(bars[index]) != hour_of(bars[at])) continue;
        if (!seeded) {
            out = bars[index];
            seeded = true;
            continue;
        }
        out.high = std::max(out.high, bars[index].high);
        out.low = std::min(out.low, bars[index].low);
        out.close = bars[index].close;
        out.volume += bars[index].volume;
    }
    return out;
}

struct Dispatch {
    Bar bar;
    bool complete = false;
    bool new_slot = false;
};

struct Observed {
    std::vector<Dispatch> dispatches;
    std::vector<double> chart;  // what each chart bar read
    double visible = na<double>();
    double last_completed_close = na<double>();
};

struct Site {
    bool lookahead_on = true;
    bool gaps_on = false;
    // Ask for the historical projection on this site: the seventh argument.
    bool historical_projection = false;
    bool previous_close = false;
};

Site site_of(bool lookahead_on, bool gaps_on, bool historical_projection) {
    Site site;
    site.lookahead_on = lookahead_on;
    site.gaps_on = gaps_on;
    site.historical_projection = historical_projection;
    return site;
}

class SiteHost final : public source::PineStrategyHost {
public:
    explicit SiteHost(const std::vector<Site>& sites, const std::string& requested = "60")
        : sites_(sites), requested_(requested), observed_(sites.size()) {
        attach_pine_execution_adapter();
        configure_pine_strategy(source::PineStrategyConfig{});
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (std::size_t index = 0; index < sites_.size(); ++index) {
            const Site& site = sites_[index];
            const int sec_id = static_cast<int>(index);
            if (site.historical_projection) {
                seven_argument_ = register_projected(*this, sec_id, requested_, input_tf_,
                                                     site.lookahead_on, site.gaps_on, 0);
            } else {
                register_security_eval(sec_id, requested_, input_tf_, site.lookahead_on,
                                       site.gaps_on, false);
            }
        }
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id < 0 || static_cast<std::size_t>(sec_id) >= observed_.size()) return;
        Observed& site = observed_[static_cast<std::size_t>(sec_id)];
        site.dispatches.push_back({bar, is_complete, security_series_slot_is_new(sec_id)});
        // close[1] reads the previous requested close before publishing the current one.
        site.visible = sites_[sec_id].previous_close ? site.last_completed_close : bar.close;
        if (is_complete) site.last_completed_close = bar.close;
    }

    void on_source_bar(const Bar&) override {
        for (Observed& site : observed_) site.chart.push_back(site.visible);
    }

    const std::vector<Observed>& observed() const { return observed_; }
    // Whether an opt-in site was registered with the seventh argument.
    bool seven_argument() const { return seven_argument_; }

private:
    // The seven-argument same-symbol registration where the tree has it.
    template<class Host>
    static auto register_projected(Host& host, int sec_id, const std::string& requested,
                                   const std::string& input, bool lookahead_on, bool gaps_on,
                                   int)
        -> decltype(host.register_security_eval(sec_id, requested, input, lookahead_on,
                                                gaps_on, false, true),
                    bool()) {
        host.register_security_eval(sec_id, requested, input, lookahead_on, gaps_on, false,
                                    true);
        return true;
    }
    // A tree without it has only the six-argument registration.
    template<class Host>
    static bool register_projected(Host& host, int sec_id, const std::string& requested,
                                   const std::string& input, bool lookahead_on, bool gaps_on,
                                   long) {
        host.register_security_eval(sec_id, requested, input, lookahead_on, gaps_on, false);
        return false;
    }

    std::vector<Site> sites_;
    std::string requested_;
    std::vector<Observed> observed_;
    bool seven_argument_ = false;
};

struct RunResult {
    std::string error;
    std::vector<Observed> sites;
    bool seven_argument = false;
};

RunResult run_batch(const std::vector<Site>& sites, const std::vector<Bar>& bars,
                    bool run_level_flag, bool realtime_tail) {
    SiteHost host(sites);
    if (run_level_flag) host.set_syminfo_metadata("historical_security_lookahead_projection", 1.0);
    if (realtime_tail) host.set_realtime_tail(true, static_cast<int>(bars.size()));
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    RunResult result;
    result.error = host.last_error();
    result.sites = host.observed();
    result.seven_argument = host.seven_argument();
    return result;
}

bool same(double left, double right) {
    if (is_na(left) && is_na(right)) return true;
    if (is_na(left) || is_na(right)) return false;
    return left == right;
}

bool same_values(const std::vector<double>& left, const std::vector<double>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (!same(left[index], right[index])) return false;
    return true;
}

std::string text_of(const std::vector<double>& values) {
    std::string out = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        char cell[48];
        if (is_na(values[index])) std::snprintf(cell, sizeof cell, "na");
        else std::snprintf(cell, sizeof cell, "%.2f", values[index]);
        if (index != 0) out += ",";
        out += cell;
    }
    return out + "]";
}

std::vector<double> dispatch_closes(const Observed& site) {
    std::vector<double> out;
    for (const Dispatch& dispatch : site.dispatches) out.push_back(dispatch.bar.close);
    return out;
}

std::string completion_text(const Observed& site) {
    std::string out;
    for (const Dispatch& dispatch : site.dispatches) out += dispatch.complete ? "1" : "0";
    return out;
}

const char* form_name(bool seven_argument) {
    return seven_argument ? "seven-argument" : "six-argument (this tree has no seventh argument)";
}

void finish(const char* row, bool pass) {
    ++rows_run;
    if (!pass) ++rows_failed;
    std::printf("[694-B5] ROW %s %s\n", row, pass ? "PASS" : "FAIL");
}

// The developing close: every chart bar reads its own close.
std::vector<double> developing_closes(const std::vector<Bar>& bars) {
    std::vector<double> out;
    for (const Bar& bar : bars) out.push_back(bar.close);
    return out;
}

// The final close of each bar's hour over the bars [0, last].
std::vector<double> final_closes(const std::vector<Bar>& bars, std::size_t last) {
    std::vector<double> out;
    for (std::size_t index = 0; index <= last && index < bars.size(); ++index)
        out.push_back(hour_aggregate(bars, index, 0, last).close);
    return out;
}

void check_chart(const char* row, const char* kind, const RunResult& got, std::size_t site,
                 const std::vector<double>& want, bool print_form) {
    const bool present = site < got.sites.size();
    const std::vector<double> seen = present ? got.sites[site].chart : std::vector<double>{};
    const bool pass = got.error.empty() && present && same_values(seen, want);
    if (print_form) {
        std::printf("[694-B5] ROW %s RUN kind=%s registration=%s\n", row, kind,
                    form_name(got.seven_argument));
    } else {
        std::printf("[694-B5] ROW %s RUN kind=%s registration=six-argument\n", row, kind);
    }
    std::printf("[694-B5] ROW %s OBSERVED chart=%s error=\"%s\"\n", row, text_of(seen).c_str(),
                got.error.c_str());
    std::printf("[694-B5] ROW %s EXPECTED chart=%s\n", row, text_of(want).c_str());
    finish(row, pass);
}

std::vector<Bar> btc_hourly_bars() {
    const std::string source = __FILE__;
    const auto path = source.substr(0, source.find_last_of("/\\") + 1)
        + "fixtures/requested_bar_grid/btc60/feed/bars.csv";
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line) || line != "timestamp,open,high,low,close,volume")
        throw std::runtime_error("missing or invalid BTCUSDT hourly feed header");

    std::vector<Bar> bars;
    std::int64_t next_hour = 1704067200000;  // 2024-01-01 00:00 UTC
    while (std::getline(input, line)) {
        if (std::count(line.begin(), line.end(), ',') != 5)
            throw std::runtime_error("invalid BTCUSDT hourly feed columns");
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        Bar b{};
        if (!(row >> b.timestamp >> b.open >> b.high >> b.low >> b.close >> b.volume)
            || !(row >> std::ws).eof() || b.timestamp != next_hour
            || !(b.low <= b.close && b.close <= b.high && b.low <= b.open && b.open <= b.high))
            throw std::runtime_error("invalid or missing BTCUSDT hourly bar");
        bars.push_back(b);
        next_hour += kHourMs;
    }
    if (!input.eof() || input.bad() || bars.size() != 1441 || bars[10].close != 42691.10
        || bars[23].close != 44179.55 || bars[1439].close != 61130.98)
        throw std::runtime_error("BTCUSDT hourly feed does not match the reported range or prices");
    return bars;
}

// daily = request.security(syminfo.tickerid, "D", close, lookahead = barmerge.lookahead_on)
// The non-repainting control changes only the expression to close[1].
void check_daily_close(const std::vector<Bar>& bars, bool previous) {
    const char* row = previous ? "previous-daily-close" : "daily-close";
    Site site = site_of(true, false, true);
    site.previous_close = previous;
    SiteHost host({site}, "D");
    source::StrategyOverrides overrides{};
    overrides.initial_capital = 10000;
    host.set_strategy_override(overrides);
    host.set_syminfo_string("tickerid", "BINANCE:BTCUSDT");
    host.set_syminfo_session("24x7");
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_type("crypto");
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    const auto& observed = host.observed()[0];
    const auto& values = observed.chart;
    std::printf("ROW %s RUN kind=%s registration=%s\n", row, previous ? "control" : "red-on-base",
                form_name(host.seven_argument()));
    std::size_t defined = 0;
    std::size_t matching = 0;
    std::size_t chart_closes = 0;
    for (std::size_t i = 0; i < values.size() && i < bars.size(); ++i) {
        const std::size_t day_start = i - i % 24;
        const std::size_t last_hour = std::min(day_start + 23, bars.size() - 1);
        double wanted = bars[last_hour].close;
        if (previous) wanted = day_start > 0 ? bars[day_start - 1].close : na<double>();
        if (!is_na(values[i])) ++defined;
        if (values[i] == bars[i].close) ++chart_closes;
        if (same(values[i], wanted)) ++matching;
        else if (matching == i)
            std::printf("ROW %s first mismatch at bar %zu: expected %.2f, got %.2f\n",
                        row, i, wanted, values[i]);
    }
    const auto complete = std::count_if(observed.dispatches.begin(), observed.dispatches.end(),
                                       [](const Dispatch& d) { return d.complete; });
    const auto partial = observed.dispatches.size() - static_cast<std::size_t>(complete);
    if (values.size() == 1441)
        std::printf("ROW %s OBSERVED bar0=%.2f bar10=%.2f bar1440=%.2f\n", row,
                    values.front(), values[10], values.back());
    std::printf("ROW %s OBSERVED bars=%zu matched=%zu defined=%zu chart_closes=%zu "
                "complete=%zu partial=%zu error=\"%s\"\n", row, values.size(), matching,
                defined, chart_closes, static_cast<std::size_t>(complete),
                partial, host.last_error().c_str());
    const bool prices = values.size() == 1441 && (previous ? values[1440] == 61130.98
        : values[0] == 44179.55 && values[10] == 44179.55 && chart_closes == 61);
    std::printf("ROW %s EXPECTED matched=1441 defined=%d %s\n", row, previous ? 1417 : 1441,
                previous ? "bar1440=61130.98"
                         : "bar0=bar10=44179.55 chart_closes=61 complete=60 partial=1");
    finish(row, host.last_error().empty() && prices && matching == 1441
                && (previous || (complete == 60 && partial == 1))
                && defined == (previous ? 1417u : 1441u));
}

void check_native_forming_tail() {
    const auto chart = quarter_hour_bars(6);
    const Bar native[] = {{10, 410, 9, 400, 100, kHourMs},
                          {40, 810, 39, 800, 200, 2 * kHourMs}};
    SiteHost host({site_of(true, false, true), site_of(true, false, false)});
    const bool installed = host.set_native_security_feed("60", native, 2);
    host.set_syminfo_metadata("historical_security_lookahead_projection", 1.0);
    host.set_realtime_tail(true, static_cast<int>(chart.size()));
    host.run(chart.data(), static_cast<int>(chart.size()), "15", "15");
    RunResult got{installed ? host.last_error() : "native feed refused", host.observed(),
                  host.seven_argument()};
    check_chart("forming-hour-excludes-native-final", "red-on-base", got, 0,
                {400, 400, 400, 400, 50, 60}, true);
    check_chart("run-level-projection-keeps-native-final", "control", got, 1,
                {400, 400, 400, 400, 800, 800}, false);

    const auto& seen = got.sites[0];
    std::string slots;
    for (const auto& d : seen.dispatches) slots += d.new_slot ? '1' : '0';
    const char* row = "forming-hour-rewrites-one-requested-slot";
    std::printf("ROW %s OBSERVED new_slot=%s complete=%s EXPECTED new_slot=110 complete=100\n",
                row, slots.c_str(), completion_text(seen).c_str());
    finish(row, got.error.empty() && slots == "110" && completion_text(seen) == "100"
                && seen.dispatches.back().bar.open == 40
                && seen.dispatches.back().bar.volume == 11);
}

void check_indivisible_native_periods() {
    const auto chart = quarter_hour_bars(6);
    // A 15-minute chart cannot reconstruct the close at 01:20 inside its 01:15 bar.
    SiteHost unbacked({site_of(true, false, true)}, "20");
    unbacked.run(chart.data(), static_cast<int>(chart.size()), "15", "15");
    check_chart("indivisible-timeframe-without-native-bars", "control",
                {unbacked.last_error(), unbacked.observed(), unbacked.seven_argument()}, 0,
                developing_closes(chart), true);

    // Each chart open selects the native 20-minute bar that contains it.
    std::vector<Bar> native;
    for (int i = 0; i < 4; ++i) {
        const double price = 700 + i;
        native.push_back({price, price, price, price, 1, kHourMs + i * 1'200'000LL});
    }
    SiteHost host({site_of(true, false, true)}, "20");
    const bool installed = host.set_native_security_feed("20", native.data(),
                                                         static_cast<int>(native.size()));
    host.run(chart.data(), static_cast<int>(chart.size()), "15", "15");
    check_chart("indivisible-timeframe-uses-native-period-at-chart-open", "control",
                {installed ? host.last_error() : "native feed refused", host.observed(),
                 host.seven_argument()}, 0, {700, 700, 701, 702, 703, 703}, true);

    // The 01:00 native row is missing. Its two chart children close at 10 and 20.
    SiteHost sparse({site_of(true, false, true)}, "20");
    const bool sparse_installed = sparse.set_native_security_feed("20", native.data() + 1, 3);
    sparse.run(chart.data(), static_cast<int>(chart.size()), "15", "15");
    check_chart("indivisible-timeframe-projects-only-the-missing-native-period", "red-on-base",
                {sparse_installed ? sparse.last_error() : "native feed refused", sparse.observed(),
                 sparse.seven_argument()}, 0, {20, 20, 701, 702, 703, 703}, true);
}

void check_foreign_literal() {
    class ForeignHost final : public source::PineStrategyHost {
    public:
        void configure_security_evaluators() override {
            register_security_eval(0, "TEST:OTHER", "60", "15", true, false, false);
        }
        void evaluate_security(int, const Bar& bar, bool) override { value = bar.close; }
        void on_source_bar(const Bar&) override { chart.push_back(value); }
        double value = na<double>();
        std::vector<double> chart;
    } host;
    const auto chart = quarter_hour_bars(6);
    const Bar foreign[] = {{500, 501, 499, 500, 10, kHourMs},
                           {900, 901, 899, 900, 20, 2 * kHourMs}};
    const std::int64_t closes[] = {2 * kHourMs, 3 * kHourMs};
    const bool installed = host.set_symbol_feed("TEST:OTHER", "60", foreign, closes, 2);
    host.run(chart.data(), static_cast<int>(chart.size()), "15", "15");
    const char* row = "literal-input-timeframe-reads-foreign-feed";
    std::printf("ROW %s OBSERVED chart=%s error=\"%s\" EXPECTED chart=[500,500,500,500,900,900]\n",
                row, text_of(host.chart).c_str(), host.last_error().c_str());
    finish(row, installed && host.last_error().empty()
                && same_values(host.chart, {500, 500, 500, 500, 900, 900}));
}

void check_registration_hash() {
    // Sample before feeding any bars: differing results cannot account for a differing hash.
    const auto fingerprint = [](bool project) {
        SiteHost host({site_of(true, false, project)});
        host.configure_security_evaluators();
        return host.broker_state_hash();
    };
    const auto plain = fingerprint(false);
    const auto asked = fingerprint(true);
    const char* row = "registration-option-enters-broker-hash";
    std::printf("ROW %s OBSERVED equal=%d EXPECTED equal=0\n", row, plain == asked);
    finish(row, plain != asked);
}

void check_suppressed_tail() {
    const auto chart = quarter_hour_bars(7);
    SiteHost host({site_of(true, false, true)});
    host.set_probe_suppress_tail_logic(true);
    host.run(chart.data(), static_cast<int>(chart.size()), "15", "15");
    // No ordinary calculation on the seventh child. The second hour's two confirmed
    // children see 60; only the suppressed child's evaluator receives 70.
    const auto& seen = host.observed()[0];
    const char* row = "suppressed-tail-excluded-from-confirmed-closes";
    std::printf("ROW %s OBSERVED chart=%s dispatch=%s EXPECTED chart=[40,40,40,40,60,60] "
                "dispatch=[40,60,70]\n", row, text_of(seen.chart).c_str(),
                text_of(dispatch_closes(seen)).c_str());
    finish(row, host.last_error().empty() && same_values(seen.chart, {40, 40, 40, 40, 60, 60})
                && same_values(dispatch_closes(seen), {40, 60, 70}));
}

}  // namespace

int main() {
    std::vector<Bar> hourly;
    try {
        hourly = btc_hourly_bars();
    } catch (const std::exception& error) {
        for (const char* row : {"daily-close", "previous-daily-close"}) {
            std::printf("ROW %s NOT COMPARED: %s\n", row, error.what());
            finish(row, false);
        }
    }
    if (!hourly.empty()) {
        check_daily_close(hourly, false);
        check_daily_close(hourly, true);
    }
    const std::vector<Bar> bars = quarter_hour_bars(12);  // three whole hours
    const std::vector<double> developing = developing_closes(bars);
    const std::vector<double> projected = final_closes(bars, bars.size() - 1);
    check_native_forming_tail();
    check_indivisible_native_periods();
    check_foreign_literal();
    check_registration_hash();
    check_suppressed_tail();
    {
        SiteHost host({site_of(true, false, true)});
        host.run(bars.data(), static_cast<int>(bars.size()), "15", "30");
        check_chart("finer-input-keeps-developing-close", "control",
                    {host.last_error(), host.observed(), host.seven_argument()}, 0,
                    {20, 40, 60, 80, 100, 120}, true);
    }

    // ---- The site that asks: every chart bar reads its hour's final close --
    {
        const RunResult got = run_batch({site_of(true, false, true)}, bars, false, false);
        check_chart("B5.site-opt-in.chart-reads-final-hour-close", "red-on-base", got, 0,
                    projected, true);

        // What the evaluator was handed: one completed bar per hour, the
        // hour's whole aggregate, on the hour's first chart bar.
        const char* row = "B5.site-opt-in.evaluator-gets-whole-hour-once";
        const Observed empty;
        const Observed& site = got.sites.empty() ? empty : got.sites[0];
        bool pass = got.error.empty() && site.dispatches.size() == 3;
        std::printf("[694-B5] ROW %s RUN kind=red-on-base registration=%s\n", row,
                    form_name(got.seven_argument));
        for (std::size_t index = 0; index < site.dispatches.size(); ++index) {
            const Dispatch& seen = site.dispatches[index];
            std::printf("[694-B5] ROW %s OBSERVED dispatch=%zu complete=%d open=%.2f high=%.2f "
                        "low=%.2f close=%.2f volume=%.2f\n",
                        row, index, seen.complete ? 1 : 0, seen.bar.open, seen.bar.high,
                        seen.bar.low, seen.bar.close, seen.bar.volume);
        }
        for (std::size_t hour = 0; hour < 3; ++hour) {
            const Bar want = hour_aggregate(bars, hour * 4, 0, bars.size() - 1);
            std::printf("[694-B5] ROW %s EXPECTED dispatch=%zu complete=1 open=%.2f high=%.2f "
                        "low=%.2f close=%.2f volume=%.2f\n",
                        row, hour, want.open, want.high, want.low, want.close, want.volume);
            if (hour < site.dispatches.size()) {
                const Dispatch& seen = site.dispatches[hour];
                pass = pass && seen.complete && same(seen.bar.open, want.open)
                    && same(seen.bar.high, want.high) && same(seen.bar.low, want.low)
                    && same(seen.bar.close, want.close) && same(seen.bar.volume, want.volume);
            }
        }
        finish(row, pass);
    }

    // ---- Control: the six-argument registration reads the developing close -
    {
        const RunResult plain = run_batch({site_of(true, false, false)}, bars, false, false);
        check_chart("B5.control.six-argument.chart-reads-developing-close", "control", plain, 0,
                    developing, false);

        const char* row = "B5.control.six-argument.evaluator-gets-every-bar";
        const Observed empty;
        const Observed& site = plain.sites.empty() ? empty : plain.sites[0];
        const std::vector<double> closes = dispatch_closes(site);
        const std::string completion = completion_text(site);
        // The bar that closes an hour is the completed evaluation.
        std::string want_completion;
        for (std::size_t index = 0; index < bars.size(); ++index) {
            const bool closes_hour = index + 1 == bars.size()
                || hour_of(bars[index + 1]) != hour_of(bars[index]);
            want_completion += closes_hour ? "1" : "0";
        }
        const bool pass = plain.error.empty() && same_values(closes, developing)
            && completion == want_completion;
        std::printf("[694-B5] ROW %s RUN kind=control registration=six-argument\n", row);
        std::printf("[694-B5] ROW %s OBSERVED dispatch_closes=%s completion=%s error=\"%s\"\n",
                    row, text_of(closes).c_str(), completion.c_str(), plain.error.c_str());
        std::printf("[694-B5] ROW %s EXPECTED dispatch_closes=%s completion=%s\n", row,
                    text_of(developing).c_str(), want_completion.c_str());
        finish(row, pass);
    }

    // ---- Control: the run-level switch still projects a six-argument site --
    {
        const RunResult got = run_batch({site_of(true, false, false)}, bars, true, false);
        check_chart("B5.control.run-level-switch.chart-reads-final-hour-close", "control", got,
                    0, projected, false);
    }

    // ---- Per site: two sites of one run, one asks and one does not --------
    {
        const RunResult got = run_batch({site_of(true, false, true), site_of(true, false, false)},
                                        bars, false, false);
        check_chart("B5.per-site.asking-site-reads-final-hour-close", "red-on-base", got, 0,
                    projected, true);
        check_chart("B5.control.per-site.other-site-reads-developing-close", "control", got, 1,
                    developing, false);
    }

    // With lookahead off only the fourth child publishes; gaps_off holds it.
    {
        const RunResult asked = run_batch({site_of(false, false, true)}, bars, false, false);
        check_chart("B5.control.lookahead-off.same-as-six-argument", "control", asked, 0,
                    {na<double>(), na<double>(), na<double>(), 40, 40, 40, 40, 80, 80, 80, 80, 120},
                    true);
    }

    // ---- Control: gaps on is unchanged by the seventh argument -------------
    {
        const RunResult asked = run_batch({site_of(true, true, true)}, bars, false, false);
        const char* row = "B5.control.gaps-on.evaluator-gets-every-bar";
        const Observed empty;
        const Observed& site = asked.sites.empty() ? empty : asked.sites[0];
        const std::vector<double> closes = dispatch_closes(site);
        const bool pass = asked.error.empty() && same_values(closes, developing);
        std::printf("[694-B5] ROW %s RUN kind=control registration=%s\n", row,
                    form_name(asked.seven_argument));
        std::printf("[694-B5] ROW %s OBSERVED dispatch_closes=%s error=\"%s\"\n", row,
                    text_of(closes).c_str(), asked.error.c_str());
        std::printf("[694-B5] ROW %s EXPECTED dispatch_closes=%s\n", row,
                    text_of(developing).c_str());
        finish(row, pass);
    }

    // ---- The forming last bar stays out of the bars before it --------------
    // Six bars, the last one still forming (the live runner's tail). The
    // first hour is complete: its four bars read its final close. The second
    // hour has one confirmed bar and the forming one: the confirmed bar reads
    // the hour as confirmed so far, and only the forming bar reads its own
    // developing close.
    {
        const std::vector<Bar> six = quarter_hour_bars(6);
        const RunResult got = run_batch({site_of(true, false, true)}, six, false, true);
        std::vector<double> want = final_closes(six, six.size() - 2);
        want.push_back(six.back().close);
        check_chart("B5.site-opt-in.forming-tail-not-read-by-confirmed-bars", "red-on-base", got,
                    0, want, true);
    }

    // ---- Control: a stream stays on the developing value --------------------
    {
        const char* row = "B5.control.stream.evaluator-gets-developing-values";
        const std::vector<Bar> six = quarter_hour_bars(6);
        SiteHost host({site_of(true, false, true)});
        bool accepted = host.stream_begin(six.data(), 4, "15", "15");
        accepted = host.stream_push_tick(TradeTick{six[4].timestamp, 1, six[4].close, 5.0})
            && accepted;
        accepted = host.stream_advance_time(six[5].timestamp) && accepted;
        accepted = host.stream_push_tick(TradeTick{six[5].timestamp, 2, six[5].close, 6.0})
            && accepted;
        accepted = host.stream_advance_time(six[5].timestamp + kQuarterMs) && accepted;
        accepted = host.stream_end() && accepted;
        const Observed empty;
        const Observed& site = host.observed().empty() ? empty : host.observed()[0];
        const std::vector<double> closes = dispatch_closes(site);
        const std::vector<double> want = developing_closes(six);
        const bool pass = accepted && same_values(closes, want);
        std::printf("[694-B5] ROW %s RUN kind=control registration=%s\n", row,
                    form_name(host.seven_argument()));
        std::printf("[694-B5] ROW %s OBSERVED dispatch_closes=%s stream_calls_accepted=%d\n", row,
                    text_of(closes).c_str(), accepted ? 1 : 0);
        std::printf("[694-B5] ROW %s EXPECTED dispatch_closes=%s stream_calls_accepted=1\n", row,
                    text_of(want).c_str());
        finish(row, pass);
    }

    std::printf("[694-B5] SUMMARY rows=%d passed=%d failed=%d\n", rows_run,
                rows_run - rows_failed, rows_failed);
    return rows_failed == 0 ? 0 : 1;
}
