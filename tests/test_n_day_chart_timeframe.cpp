#include <pineforge/engine.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/selected_window.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {
constexpr std::int64_t day = 86400000;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

std::int64_t date(int year, int month, int dom) {
    return native_calendar::native_civil_days(year, month, dom) * day;
}

const std::int64_t begin = date(2023, 12, 1);
const std::int64_t end = date(2024, 6, 7); // Exclusive input end.

double price(std::int64_t stamp) {
    return 1000.0 + static_cast<double>((stamp - begin) / day);
}

std::vector<Bar> daily_bars() {
    std::vector<Bar> bars;
    for (auto stamp = begin; stamp < end; stamp += day) {
        const double p = price(stamp);
        bars.push_back({p, p, p, p, 1.0, stamp});
    }
    return bars;
}

// On a continuous UTC calendar, ordinal-within-year % N == 0 opens a
// bucket. Only opens within the returned input range are chart bars.
std::vector<std::int64_t> grid(int n) {
    std::vector<std::int64_t> opens;
    for (int year = 2023; year <= 2024; ++year) {
        for (auto stamp = date(year, 1, 1);
             stamp < date(year + 1, 1, 1); stamp += n * day) {
            if (stamp >= begin && stamp < end) opens.push_back(stamp);
        }
    }
    return opens;
}

enum class Action { Trace, RelativeClose, LastClose, Hold, RoundTrips };

class Probe final : public source::PineStrategyHost {
public:
    struct Row {
        std::int64_t stamp;
        std::int64_t close_stamp;
        int index;
        int last;
        bool islast;
        double open;
        double close;
        double high;
        double low;
        double volume;
    };
    std::vector<Row> rows;
    std::vector<int> entries;

    explicit Probe(Action action, bool on_close = false) : action_(action) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = on_close;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar& bar) override {
        rows.push_back({bar.timestamp, time_close(), pine_bar_index(), pine_last_bar_index(),
                        barstate_islast_, bar.open, bar.close, bar.high, bar.low, bar.volume});
        const int index = pine_bar_index();
        if (action_ == Action::Trace) return;
        if (action_ == Action::RoundTrips) {
            if (std::find(entries.begin(), entries.end(), index) != entries.end())
                strategy_entry("L", true, missing, missing, 1.0);
            if (std::find(entries.begin(), entries.end(), index - 1) != entries.end())
                strategy_close("L");
            return;
        }
        if (index == 2) strategy_entry("L", true, missing, missing, 1.0);
        if ((action_ == Action::RelativeClose && index == pine_last_bar_index() - 4)
            || (action_ == Action::LastClose && barstate_islast_))
            strategy_close("L");
    }

private:
    Action action_;
};

void run(Probe& probe, const std::vector<Bar>& bars,
         const std::string& input, const std::string& chart) {
    auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "UTC");
    strategy_set_syminfo_session(handle, "24x7");
    probe.run(bars.data(), static_cast<int>(bars.size()), input, chart, false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
}

void trace(const Probe& probe, const std::vector<std::int64_t>& opens,
           bool check_last = true) {
    assert(probe.rows.size() == opens.size());
    for (std::size_t i = 0; i < opens.size(); ++i) {
        const auto& row = probe.rows[i];
        assert(row.stamp == opens[i]);
        assert(row.index == static_cast<int>(i));
        if (check_last) {
            assert(row.last == static_cast<int>(opens.size()) - 1);
            assert(row.islast == (i + 1 == opens.size()));
        }
    }
}

void closed_fill(const Trade& trade, std::int64_t entry_stamp, double entry_price,
                 std::int64_t exit_stamp, double exit_price) {
    assert(trade.entry_time == entry_stamp);
    assert(trade.entry_price == entry_price);
    assert(trade.exit_time == exit_stamp);
    assert(trade.exit_price == exit_price);
}

void three_days(const std::vector<Bar>& bars, int row) {
    const auto opens = grid(3);
    // TradingView tape sha256:1b2ef41a6f1ba4096e6659e12d5ef3f03c8bf2a51e4bb0d8331d814d9aade262
    if (row == 0 || row == 1) {
        Probe chart(Action::Trace);
        run(chart, bars, "1D", "3D");
        trace(chart, opens);
        for (std::size_t i = 0; i < opens.size(); ++i) {
            const auto next = i + 1 < opens.size() ? opens[i + 1] : end;
            assert(chart.rows[i].open == price(opens[i]));
            assert(chart.rows[i].close == price(next - day));
            const auto year = native_calendar::native_civil_date(opens[i] / day).year;
            const auto boundary = std::min(opens[i] + 3 * day,
                                          date(static_cast<int>(year) + 1, 1, 1));
            assert(chart.rows[i].close_stamp == boundary);
        }
    }

    // TradingView tape sha256:05617c83d33cbd8be7a0d64356dc8f40e5b8d2ab20baa1b74b08b8aa66a07d25
    if (row == 0 || row == 2) {
        Probe relative(Action::RelativeClose);
        run(relative, bars, "1D", "3D");
        assert(relative.closed_trade_count() == 1);
        // Entry at index 2 fills on 3; close at last-4 fills on last-3.
        const auto exit = opens[opens.size() - 4];
        closed_fill(relative.closed_trade(0), opens[3], price(opens[3]),
                    exit, price(exit));
    }

    // TradingView tape sha256:4ae8e0a932b7da6ffb4e0bcd5de9ee4d514cd6b78dbbb3fa4207aba1a2c6420d
    if (row == 0 || row == 3) {
        Probe last(Action::LastClose, true);
        run(last, bars, "1D", "3D");
        assert(last.closed_trade_count() == 1);
        // A close execution is labelled by the bucket open; its price is the
        // final contributing daily close, including the shortened final bucket.
        closed_fill(last.closed_trade(0), opens[2], price(opens[3] - day),
                    opens.back(), price(end - day));
    }

    // TradingView tape sha256:1b2ef41a6f1ba4096e6659e12d5ef3f03c8bf2a51e4bb0d8331d814d9aade262
    if (row == 0 || row == 4) {
        Probe held(Action::Hold);
        run(held, bars, "1D", "3D");
        assert(held.closed_trade_count() == 0);
        assert(held.report_trade_count() == 1);
        assert(held.get_report_trade(0).open_at_end);
        closed_fill(held.get_report_trade(0), opens[3], price(opens[3]),
                    opens.back(), price(end - day));
    }
}

void other_days(const std::vector<Bar>& bars, int n, const std::vector<int>& targets) {
    const auto opens = grid(n);
    Probe probe(Action::RoundTrips);
    // The year's first chart index + a zero-based within-year bucket number
    // locates each fill; placing one bar earlier fills at that bucket's open.
    const auto january = std::lower_bound(opens.begin(), opens.end(), date(2024, 1, 1));
    const int first = static_cast<int>(january - opens.begin());
    for (int target : targets) probe.entries.push_back(first + target - 1);
    run(probe, bars, "1D", std::to_string(n) + "D");
    trace(probe, opens);
    assert(probe.closed_trade_count() == targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto index = static_cast<std::size_t>(first + targets[i]);
        closed_fill(probe.closed_trade(i), opens[index], price(opens[index]),
                    opens[index + 1], price(opens[index + 1]));
    }
}

