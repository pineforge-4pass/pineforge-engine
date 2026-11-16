// Complete-prefix stream reports equal batch from the first realtime script bar.
// An open position at zero realtime bars retains the warmup mark; mid-bar reads
// are host presentation of a forming bar, not a pinned batch-comparable boundary.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "oracle_fixture_config_shim.hpp"

namespace {

int failures = 0;
int checks = 0;

#define CHECK(expression) \
    do { \
        ++checks; \
        if (!(expression)) { \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #expression); \
            ++failures; \
        } \
    } while (false)

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

class ReportStrategy final : public pineforge::source::PineStrategyHost {
public:
    ReportStrategy() {
        initial_capital_ = 10000.0;
        syminfo_mintick_ = 0.01;
        default_qty_type_ = pineforge::QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 15.0;
        commission_type_ = pineforge::CommissionType::PERCENT;
        commission_value_ = 0.075;
        pyramiding_ = 2;
        margin_call_enabled_ = false;
        set_pine_risk_max_drawdown(95.0, true);
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 4 || bar_index_ == 16)
            strategy_entry("long", true);
        if (bar_index_ == 7)
            strategy_close("long", {}, std::numeric_limits<double>::quiet_NaN(), 40.0);
        if (bar_index_ == 10) strategy_entry("short", false);
        if (bar_index_ == 14) strategy_close_all();
    }

    double stored_open_profit() const {
        return equity_curve_.empty() ? 0.0 : equity_curve_.back().open_profit;
    }

    std::size_t real_trade_count() const { return trades_.size(); }
};

void compare_reports(const pineforge::ReportC& batch, const pineforge::ReportC& stream) {
    CHECK(batch.input_bars_processed == stream.input_bars_processed);
    CHECK(batch.script_bars_processed == stream.script_bars_processed);
    CHECK(batch.total_trades == stream.total_trades);
    CHECK(bits(batch.net_profit) == bits(stream.net_profit));
    CHECK(batch.trades_len == stream.trades_len);
    for (int index = 0; index < batch.trades_len && index < stream.trades_len; ++index) {
        const auto& expected = batch.trades[index];
        const auto& actual = stream.trades[index];
        CHECK(expected.entry_time == actual.entry_time);
        CHECK(expected.exit_time == actual.exit_time);
        CHECK(expected.is_long == actual.is_long);
        CHECK(expected.open_at_end == actual.open_at_end);
        CHECK(bits(expected.qty) == bits(actual.qty));
        CHECK(bits(expected.entry_price) == bits(actual.entry_price));
        CHECK(bits(expected.exit_price) == bits(actual.exit_price));
        CHECK(bits(expected.pnl) == bits(actual.pnl));
        CHECK(bits(expected.commission) == bits(actual.commission));
        CHECK(bits(expected.max_drawdown) == bits(actual.max_drawdown));
        CHECK(bits(expected.max_runup) == bits(actual.max_runup));
    }
    CHECK(batch.equity_curve_len == stream.equity_curve_len);
    bool diagnosed = false;
    for (std::int64_t index = 0;
         index < batch.equity_curve_len && index < stream.equity_curve_len; ++index) {
        const auto& expected = batch.equity_curve[index];
        const auto& actual = stream.equity_curve[index];
        if (!diagnosed && (bits(expected.equity) != bits(actual.equity)
            || bits(expected.open_profit) != bits(actual.open_profit))) {
            std::fprintf(stderr, "curve prefix=%lld index=%lld equity=%.17g/%.17g open=%.17g/%.17g\n",
                static_cast<long long>(batch.script_bars_processed),
                static_cast<long long>(index), expected.equity, actual.equity,
                expected.open_profit, actual.open_profit);
            diagnosed = true;
        }
        CHECK(expected.time_ms == actual.time_ms);
        CHECK(bits(expected.equity) == bits(actual.equity));
        CHECK(bits(expected.open_profit) == bits(actual.open_profit));
    }
    CHECK(bits(batch.metrics.equity.max_equity_drawdown)
        == bits(stream.metrics.equity.max_equity_drawdown));
    CHECK(bits(batch.metrics.equity.max_equity_runup)
        == bits(stream.metrics.equity.max_equity_runup));
}

