// timeframe.change plans each N-day partition once per run.
//
// PineStrategyHost::tf_change, the class-scope call generated code makes for
// timeframe.change(tf), compares two bars by the period of TradingView's N-day
// partition holding each (tests/test_n_day_chart_timeframe.cpp holds the
// pulses of "2D", "3D" and "5D"). Planning a partition walks the run's feeds
// twice (src/source/multi_day_periods.hpp), so a run plans the partition of
// each timeframe it names once and keeps it: a call that plans again costs the
// whole feed on every bar.
//
// The witness is the planner's own count (multi_day_plans_for_test), read
// around every call. Every bar and price is invented: bars inside one year on
// a 24x7 UTC symbol, where an N-day period opens with the first bar of each
// day whose count from 1 January is a multiple of N.
//
// 1. Nine N-day timeframes, "2D" to "10D", on daily bars: each is planned
//    once in the run, with the consumer's host cache and without it
//    (set_host_cache(false)).
// 2. Twenty, "2D" to "21D": the same.
// 3. The same host run again over other bars: each timeframe is planned once
//    more, for that run, and pulses on that run's bars.
// 4. A selected run, where the selected window's capture state already holds
//    the consumer's host-state slot (one-minute bars on a "3" chart, the shape
//    of the selected control in tests/test_n_day_chart_timeframe.cpp): each of
//    the nine is planned once, and the capture state is still the run's at
//    every bar.
// In every run "D", which has no partition, plans nothing; nothing but these
// calls plans one; and every timeframe pulses on its own days.
//
// Source-bound (includes pineforge/source): release profile only.

#include <pineforge/engine.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/selected_window.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"
#include "../src/source/multi_day_periods.hpp"
#include "../src/source/pine_host_reads.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
std::string scenario = "init";

#define CHECK(expr)                                                                    \
    do {                                                                               \
        ++checks;                                                                      \
        if (!(expr)) {                                                                 \
            ++failures;                                                                \
            std::printf("  FAIL [%s] line %d: %s\n", scenario.c_str(), __LINE__, #expr); \
        }                                                                              \
    } while (0)

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kDay = 24 * 60 * kMinute;

std::int64_t date(int year, int month, int dom) {
    return native_calendar::native_civil_days(year, month, dom) * kDay;
}

const std::int64_t kYearOpen = date(2031, 1, 1);

// The input of one run and the chart bars it makes.
struct Feed {
    std::vector<Bar> bars;
    std::string input_tf;
    std::string chart_tf;
    std::vector<std::int64_t> opens;  // The chart bars' opens.
};

// `count` daily bars from `first`, one invented price each; a daily chart.
Feed daily_feed(std::int64_t first, int count) {
    Feed feed;
    feed.input_tf = "1D";
    feed.chart_tf = "1D";
    for (int i = 0; i < count; ++i) {
        const double price = 50.0 + i;
        feed.bars.push_back({price, price, price, price, 1.0, first + i * kDay});
        feed.opens.push_back(feed.bars.back().timestamp);
    }
    return feed;
}

// Three one-minute bars for each of `charts` bars of a "3" chart, from `first`.
Feed minute_feed(std::int64_t first, int charts) {
    Feed feed;
    feed.input_tf = "1";
    feed.chart_tf = "3";
    for (int i = 0; i < charts * 3; ++i) {
        const double price = 70.0 + i;
        feed.bars.push_back({price, price, price, price, 1.0, first + i * kMinute});
        if (i % 3 == 0) feed.opens.push_back(feed.bars.back().timestamp);
    }
    return feed;
}

// Whether the bar opening at `stamp` is the first of its day.
bool opens_day(std::int64_t stamp) {
    return (stamp - kYearOpen) % kDay == 0;
}

// Whether the bar opening at `stamp` opens an N-day period: it opens a day
// whose count from 1 January is a multiple of N. Every day trades on a 24x7
// symbol and the bars stay inside 2031, so this is the whole rule here.
bool opens_period(std::int64_t stamp, int n) {
    return opens_day(stamp) && ((stamp - kYearOpen) / kDay) % n == 0;
}

// One timeframe a script names. `days` is N of "ND", and 0 for "D", which has
// no partition: a D period opens with every day.
struct Column {
    std::string tf;
    int days;
};

// "2D" to "<last>D", then "D".
std::vector<Column> columns_through(int last) {
    std::vector<Column> columns;
    for (int n = 2; n <= last; ++n) columns.push_back({std::to_string(n) + "D", n});
    columns.push_back({"D", 0});
    return columns;
}

// timeframe.change(tf) for every column at every bar, in the spelling
// generated code gives it, with the plannings each call made.
class PlanProbe final : public source::PineStrategyHost {
public:
    struct Row {
        std::int64_t stamp;
        std::vector<int> changed;  // One flag per column.
    };
    std::vector<Row> rows;
    std::vector<std::uint64_t> plans;  // Plannings per column in this run.
    std::size_t cached_bars = 0;       // Bars at which the consumer held a host cache.
    std::size_t capture_bars = 0;      // Bars at which the run's capture state was parked.

    explicit PlanProbe(std::vector<Column> columns) : columns_(std::move(columns)) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar& bar) override {
        Row row{bar.timestamp, {}};
        for (std::size_t c = 0; c < columns_.size(); ++c) {
            const std::uint64_t before = source::multi_day_plans_for_test;
            const bool changed = tf_change(prev_bar_timestamp_, current_bar_.timestamp,
                                           columns_[c].tf, syminfo_.timezone, syminfo_.session);
            plans[c] += source::multi_day_plans_for_test - before;
            row.changed.push_back(changed ? 1 : 0);
        }
        // Read after the calls: they must leave both where the run put them.
        if (consumer().host_cache() != nullptr) ++cached_bars;
        if (source::detail::selected_intents(*this) != nullptr) ++capture_bars;
        rows.push_back(std::move(row));
    }

    NativeExecutionConsumer& consumer() { return source::detail::run_consumer(*this); }
    const std::vector<Column>& columns() const { return columns_; }

    // Clears what the run before this one observed.
    void begin_observation() {
        rows.clear();
        plans.assign(columns_.size(), 0);
        cached_bars = 0;
        capture_bars = 0;
    }

private:
    std::vector<Column> columns_;
};

