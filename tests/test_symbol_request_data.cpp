// The request data of other symbols through the runtime C ABI (lane XSYM-D):
// strategy_set_symbol_feed, strategy_set_symbol_feed_column,
// strategy_set_symbol_facts and strategy_set_recorded_series, each behind its
// own PINEFORGE_HAS_..._V1 probe, and the recorded-series store the Pine
// source host keeps.
//
//   1. the recorded series: a chart bar whose open has a row reads that row's
//      value exactly; every other chart bar reads na; a key nobody installed
//      fails the read closed, naming the key; n == 0 installs a series that
//      reads na on every bar (a request na throughout records no row);
//   2. the four setters install through the C ABI on a Pine handle: a foreign
//      site then reads the feed's bars, its column and its facts; a feed of
//      n == 0 bars is installed, and its site reads na on every chart bar;
//   3. every argument refusal answers -1 without installing anything, and the
//      validation refusals name their cause in strategy_get_last_error;
//   4. a host with no source layer answers each setter -1;
//   5. strategy_stream_begin fails closed while a symbol feed or a recorded
//      series is installed (the C ABI pin in scripts/check_c_abi_runtime.py),
//      and while a run is in progress every setter answers -1, naming it;
//   6. the symbol overload of register_security_eval cannot be spelled with a
//      literal input_tf in five or six arguments, where it would bind the
//      same-symbol overload instead.
//
// Synthetic values only: no TradingView bar enters the repository.

// Include order is load-bearing: pineforge.h BEFORE engine.hpp keeps the
// per-strategy declarations visible (engine.hpp defines PINEFORGE_NO_STRATEGY_DECLS).
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/na.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(PINEFORGE_HAS_SYMBOL_FEED_V1) || !defined(PINEFORGE_HAS_SYMBOL_FEED_COLUMN_V1) \
    || !defined(PINEFORGE_HAS_SYMBOL_FACTS_V1) || !defined(PINEFORGE_HAS_RECORDED_SERIES_V1)
#error "the symbol-data C ABI probes are missing"
#endif
#ifndef PINEFORGE_HAS_SYMBOL_SECURITY_EVAL_V1
#error "the symbol key overload of register_security_eval is missing"
#endif

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "initialization";

#define CHECK(expression)                                                      \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expression)) {                                                   \
            ++failures;                                                        \
            std::printf("FAIL [%s] line %d: %s\n", scenario, __LINE__,         \
                        #expression);                                          \
        }                                                                      \
    } while (false)

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;
// Monday 2025-04-07 13:30Z, the NYSE-like session open.
constexpr std::int64_t kFirstOpen = 1744032600000LL;

std::vector<Bar> daily_chart(int n) {
    std::vector<Bar> bars;
    std::int64_t t = kFirstOpen;
    for (int k = 0; bars.size() < static_cast<std::size_t>(n); t += kDay) {
        const std::int64_t days = t / kDay;
        const int weekday = static_cast<int>(((days % 7) + 7 + 3) % 7);  // 0 Monday
        if (weekday >= 5) continue;
        Bar bar{};
        bar.open = 12.0 + 0.1 * k;
        bar.high = bar.open + 0.3;
        bar.low = bar.open - 0.2;
        bar.close = bar.open + 0.05;
        bar.volume = 1000.0 + k;
        bar.timestamp = t;
        bars.push_back(bar);
        ++k;
    }
    return bars;
}

pf_strategy_t handle(BacktestEngine& engine) { return static_cast<void*>(&engine); }

void prime(BacktestEngine& engine) {
    engine.set_syminfo_timezone("America/New_York");
    engine.set_syminfo_session("0930-1600");
    engine.set_syminfo_type("stock");
    engine.set_syminfo_string("tickerid", "TEST:F");
}

const char* kEarnings = "earnings|TEST:F|actual|-|gaps_on|lookahead_off";