void cumulative_reports_do_not_change_continuation(int split) {
    std::vector<pineforge::Bar> bars;
    for (int index = 0; index < 300; ++index) {
        const double price = 100.003 + (index % 31) * 0.137;
        bars.push_back({price, price + 0.42, price - 0.31, price + 0.093,
                        1.0, 1743465600000LL + index * 60000LL});
    }
    ReportStrategy stream;
    ReportStrategy unobserved;
    CHECK(stream.stream_begin(bars.data(), split, "1", "15"));
    CHECK(unobserved.stream_begin(bars.data(), split, "1", "15"));
    bool saw_unrealized_profit = false;
    for (int index = split; index < static_cast<int>(bars.size()); ++index) {
        CHECK(stream.stream_push_bar(bars[index]));
        CHECK(unobserved.stream_push_bar(bars[index]));
        const auto hash = stream.stream_state_hash();
        const double ordinary_profit = stream.stored_open_profit();
        pineforge::ReportC observed{};
        CHECK(strategy_stream_fill_report(&stream,
            reinterpret_cast<pf_report_t*>(&observed)) == 0);
        CHECK(hash == stream.stream_state_hash());
        CHECK(stream.stream_state_hash() == unobserved.stream_state_hash());
        CHECK(bits(ordinary_profit) == bits(stream.stored_open_profit()));
        if (ordinary_profit != 0.0) saw_unrealized_profit = true;
        if ((index + 1) % 15 == 0) {
            ReportStrategy batch;
            batch.run(bars.data(), index + 1, "1", "15");
            CHECK(batch.last_error().empty());
            pineforge::ReportC expected{};
            batch.fill_report(&expected);
            compare_reports(expected, observed);
            pineforge::BacktestEngine::free_report(&expected);
        }
        pineforge::BacktestEngine::free_report(&observed);
    }
    CHECK(saw_unrealized_profit);
    CHECK(stream.real_trade_count() >= 3);
    CHECK(stream.stream_end(false));
    pineforge::ReportC ended{};
    CHECK(strategy_stream_fill_report(&stream,
        reinterpret_cast<pf_report_t*>(&ended)) == 0);
    ReportStrategy batch;
    batch.run(bars.data(), static_cast<int>(bars.size()), "1", "15");
    pineforge::ReportC expected{};
    batch.fill_report(&expected);
    compare_reports(expected, ended);
    pineforge::BacktestEngine::free_report(&expected);
    pineforge::BacktestEngine::free_report(&ended);
}

class BracketStrategy final : public pineforge::source::PineStrategyHost {
public:
    BracketStrategy(int entry_kind, bool same_calculation, bool is_long,
                    bool limit_exit, double offset, bool prearm)
        : entry_kind_(entry_kind), same_calculation_(same_calculation),
          is_long_(is_long), limit_exit_(limit_exit), offset_(offset), prearm_(prearm) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config{};
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_string("tickerid", "TEST:BRACKET");
        set_syminfo_string("ticker", "BRACKET");
        set_syminfo_string("type", "stock");
        set_syminfo_metadata("qty_step", 1.0);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_chart_timezone("UTC");
        fixture_retain_all_events();
    }

    void on_source_bar(const pineforge::Bar&) override {
        const double absent = std::numeric_limits<double>::quiet_NaN();
        if (pine_bar_index() == 1)
            strategy_entry("ENTRY", is_long_, entry_kind_ == 2 ? 100.0 + offset_ : absent,
                           entry_kind_ == 1 ? 100.0 + offset_ : absent, 2.0);
        if (pine_bar_index() == (same_calculation_ ? 1 : 2)
            || (prearm_ && same_calculation_ && pine_bar_index() == 0)) {
            const double level = (is_long_ == limit_exit_ ? 105.0 : 95.0) + offset_;
            strategy_exit("EXIT", "ENTRY", limit_exit_ ? level : absent,
                          limit_exit_ ? absent : level);
        }
    }