// One run of `probe` over `feed`, with the consumer's host cache or without
// it, as a selected run or not, held to one planning per N-day timeframe and
// to the rule's pulses.
void observe_run(PlanProbe& probe, const Feed& feed, bool host_cache, bool selected) {
    auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "UTC");
    strategy_set_syminfo_session(handle, "24x7");
    if (selected) {
        // The window is the feed's day, so no bar comes before it. Both
        // bounds open a chart bar, as a selected window's must.
        pf_selected_window_config_v1 window{};
        window.struct_size = sizeof window;
        window.version = 1;
        window.start_ms = feed.opens.front();
        window.end_ms = feed.opens.front() + kDay;
        CHECK(strategy_set_selected_window_v1(handle, &window) == 0);
    }
    probe.consumer().set_host_cache(host_cache);
    probe.begin_observation();
    const std::uint64_t before = source::multi_day_plans_for_test;
    probe.run(feed.bars.data(), static_cast<int>(feed.bars.size()), feed.input_tf, feed.chart_tf,
              false, 4, MagnifierDistribution::ENDPOINTS);
    const std::uint64_t run_plans = source::multi_day_plans_for_test - before;
    CHECK(probe.last_error().empty());
    CHECK(probe.rows.size() == feed.opens.size());
    if (probe.rows.size() != feed.opens.size()) return;
    // The run held the consumer's host cache exactly when it was asked to,
    // and a capture state exactly when it was selected.
    CHECK(probe.cached_bars == (host_cache ? feed.opens.size() : std::size_t{0}));
    CHECK(probe.capture_bars == (selected ? feed.opens.size() : std::size_t{0}));

    const std::vector<Column>& columns = probe.columns();
    std::uint64_t n_day_columns = 0;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        const std::uint64_t want = columns[c].days != 0 ? 1 : 0;
        n_day_columns += want;
        ++checks;
        if (probe.plans[c] != want) {
            ++failures;
            std::printf("  FAIL [%s] timeframe.change(\"%s\"): planned %llu times in one run, "
                        "expected %llu\n",
                        scenario.c_str(), columns[c].tf.c_str(),
                        static_cast<unsigned long long>(probe.plans[c]),
                        static_cast<unsigned long long>(want));
        }
    }
    // Nothing but those calls planned a partition during the run.
    CHECK(run_plans == n_day_columns);

    // Each timeframe pulses on its own days: never on the run's first bar,
    // then with the first bar of each day that opens one of its own periods
    // ("D": of every day).
    for (std::size_t c = 0; c < columns.size(); ++c) {
        int wrong = 0;
        std::size_t first_wrong = 0;
        for (std::size_t i = 0; i < feed.opens.size(); ++i) {
            const PlanProbe::Row& row = probe.rows[i];
            const bool want = i > 0
                && (columns[c].days == 0 ? opens_day(row.stamp)
                                         : opens_period(row.stamp, columns[c].days));
            if (row.stamp != feed.opens[i] || row.changed[c] != (want ? 1 : 0)) {
                if (wrong == 0) first_wrong = i;
                ++wrong;
            }
        }
        ++checks;
        if (wrong != 0) {
            ++failures;
            std::printf("  FAIL [%s] timeframe.change(\"%s\"): %d of %zu bars pulse off the rule, "
                        "first at bar %zu\n",
                        scenario.c_str(), columns[c].tf.c_str(), wrong, feed.opens.size(),
                        first_wrong);
        }
    }
}

}  // namespace

int main() {
    // 240 days from 1 January and 120 from 1 March: both inside 2031.
    const Feed days = daily_feed(date(2031, 1, 1), 240);
    const Feed later_days = daily_feed(date(2031, 3, 1), 120);
    for (const int last : {10, 21}) {
        for (const bool host_cache : {true, false}) {
            const std::string name = std::to_string(last - 1) + " N-day timeframes, "
                + (host_cache ? "host cache" : "no host cache");
            PlanProbe probe(columns_through(last));
            scenario = name + ", first run";
            observe_run(probe, days, host_cache, false);
            // The same host run again: this run's bars, planned for this run.
            scenario = name + ", second run";
            observe_run(probe, later_days, host_cache, false);
        }
    }
    // Six "3" bars of 1 January, selected: the capture state holds the slot.
    const Feed minutes = minute_feed(date(2031, 1, 1), 6);
    for (const bool host_cache : {true, false}) {
        PlanProbe probe(columns_through(10));
        scenario = std::string("9 N-day timeframes, selected run, ")
            + (host_cache ? "host cache" : "no host cache");
        observe_run(probe, minutes, host_cache, true);
    }
    if (failures != 0) {
        std::printf("test_timeframe_change_partition_plans: %d failure(s) in %d checks\n",
                    failures, checks);
        return 1;
    }
    std::printf("test_timeframe_change_partition_plans: ok (%d checks)\n", checks);
    return 0;
}