// A strategy reading one recorded series per chart bar, and (optionally) one
// foreign site: its close, its column and its syminfo.type.
class Reader final : public source::PineStrategyHost {
public:
    std::string key = kEarnings;
    bool read_series = true;
    bool foreign = false;
    std::vector<double> series;
    std::vector<double> close, column;
    std::vector<std::string> type;
    double close_ = na<double>(), column_ = na<double>();
    std::string type_;
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        if (foreign) register_security_eval(0, "SYN:US10Y", "D", input_tf_, false, false, false);
    }
    void evaluate_security(int sec_id, const Bar& bar, bool) override {
        close_ = bar.close;
        column_ = security_column_value(sec_id, "carry");
        type_ = syminfo_.type;
    }
    void clear_security(int) override { close_ = na<double>(); }
    void on_source_bar(const Bar&) override {
        if (read_series) series.push_back(recorded_series_value(key));
        close.push_back(close_);
        column.push_back(column_);
        type.push_back(type_);
    }
};

// ---- 1. the recorded series -------------------------------------------------

void recorded_series_exact_on_event_bars() {
    scenario = "recorded series";
    const std::vector<Bar> chart = daily_chart(10);
    // Three event bars; the rest have no row.
    const std::vector<std::int64_t> open = {chart[2].timestamp, chart[5].timestamp,
                                            chart[9].timestamp};
    const std::vector<double> values = {1.65, -0.003289, 0.14};
    Reader reader;
    prime(reader);
    CHECK(strategy_set_recorded_series(handle(reader), kEarnings, open.data(), values.data(),
                                       static_cast<int>(open.size())) == 0);
    reader.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(reader.last_error().empty());
    CHECK(reader.series.size() == chart.size());
    for (std::size_t j = 0; j < reader.series.size(); ++j) {
        if (j == 2) CHECK(reader.series[j] == 1.65);
        else if (j == 5) CHECK(reader.series[j] == -0.003289);
        else if (j == 9) CHECK(reader.series[j] == 0.14);
        else CHECK(std::isnan(reader.series[j]));
    }

    // A key nobody installed fails the read closed, naming it.
    Reader missing;
    prime(missing);
    missing.key = "earnings|TEST:F|estimate|-|gaps_on|lookahead_off";
    CHECK(strategy_set_recorded_series(handle(missing), kEarnings, open.data(), values.data(),
                                       static_cast<int>(open.size())) == 0);
    missing.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
                MagnifierDistribution::ENDPOINTS);
    CHECK(missing.last_error()
          == "request data: no recorded series is installed for key "
             "'earnings|TEST:F|estimate|-|gaps_on|lookahead_off'");

    // n == 0 installs the key with no row -- a request that was na on every
    // chart bar -- replacing the rows before it: every read is na, no failure.
    Reader empty;
    prime(empty);
    CHECK(strategy_set_recorded_series(handle(empty), kEarnings, open.data(), values.data(),
                                       static_cast<int>(open.size())) == 0);
    CHECK(strategy_set_recorded_series(handle(empty), kEarnings, nullptr, nullptr, 0) == 0);
    empty.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    CHECK(empty.last_error().empty());
    CHECK(empty.series.size() == chart.size());
    for (const double value : empty.series) CHECK(std::isnan(value));

    // Unordered open times are refused by name and install nothing.
    Reader refused;
    const std::vector<std::int64_t> backwards = {open[1], open[0]};
    CHECK(strategy_set_recorded_series(handle(refused), kEarnings, backwards.data(),
                                       values.data(), 2) == -1);
    CHECK(std::string(strategy_get_last_error(handle(refused)))
              .find("chart open times must be strictly increasing") != std::string::npos);
}

// ---- 2. the four setters install through the C ABI ---------------------------