private:
    int entry_kind_;
    bool same_calculation_;
    bool is_long_;
    bool limit_exit_;
    double offset_;
    bool prearm_;
};

void same_calculation_bracket_reports(int timeframe, int entry_kind,
                                     bool same_calculation, bool is_long,
                                     bool limit_exit, double offset, bool prearm) {
    // Kernel-only DEFECT-7 characterization: aggregated input 1 is not runner admission.
    std::vector<pineforge::Bar> bars;
    const int exit_bar = same_calculation ? 2 : 3;
    const double sign = is_long == limit_exit ? 1.0 : -1.0;
    for (int index = 0; index < 5 * timeframe; ++index) {
        double open = 100.0;
        double close = 100.0;
        double high = 100.1;
        double low = 99.9;
        if (index / timeframe == exit_bar && index % timeframe >= std::min(3, timeframe - 1)) {
            close += sign * 5.0;
            if (timeframe != 1) open = close;
            high = std::max(high, 100.0 + sign * 10.0);
            low = std::min(low, 100.0 + sign * 10.0);
        }
        bars.push_back({open + offset, high + offset, low + offset, close + offset,
                        10.0, 1743465600000LL + index * 60000LL});
    }
    BracketStrategy batch(entry_kind, same_calculation, is_long, limit_exit, offset, prearm);
    BracketStrategy stream(entry_kind, same_calculation, is_long, limit_exit, offset, prearm);
    const auto script_tf = std::to_string(timeframe);
    batch.run(bars.data(), static_cast<int>(bars.size()), "1", script_tf);
    CHECK(batch.last_error().empty());
    CHECK(stream.stream_begin(bars.data(), timeframe, "1", script_tf));
    for (int index = timeframe; index < static_cast<int>(bars.size()); ++index) {
        CHECK(stream.stream_push_bar(bars[index]));
        pineforge::ReportC interim{};
        CHECK(strategy_stream_fill_report(&stream, reinterpret_cast<pf_report_t*>(&interim)) == 0);
        pineforge::BacktestEngine::free_report(&interim);
    }
    CHECK(stream.stream_end(false));
    pineforge::ReportC expected{};
    pineforge::ReportC observed{};
    batch.fill_report(&expected);
    CHECK(strategy_stream_fill_report(&stream, reinterpret_cast<pf_report_t*>(&observed)) == 0);
    CHECK(expected.trades_len == 1);
    if (expected.trades_len == 1) CHECK(!expected.trades[0].open_at_end);
    const int prior_failures = failures;
    if (timeframe == 15 && entry_kind == 0 && same_calculation && is_long
        && !limit_exit && offset == 0.0 && prearm && expected.trades_len == 1)
        std::fprintf(stderr, "minimal bracket entry=%.17g exit=%.17g drawdown=%.17g/%.17g\n",
                     expected.trades[0].entry_price, expected.trades[0].exit_price,
                     expected.trades[0].max_drawdown, observed.trades[0].max_drawdown);
    compare_reports(expected, observed);
    if (prior_failures != failures)
        std::fprintf(stderr, "bracket tf=%d entry=%d same=%d long=%d limit=%d offset=%.3f prearm=%d\n",
                     timeframe, entry_kind, same_calculation, is_long, limit_exit, offset, prearm);
    pineforge::BacktestEngine::free_report(&expected);
    pineforge::BacktestEngine::free_report(&observed);
}

// Civil dates and UTC offsets are the independent fixture oracle. All dates
// below are in 2026 (a non-leap year); no production calendar resolves them.
struct CalendarDate {
    int month, day, utc_offset_hours;
};

