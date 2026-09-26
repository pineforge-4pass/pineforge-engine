// TradingView's bar-magnifier intrabars (R5 lane MAG-INTRABAR): the table
// that picks the intrabar timeframe for a chart, and the lower-timeframe path
// the Pine adapter hands the kernel's intrabar path for a magnified run.
#include <pineforge/source/magnifier_intrabars.hpp>

#include <pineforge/native_calendar.hpp>

#include "../native_calendar_memo.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace pineforge::source {

namespace {

namespace cal = native_calendar;

constexpr std::int64_t kSecondMs = 1000;
constexpr std::int64_t kMinuteMs = 60 * kSecondMs;

// A fixed timeframe's duration, or 0 for a calendar one.
std::int64_t fixed_ms(const cal::Timeframe& tf) noexcept {
    switch (tf.unit()) {
    case cal::TimeframeUnit::Second: return tf.count() * kSecondMs;
    case cal::TimeframeUnit::Minute: return tf.count() * kMinuteMs;
    default: return 0;
    }
}

Bar one_price(double price, std::int64_t timestamp) noexcept {
    Bar bar{};
    bar.open = bar.high = bar.low = bar.close = price;
    bar.volume = 0.0;
    bar.timestamp = timestamp;
    return bar;
}

}  // namespace

std::string tradingview_intrabar_timeframe(std::string_view chart_tf) {
    const auto chart = cal::parse_timeframe(chart_tf);
    if (!chart || !chart->valid()) return {};
    std::int64_t seconds = 0;
    switch (chart->unit()) {
    case cal::TimeframeUnit::Second: seconds = chart->count(); break;
    case cal::TimeframeUnit::Minute: seconds = std::int64_t{chart->count()} * 60; break;
    case cal::TimeframeUnit::Day: seconds = std::int64_t{chart->count()} * 86400; break;
    case cal::TimeframeUnit::Week:
    case cal::TimeframeUnit::Month:
        return "1D";
    }
    constexpr std::int64_t kMinute = 60;
    constexpr std::int64_t kDay = 1440 * kMinute;
    if (seconds < 30) return "1S";
    if (seconds < kMinute) return "5S";
    if (seconds < 5 * kMinute) return "10S";
    if (seconds < 10 * kMinute) return "30S";
    if (seconds < 15 * kMinute) return "1";
    if (seconds < 30 * kMinute) return "2";
    if (seconds < 60 * kMinute) return "5";
    if (seconds < 240 * kMinute) return "10";
    if (seconds < kDay) return "30";
    if (seconds < 3 * kDay) return "60";
    if (seconds < 7 * kDay) return "240";
    return "1D";
}