void setters_install_a_foreign_site() {
    scenario = "C ABI setters";
    const std::vector<Bar> chart = daily_chart(6);
    // US10Y-like: opens 23:00Z the evening before its trade date and closes
    // 21:30Z on it, after the chart's 20:00Z close -- seen one day late.
    std::vector<pf_bar_t> bars;
    std::vector<std::int64_t> closes;
    std::vector<double> carry;
    for (std::size_t k = 0; k < chart.size(); ++k) {
        const std::int64_t trade_day = chart[k].timestamp - (13 * kHour + 30 * kMinute);
        pf_bar_t bar{};
        bar.open = 4.2 + 0.01 * static_cast<double>(k);
        bar.high = bar.open + 0.02;
        bar.low = bar.open - 0.02;
        bar.close = bar.open + 0.005;
        bar.volume = std::nan("");
        bar.timestamp = trade_day - kHour;
        bars.push_back(bar);
        closes.push_back(trade_day + 21 * kHour + 30 * kMinute);
        carry.push_back(100.0 + static_cast<double>(k));
    }
    Reader reader;
    prime(reader);
    reader.foreign = true;
    reader.read_series = false;
    const pf_strategy_t s = handle(reader);
    CHECK(strategy_set_symbol_feed(s, "SYN:US10Y", "1D", bars.data(), closes.data(),
                                   static_cast<int>(bars.size())) == 0);
    CHECK(strategy_set_symbol_feed_column(s, "SYN:US10Y", "D", "carry", carry.data(),
                                          static_cast<int>(carry.size())) == 0);
    CHECK(strategy_set_symbol_facts(s, "SYN:US10Y", "valid", "true") == 0);
    CHECK(strategy_set_symbol_facts(s, "SYN:US10Y", "type", "bond") == 0);
    CHECK(strategy_set_symbol_facts(s, "SYN:US10Y", "mintick", "0.001") == 0);
    reader.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(reader.last_error().empty());
    CHECK(reader.close.size() == chart.size());
    for (std::size_t j = 0; j < reader.close.size(); ++j) {
        if (j == 0) {
            CHECK(std::isnan(reader.close[j]));
            continue;
        }
        CHECK(reader.close[j] == bars[j - 1].close);
        CHECK(reader.column[j] == carry[j - 1]);
        CHECK(reader.type[j] == "bond");
    }

    // A symbol with no bars over the run: a feed of n == 0 is installed (a
    // feed file with a header only), so its site reads na on every chart bar
    // and the run does not fail as unfed; its column is the empty one.
    Reader barren;
    prime(barren);
    barren.foreign = true;
    barren.read_series = false;
    const pf_strategy_t b = handle(barren);
    CHECK(strategy_set_symbol_feed(b, "SYN:US10Y", "1D", bars.data(), closes.data(),
                                   static_cast<int>(bars.size())) == 0);
    CHECK(strategy_set_symbol_feed(b, "SYN:US10Y", "D", nullptr, nullptr, 0) == 0);
    CHECK(strategy_set_symbol_feed_column(b, "SYN:US10Y", "D", "carry", nullptr, 0) == 0);
    CHECK(strategy_set_symbol_facts(b, "SYN:US10Y", "valid", "true") == 0);
    barren.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(barren.last_error().empty());
    CHECK(barren.close.size() == chart.size());
    for (std::size_t j = 0; j < barren.close.size(); ++j) {
        CHECK(std::isnan(barren.close[j]));
        CHECK(std::isnan(barren.column[j]));
    }
}

// ---- 3. argument refusals ------------------------------------------------

