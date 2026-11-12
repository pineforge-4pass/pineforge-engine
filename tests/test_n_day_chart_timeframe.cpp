#include <pineforge/engine.hpp>
#include <pineforge/native_calendar.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/selected_window.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
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
                        barstate_islast_, bar.open, bar.close});
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
            // partial week is retained; the unfinished final week is not sealed.
            const auto monday = begin - ((begin / day + 3) % 7) * day;
            for (auto stamp = monday; stamp + 7 * day <= end; stamp += 7 * day)
                opens.push_back(stamp);
        } else {
            // A month seals only once its next first-of-month is reached.
            for (int month = 12; month <= 17; ++month)
                opens.push_back(date(2023 + month / 13, (month - 1) % 12 + 1, 1));
        }
        Probe probe(Action::RoundTrips);
        probe.entries = {2};
        run(probe, bars, "1D", tf);
        trace(probe, opens, false);
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

void stream_tick_labels() {
    // Wrapped sessions use their closing date as the trading date. Start at
    // an epoch-grid bucket's first session and retain only that daily bar.
    const auto trading_date = epoch_grid(3, date(2024, 1, 15), date(2024, 2, 1)).front();
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
    const auto warmup_opens = epoch_grid(3, bars.front().timestamp, live->timestamp);
    trace(probe, warmup_opens, false);
    for (auto it = live; it != bars.end(); ++it)
        assert(probe.stream_push_bar(*it));
    const auto opens = epoch_grid(3, bars.front().timestamp, bars.back().timestamp + day);
    trace(probe, opens, false);
    assert(probe.rows.size() > warmup_opens.size());
    for (std::size_t i = 0; i < opens.size(); ++i) {
        assert(probe.rows[i].open == price(std::max(opens[i], bars.front().timestamp)));
        assert(probe.rows[i].close == price(opens[i] + 2 * day));
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
} // namespace

int main(int argc, char** argv) {
    assert(argc <= 2);
    const int row = argc == 2 ? std::stoi(argv[1]) : 0;
    assert(row >= 0 && row <= 12);
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
}