void controls(const std::vector<Bar>& bars) {
    for (const std::string tf : {"D", "1D", "W", "M"}) {
        std::vector<std::int64_t> opens;
        if (tf == "D" || tf == "1D") {
            for (const Bar& bar : bars) opens.push_back(bar.timestamp);
        } else if (tf == "W") {
            // Monday origin: (epoch-day + 3) % 7 == 0. The leading
            // partial week and the available final week are chart bars.
            // expectation corrected: 27 -> 28, because batch chart output
            // includes the final open calendar period.
            const auto first = bars.front().timestamp;
            const auto monday = first - ((first / day + 3) % 7) * day;
            for (auto stamp = monday; stamp <= bars.back().timestamp; stamp += 7 * day)
                opens.push_back(stamp);
        } else {
            // expectation corrected: 6 -> 7, because June's available
            // daily contributions also form a final open chart month.
            // Dec=31, Jan=31, leap Feb=29, Mar=31, Apr=30, May=31.
            // Fold the supplied daily input, without asking engine calendars.
            for (int offset : {0, 31, 62, 91, 122, 152, 183})
                opens.push_back(bars.front().timestamp + offset * day);
        }
        std::vector<Bar> expected;
        std::vector<std::int64_t> closes;
        for (std::size_t i = 0; i < opens.size(); ++i) {
            const auto close = i + 1 < opens.size() ? opens[i + 1]
                : tf == "W" ? opens[i] + 7 * day
                : tf == "M" ? bars.front().timestamp + 213 * day
                : opens[i] + day;
            closes.push_back(close);
            Bar folded{};
            bool first = true;
            for (const auto& bar : bars) {
                if (bar.timestamp < opens[i] || bar.timestamp >= close) continue;
                if (first) {
                    folded = bar;
                    folded.timestamp = opens[i];
                    first = false;
                } else {
                    folded.high = std::max(folded.high, bar.high);
                    folded.low = std::min(folded.low, bar.low);
                    folded.close = bar.close;
                    folded.volume += bar.volume;
                }
            }
            assert(!first);
            expected.push_back(folded);
        }
        Probe probe(Action::RoundTrips);
        probe.entries = {2};
        auto handle = static_cast<pf_strategy_t>(&probe);
        strategy_set_syminfo_timezone(handle, "UTC");
        strategy_set_syminfo_session(handle, "24x7");
        probe.run(bars.data(), static_cast<int>(bars.size()), "1D", tf, false, 4,
                  MagnifierDistribution::ENDPOINTS);
        std::fprintf(stderr, "\ncalendar selector=6 input_tf=1D chart_tf=%s"
            " input_count=%zu first_input=%lld last_input=%lld"
            " rows_actual=%zu independent_expected=%zu"
            " run_status_actual=%d expected=0 last_error_actual=\"%s\" expected=\"\"\n",
            tf.c_str(), bars.size(), static_cast<long long>(bars.front().timestamp),
            static_cast<long long>(bars.back().timestamp), probe.rows.size(), expected.size(),
            probe.last_run_status(), probe.last_error().c_str());
        for (std::size_t i = 0; i < std::max(probe.rows.size(), expected.size()); ++i) {
            std::fprintf(stderr, "calendar selector=6 row[%zu] actual={", i);
            if (i < probe.rows.size()) {
                const auto& row = probe.rows[i];
                std::fprintf(stderr, "time=%lld close_time=%lld index=%d last=%d islast=%d"
                    " O=%.17g H=%.17g L=%.17g C=%.17g V=%.17g",
                    static_cast<long long>(row.stamp), static_cast<long long>(row.close_stamp),
                    row.index, row.last, row.islast,
                    row.open, row.high, row.low, row.close, row.volume);
            } else std::fprintf(stderr, "missing");
            std::fprintf(stderr, "} independent_expected={");
            if (i < expected.size()) {
                const auto& bar = expected[i];
                std::fprintf(stderr, "time=%lld close_time=%lld index=%zu last=%zu islast=%d"
                    " O=%.17g H=%.17g L=%.17g C=%.17g V=%.17g",
                    static_cast<long long>(bar.timestamp), static_cast<long long>(closes[i]),
                    i, expected.size() - 1, i + 1 == expected.size(),
                    bar.open, bar.high, bar.low, bar.close, bar.volume);
            } else std::fprintf(stderr, "absent");
            std::fprintf(stderr, "}\n");
        }
        std::fprintf(stderr, "calendar selector=6 closed_trades_actual=%zu expected=1\n",
            static_cast<std::size_t>(probe.closed_trade_count()));
        std::fprintf(stderr, "calendar selector=6 trade[0] actual={");
        if (probe.closed_trade_count() > 0) {
            const auto& trade = probe.closed_trade(0);
            std::fprintf(stderr, "entry_time=%lld entry_price=%.17g"
                " exit_time=%lld exit_price=%.17g",
                static_cast<long long>(trade.entry_time), trade.entry_price,
                static_cast<long long>(trade.exit_time), trade.exit_price);
        } else std::fprintf(stderr, "missing");
        std::fprintf(stderr, "} independent_expected={entry_time=%lld entry_price=%.17g"
            " exit_time=%lld exit_price=%.17g}\n",
            static_cast<long long>(opens[3]), expected[3].open,
            static_cast<long long>(opens[4]), expected[4].open);
        std::fflush(stderr);
        assert(probe.last_error().empty());
        assert(probe.last_run_status() == 0);
        trace(probe, opens);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto& row = probe.rows[i];
            assert(row.open == expected[i].open);
            assert(row.high == expected[i].high);
            assert(row.low == expected[i].low);
            assert(row.close == expected[i].close);
            assert(row.volume == expected[i].volume);
            assert(row.close_stamp == closes[i]);
        }
        assert(probe.closed_trade_count() == 1);
        closed_fill(probe.closed_trade(0), opens[3], price(opens[3]),
                    opens[4], price(opens[4]));
    }
    // Twelve five-minute inputs make one hourly bar. Use a midnight origin.
    const auto hour = day / 24;
    const auto minute5 = hour / 12;
    std::vector<Bar> minutes;
    std::vector<std::int64_t> opens;
    for (int i = 0; i < 6 * 12; ++i) {
        const double p = 1000.0 + i;
        minutes.push_back({p, p, p, p, 1.0, begin + i * minute5});
        if (i % 12 == 0) opens.push_back(begin + i * minute5);
    }
    Probe hourly(Action::RoundTrips);
    hourly.entries = {2};
    run(hourly, minutes, "5", "60");
    trace(hourly, opens, false);
    assert(hourly.closed_trade_count() == 1);
    closed_fill(hourly.closed_trade(0), opens[3], 1000.0 + 3 * 12,
                opens[4], 1000.0 + 4 * 12);

    // Equal input/chart literals deliver the supplied bars themselves.
    for (int n : {2, 3, 5}) {
        const auto native_opens = grid(n);
        std::vector<Bar> native_bars;
        for (auto stamp : native_opens) {
            const double p = price(stamp);
            native_bars.push_back({p, p, p, p, 1.0, stamp});
        }
        Probe native(Action::RoundTrips);
        native.entries = {2};
        const auto tf = std::to_string(n) + "D";
        run(native, native_bars, tf, tf);
        trace(native, native_opens);
        assert(native.closed_trade_count() == 1);
        closed_fill(native.closed_trade(0), native_opens[3], price(native_opens[3]),
                    native_opens[4], price(native_opens[4]));
    }
}

// The previous chart path groups days since 1970, retains a leading partial
// bucket, and leaves an unfinished stream bucket pending.
std::vector<std::int64_t> epoch_grid(int n, std::int64_t first,
                                    std::int64_t until) {
    std::vector<std::int64_t> opens;
    const auto step = n * day;
    for (auto stamp = first - first % step; stamp + step <= until; stamp += step)
        opens.push_back(stamp);
    return opens;
}

// Independent UTC 24x7 fixture oracle: scan civil years rather than querying
// the production calendar bounds. Retain a leading partial period, but publish
// only periods whose independently clipped close is at or before until.
std::vector<std::pair<std::int64_t, std::int64_t>> completed_year_day_periods(
        int n, std::int64_t first, std::int64_t until) {
    assert(n > 0);
    std::vector<std::pair<std::int64_t, std::int64_t>> periods;
    const int first_year = static_cast<int>(
        native_calendar::native_civil_date(first / day).year);
    for (int year = first_year; date(year, 1, 1) < until; ++year) {
        const auto year_end = date(year + 1, 1, 1);
        for (auto open = date(year, 1, 1); open < year_end; open += n * day) {
            const auto close = std::min(open + n * day, year_end);
            if (close > first && close <= until) periods.emplace_back(open, close);
        }
    }
    return periods;
}

void stream_tick_labels() {
    // Wrapped sessions use their closing date as the trading date. Start at
    // the first session of a year-aligned bucket and retain only that bar.
    // TradingView's 2D/3D/5D charts restart on 1 January (#811).
    const auto earliest = date(2024, 1, 15);
    const auto periods = completed_year_day_periods(3, earliest, date(2024, 2, 1));
    const auto first_period = std::find_if(periods.begin(), periods.end(),
        [earliest](const auto& period) { return period.first >= earliest; });
    assert(first_period != periods.end());
    const auto trading_date = first_period->first;
    const auto local_stamp = [&](int offset, int hour) {
        const auto civil = native_calendar::native_civil_date(trading_date / day - 1 + offset);
        const auto resolved = native_calendar::resolve_civil("America/New_York",
            static_cast<int>(civil.year), civil.month, civil.day, hour, 0);
        assert(resolved);
        return resolved->epoch_ms;
    };
    Probe probe(Action::Trace);
    auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "America/New_York");
    strategy_set_syminfo_session(handle, "1800-1700");
    const Bar warmup{100.0, 100.0, 100.0, 100.0, 1.0, local_stamp(0, 18)};
    // Tick/clock harness: test_streaming.cpp's
    // test_partial_mtf_aggregator_survives_handoff.
    assert(probe.stream_begin(&warmup, 1, "1D", "3D"));
    assert(probe.stream_is_realtime());
    assert(probe.rows.empty());
    const TradeTick first{local_stamp(1, 18) + 1, 1, 101.0, 1.0};
    const TradeTick last{local_stamp(2, 18) + 1, 2, 102.0, 1.0};
    assert(probe.stream_push_tick(first));
    assert(probe.stream_push_tick(last));
    assert(probe.stream_advance_time(local_stamp(3, 18)));
    assert(probe.rows.size() == 1);
    // The old observed-tick path uses the 17:00 daily compatibility label,
    // floored at the session after warmup, even on a multi-day chart.
    assert(probe.rows[0].stamp == local_stamp(1, 17));
    assert(probe.rows[0].index == 0);
    assert(probe.rows[0].open == warmup.open);
    assert(probe.rows[0].close == last.price);
    assert(probe.stream_end(false));
    assert(probe.last_error().empty());
    assert(probe.rows.size() == 1);
}

void stream_chart(const std::vector<Bar>& bars) {
    Probe probe(Action::Trace);
    auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "UTC");
    strategy_set_syminfo_session(handle, "24x7");
    // Public confirmed-bar harness: test_streaming.cpp's
    // test_confirmed_mtf_and_rejected_input. Warmup crosses the year boundary;
    // the pending aggregate must then continue through real stream pushes.
    const auto live = std::lower_bound(bars.begin(), bars.end(), date(2024, 1, 10),
        [](const Bar& bar, std::int64_t stamp) { return bar.timestamp < stamp; });
    const int warmup = static_cast<int>(live - bars.begin());
    assert(probe.stream_begin(bars.data(), warmup, "1D", "3D"));
    assert(probe.stream_is_realtime());
    // expectation corrected: epoch grid -> year-relative grid. TradingView's
    // 2D/3D/5D charts restart N-day bars on 1 January (#811), including the
    // shortened year-end period.
    const auto warmup_periods = completed_year_day_periods(
        3, bars.front().timestamp, live->timestamp);
    std::vector<std::int64_t> warmup_opens;
    for (const auto& period : warmup_periods) warmup_opens.push_back(period.first);
    trace(probe, warmup_opens, false);
    for (auto it = live; it != bars.end(); ++it)
        assert(probe.stream_push_bar(*it));
    const auto periods = completed_year_day_periods(
        3, bars.front().timestamp, bars.back().timestamp + day);
    std::vector<std::int64_t> opens;
    for (const auto& period : periods) opens.push_back(period.first);
    trace(probe, opens, false);
    assert(probe.rows.size() > warmup_opens.size());
    for (std::size_t i = 0; i < opens.size(); ++i) {
        assert(probe.rows[i].open == price(std::max(opens[i], bars.front().timestamp)));
        assert(probe.rows[i].close == price(periods[i].second - day));
    }
    assert(probe.stream_end(false));
    assert(probe.last_error().empty());
    trace(probe, opens, false);
    stream_tick_labels();
}

pf_selected_window_config_v1 selected_window(const std::vector<Bar>& bars) {
    pf_selected_window_config_v1 window{};
    window.struct_size = sizeof window;
    window.version = 1;
    window.start_ms = bars.front().timestamp;
    window.end_ms = bars.back().timestamp + day;
    return window;
}