std::int64_t calendar_stamp(CalendarDate date, int hour, int minute) {
    constexpr int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = date.day - 1;
    for (int month = 1; month < date.month; ++month) days += month_days[month - 1];
    constexpr std::int64_t january_1_2026_utc = 1767225600000LL;
    return january_1_2026_utc + days * 86400000LL
        + (hour - date.utc_offset_hours) * 3600000LL + minute * 60000LL;
}

struct CalendarCase {
    const char* name;
    const char* timezone;
    const char* timeframe;
    CalendarDate openings[5];
    CalendarDate closes[5];
};

class CalendarReportStrategy final : public pineforge::source::PineStrategyHost {
public:
    std::vector<pineforge::Bar> consumed;
    std::vector<std::int64_t> close_times;

    explicit CalendarReportStrategy(const CalendarCase& cell) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_timezone(cell.timezone);
        set_chart_timezone(cell.timezone);
        set_syminfo_session("0930-1600:23456");
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        consumed.push_back(bar);
        close_times.push_back(time_close());
        if (bar_index_ == 0) strategy_entry("calendar-long", true);
        if (bar_index_ == 1) strategy_close_all();
        if (bar_index_ == 2) strategy_entry("calendar-short", false);
        if (bar_index_ == 3) strategy_close_all();
    }
};

void calendar_values(const CalendarReportStrategy& host, const CalendarCase& cell,
                     const pineforge::Bar* bars, int count) {
    CHECK(host.consumed.size() == static_cast<std::size_t>(count));
    CHECK(host.close_times.size() == static_cast<std::size_t>(count));
    for (int index = 0; index < count
         && index < static_cast<int>(host.consumed.size())
         && index < static_cast<int>(host.close_times.size()); ++index) {
        const auto& actual = host.consumed[index];
        CHECK(actual.timestamp == bars[index].timestamp);
        CHECK(bits(actual.open) == bits(bars[index].open));
        CHECK(bits(actual.high) == bits(bars[index].high));
        CHECK(bits(actual.low) == bits(bars[index].low));
        CHECK(bits(actual.close) == bits(bars[index].close));
        CHECK(bits(actual.volume) == bits(bars[index].volume));
        CHECK(host.close_times[index] == calendar_stamp(cell.closes[index], 16, 0));
    }
}