void argument_refusals() {
    scenario = "argument refusals";
    Reader reader;
    const pf_strategy_t s = handle(reader);
    pf_bar_t bar{1.0, 1.0, 1.0, 1.0, 0.0, kFirstOpen};
    const std::int64_t close = kFirstOpen + kHour;
    const double value = 1.0;
    CHECK(strategy_set_symbol_feed(nullptr, "K", "15", &bar, &close, 1) == -1);
    CHECK(strategy_set_symbol_feed(s, nullptr, "15", &bar, &close, 1) == -1);
    CHECK(strategy_set_symbol_feed(s, "K", nullptr, &bar, &close, 1) == -1);
    CHECK(strategy_set_symbol_feed(s, "K", "15", nullptr, &close, 1) == -1);
    CHECK(strategy_set_symbol_feed(s, "K", "15", &bar, nullptr, 1) == -1);
    CHECK(strategy_set_symbol_feed(s, "K", "15", &bar, &close, -1) == -1);
    CHECK(strategy_set_symbol_feed(s, "", "15", &bar, &close, 1) == -1);
    CHECK(std::string(strategy_get_last_error(s)) == "strategy_set_symbol_feed: empty symbol key");
    CHECK(strategy_set_symbol_feed_column(nullptr, "K", "15", "c", &value, 1) == -1);
    CHECK(strategy_set_symbol_feed_column(s, "K", "15", nullptr, &value, 1) == -1);
    CHECK(strategy_set_symbol_feed_column(s, "K", "15", "c", &value, 1) == -1);
    CHECK(std::string(strategy_get_last_error(s)).find("no feed is installed") != std::string::npos);
    // A column name the kernel would refuse at the next begin is refused here,
    // by name, and installs nothing.
    {
        Reader named;
        const pf_strategy_t h = handle(named);
        CHECK(strategy_set_symbol_feed(h, "K", "15", &bar, &close, 1) == 0);
        CHECK(strategy_set_symbol_feed_column(h, "K", "15", "\xff", &value, 1) == -1);
        CHECK(std::string(strategy_get_last_error(h))
                  .find("strategy_set_symbol_feed_column: column '\xff' refused (NativeRunSpecError")
              == 0);
        CHECK(strategy_set_symbol_feed_column(h, "K", "15", "c", &value, 1) == 0);
    }
    CHECK(strategy_set_symbol_facts(nullptr, "K", "type", "index") == -1);
    CHECK(strategy_set_symbol_facts(s, "K", nullptr, "index") == -1);
    CHECK(strategy_set_symbol_facts(s, "K", "type", nullptr) == -1);
    CHECK(strategy_set_symbol_facts(s, "K", "mintick", "0") == -1);
    CHECK(strategy_set_symbol_facts(s, "K", "mintick", "0.01x") == -1);
    CHECK(strategy_set_recorded_series(nullptr, kEarnings, &close, &value, 1) == -1);
    CHECK(strategy_set_recorded_series(s, nullptr, &close, &value, 1) == -1);
    CHECK(strategy_set_recorded_series(s, kEarnings, nullptr, &value, 1) == -1);
    CHECK(strategy_set_recorded_series(s, kEarnings, &close, nullptr, 1) == -1);
    CHECK(strategy_set_recorded_series(s, kEarnings, &close, &value, -1) == -1);
    // Nothing above installed anything: a run reading the key fails closed.
    const std::vector<Bar> chart = daily_chart(3);
    prime(reader);
    reader.run(chart.data(), static_cast<int>(chart.size()), "D", "D", false, 4,
               MagnifierDistribution::ENDPOINTS);
    CHECK(reader.last_error().find("no recorded series is installed") != std::string::npos);
}

// ---- 4. a host with no source layer --------------------------------------

class BareHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

void bare_host_answers_minus_one() {
    scenario = "bare host";
    BareHost host;
    const pf_strategy_t s = handle(host);
    pf_bar_t bar{1.0, 1.0, 1.0, 1.0, 0.0, kFirstOpen};
    const std::int64_t close = kFirstOpen + kHour;
    const double value = 1.0;
    CHECK(strategy_set_symbol_feed(s, "K", "60", &bar, &close, 1) == -1);
    CHECK(strategy_set_symbol_feed_column(s, "K", "60", "c", &value, 1) == -1);
    CHECK(strategy_set_symbol_facts(s, "K", "type", "index") == -1);
    CHECK(strategy_set_recorded_series(s, kEarnings, &close, &value, 1) == -1);
}

// ---- 5. streams refuse installed request data ---------------------------