void selected_chart(const std::vector<Bar>& bars) {
    const auto window = selected_window(bars);
    Probe refused(Action::Hold);
    auto handle = static_cast<pf_strategy_t>(&refused);
    strategy_set_syminfo_timezone(handle, "UTC");
    strategy_set_syminfo_session(handle, "24x7");
    assert(strategy_set_selected_window_v1(handle, &window) == 0);
    refused.run(bars.data(), static_cast<int>(bars.size()), "1D", "3D", false, 4,
                MagnifierDistribution::ENDPOINTS);
    assert(std::string(strategy_get_last_error_code(handle)) == "window_mode_unsupported");
    assert(std::string(strategy_get_last_error_args(handle))
           == "{\"capability\":\"selected_window_chart_timeframe\",\"timeframe\":\"3D\"}");
    assert(refused.last_run_status() != 0);
    assert(refused.native_state().kind == NativeLifecycleKind::Failed);
    assert(!refused.last_error().empty());
    assert(refused.rows.empty());
    assert(refused.closed_trade_count() == 0);
    assert(refused.report_trade_count() == 0);
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = sizeof counts;
    counts.version = 1;
    assert(strategy_selected_window_counts_v1(handle, &counts) == -2);
}

void selected_controls(const std::vector<Bar>& bars) {
    const auto window = selected_window(bars);
    // These non-partition charts keep their existing preparation failure:
    // count_retained (selected_window_plan.cpp) admits only eight intraday
    // script tokens. The scheduler maps its refusal to engine_invariant.
    // In particular, a successful selected D run is not a baseline behaviour.
    const auto unchanged_refusal = [&](const std::vector<Bar>& input,
                                       const std::string& input_tf,
                                       const std::string& chart_tf) {
        Probe probe(Action::Hold);
        auto handle = static_cast<pf_strategy_t>(&probe);
        strategy_set_syminfo_timezone(handle, "UTC");
        strategy_set_syminfo_session(handle, "24x7");
        assert(strategy_set_selected_window_v1(handle, &window) == 0);
        probe.run(input.data(), static_cast<int>(input.size()), input_tf, chart_tf, false, 4,
                  MagnifierDistribution::ENDPOINTS);
        assert(std::string(strategy_get_last_error_code(handle)) == "engine_invariant");
        assert(probe.rows.empty());
        assert(probe.closed_trade_count() == 0);
        assert(probe.report_trade_count() == 0);
    };
    for (const std::string tf : {"D", "1D", "W", "M"})
        unchanged_refusal(bars, "1D", tf);

    // Equal N-day input/chart bars also have no generated partition.
    const auto native_opens = grid(3);
    std::vector<Bar> native_bars;
    for (auto stamp : native_opens) {
        const double p = price(stamp);
        native_bars.push_back({p, p, p, p, 1.0, stamp});
    }
    unchanged_refusal(native_bars, "3D", "3D");

    // An admitted intraday token supplies the successful selected control.
    // Three one-minute inputs per chart bar, with all expected labels and
    // fills taken from those input rows rather than an engine result.
    const auto minute = day / (24 * 60);
    std::vector<Bar> minutes;
    std::vector<std::int64_t> opens;
    for (int i = 0; i < 6 * 3; ++i) {
        const double p = 1000.0 + i;
        minutes.push_back({p, p, p, p, 1.0, begin + i * minute});
        if (i % 3 == 0) opens.push_back(minutes.back().timestamp);
    }
    Probe intraday(Action::RoundTrips);
    intraday.entries = {2};
    auto handle = static_cast<pf_strategy_t>(&intraday);
    assert(strategy_set_selected_window_v1(handle, &window) == 0);
    run(intraday, minutes, "1", "3");
    trace(intraday, opens);
    assert(intraday.closed_trade_count() == 1);
    closed_fill(intraday.closed_trade(0), opens[3], minutes[3 * 3].open,
                opens[4], minutes[4 * 3].open);
    pf_selected_window_counts_v1 counts{};
    counts.struct_size = sizeof counts;
    counts.version = 1;
    assert(strategy_selected_window_counts_v1(handle, &counts) == 0);
    assert(counts.window_script_bars == opens.size());
}

// Direct public time API, as in test_time_bars_back_tapes.cpp. These are the
// chart-history time[1]/time_close[1] reads and time("3D", ..., 1) spellings;
// a negative offset asks for the following chart bar instead.
class OffsetProbe final : public source::PineStrategyHost {
public:
    struct Row {
        std::int64_t stamp;
        std::int64_t chart;
        std::int64_t named;
    };
    std::vector<Row> rows;

    OffsetProbe(int back, bool close) : back_(back), close_(close) {}

    void on_source_bar(const Bar& bar) override {
        rows.push_back({bar.timestamp,
            pine_time_offset(bar.timestamp, back_, "", "", "", 0, close_),
            pine_time_offset(bar.timestamp, 0, "3D", "", "", back_, close_)});
    }

private:
    int back_;
    bool close_;
};

void run_offsets(OffsetProbe& probe, const std::vector<Bar>& bars) {
    auto handle = static_cast<pf_strategy_t>(&probe);
    strategy_set_syminfo_timezone(handle, "UTC");
    strategy_set_syminfo_session(handle, "24x7");
    probe.run(bars.data(), static_cast<int>(bars.size()), "1D", "3D", false, 4,
              MagnifierDistribution::ENDPOINTS);
    assert(probe.last_error().empty());
}

void history_offsets(const std::vector<Bar>& bars, bool close) {
    const auto opens = grid(3);
    OffsetProbe probe(1, close);
    run_offsets(probe, bars);
    assert(probe.rows.size() == opens.size());
    // Include every adjacent pair, especially the shortened year-end bucket.
    for (std::size_t i = 1; i < opens.size(); ++i) {
        const auto expected = close ? opens[i] : opens[i - 1];
        assert(probe.rows[i].stamp == opens[i]);
        assert(probe.rows[i].chart == expected);
        assert(probe.rows[i].named == expected);
    }
}

void forward_offsets(const std::vector<Bar>& bars) {
    const auto opens = grid(3);
    // End the retained input inside a known bucket. At the final callback,
    // next_chart_open must leave retained input and use chart_slot_of on the
    // generated calendar. Its expected successor still comes from grid(3).
    const auto last = std::lower_bound(opens.begin(), opens.end(), date(2024, 1, 10));
    const auto cut = std::upper_bound(bars.begin(), bars.end(), *last,
        [](std::int64_t stamp, const Bar& bar) { return stamp < bar.timestamp; });
    const std::vector<Bar> prefix(bars.begin(), cut);
    OffsetProbe probe(-1, false);
    run_offsets(probe, prefix);
    assert(probe.rows.size() == static_cast<std::size_t>(last - opens.begin()) + 1);
    for (std::size_t i = 0; i < probe.rows.size(); ++i) {
        assert(probe.rows[i].stamp == opens[i]);
        assert(probe.rows[i].chart == opens[i + 1]);
        assert(probe.rows[i].named == opens[i + 1]);
    }
}

// Public synthetic lifecycle regressions, not TradingView parity probes.
// 2024-01-01T00:00Z is a Monday. The independent calendar below uses UTC
// day offsets, Monday + 7 days, and Gregorian Jan=31 / leap Feb=29 / Mar=31.
// It never calls the engine's calendar, bucket or aggregation functions.
constexpr std::int64_t calendar_origin = 1704067200000LL;
constexpr std::int64_t four_hours = day / 6;

std::vector<Bar> calendar_inputs(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const double p = 100.0 + 2 * (i % 13);
        bars.push_back({p, p + 20 + 3 * (i % 7), p - 20 - 4 * (i % 5),
                        p + (i % 3 - 1) * 0.5, 1.0 + i,
                        calendar_origin + i * four_hours});
    }
    return bars;
}

Bar calendar_fold(const std::vector<Bar>& input, int first, int past,
                  std::int64_t label) {
    assert(first >= 0 && first < past && past <= static_cast<int>(input.size()));
    Bar result = input[static_cast<std::size_t>(first)];
    result.timestamp = label;
    for (int i = first + 1; i < past; ++i) {
        const auto& bar = input[static_cast<std::size_t>(i)];
        result.high = std::max(result.high, bar.high);
        result.low = std::min(result.low, bar.low);
        result.close = bar.close;
        result.volume += bar.volume;
    }
    return result;
}

void calendar_bar_equal(const Bar& actual, const Bar& expected) {
    assert(actual.timestamp == expected.timestamp);
    assert(actual.open == expected.open);
    assert(actual.high == expected.high);
    assert(actual.low == expected.low);
    assert(actual.close == expected.close);
    assert(actual.volume == expected.volume);
}

class CalendarTailProbe final : public source::PineStrategyHost {
public:
    struct Row {
        Bar bar;
        std::int64_t close_stamp;
        int index;
        int last;
        bool islast;
        std::size_t requested_count;
    };
    struct Requested {
        Bar bar;
        bool complete;
    };
    std::vector<Row> rows;
    std::vector<Requested> requested;
    std::vector<Requested> finer;
    const bool requests_enabled;
    const bool orders_enabled;
    int entry_calls = 0;

    explicit CalendarTailProbe(const std::string& chart, bool requests = false,
                               bool enable_orders = true)
        : requests_enabled(requests), orders_enabled(enable_orders) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        if (requests) {
            register_security_eval(0, chart, "", false, false);
            register_security_eval(1, "480", "", false, false);
        }
    }

    void evaluate_security(int id, const Bar& bar, bool complete) override {
        (id == 0 ? requested : finer).push_back({bar, complete});
    }

    void on_source_bar(const Bar& bar) override {
        rows.push_back({bar, time_close(), pine_bar_index(), pine_last_bar_index(),
                        barstate_islast_, requested.size()});
        if (orders_enabled && pine_bar_index() == 0) {
            ++entry_calls;
            strategy_entry("L", true, missing, missing, 1.0);
        }
    }
};

std::vector<Bar> calendar_expected(const std::vector<Bar>& input,
                                  const std::vector<int>& boundaries) {
    std::vector<Bar> expected;
    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        const int first = boundaries[i] * 6;
        if (first >= static_cast<int>(input.size())) break;
        expected.push_back(calendar_fold(input, first,
            std::min(boundaries[i + 1] * 6, static_cast<int>(input.size())),
            calendar_origin + boundaries[i] * day));
    }
    return expected;
}

struct CalendarDiagnostic {
    int selector;
    const char* phase;
    std::string input_tf;
    std::string chart;
    const std::vector<Bar>& input;
    const std::vector<int>& boundaries;
    std::size_t input_count;
    bool magnifier = false;
    bool expected_orders_enabled = true;
};

// Unasserted fields stay labelled as such; diagnostics do not infer an
// expectation from the engine's rows, flags, counts or reported trades.
enum class CalendarFlags { Historical, Pending, StreamComplete, Live, Unasserted };

void calendar_print_bar(const Bar& bar) {
    std::fprintf(stderr, "time=%lld O=%.17g H=%.17g L=%.17g C=%.17g V=%.17g",
        static_cast<long long>(bar.timestamp), bar.open, bar.high, bar.low,
        bar.close, bar.volume);
}