void calendar_confirmed_reports_and_recovery() {
    const CalendarCase cases[] = {
        {"UTC daily control", "UTC", "1D",
         {{3, 5, 0}, {3, 6, 0}, {3, 9, 0}, {3, 10, 0}, {3, 11, 0}},
         {{3, 5, 0}, {3, 6, 0}, {3, 9, 0}, {3, 10, 0}, {3, 11, 0}}},
        {"NY daily winter control", "America/New_York", "1D",
         {{1, 8, -5}, {1, 9, -5}, {1, 12, -5}, {1, 13, -5}, {1, 14, -5}},
         {{1, 8, -5}, {1, 9, -5}, {1, 12, -5}, {1, 13, -5}, {1, 14, -5}}},
        {"NY daily spring", "America/New_York", "1D",
         {{3, 5, -5}, {3, 6, -5}, {3, 9, -4}, {3, 10, -4}, {3, 11, -4}},
         {{3, 5, -5}, {3, 6, -5}, {3, 9, -4}, {3, 10, -4}, {3, 11, -4}}},
        {"NY daily autumn", "America/New_York", "1D",
         {{10, 29, -4}, {10, 30, -4}, {11, 2, -5}, {11, 3, -5}, {11, 4, -5}},
         {{10, 29, -4}, {10, 30, -4}, {11, 2, -5}, {11, 3, -5}, {11, 4, -5}}},
        {"NY weekly spring", "America/New_York", "1W",
         {{2, 23, -5}, {3, 2, -5}, {3, 9, -4}, {3, 16, -4}, {3, 23, -4}},
         {{2, 27, -5}, {3, 6, -5}, {3, 13, -4}, {3, 20, -4}, {3, 27, -4}}},
        {"NY weekly autumn", "America/New_York", "1W",
         {{10, 19, -4}, {10, 26, -4}, {11, 2, -5}, {11, 9, -5}, {11, 16, -5}},
         {{10, 23, -4}, {10, 30, -4}, {11, 6, -5}, {11, 13, -5}, {11, 20, -5}}},
    };
    for (const auto& cell : cases) {
        pineforge::Bar bars[5];
        for (int index = 0; index < 5; ++index) {
            // The first three OHLCV rows are the issue's unchanged reproducer.
            bars[index] = {100.0 + index, 102.0 + index, 99.0 + index, 101.0 + index,
                           1000.0, calendar_stamp(cell.openings[index], 9, 30)};
        }
        CalendarReportStrategy batch(cell);
        batch.run(bars, 5, cell.timeframe, cell.timeframe, false);
        CHECK(batch.last_error().empty());
        calendar_values(batch, cell, bars, 5);
        CHECK(batch.trade_count() == 2);
        pineforge::ReportC expected{};
        batch.fill_report(&expected);

        // Split 2 is the exact reported transition. Split 3 reconstructs the
        // accepted prefix in a new process-equivalent host after that boundary.
        for (int split : {2, 3}) {
            CalendarReportStrategy stream(cell), replay(cell);
            const bool began = stream.stream_begin(bars, split, cell.timeframe, cell.timeframe);
            const bool replay_began = replay.stream_begin(bars, split, cell.timeframe, cell.timeframe);
            CHECK(began && replay_began);
            if (!began || !replay_began) continue;
            calendar_values(stream, cell, bars, split);
            CHECK(stream.stream_state_hash() == replay.stream_state_hash());
            bool completed = true;
            for (int index = split; index < 5; ++index) {
                const auto before = stream.stream_state_hash();
                auto wrong = bars[index];
                wrong.timestamp += 60000; // 09:31 is not the configured opening.
                std::vector<pineforge::Bar> rejected{bars[index - 1], wrong};
                if (index + 1 < 5) rejected.push_back(bars[index + 1]);
                for (const auto& invalid : rejected) {
                    // Duplicate, misaligned and (when available) missing input.
                    CHECK(!stream.stream_push_bar(invalid));
                    CHECK(stream.stream_state_hash() == before);
                    CHECK(stream.consumed.size() == static_cast<std::size_t>(index));
                    CHECK(stream.stream_order_actions_len() == 0);
                }
                const bool accepted = stream.stream_push_bar(bars[index]);
                const bool replayed = replay.stream_push_bar(bars[index]);
                if (!accepted || !replayed)
                    std::fprintf(stderr, "FAIL calendar %s warmup=%d input=%d: %s / %s\n",
                        cell.name, split, index, stream.last_error().c_str(),
                        replay.last_error().c_str());
                CHECK(accepted && replayed);
                if (!accepted || !replayed) { completed = false; break; }
                calendar_values(stream, cell, bars, index + 1);
                calendar_values(replay, cell, bars, index + 1);
                const int offset_change = cell.openings[index].utc_offset_hours
                    - cell.openings[index - 1].utc_offset_hours;
                if (offset_change != 0) {
                    // Keeping the old UTC offset gives 10:30 in spring and
                    // 08:30 in autumn. Reject it on an independent prefix so
                    // a bad admission cannot contaminate the valid stream.
                    CalendarReportStrategy shifted(cell);
                    const bool shifted_began = shifted.stream_begin(
                        bars, index, cell.timeframe, cell.timeframe);
                    CHECK(shifted_began);
                    if (shifted_began) {
                        auto old_offset = bars[index];
                        old_offset.timestamp += offset_change * 3600000LL;
                        const auto unchanged = shifted.stream_state_hash();
                        CHECK(!shifted.stream_push_bar(old_offset));
                        CHECK(shifted.stream_state_hash() == unchanged);
                        CHECK(shifted.stream_order_actions_len() == 0);
                        calendar_values(shifted, cell, bars, index);
                    }
                }
                CHECK(stream.stream_state_hash() == replay.stream_state_hash());
                CHECK(stream.stream_order_actions_len() == 1);
                CHECK(replay.stream_order_actions_len() == 1);
                if (stream.stream_order_actions_len() == 1
                    && replay.stream_order_actions_len() == 1) {
                    const auto& actual = stream.stream_order_action_at(0);
                    const auto& again = replay.stream_order_action_at(0);
                    CHECK(actual.sequence == static_cast<std::uint64_t>(index - split + 1));
                    CHECK(actual.timestamp_ms == bars[index].timestamp);
                    CHECK(actual.bar_index == index);
                    CHECK(actual.is_entry == (index % 2 == 1));
                    CHECK(actual.is_long == (index <= 2));
                    CHECK(actual.quantity == 1.0 && actual.price == bars[index].open);
                    CHECK(actual.entry_incarnation != 0);
                    CHECK(actual.sequence == again.sequence);
                    CHECK(actual.timestamp_ms == again.timestamp_ms);
                    CHECK(actual.bar_index == again.bar_index);
                    CHECK(actual.is_entry == again.is_entry && actual.is_long == again.is_long);
                    CHECK(bits(actual.quantity) == bits(again.quantity));
                    CHECK(bits(actual.price) == bits(again.price));
                    CHECK(actual.order_id == again.order_id && actual.comment == again.comment);
                    CHECK(actual.entry_incarnation == again.entry_incarnation);
                }
                stream.stream_order_actions_clear();
                replay.stream_order_actions_clear();
            }
            if (!completed) continue;
            CHECK(stream.trade_count() == 2 && stream.live_position_size() == 0.0);
            for (int index = 0; index < 2 && index < stream.trade_count(); ++index) {
                const auto& trade = stream.get_trade(index);
                CHECK(trade.entry_time == bars[2 * index + 1].timestamp);
                CHECK(trade.exit_time == bars[2 * index + 2].timestamp);
                CHECK(trade.entry_price == bars[2 * index + 1].open);
                CHECK(trade.exit_price == bars[2 * index + 2].open);
                CHECK(trade.qty == 1.0);
            }
            pineforge::ReportC actual{}, recovered{};
            stream.fill_report(&actual);
            replay.fill_report(&recovered);
            CHECK(actual.input_bars_processed == 5 && actual.script_bars_processed == 5);
            compare_reports(expected, actual);
            compare_reports(actual, recovered);
            pineforge::BacktestEngine::free_report(&actual);
            pineforge::BacktestEngine::free_report(&recovered);
            CHECK(stream.stream_end(false));
            CHECK(replay.stream_end(false));
        }
        pineforge::BacktestEngine::free_report(&expected);
    }
}