std::vector<Bar> tradingview_magnifier_bars(const Bar* bars, int n,
                                            const std::string& input_tf,
                                            const std::string& script_tf,
                                            const std::string& session,
                                            const std::string& timezone) {
    std::vector<Bar> unchanged;
    if (!bars || n <= 0) return unchanged;
    unchanged.assign(bars, bars + n);

    const auto input = cal::parse_timeframe(input_tf);
    const auto chart = cal::parse_timeframe(script_tf);
    const auto intrabar = cal::parse_timeframe(tradingview_intrabar_timeframe(script_tf));
    if (!input || !chart || !intrabar) return unchanged;
    // The intrabars are aggregated from the input, so the input has to be a
    // strictly finer fixed grid that tiles them: an intrabar of the input's
    // own timeframe is the input path already, and a finer one (a 1- or
    // 5-minute chart walks 10- or 30-second bars) cannot be built from it.
    const std::int64_t input_ms = fixed_ms(*input);
    const std::int64_t intrabar_ms = fixed_ms(*intrabar);
    if (input_ms <= 0) return unchanged;
    if (intrabar_ms != 0 && (intrabar_ms <= input_ms || intrabar_ms % input_ms != 0))
        return unchanged;
    for (int i = 1; i < n; ++i) {
        if (bars[i].timestamp <= bars[i - 1].timestamp) return unchanged;
    }
    const auto calendar = cal::parse_session(session, timezone);
    if (!calendar) return unchanged;
    cal::SessionDayMemo memo;

    // Every input bar's chart bar, and its intrabar with the chart bar that
    // owns it -- the one holding the intrabar's last traded instant. Both are
    // the calendar's own intervals, the ones the kernel aggregates with.
    struct Row { std::int64_t chart_open; std::int64_t slot_open; std::int64_t owner_open; };
    std::vector<Row> rows(static_cast<std::size_t>(n));
    std::optional<cal::NativeInterval> chart_at, slot_at, owner_at;
    for (int i = 0; i < n; ++i) {
        const std::int64_t t = bars[i].timestamp;
        if (!chart_at || t < chart_at->open_ms || t >= chart_at->next_period_open_ms) {
            chart_at = cal::interval_containing(*calendar, *chart, t, memo);
            if (!chart_at) return unchanged;
        }
        if (!slot_at || t < slot_at->open_ms || t >= slot_at->next_period_open_ms) {
            slot_at = cal::interval_containing(*calendar, *intrabar, t, memo);
            if (!slot_at || slot_at->last_traded_close_ms <= slot_at->open_ms) return unchanged;
            owner_at = cal::interval_containing(*calendar, *chart,
                                                slot_at->last_traded_close_ms - 1, memo);
            if (!owner_at) return unchanged;
        }
        rows[static_cast<std::size_t>(i)] = {chart_at->open_ms, slot_at->open_ms, owner_at->open_ms};
    }

    // Consecutive runs of the input: chart bars, and intrabars with their
    // owner. Both partitions are time-ordered, and an owner never precedes
    // the chart bar of its intrabar's first input.
    struct Span { std::int64_t key; std::int64_t owner; int first; int last; };
    std::vector<Span> charts, slots;
    for (int i = 0; i < n; ++i) {
        const auto& row = rows[static_cast<std::size_t>(i)];
        if (charts.empty() || charts.back().key != row.chart_open)
            charts.push_back({row.chart_open, row.chart_open, i, i});
        else
            charts.back().last = i;
        if (slots.empty() || slots.back().key != row.slot_open)
            slots.push_back({row.slot_open, row.owner_open, i, i});
        else
            slots.back().last = i;
    }

    std::vector<Bar> path;
    path.reserve(slots.size() + 2 * charts.size());
    std::size_t slot = 0;
    for (const auto& bar : charts) {
        // An intrabar owned by a chart bar the input never reaches has no
        // chart bar to walk in; TradingView has none either.
        while (slot < slots.size() && slots[slot].owner < bar.key) ++slot;
        const std::size_t owned_begin = slot;
        while (slot < slots.size() && slots[slot].owner == bar.key) ++slot;
        if (owned_begin == slot) continue;  // the kernel walks the chart bar's own OHLC
        const Bar& chart_first = bars[bar.first];
        const Bar& chart_last = bars[bar.last];
        std::int64_t floor = chart_first.timestamp;
        if (slots[owned_begin].first < bar.first) {
            // The straddler reaches back into the previous chart bar, so its
            // open is not this bar's: the path opens at the chart bar's own
            // open first (where an order placed at the last close fills).
            path.push_back(one_price(chart_first.open, floor));
            ++floor;
        }
        for (std::size_t s = owned_begin; s < slot; ++s) {
            const Span& span = slots[s];
            Bar intrabar = bars[span.first];
            for (int i = span.first + 1; i <= span.last; ++i) {
                intrabar.high = std::max(intrabar.high, bars[i].high);
                intrabar.low = std::min(intrabar.low, bars[i].low);
                intrabar.close = bars[i].close;
                intrabar.volume += bars[i].volume;
            }
            intrabar.timestamp = std::max(bars[span.first].timestamp, floor);
            floor = intrabar.timestamp + 1;
            path.push_back(intrabar);
        }
        if (slots[slot - 1].last < bar.last) {
            // The chart bar's last inputs open a later bar's straddler: the
            // path still closes at this chart bar's own close.
            path.push_back(one_price(chart_last.close, std::max(chart_last.timestamp, floor)));
        }
    }
    return path;
}

}  // namespace pineforge::source