void calendar_input_diagnostic(const CalendarDiagnostic& diagnostic,
                               const CalendarTailProbe& probe) {
    const bool direct = diagnostic.input_tf == diagnostic.chart;
    const auto last_close = direct
        ? calendar_origin + diagnostic.boundaries[diagnostic.input_count] * day
        : calendar_origin + static_cast<std::int64_t>(diagnostic.input_count) * four_hours;
    const auto final_close = calendar_origin + diagnostic.boundaries.back() * day;
    std::fprintf(stderr,
        "\ncalendar selector=%d case=%s phase=%s feed=%s chart=%s input_tf=%s"
        " input_count=%zu magnifier=%d orders_enabled_actual=%d expected=%d order_policy=%s"
        " input_last_open=%lld independent_last_close=%lld"
        " final_calendar_close=%lld close_minus_boundary_ms=%lld\n",
        diagnostic.selector, diagnostic.chart.c_str(), diagnostic.phase,
        direct ? "direct" : "aggregated", diagnostic.chart.c_str(),
        diagnostic.input_tf.c_str(), diagnostic.input_count, diagnostic.magnifier,
        probe.orders_enabled, diagnostic.expected_orders_enabled,
        diagnostic.expected_orders_enabled ? "entry_at_index_0" : "no_orders",
        static_cast<long long>(diagnostic.input[diagnostic.input_count - 1].timestamp),
        static_cast<long long>(last_close), static_cast<long long>(final_close),
        static_cast<long long>(last_close - final_close));
    std::fflush(stderr);
}

void calendar_request_diagnostics(const CalendarDiagnostic& diagnostic,
                                  const CalendarTailProbe& probe,
                                  const std::vector<Bar>& expected) {
    std::fprintf(stderr, "calendar requests actual=%zu expected=2"
        " final_callback_requested_count_actual=", probe.requested.size());
    if (probe.rows.empty()) std::fprintf(stderr, "missing");
    else std::fprintf(stderr, "%zu", probe.rows.back().requested_count);
    std::fprintf(stderr, " expected=2\n");
    for (std::size_t i = 0; i < std::max(probe.requested.size(), std::size_t{2}); ++i) {
        std::fprintf(stderr, "calendar request[%zu] actual={", i);
        if (i < probe.requested.size()) {
            calendar_print_bar(probe.requested[i].bar);
            std::fprintf(stderr, " complete=%d", probe.requested[i].complete);
        } else std::fprintf(stderr, "missing");
        std::fprintf(stderr, "} independent_expected={");
        if (i < 2) {
            calendar_print_bar(expected[i]);
            std::fprintf(stderr, " complete=1");
        } else std::fprintf(stderr, "absent");
        std::fprintf(stderr, "}\n");
    }
    std::fprintf(stderr, "calendar finer actual_publications=%zu"
        " expected_publications=nonempty_repeats_allowed expected_unique=%zu\n",
        probe.finer.size(), diagnostic.input_count / 2);
    for (std::size_t i = 0; i < probe.finer.size(); ++i) {
        const auto& value = probe.finer[i];
        std::fprintf(stderr, "calendar finer[%zu] actual={", i);
        calendar_print_bar(value.bar);
        std::fprintf(stderr, " complete=%d} independent_expected={", value.complete);
        // Construct every allowed pair from fixture positions, then locate
        // this publication. Repeated publication is allowed by the old test.
        bool found = false;
        for (std::size_t first = 0; first + 2 <= diagnostic.input_count; first += 2) {
            const auto label = calendar_origin + static_cast<std::int64_t>(first) * four_hours;
            if (value.bar.timestamp != label) continue;
            calendar_print_bar(calendar_fold(diagnostic.input, static_cast<int>(first),
                static_cast<int>(first) + 2, label));
            found = true;
            break;
        }
        if (!found) std::fprintf(stderr, "no_matching_complete_fixture_pair");
        std::fprintf(stderr, " complete=unasserted}\n");
    }
}

void calendar_diagnostics(const CalendarDiagnostic& diagnostic,
                          const CalendarTailProbe& probe,
                          const std::vector<Bar>& expected,
                          CalendarFlags flags = CalendarFlags::Historical,
                          int expected_reports = 1) {
    calendar_input_diagnostic(diagnostic, probe);
    std::fprintf(stderr, "calendar rows actual=%zu independent_expected=%zu\n",
        probe.rows.size(), expected.size());
    // Dump the whole comparison before the row-count assertion can abort.
    for (std::size_t i = 0; i < std::max(probe.rows.size(), expected.size()); ++i) {
        std::fprintf(stderr, "calendar row[%zu] actual={", i);
        if (i < probe.rows.size()) {
            const auto& row = probe.rows[i];
            calendar_print_bar(row.bar);
            std::fprintf(stderr, " close_time=%lld index=%d last=%d islast=%d"
                " requested_count=%zu", static_cast<long long>(row.close_stamp),
                row.index, row.last, row.islast, row.requested_count);
        } else std::fprintf(stderr, "missing");
        std::fprintf(stderr, "} independent_expected={");
        if (i < expected.size()) {
            calendar_print_bar(expected[i]);
            std::fprintf(stderr, " close_time=%lld index=%zu last=",
                static_cast<long long>(calendar_origin + diagnostic.boundaries[i + 1] * day), i);
            if (flags == CalendarFlags::Historical)
                std::fprintf(stderr, "%d", static_cast<int>(expected.size()) - 1);
            else if (flags == CalendarFlags::Live) std::fprintf(stderr, "19");
            else std::fprintf(stderr, "unasserted");
            std::fprintf(stderr, " islast=");
            if (flags == CalendarFlags::Unasserted) std::fprintf(stderr, "unasserted");
            else std::fprintf(stderr, "%d",
                (flags == CalendarFlags::Historical || flags == CalendarFlags::StreamComplete)
                    && i + 1 == expected.size());
        } else std::fprintf(stderr, "absent");
        std::fprintf(stderr, "}\n");
    }
    std::fprintf(stderr, "calendar entry_calls actual=%d expected=%s"
        " closed_trades actual=%zu expected=0 report_trades actual=%d expected=",
        probe.entry_calls,
        diagnostic.expected_orders_enabled ? "one_at_index_0_per_lifecycle" : "0",
        probe.closed_trade_count(), probe.report_trade_count());
    if (expected_reports >= 0) std::fprintf(stderr, "%d\n", expected_reports);
    else std::fprintf(stderr, "unasserted\n");
    if (expected_reports == 1) {
        std::fprintf(stderr, "calendar report[0] independent_expected={open_at_end=1"
            " entry_index=1 exit_index=%d qty=1 entry_time=%lld entry_price=%.17g"
            " exit_time=%lld exit_price=%.17g pnl=%.17g}\n",
            static_cast<int>(expected.size()) - 1,
            static_cast<long long>(expected[1].timestamp), expected[1].open,
            static_cast<long long>(expected.back().timestamp), expected.back().close,
            expected.back().close - expected[1].open);
        if (probe.report_trade_count() == 0)
            std::fprintf(stderr, "calendar report[0] actual=missing\n");
    }
    for (int i = 0; i < probe.report_trade_count(); ++i) {
        const auto trade = probe.get_report_trade(i);
        std::fprintf(stderr, "calendar report[%d] actual={open_at_end=%d"
            " entry_index=%d exit_index=%d qty=%.17g entry_time=%lld entry_price=%.17g"
            " exit_time=%lld exit_price=%.17g pnl=%.17g} expected_slot=%s\n",
            i, trade.open_at_end, trade.entry_bar_index, trade.exit_bar_index, trade.qty,
            static_cast<long long>(trade.entry_time), trade.entry_price,
            static_cast<long long>(trade.exit_time), trade.exit_price, trade.pnl,
            expected_reports < 0 ? "unasserted" : (i < expected_reports ? "present" : "absent"));
    }
    if (probe.requests_enabled) calendar_request_diagnostics(diagnostic, probe, expected);
    std::fflush(stderr);
}

void run_calendar(CalendarTailProbe& probe, const CalendarDiagnostic& diagnostic,
                  const std::vector<Bar>& expected,
                  CalendarFlags flags = CalendarFlags::Historical,
                  int expected_reports = 1) {
    calendar_input_diagnostic(diagnostic, probe);
    probe.run(diagnostic.input.data(), static_cast<int>(diagnostic.input_count),
              diagnostic.input_tf, diagnostic.chart, diagnostic.magnifier, 4,
              MagnifierDistribution::ENDPOINTS);
    calendar_diagnostics(diagnostic, probe, expected, flags, expected_reports);
    std::fprintf(stderr, "calendar run_status actual=%d expected=0"
        " last_error actual=\"%s\" expected=\"\"\n",
        probe.last_run_status(), probe.last_error().c_str());
    std::fflush(stderr);
    assert(probe.last_error().empty());
    assert(probe.last_run_status() == 0);
}

void calendar_operation_diagnostic(const CalendarDiagnostic& diagnostic,
                                   const CalendarTailProbe& probe,
                                   const char* operation, bool actual) {
    std::fprintf(stderr, "calendar selector=%d case=%s phase=%s input_count=%zu"
        " operation=%s actual=%d expected=1 last_error actual=\"%s\" expected=\"\"\n",
        diagnostic.selector, diagnostic.chart.c_str(), diagnostic.phase,
        diagnostic.input_count, operation, actual, probe.last_error().c_str());
    std::fflush(stderr);
}

void calendar_rows(const CalendarTailProbe& probe, const std::vector<Bar>& expected,
                   const std::vector<int>& boundaries, bool last_flags = true) {
    assert(probe.rows.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& row = probe.rows[i];
        calendar_bar_equal(row.bar, expected[i]);
        assert(row.close_stamp == calendar_origin + boundaries[i + 1] * day);
        assert(row.index == static_cast<int>(i));
        if (last_flags) {
            assert(row.last == static_cast<int>(expected.size()) - 1);
            assert(row.islast == (i + 1 == expected.size()));
        }
    }
}

void calendar_open_trade(const CalendarTailProbe& probe,
                         const std::vector<Bar>& expected) {
    assert(probe.closed_trade_count() == 0);
    assert(probe.report_trade_count() == 1);
    const auto trade = probe.get_report_trade(0);
    assert(trade.open_at_end);
    assert(trade.entry_bar_index == 1);
    assert(trade.exit_bar_index == static_cast<int>(expected.size()) - 1);
    assert(trade.qty == 1.0);
    closed_fill(trade, expected[1].timestamp, expected[1].open,
                expected.back().timestamp, expected.back().close);
    assert(trade.pnl == expected.back().close - expected[1].open);
}