void lord_howe_confirmed_daily_successors() {
    // Lord Howe ends DST on 2026-04-05 (+11:00 -> +10:30), and starts
    // it on 2026-10-04 (+10:30 -> +11:00). Friday -> Monday at 09:30
    // therefore takes 72.5 / 71.5 hours, independent of engine output.
    const struct {
        int month;
        int days[3];
        int utc_offset_minutes[3];
        int weekend_minutes;
    } cases[] = {
        {4, {2, 3, 6}, {660, 660, 630}, 4350},
        {10, {1, 2, 5}, {630, 630, 660}, 4290},
    };
    const CalendarCase cell{"Lord Howe daily", "Australia/Lord_Howe", "1D", {}, {}};
    for (const auto& fixture : cases) {
        pineforge::Bar bars[3];
        std::int64_t closes[3];
        for (int index = 0; index < 3; ++index) {
            const CalendarDate date{fixture.month, fixture.days[index], 0};
            const auto offset = fixture.utc_offset_minutes[index] * 60000LL;
            bars[index] = {100.0 + index, 102.0 + index, 99.0 + index,
                           101.0 + index, 1000.0, calendar_stamp(date, 9, 30) - offset};
            closes[index] = calendar_stamp(date, 16, 0) - offset;
        }
        CHECK(bars[2].timestamp - bars[1].timestamp == fixture.weekend_minutes * 60000LL);
        CalendarReportStrategy batch(cell), stream(cell);
        batch.run(bars, 3, cell.timeframe, cell.timeframe, false);
        CHECK(batch.last_error().empty());
        const bool began = stream.stream_begin(bars, 2, cell.timeframe, cell.timeframe);
        CHECK(began);
        if (!began) continue;
        const auto before = stream.stream_state_hash();
        for (const auto shift : {60000LL,
             (fixture.utc_offset_minutes[2] - fixture.utc_offset_minutes[1]) * 60000LL}) {
            auto wrong = bars[2];
            wrong.timestamp += shift; // 09:31, or an opening carried at the old UTC offset.
            CHECK(!stream.stream_push_bar(wrong));
            CHECK(stream.stream_state_hash() == before);
            CHECK(stream.consumed.size() == 2 && stream.close_times.size() == 2);
            CHECK(stream.stream_order_actions_len() == 0);
        }
        const bool accepted = stream.stream_push_bar(bars[2]);
        CHECK(accepted);
        if (!accepted) continue;
        CHECK(stream.consumed.size() == 3 && stream.close_times.size() == 3);
        for (int index = 0; index < 3
             && index < static_cast<int>(stream.consumed.size())
             && index < static_cast<int>(stream.close_times.size()); ++index) {
            const auto& actual = stream.consumed[index];
            CHECK(actual.timestamp == bars[index].timestamp);
            CHECK(bits(actual.open) == bits(bars[index].open));
            CHECK(bits(actual.high) == bits(bars[index].high));
            CHECK(bits(actual.low) == bits(bars[index].low));
            CHECK(bits(actual.close) == bits(bars[index].close));
            CHECK(bits(actual.volume) == bits(bars[index].volume));
            CHECK(stream.close_times[index] == closes[index]);
        }
        CHECK(stream.trade_count() == 1 && stream.live_position_size() == 0.0);
        if (stream.trade_count() == 1) {
            const auto& trade = stream.get_trade(0);
            CHECK(trade.entry_time == bars[1].timestamp && trade.exit_time == bars[2].timestamp);
            CHECK(trade.entry_price == 101.0 && trade.exit_price == 102.0 && trade.qty == 1.0);
        }
        CHECK(stream.stream_order_actions_len() == 1);
        if (stream.stream_order_actions_len() == 1) {
            const auto& fill = stream.stream_order_action_at(0);
            CHECK(fill.timestamp_ms == bars[2].timestamp && fill.bar_index == 2);
            CHECK(!fill.is_entry && fill.is_long && fill.quantity == 1.0 && fill.price == 102.0);
        }
        pineforge::ReportC expected{}, actual{};
        batch.fill_report(&expected);
        stream.fill_report(&actual);
        compare_reports(expected, actual);
        pineforge::BacktestEngine::free_report(&expected);
        pineforge::BacktestEngine::free_report(&actual);
        CHECK(stream.stream_end(false));
    }
}

}

int main() {
    calendar_confirmed_reports_and_recovery();
    lord_howe_confirmed_daily_successors();
    for (int split : {15, 22, 29}) cumulative_reports_do_not_change_continuation(split);
    for (int timeframe : {1, 5, 15, 60})
        for (int entry_kind : {0, 1, 2})
            for (bool same_calculation : {false, true})
                for (bool is_long : {false, true})
                    for (bool limit_exit : {false, true})
                        for (double offset : {0.0, 0.003})
                            for (bool prearm : {false, true})
                                same_calculation_bracket_reports(timeframe, entry_kind,
                                    same_calculation, is_long, limit_exit, offset, prearm);
    std::printf("stream report equivalence: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