void streams_refuse_request_data() {
    scenario = "stream refusal";
    const std::vector<Bar> chart = daily_chart(4);
    std::vector<pf_bar_t> warmup;
    for (const Bar& bar : chart)
        warmup.push_back(pf_bar_t{bar.open, bar.high, bar.low, bar.close, bar.volume, bar.timestamp});
    const std::string refusal =
        "request.security symbol feeds and recorded request series support historical runs only";
    {
        Reader reader;
        prime(reader);
        reader.read_series = false;
        const std::int64_t open = chart[1].timestamp;
        const double value = 2.0;
        CHECK(strategy_set_recorded_series(handle(reader), kEarnings, &open, &value, 1) == 0);
        CHECK(strategy_stream_begin(handle(reader), warmup.data(), static_cast<int>(warmup.size()),
                                    "D", "D") != 0);
        CHECK(std::string(strategy_get_last_error(handle(reader))) == refusal);
    }
    {
        Reader reader;
        prime(reader);
        reader.read_series = false;
        pf_bar_t bar{4.2, 4.3, 4.1, 4.25, 0.0, chart[0].timestamp - 15 * kHour};
        const std::int64_t close = chart[0].timestamp - kHour;
        CHECK(strategy_set_symbol_feed(handle(reader), "SYN:US10Y", "D", &bar, &close, 1) == 0);
        CHECK(strategy_stream_begin(handle(reader), warmup.data(), static_cast<int>(warmup.size()),
                                    "D", "D") != 0);
        CHECK(std::string(strategy_get_last_error(handle(reader))) == refusal);
    }
    {
        // With nothing installed the same stream begins; while it runs, every
        // setter answers -1, naming the run, and installs nothing.
        Reader reader;
        prime(reader);
        reader.read_series = false;
        const pf_strategy_t s = handle(reader);
        CHECK(strategy_stream_begin(s, warmup.data(), static_cast<int>(warmup.size()),
                                    "D", "D") == 0);
        CHECK(reader.native_state().kind == NativeLifecycleKind::Running);
        const std::string running =
            "a run is in progress; request data is installed before a run begins";
        pf_bar_t bar{4.2, 4.3, 4.1, 4.25, 0.0, chart[0].timestamp - 15 * kHour};
        const std::int64_t close = chart[0].timestamp - kHour;
        const std::int64_t open = chart[1].timestamp;
        const double value = 2.0;
        CHECK(strategy_set_symbol_feed(s, "SYN:US10Y", "D", &bar, &close, 1) == -1);
        CHECK(std::string(strategy_get_last_error(s)) == "strategy_set_symbol_feed: " + running);
        CHECK(strategy_set_symbol_feed_column(s, "SYN:US10Y", "D", "c", &value, 1) == -1);
        CHECK(std::string(strategy_get_last_error(s))
              == "strategy_set_symbol_feed_column: " + running);
        CHECK(strategy_set_symbol_facts(s, "SYN:US10Y", "type", "bond") == -1);
        CHECK(std::string(strategy_get_last_error(s)) == "strategy_set_symbol_facts: " + running);
        CHECK(strategy_set_recorded_series(s, kEarnings, &open, &value, 1) == -1);
        CHECK(std::string(strategy_get_last_error(s))
              == "strategy_set_recorded_series: " + running);
    }
}

// ---- 6. the symbol overload's literal trap --------------------------------

// Binds<Host, T> answers whether the symbol overload, called on a Host with
// its input_tf spelled as a T in five arguments, compiles (a member, so the
// protected overload is in reach). A literal would otherwise bind the
// same-symbol overload (const char* converts to bool, a standard conversion,
// ahead of std::string's user-defined one) and read "SYN:US10Y" as the
// requested timeframe; the host deletes that spelling.
struct OverloadProbe : source::PineStrategyHost {
    template <typename Host, typename T, typename = void>
    struct Binds : std::false_type {};
    template <typename Host, typename T>
    struct Binds<Host, T, std::void_t<decltype(std::declval<Host&>().register_security_eval(
                              0, "SYN:US10Y", "D", std::declval<T>(), false))>>
        : std::true_type {};
};
static_assert(!OverloadProbe::Binds<OverloadProbe, const char (&)[2]>::value,
              "a literal input_tf must not bind the same-symbol overload");
static_assert(!OverloadProbe::Binds<OverloadProbe, const char*>::value,
              "a C string input_tf must not bind the same-symbol overload");
static_assert(OverloadProbe::Binds<OverloadProbe, const std::string&>::value,
              "a std::string input_tf binds the symbol overload");

}  // namespace

int main() {
    recorded_series_exact_on_event_bars();
    setters_install_a_foreign_site();
    argument_refusals();
    bare_host_answers_minus_one();
    streams_refuse_request_data();
    std::printf("test_symbol_request_data: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