void calendar_filled_entry(const CalendarTailProbe& probe,
                           const std::vector<Bar>& input,
                           const std::vector<int>& boundaries,
                           int expected_entry_calls = 1) {
    // One unit is ordered at chart index 0, with next-point execution and
    // no exit. It must fill at the next period's first input open, even if
    // that period's script callback is suppressed. No engine result supplies
    // the expected fill's price, date, index or quantity.
    const auto& fill = input[static_cast<std::size_t>(boundaries[1] * 6)];
    assert(probe.orders_enabled);
    assert(probe.entry_calls == expected_entry_calls);
    const auto position = probe.physical_position();
    assert(position.signed_units == 1.0);
    assert(position.lot_count == 1);
    assert(position.average_price == fill.open);
    const auto lots = probe.native_open_lots(missing);
    assert(lots.size() == 1);
    assert(lots[0].entry_label == "L");
    assert(lots[0].entry_bar_index == 1);
    assert(lots[0].entry_time_ms == calendar_origin + boundaries[1] * day);
    assert(lots[0].entry_price == fill.open);
    assert(lots[0].signed_units == 1.0);
    assert(probe.closed_trade_count() == 0);
}

void calendar_held_report_count(const CalendarTailProbe& probe, int expected_reports) {
    // The held strategy has no realized exit. A historical terminal mark
    // contributes one report row; warmup, live-horizon and suppressed-tail
    // modes explicitly exclude that mark (pine_strategy_host.hpp).
    assert(probe.report_trade_count() == expected_reports
           && "F1 held-position report count must follow the terminal-mark policy");
    ReportC report{};
    probe.fill_report(&report);
    const int rows = report.trades_len;
    const int trades = report.total_trades;
    BacktestEngine::free_report(&report);
    assert(rows == expected_reports);
    assert(trades == expected_reports);
}

struct CalendarCase {
    std::string tf;
    std::vector<int> boundaries;
    int tail_inputs;
};

const std::vector<CalendarCase> calendar_cases = {
    {"D", {0, 1, 2, 3}, 3},
    {"W", {0, 7, 14, 21}, 15},
    {"M", {0, 31, 60, 91}, 15},
};

void calendar_batch_tail(const CalendarCase& test, int selector) {
    // Smaller analogues of D187->188 and W27->28: two complete
    // periods plus an open third. W is explicitly 240->W.
    // One contributor also tests a new period opened by the final input.
    for (const int tail : {1, test.tail_inputs}) {
        const auto input = calendar_inputs(test.boundaries[2] * 6 + tail);
        const auto expected = calendar_expected(input, test.boundaries);
        std::fprintf(stderr, "calendar selector=%d case=%s input_count=%zu"
            " reference_rows actual=%zu expected=3\n",
            selector, test.tf.c_str(), input.size(), expected.size());
        std::fflush(stderr);
        assert(expected.size() == 3);
        for (bool magnifier : {false, true}) {
            CalendarTailProbe probe(test.tf);
            CalendarDiagnostic diagnostic{selector, "batch-tail", "240", test.tf,
                input, test.boundaries, input.size(), magnifier};
            run_calendar(probe, diagnostic, expected);
            calendar_rows(probe, expected, test.boundaries);
            calendar_open_trade(probe, expected);
            if (!magnifier) {
                // The batch tail choice must not leak into a later stream
                // on the same host; its partial warmup stays pending.
                probe.rows.clear();
                diagnostic.phase = "batch-host-stream-warmup";
                const std::vector<Bar> complete(expected.begin(), expected.begin() + 2);
                calendar_input_diagnostic(diagnostic, probe);
                const bool began = probe.stream_begin(input.data(), static_cast<int>(input.size()),
                                                       "240", test.tf);
                calendar_diagnostics(diagnostic, probe, complete, CalendarFlags::Pending, 0);
                calendar_operation_diagnostic(diagnostic, probe, "stream_begin", began);
                assert(began);
                assert(probe.rows.size() == 2);
                calendar_filled_entry(probe, input, test.boundaries, 2);
                calendar_held_report_count(probe, 0);
                diagnostic.phase = "batch-host-stream-end";
                const bool ended = probe.stream_end(false);
                calendar_diagnostics(diagnostic, probe, complete, CalendarFlags::Pending, 0);
                calendar_operation_diagnostic(diagnostic, probe, "stream_end(false)", ended);
                assert(ended);
                assert(probe.last_error().empty());
                assert(probe.rows.size() == 2);
                calendar_filled_entry(probe, input, test.boundaries, 2);
                calendar_held_report_count(probe, 0);
            }
        }
    }
}

void calendar_complete_period_controls() {
    for (const auto& test : calendar_cases) {
        // Nominally complete: N = 6 * final_boundary_days. Last input opens
        // at origin + (N-1)*4h and closes at origin + N*4h = final boundary.
        // Compare aggregated and direct calendars on the same complete input,
        // including their last-index, close-time and held-trade expectations.
        const auto full = calendar_inputs(test.boundaries.back() * 6);
        const auto expected = calendar_expected(full, test.boundaries);
        CalendarTailProbe aggregated(test.tf), direct(test.tf);
        run_calendar(aggregated, {17, "complete-periods", "240", test.tf,
            full, test.boundaries, full.size()}, expected);
        run_calendar(direct, {17, "complete-periods", test.tf, test.tf,
            expected, test.boundaries, expected.size()}, expected);
        calendar_rows(aggregated, expected, test.boundaries);
        calendar_rows(direct, expected, test.boundaries);
        calendar_open_trade(aggregated, expected);
        calendar_open_trade(direct, expected);
    }
}

void calendar_direct_natural_close_controls() {
    for (const auto& test : calendar_cases) {
        // Independently fold exactly three whole UTC periods. These three
        // chart bars are the direct input; the final one closes at boundary[3].
        // Selector 19 also exposes this direct-input control on its own.
        const auto full = calendar_inputs(test.boundaries.back() * 6);
        const auto expected = calendar_expected(full, test.boundaries);
        CalendarTailProbe direct(test.tf);
        run_calendar(direct, {19, "direct-natural-close", test.tf, test.tf,
            expected, test.boundaries, expected.size()}, expected);
        std::fprintf(stderr, "calendar selector=19 case=%s"
            " reference_rows actual=%zu expected=3\n", test.tf.c_str(), expected.size());
        std::fflush(stderr);
        assert(expected.size() == 3);
        assert(direct.rows.size() == 3);
        calendar_rows(direct, expected, test.boundaries);
        calendar_open_trade(direct, expected);
    }
}

void calendar_request_tail() {
    for (const auto& test : calendar_cases) {
        const auto input = calendar_inputs(test.boundaries[2] * 6 + test.tail_inputs);
        const auto expected = calendar_expected(input, test.boundaries);
        std::size_t natural_requests = 0;
        for (std::size_t i = 1; i < test.boundaries.size(); ++i)
            if (test.boundaries[i] * 6 <= static_cast<int>(input.size())) ++natural_requests;
        std::fprintf(stderr, "\ncalendar selector=16 case=%s independent_input_close=%lld"
            " independent_chart_rows=%zu independent_natural_requests=%zu"
            " final_chart_close=%lld\n", test.tf.c_str(),
            static_cast<long long>(calendar_origin
                + static_cast<std::int64_t>(input.size()) * four_hours),
            expected.size(), natural_requests,
            static_cast<long long>(calendar_origin + test.boundaries.back() * day));
        CalendarTailProbe probe(test.tf, true);
        run_calendar(probe, {16, "request-tail", "240", test.tf,
            input, test.boundaries, input.size()}, expected);
        assert(expected.size() == 3);
        assert(natural_requests == 2);
        calendar_rows(probe, expected, test.boundaries);
        // The chart's final callback must not promote its still-open
        // lookahead-off calendar request to a completed requested bar.
        assert(probe.requested.size() == 2);
        assert(probe.rows.back().requested_count == 2);
        for (std::size_t i = 0; i < probe.requested.size(); ++i) {
            assert(probe.requested[i].complete);
            calendar_bar_equal(probe.requested[i].bar, expected[i]);
        }
        // Every dispatched 8-hour value must contain a full pair of 4-hour
        // inputs. The last singleton is still pending, even though its chart
        // bar was emitted. Repeated publication of an earlier pair is allowed
        // by the existing calling-chart boundary gate.
        assert(!probe.finer.empty());
        std::vector<std::int64_t> seen;
        for (const auto& value : probe.finer) {
            const auto first = (value.bar.timestamp - calendar_origin) / four_hours;
            assert(first >= 0 && first % 2 == 0);
            assert(first + 2 <= static_cast<std::int64_t>(input.size()));
            calendar_bar_equal(value.bar, calendar_fold(input, static_cast<int>(first),
                static_cast<int>(first) + 2, value.bar.timestamp));
            if (seen.empty() || seen.back() != value.bar.timestamp)
                seen.push_back(value.bar.timestamp);
        }
        std::fprintf(stderr, "calendar selector=16 case=%s"
            " finer_unique actual=%zu independent_expected=%zu\n",
            test.tf.c_str(), seen.size(), input.size() / 2);
        std::fflush(stderr);
        assert(seen.size() == input.size() / 2);
    }
}

void calendar_stream_and_live_controls() {
    for (const auto& test : calendar_cases) {
        const int complete_inputs = test.boundaries[2] * 6;
        const auto input = calendar_inputs(complete_inputs + test.tail_inputs);
        const auto tail_expected = calendar_expected(input, test.boundaries);
        const std::vector<Bar> two_periods(tail_expected.begin(), tail_expected.begin() + 2);
        // Keep the entry-at-zero strategy and the original input timeline.
        // Two whole periods guarantee its next-period fill in all three
        // modes, including the bar whose calculation suppression removes.
        // expectation corrected: entry_calls 0 -> 1, because orders are enabled.
        // expectation corrected: completed-stream reports 0 -> 1, because the
        // restored held lot receives the naturally completed terminal mark.
        CalendarTailProbe probe(test.tf);
        CalendarDiagnostic warmup{18, "stream-warmup", "240", test.tf,
            input, test.boundaries, static_cast<std::size_t>(complete_inputs + 1)};
        // Warmup keeps the third period open, then confirmed realtime inputs
        // supply its remaining contributions before the fourth period opens.
        calendar_input_diagnostic(warmup, probe);
        const bool began = probe.stream_begin(input.data(), complete_inputs + 1, "240", test.tf);
        calendar_diagnostics(warmup, probe, two_periods, CalendarFlags::Pending, 0);
        calendar_operation_diagnostic(warmup, probe, "stream_begin", began);
        std::fprintf(stderr, "calendar stream_is_realtime actual=%d expected=1\n",
            probe.stream_is_realtime());
        std::fflush(stderr);
        assert(began);
        assert(probe.stream_is_realtime());
        assert(probe.rows.size() == 2);
        for (const auto& row : probe.rows) assert(!row.islast);
        calendar_filled_entry(probe, input, test.boundaries);
        calendar_held_report_count(probe, 0);
        warmup.phase = "stream-open-third";
        for (int i = complete_inputs + 1; i < static_cast<int>(input.size()); ++i) {
            const bool pushed = probe.stream_push_bar(input[static_cast<std::size_t>(i)]);
            warmup.input_count = static_cast<std::size_t>(i + 1);
            calendar_operation_diagnostic(warmup, probe, "stream_push_bar", pushed);
            assert(pushed);
        }
        calendar_diagnostics(warmup, probe, two_periods, CalendarFlags::Pending, 0);
        assert(probe.rows.size() == 2);
        calendar_filled_entry(probe, input, test.boundaries);
        calendar_held_report_count(probe, 0);
        const auto full = calendar_inputs(test.boundaries.back() * 6 + 1);
        CalendarDiagnostic completed{18, "stream-natural-close", "240", test.tf,
            full, test.boundaries, input.size()};
        for (std::size_t i = input.size(); i < full.size(); ++i) {
            const bool pushed = probe.stream_push_bar(full[i]);
            completed.input_count = i + 1;
            calendar_operation_diagnostic(completed, probe, "stream_push_bar", pushed);
            assert(pushed);
        }
        const std::vector<Bar> three_periods(full.begin(), full.end() - 1);
        const auto expected = calendar_expected(three_periods, test.boundaries);
        calendar_diagnostics(completed, probe, expected, CalendarFlags::StreamComplete, 1);
        calendar_rows(probe, calendar_expected(three_periods, test.boundaries),
                      test.boundaries, false);
        assert(probe.rows.back().islast);
        calendar_filled_entry(probe, full, test.boundaries);
        calendar_held_report_count(probe, 1);
        calendar_open_trade(probe, expected);
        completed.phase = "stream-end";
        const bool ended = probe.stream_end(false);
        calendar_diagnostics(completed, probe, expected, CalendarFlags::StreamComplete, 1);
        calendar_operation_diagnostic(completed, probe, "stream_end(false)", ended);
        assert(ended);
        assert(probe.last_error().empty());
        assert(probe.rows.size() == 3);
        calendar_filled_entry(probe, full, test.boundaries);
        calendar_held_report_count(probe, 1);
        calendar_open_trade(probe, expected);

        CalendarTailProbe live(test.tf);
        const CalendarDiagnostic live_context{18, "live-horizon", "240", test.tf,
            input, test.boundaries, input.size()};
        const bool live_set = live.set_realtime_tail(true, 20);
        calendar_operation_diagnostic(live_context, live, "set_realtime_tail(true,20)", live_set);
        assert(live_set);
        run_calendar(live, live_context, two_periods, CalendarFlags::Live, 0);
        assert(live.rows.size() == 2);
        calendar_rows(live, two_periods, test.boundaries, false);
        for (const auto& row : live.rows) {
            assert(row.last == 19);
            assert(!row.islast);
        }
        calendar_filled_entry(live, input, test.boundaries);
        calendar_held_report_count(live, 0);

        CalendarTailProbe suppressed(test.tf);
        const CalendarDiagnostic suppressed_context{18, "suppressed-tail", "240", test.tf,
            input, test.boundaries, input.size()};
        const bool suppression_set = suppressed.set_probe_suppress_tail_logic(true);
        calendar_operation_diagnostic(suppressed_context, suppressed,
            "set_probe_suppress_tail_logic(true)", suppression_set);
        assert(suppression_set);
        const std::vector<Bar> first_period(two_periods.begin(), two_periods.begin() + 1);
        run_calendar(suppressed, suppressed_context, first_period, CalendarFlags::Unasserted, 0);
        assert(suppressed.rows.size() == 1);
        calendar_rows(suppressed, first_period, test.boundaries, false);
        calendar_filled_entry(suppressed, input, test.boundaries);
        calendar_held_report_count(suppressed, 0);
    }
}
// Independent UTC calendar fixtures: Monday 2024-01-01, Jan=31 days,
// leap February=29 days, March=31 days. No engine calendar or aggregation
// routine constructs these complete chart bars or their expected metadata.
constexpr std::int64_t monthidx_origin = 1704067200000LL;

std::vector<Bar> monthidx_chart_bars(const std::vector<int>& boundaries) {
    std::vector<Bar> bars;
    for (std::size_t period = 0; period + 1 < boundaries.size(); ++period) {
        Bar bar{};
        bar.timestamp = monthidx_origin + boundaries[period] * day;
        // Six four-hour contributors per UTC day, independently folded
        // from the supplied fixture's price/volume formula.
        const int first = boundaries[period] * 6;
        const int past = boundaries[period + 1] * 6;
        for (int i = first; i < past; ++i) {
            const double p = 100.0 + 2 * (i % 13);
            const double high = p + 20 + 3 * (i % 7);
            const double low = p - 20 - 4 * (i % 5);
            if (i == first) {
                bar.open = p;
                bar.high = high;
                bar.low = low;
            } else {
                bar.high = std::max(bar.high, high);
                bar.low = std::min(bar.low, low);
            }
            bar.close = p + (i % 3 - 1) * 0.5;
            bar.volume += 1.0 + i;
        }
        bars.push_back(bar);
    }
    return bars;
}

class monthidx_probe final : public source::PineStrategyHost {
public:
    struct Row {
        Bar bar;
        std::int64_t close_stamp;
        int index;
        int last;
        bool islast;
        std::size_t monthly_count;
        std::size_t quarterly_count;
    };
    struct Requested {
        Bar bar;
        bool complete;
    };
    std::vector<Row> rows;
    std::vector<Requested> requested[2];

    explicit monthidx_probe(bool requests = false) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        if (requests) {
            register_security_eval(0, "M", "", false, false);
            register_security_eval(1, "3M", "", false, false);
        }
    }

    void evaluate_security(int id, const Bar& bar, bool complete) override {
        assert(id == 0 || id == 1);
        requested[id].push_back({bar, complete});
    }

    void on_source_bar(const Bar& bar) override {
        rows.push_back({bar, time_close(), pine_bar_index(), pine_last_bar_index(),
                        barstate_islast_, requested[0].size(), requested[1].size()});
    }
};

void monthidx_print_bar(const Bar& bar) {
    std::fprintf(stderr, "time=%lld O=%.17g H=%.17g L=%.17g C=%.17g V=%.17g",
        static_cast<long long>(bar.timestamp), bar.open, bar.high, bar.low,
        bar.close, bar.volume);
}

// Fold the supplied monthly OHLCV using only the explicit fixture bounds.
// The final output may contain fewer contributors than its natural period.
std::vector<Bar> monthidx_fold(const std::vector<Bar>& input,
                              const std::vector<int>& boundaries) {
    std::vector<Bar> result;
    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        const auto open = monthidx_origin + boundaries[i] * day;
        const auto close = monthidx_origin + boundaries[i + 1] * day;
        auto first = std::lower_bound(input.begin(), input.end(), open,
            [](const Bar& bar, std::int64_t stamp) { return bar.timestamp < stamp; });
        auto past = std::lower_bound(first, input.end(), close,
            [](const Bar& bar, std::int64_t stamp) { return bar.timestamp < stamp; });
        if (first == past) continue;
        Bar folded = *first;
        folded.timestamp = open;
        for (++first; first != past; ++first) {
            folded.high = std::max(folded.high, first->high);
            folded.low = std::min(folded.low, first->low);
            folded.close = first->close;
            folded.volume += first->volume;
        }
        result.push_back(folded);
    }
    return result;
}

void monthidx_calendar(int selector, const char* phase,
                      const char* input_tf, const char* chart_tf,
                      const std::vector<Bar>& input,
                      const std::vector<int>& input_boundaries,
                      const std::vector<int>& boundaries, bool requests = false) {
    const auto expected = monthidx_fold(input, boundaries);
    const int expected_last = static_cast<int>(expected.size()) - 1;
    const auto final_close = monthidx_origin + boundaries.back() * day;
    const auto last_close = monthidx_origin + input_boundaries.back() * day;
    std::vector<Bar> expected_requests[2];
    if (requests) {
        expected_requests[0] = input;
        const std::vector<int> quarters = {0, 91, 182};
        const auto folded = monthidx_fold(input, quarters);
        for (std::size_t i = 0; i < folded.size(); ++i)
            if (quarters[i + 1] <= input_boundaries.back())
                expected_requests[1].push_back(folded[i]);
    }
    monthidx_probe probe(requests);
    probe.run(input.data(), static_cast<int>(input.size()), input_tf, chart_tf, false, 4,
              MagnifierDistribution::ENDPOINTS);
    std::fprintf(stderr,
        "\nmonthidx selector=%d input_tf=%s chart_tf=%s phase=%s"
        " input_count=%zu rows_actual=%zu independent_expected=%zu"
        " input_last_open=%lld independent_last_close=%lld"
        " final_calendar_close=%lld close_minus_boundary_ms=%lld"
        " run_status_actual=%d expected=0 last_error_actual=\"%s\" expected=\"\"\n",
        selector, input_tf, chart_tf, phase, input.size(), probe.rows.size(), expected.size(),
        static_cast<long long>(input.back().timestamp),
        static_cast<long long>(last_close), static_cast<long long>(final_close),
        static_cast<long long>(last_close - final_close),
        probe.last_run_status(), probe.last_error().c_str());
    // Print missing/extra rows as well, before any runtime assertion aborts.
    for (std::size_t i = 0; i < std::max(probe.rows.size(), expected.size()); ++i) {
        std::fprintf(stderr, "monthidx row[%zu] actual={", i);
        if (i < probe.rows.size()) {
            const auto& row = probe.rows[i];
            monthidx_print_bar(row.bar);
            std::fprintf(stderr, " close_time=%lld index=%d last=%d islast=%d"
                " monthly_count=%zu quarterly_count=%zu",
                static_cast<long long>(row.close_stamp), row.index, row.last, row.islast,
                row.monthly_count, row.quarterly_count);
        } else std::fprintf(stderr, "missing");
        std::fprintf(stderr, "} independent_expected={");
        if (i < expected.size()) {
            monthidx_print_bar(expected[i]);
            std::fprintf(stderr, " close_time=%lld index=%zu last=%d islast=%d"
                " monthly_count=%zu quarterly_count=%zu",
                static_cast<long long>(monthidx_origin + boundaries[i + 1] * day),
                i, expected_last, i + 1 == expected.size(),
                requests ? i + 1 : std::size_t{0},
                requests ? (i + 1) / 3 : std::size_t{0});
        } else std::fprintf(stderr, "absent");
        std::fprintf(stderr, "}\n");
    }
    if (requests) {
        for (int id = 0; id < 2; ++id) {
            std::fprintf(stderr, "monthidx request_tf=%s actual=%zu independent_expected=%zu\n",
                id == 0 ? "M" : "3M", probe.requested[id].size(), expected_requests[id].size());
            for (std::size_t i = 0;
                 i < std::max(probe.requested[id].size(), expected_requests[id].size()); ++i) {
                std::fprintf(stderr, "monthidx request[%d][%zu] actual={", id, i);
                if (i < probe.requested[id].size()) {
                    monthidx_print_bar(probe.requested[id][i].bar);
                    std::fprintf(stderr, " complete=%d", probe.requested[id][i].complete);
                } else std::fprintf(stderr, "missing");
                std::fprintf(stderr, "} independent_expected={");
                if (i < expected_requests[id].size()) {
                    monthidx_print_bar(expected_requests[id][i]);
                    const auto close_offset = id == 0 ? input_boundaries[i + 1]
                        : boundaries[(i + 1) * 3];
                    std::fprintf(stderr, " natural_close=%lld complete=1",
                        static_cast<long long>(monthidx_origin + close_offset * day));
                } else std::fprintf(stderr, "absent");
                std::fprintf(stderr, "}\n");
            }
        }
    }
    std::fflush(stderr);
    assert(probe.last_error().empty());
    assert(probe.last_run_status() == 0);
    assert(probe.rows.size() == boundaries.size() - 1);
    assert(expected.size() == boundaries.size() - 1);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& row = probe.rows[i];
        assert(row.bar.timestamp == expected[i].timestamp);
        assert(row.bar.open == expected[i].open);
        assert(row.bar.high == expected[i].high);
        assert(row.bar.low == expected[i].low);
        assert(row.bar.close == expected[i].close);
        assert(row.bar.volume == expected[i].volume);
        assert(row.close_stamp == monthidx_origin + boundaries[i + 1] * day);
        assert(row.index == static_cast<int>(i));
        assert(row.last == expected_last);
        assert(row.islast == (i + 1 == expected.size()));
        if (requests) {
            assert(row.monthly_count == i + 1);
            assert(row.quarterly_count == (i + 1) / 3);
        }
    }
    if (requests) {
        for (int id = 0; id < 2; ++id) {
            assert(probe.requested[id].size() == expected_requests[id].size());
            for (std::size_t i = 0; i < expected_requests[id].size(); ++i) {
                calendar_bar_equal(probe.requested[id][i].bar, expected_requests[id][i]);
                assert(probe.requested[id][i].complete);
            }
        }
    }
}

void monthidx_direct_calendar(int selector, const char* tf,
                              const std::vector<int>& boundaries) {
    const auto input = monthidx_chart_bars(boundaries);
    monthidx_calendar(selector, "direct-natural-close", tf, tf,
        input, boundaries, boundaries);
}

void monthidx_monthly_witness(int selector) {
    const std::vector<int> three = {0, 31, 60, 91};
    const auto three_months = monthidx_chart_bars(three);
    if (selector == 20 || selector == 23)
        monthidx_calendar(selector, "alias-natural-close", "1M", "M",
            three_months, three, three);
    if (selector == 20 || selector == 24)
        monthidx_calendar(selector, "alias-natural-close", "M", "1M",
            three_months, three, three);
    const std::vector<int> six = {0, 31, 60, 91, 121, 152, 182};
    const std::vector<int> quarters = {0, 91, 182};
    if (selector == 20 || selector == 25)
        monthidx_calendar(selector, "coarser-natural-close", "M", "3M",
            monthidx_chart_bars(six), six, quarters);
    const std::vector<int> five = {0, 31, 60, 91, 121, 152};
    if (selector == 20 || selector == 26)
        monthidx_calendar(selector, "coarser-developing-tail", "M", "3M",
            monthidx_chart_bars(five), five, quarters);
}

void monthidx_monthly_metadata() {
    // Keep the exact three-bar natural-close witness first. Prefixes and
    // the explicit one-month spelling exercise the same population rule.
    for (const char* tf : {"M", "1M"}) {
        monthidx_direct_calendar(20, tf, {0, 31, 60, 91});
        monthidx_direct_calendar(20, tf, {0, 31, 60});
        monthidx_direct_calendar(20, tf, {0, 31});
    }
    monthidx_monthly_witness(20);
}

void monthidx_monthly_requests() {
    for (const char* tf : {"M", "1M"}) {
        const std::vector<int> six = {0, 31, 60, 91, 121, 152, 182};
        monthidx_calendar(22, "request-natural-close", tf, tf,
            monthidx_chart_bars(six), six, six, true);
        const std::vector<int> five = {0, 31, 60, 91, 121, 152};
        monthidx_calendar(22, "request-developing-quarter", tf, tf,
            monthidx_chart_bars(five), five, five, true);
    }
}

void monthidx_daily_weekly_controls() {
    // Separate selector: a monthly abort cannot hide either direct control.
    monthidx_direct_calendar(21, "D", {0, 1, 2, 3});
    monthidx_direct_calendar(21, "W", {0, 7, 14, 21});
}

class AuxiliaryCalendarHost final : public NativeStrategyHost {
public:
    struct Delivery {
        Bar bar;
        NativeTimeframeBarContext context;
    };
    std::vector<Delivery> delivered[3];
    std::vector<Bar> rows;
    std::vector<std::size_t> annual_at_bar;
    std::vector<std::size_t> monthly_at_bar;
    std::vector<bool> annual_visible;

    void on_native_timeframe_bar(const Bar& bar,
                                 const NativeTimeframeBarContext& context) override {
        assert(context.subscription < 3);
        const auto visible = native_series_bar(context.subscription);
        assert(visible.has_value());
        calendar_bar_equal(*visible, bar);
        delivered[context.subscription].push_back({bar, context});
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        rows.push_back(bar);
        annual_at_bar.push_back(delivered[0].size());
        monthly_at_bar.push_back(delivered[2].size());
        annual_visible.push_back(native_series_bar(0).has_value());
    }
};

void auxiliary_calendar_feed_timeframe() {
    // Gregorian UTC month boundaries in leap year 2024, supplied directly.
    // A monthly October bar ends on November 1, not at Q4's January 1 close.
    // Neither native_calendar nor TimeframeAggregator constructs this oracle.
    const std::vector<int> months = {0, 31, 60, 91, 121, 152, 182,
                                     213, 244, 274, 305, 335, 366};
    std::vector<Bar> monthly;
    for (int i = 0; i < 12; ++i) {
        const double p = 200.0 + 7 * i;
        monthly.push_back({p, p + 10 + i % 3, p - 5 - i % 4,
            p + (i % 2 ? 2.0 : -1.0), 10.0 + i,
            calendar_origin + months[static_cast<std::size_t>(i)] * day});
    }
    std::vector<Bar> primary;
    for (int i = 0; i < 4; ++i) {
        // Deliberately different prints: the annual Input control must not
        // acquire the auxiliary series' values or its monthly interval.
        const double p = 1000.0 + 100 * i;
        primary.push_back({p, p + 80.0, p - 70.0, p + 25.0, 5000.0 + i,
            calendar_origin + months[static_cast<std::size_t>(3 * i)] * day});
    }
    const Bar expected_feed = calendar_fold(monthly, 0, 12, calendar_origin);
    const Bar expected_primary = calendar_fold(primary, 0, 4, calendar_origin);
    for (int feed_count : {10, 11, 12}) {
        for (bool lookahead : {false, true}) {
            NativeRunSpec spec;
            spec.identity = {"auxiliary-calendar-feed-timeframe", 1};
            spec.input_tf = "3M";
            spec.script_tf = "3M";
            spec.timezone = "UTC";
            spec.session = "24x7";
            spec.ticker = "AUX";
            spec.tickerid = "TEST:AUX";
            spec.type = "crypto";
            spec.currency = "USD";
            spec.basecurrency = "USD";
            spec.description = "auxiliary calendar feed";
            spec.volumetype = "base";
            spec.initial_capital = 10000.0;
            spec.point_value = 1.0;
            spec.account_fx = 1.0;
            NativeAuxiliaryFeed feed;
            feed.tf = "M";
            feed.bars.assign(monthly.begin(), monthly.begin() + feed_count);
            spec.auxiliary_feed = std::move(feed);
            NativeTimeframeSubscription annual;
            annual.tf = "12M";
            annual.source = NativeSeriesSource::AuxiliaryFeed;
            annual.lookahead = lookahead;
            spec.subscriptions.push_back(annual);
            annual.source = NativeSeriesSource::Input;
            annual.lookahead = false;
            spec.subscriptions.push_back(annual);
            NativeTimeframeSubscription own;
            own.tf = "M";
            own.source = NativeSeriesSource::AuxiliaryFeed;
            spec.subscriptions.push_back(own);
            AuxiliaryCalendarHost host;
            const auto setup = host.configure_native(spec);
            assert(setup.status == NativeSetupStatus::Applied);
            host.run(primary.data(), static_cast<int>(primary.size()), "3M", "3M",
                     false, 4, MagnifierDistribution::ENDPOINTS);
            assert(host.last_error().empty());
            assert(host.last_run_status() == 0);
            assert(host.rows.size() == primary.size());
            const bool feed_complete =
                months[static_cast<std::size_t>(feed_count)] >= months.back();
            const std::size_t expected_annual = feed_complete ? 1 : 0;
            if (host.delivered[0].size() != expected_annual) {
                std::fprintf(stderr, "F2 auxiliary M / primary 3M / requested 12M:"
                    " months=%d lookahead=%d annual_actual=%zu independent_expected=%zu\n",
                    feed_count, lookahead, host.delivered[0].size(), expected_annual);
                for (const auto& value : host.delivered[0]) {
                    calendar_print_bar(value.bar);
                    std::fprintf(stderr, " delivered_at=%lld natural_close=%lld\n",
                        static_cast<long long>(value.context.delivered_at_ms),
                        static_cast<long long>(calendar_origin + months.back() * day));
                }
                std::fflush(stderr);
            }
            assert(host.delivered[0].size() == expected_annual
                   && "F2 October/November monthly feed must not complete the annual bucket");
            // Twelve actual monthly contributions are required. Truncated
            // feeds publish no annual value under either historical mode.
            for (std::size_t i = 0; i < primary.size(); ++i) {
                calendar_bar_equal(host.rows[i], primary[i]);
                const bool input_reaches_year_end = months[(i + 1) * 3] >= months.back();
                const std::size_t count =
                    feed_complete && (lookahead || input_reaches_year_end) ? 1 : 0;
                assert(host.annual_at_bar[i] == count);
                assert(host.annual_visible[i] == (count != 0));
                assert(host.monthly_at_bar[i]
                       == std::min(static_cast<std::size_t>(feed_count), (i + 1) * 3));
            }
            // The same native host's primary-built annual series still
            // closes on the fourth quarterly input, from four primary bars.
            assert(host.delivered[1].size() == 1);
            calendar_bar_equal(host.delivered[1][0].bar, expected_primary);
            assert(host.delivered[1][0].context.delivered_at_ms == primary[3].timestamp);
            assert(host.delivered[1][0].context.completion == NativeCompletionKind::Confirmed);
            assert(host.delivered[1][0].context.interval.open_ms == calendar_origin);
            assert(host.delivered[1][0].context.interval.last_traded_close_ms
                   == calendar_origin + months.back() * day);
            assert(host.delivered[1][0].context.interval.next_period_open_ms
                   == calendar_origin + months.back() * day);
            // Independently check every feed observation and its own monthly
            // interval, including October, before treating the count as proof.
            assert(host.delivered[2].size() == static_cast<std::size_t>(feed_count));
            for (int i = 0; i < feed_count; ++i) {
                const auto& value = host.delivered[2][static_cast<std::size_t>(i)];
                calendar_bar_equal(value.bar, monthly[static_cast<std::size_t>(i)]);
                assert(value.context.delivered_at_ms
                       == primary[static_cast<std::size_t>(i / 3)].timestamp);
                assert(value.context.completion == NativeCompletionKind::Confirmed);
                assert(value.context.interval.open_ms == monthly[static_cast<std::size_t>(i)].timestamp);
                assert(value.context.interval.last_traded_close_ms
                       == calendar_origin + months[static_cast<std::size_t>(i + 1)] * day);
                assert(value.context.interval.next_period_open_ms
                       == calendar_origin + months[static_cast<std::size_t>(i + 1)] * day);
            }
            if (feed_complete) {
                const auto& value = host.delivered[0][0];
                calendar_bar_equal(value.bar, expected_feed);
                assert(value.context.delivered_at_ms == primary[lookahead ? 0 : 3].timestamp);
                assert(value.context.completion == NativeCompletionKind::Confirmed);
                assert(value.context.interval.open_ms == calendar_origin);
                assert(value.context.interval.last_traded_close_ms
                       == calendar_origin + months.back() * day);
                assert(value.context.interval.next_period_open_ms
                       == calendar_origin + months.back() * day);
            }
        }
    }
}
// The daily-input rows above do not exercise the intraday N-day partition.
// Keep this input-width target separate: all OHLCV comes from four-hour
// contributors, including a leading partial bucket and a shortened year end.
void intraday_n_day_population() {
    constexpr std::int64_t january = 1704067200000LL; // 2024-01-01 UTC.
    constexpr std::int64_t step = day / 6;
    std::vector<Bar> input;
    for (int i = 0; i < 23 * 6; ++i) {
        const double p = 500.0 + i;
        input.push_back({p, p + 9.0 + i % 5, p - 7.0 - i % 3,
                         p + (i % 2 ? 0.5 : -0.5), 1.0 + i,
                         january - 4 * day + i * step});
    }
    // Gregorian 2023 has 365 days. Its ordinal 360 is December 27;
    // the following year starts a fresh ordinal zero on January 1.
    for (int n : {2, 3, 5}) {
        std::vector<std::int64_t> opens;
        std::vector<std::int64_t> closes;
        for (int ordinal = 360; ordinal < 365; ++ordinal) {
            if (ordinal % n != 0) continue;
            const auto open = january + (ordinal - 365) * day;
            if (open < input.front().timestamp) continue;
            opens.push_back(open);
            closes.push_back(january + (std::min(ordinal + n, 365) - 365) * day);
        }
        for (int ordinal = 0; ordinal < 19; ordinal += n) {
            opens.push_back(january + ordinal * day);
            closes.push_back(january + (ordinal + n) * day);
        }
        std::vector<Bar> expected;
        for (std::size_t period = 0; period < opens.size(); ++period) {
            Bar folded{};
            bool seen = false;
            for (const auto& bar : input) {
                if (bar.timestamp < opens[period] || bar.timestamp >= closes[period]) continue;
                if (!seen) {
                    folded = bar;
                    folded.timestamp = opens[period];
                    seen = true;
                } else {
                    folded.high = std::max(folded.high, bar.high);
                    folded.low = std::min(folded.low, bar.low);
                    folded.close = bar.close;
                    folded.volume += bar.volume;
                }
            }
            assert(seen);
            expected.push_back(folded);
        }
        const auto tf = std::to_string(n) + "D";
        Probe empty(Action::Trace);
        run(empty, {}, "240", tf);
        assert(empty.last_run_status() == 0);
        assert(empty.rows.empty());

        // None of the first three inputs reaches an in-range N-day open.
        // This is an empty-population control, not a held-trade witness.
        Probe leading(Action::Trace);
        run(leading, std::vector<Bar>(input.begin(), input.begin() + 3), "240", tf);
        assert(leading.last_run_status() == 0);
        assert(leading.rows.empty());

        // A single contributor at a real period open is a developing final
        // chart bar, with the full calendar close and exactly one last flag.
        const std::vector<Bar> partial{input[4 * 6]};
        Probe tail(Action::Trace);
        run(tail, partial, "240", tf);
        assert(tail.last_run_status() == 0);
        trace(tail, {january});
        assert(tail.rows[0].close_stamp == january + n * day);
        assert(tail.rows[0].open == partial[0].open);
        assert(tail.rows[0].high == partial[0].high);
        assert(tail.rows[0].low == partial[0].low);
        assert(tail.rows[0].close == partial[0].close);
        assert(tail.rows[0].volume == partial[0].volume);

        Probe aggregated(Action::Hold), direct(Action::Hold);
        run(aggregated, input, "240", tf);
        run(direct, expected, tf, tf);
        assert(aggregated.last_run_status() == 0);
        assert(direct.last_run_status() == 0);
        trace(aggregated, opens);
        trace(direct, opens);
        for (const auto* probe : {&aggregated, &direct}) {
            for (std::size_t i = 0; i < expected.size(); ++i) {
                const auto& row = probe->rows[i];
                assert(row.close_stamp == closes[i]);
                assert(row.open == expected[i].open);
                assert(row.high == expected[i].high);
                assert(row.low == expected[i].low);
                assert(row.close == expected[i].close);
                assert(row.volume == expected[i].volume);
            }
            assert(probe->closed_trade_count() == 0);
            assert(probe->report_trade_count() == 1);
            const auto trade = probe->get_report_trade(0);
            assert(trade.open_at_end);
            assert(trade.is_long);
            assert(trade.qty == 1.0);
            assert(trade.entry_bar_index == 3);
            assert(trade.exit_bar_index == static_cast<int>(expected.size()) - 1);
            closed_fill(trade, expected[3].timestamp, expected[3].open,
                        expected.back().timestamp, expected.back().close);
            assert(trade.pnl == expected.back().close - expected[3].open);
            assert(probe->physical_position().signed_units == 1.0);
            assert(probe->physical_position().lot_count == 1);
            assert(probe->physical_position().average_price == expected[3].open);
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    assert(argc <= 2);
    const int row = argc == 2 ? std::stoi(argv[1]) : 0;
    // expectation corrected: 21 -> 22, because selector 22 pins direct monthly requests.
    // expectation corrected: 22 -> 26, because four existing witnesses are selectable alone.
    // expectation corrected: 26 -> 27, because selector 27 checks auxiliary calendar routing.
    // expectation corrected: 27 -> 28, because selector 28 covers intraday N-day input.
    assert(row >= 0 && row <= 28);
    const auto bars = daily_bars();
    if (row <= 4) three_days(bars, row);
    if (row == 0 || row == 5) {
        // TradingView tape sha256:ea679d91cbc0cd078c80604e2c73b3c9bb3104ff8d1989695da6abecb8b9e174
        other_days(bars, 2, {1, 7}); // Within-year offsets N*1 and N*7.
        // TradingView tape sha256:5342be10bdda8185b12b2c6a95b9b26101334cd1f58663ceb1f28c42197cb5dc
        other_days(bars, 5, {8, 22}); // Within-year offsets N*8 and N*22.
    }
    if (row == 0 || row == 6) controls(bars);
    if (row == 0 || row == 7) stream_chart(bars);
    if (row == 0 || row == 8) selected_chart(bars);
    if (row == 0 || row == 9) history_offsets(bars, false);
    if (row == 0 || row == 10) history_offsets(bars, true);
    if (row == 0 || row == 11) forward_offsets(bars);
    if (row == 0 || row == 12) selected_controls(bars);
    if (row == 0 || row == 13) calendar_batch_tail(calendar_cases[0], 13);
    if (row == 0 || row == 14) calendar_batch_tail(calendar_cases[1], 14);
    if (row == 0 || row == 15) calendar_batch_tail(calendar_cases[2], 15);
    if (row == 0 || row == 16) calendar_request_tail();
    if (row == 0 || row == 17) calendar_complete_period_controls();
    if (row == 0 || row == 18) calendar_stream_and_live_controls();
    if (row == 0 || row == 19) calendar_direct_natural_close_controls();
    if (row == 0 || row == 20) monthidx_monthly_metadata();
    if (row == 0 || row == 21) monthidx_daily_weekly_controls();
    if (row == 0 || row == 22) monthidx_monthly_requests();
    // Selector 0 already reaches these same bodies through selector 20.
    if (row >= 23 && row <= 26) monthidx_monthly_witness(row);
    if (row == 0 || row == 27) auxiliary_calendar_feed_timeframe();
    if (row == 0 || row == 28) intraday_n_day_population();
}
