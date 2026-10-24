// TradingView's bar-magnifier intrabars (R5 lane MAG-INTRABAR): the table
// that picks the intrabar timeframe for a chart, and the lower-timeframe path
// the Pine adapter hands the kernel's intrabar path for a magnified run.
#include <pineforge/source/magnifier_intrabars.hpp>

#include <pineforge/native_calendar.hpp>
#include <pineforge/run_failure.hpp>

#include "../native_calendar_memo.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

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

namespace {

[[noreturn]] void path_failure(const char* message) {
    throw coded<std::runtime_error>(RunFailureCode::run_options_rejected,
                                    {{"option", "intrabar_path"}}, message);
}

[[noreturn]] void path_invariant(const char* message) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, message);
}

std::int64_t after_stamp(std::int64_t stamp) {
    if (stamp == std::numeric_limits<std::int64_t>::max())
        throw coded<std::overflow_error>(RunFailureCode::run_options_rejected,
                                         {{"option", "intrabar_path"}},
                                         "magnifier timestamp exhausted");
    return stamp + 1;
}

// All durable fields below are derived cursor/storage state. Identity is the
// count-first fold of the complete logical path, taken eagerly at configure;
// caches, cursor positions and buffer addresses introduce no broker state.
class MagnifierPathSource final : public NativeLowerPathSource {
    const Bar* input_;
    int n_;
    std::optional<cal::Timeframe> chart_tf_, slot_tf_;
    std::optional<cal::SessionCalendar> calendar_;
    bool fallback_ = true;
    std::size_t count_ = 0;

    struct Row {
        std::int64_t chart, slot, owner;
        int index;
    };
    struct Rows {
        const MagnifierPathSource* source;
        int at = 0;
        bool failed = false;
        cal::SessionDayMemo memo;
        std::optional<cal::NativeInterval> chart, slot, owner;

        explicit Rows(const MagnifierPathSource* value) : source(value) {}
        // Calendar memos are disposable caches, not part of cursor position.
        Rows(const Rows& other)
            : source(other.source), at(other.at), failed(other.failed),
              chart(other.chart), slot(other.slot), owner(other.owner) {}
        bool next(Row& row) {
            if (at == source->n_) return false;
            const auto t = source->input_[at].timestamp;
            if (!chart || t < chart->open_ms || t >= chart->next_period_open_ms) {
                chart = cal::interval_containing(*source->calendar_, *source->chart_tf_, t, memo);
                if (!chart) { failed = true; return false; }
            }
            if (!slot || t < slot->open_ms || t >= slot->next_period_open_ms) {
                slot = cal::interval_containing(*source->calendar_, *source->slot_tf_, t, memo);
                if (!slot || slot->last_traded_close_ms <= slot->open_ms) {
                    failed = true; return false;
                }
                owner = cal::interval_containing(*source->calendar_, *source->chart_tf_,
                                                 slot->last_traded_close_ms - 1, memo);
                if (!owner) { failed = true; return false; }
            }
            row = {chart->open_ms, slot->open_ms, owner->open_ms, at++};
            return true;
        }
    };
    struct Span {
        std::int64_t key, owner;
        int first, last;
        Bar value;
    };
    struct Spans {
        Rows rows;
        bool slots;
        std::optional<Row> pending;
        Spans(const MagnifierPathSource* source, bool is_slot) : rows(source), slots(is_slot) {}
        std::optional<Span> next() {
            Row row{};
            if (pending) { row = *pending; pending.reset(); }
            else if (!rows.next(row)) {
                if (rows.failed) path_failure("magnifier calendar lookup changed after pre-scan");
                return std::nullopt;
            }
            Span out{slots ? row.slot : row.chart, slots ? row.owner : row.chart,
                     row.index, row.index, rows.source->input_[row.index]};
            while (rows.next(row)) {
                if ((slots ? row.slot : row.chart) != out.key) {
                    pending = row;
                    break;
                }
                out.last = row.index;
                if (slots) {
                    const Bar& bar = rows.source->input_[row.index];
                    out.value.high = std::max(out.value.high, bar.high);
                    out.value.low = std::min(out.value.low, bar.low);
                    out.value.close = bar.close;
                    out.value.volume += bar.volume;
                }
            }
            if (rows.failed) path_failure("magnifier calendar lookup changed after pre-scan");
            return out;
        }
    };
    struct Cursor {
        const MagnifierPathSource* source;
        Spans charts, slots;
        std::optional<Span> chart, slot;
        std::int64_t floor = 0;
        bool active = false, opening = false, closing = false;
        explicit Cursor(const MagnifierPathSource* value)
            : source(value), charts(value, false), slots(value, true), slot(slots.next()) {}

        bool next(Bar& out) {
            while (!active) {
                chart = charts.next();
                if (!chart) return false;
                while (slot && slot->owner < chart->key) slot = slots.next();
                // No owned slot: the kernel walks this chart bar's own OHLC.
                if (!slot || slot->owner != chart->key) continue;
                floor = source->input_[chart->first].timestamp;
                opening = slot->first < chart->first;
                closing = false;
                active = true;
            }
            if (opening) {
                out = one_price(source->input_[chart->first].open, floor);
                floor = after_stamp(floor);
                opening = false;
            } else if (closing) {
                const Bar& last = source->input_[chart->last];
                out = one_price(last.close, std::max(last.timestamp, floor));
                active = false;
            } else {
                out = slot->value;
                out.timestamp = std::max(out.timestamp, floor);
                floor = after_stamp(out.timestamp);
                const int last = slot->last;
                slot = slots.next();
                if (!slot || slot->owner != chart->key) {
                    closing = last < chart->last;
                    if (!closing) active = false;
                }
            }
            return true;
        }
    };

    bool selected_ = false;
    std::int64_t begin_ = 0, end_ = 0;
    NativeBarView view_{nullptr, 0};
    std::vector<Bar> segment_, scratch_;
    std::unique_ptr<Cursor> cursor_;
    std::optional<Bar> next_;

public:
    MagnifierPathSource(const Bar* input, int n, const std::string& input_tf,
                         const std::string& script_tf, const std::string& session,
                         const std::string& timezone)
        : input_(input), n_(input && n > 0 ? n : 0) {
        count_ = static_cast<std::size_t>(n_);
        if (n_ == 0) return;
        const auto input_grid = cal::parse_timeframe(input_tf);
        chart_tf_ = cal::parse_timeframe(script_tf);
        slot_tf_ = cal::parse_timeframe(tradingview_intrabar_timeframe(script_tf));
        if (!input_grid || !chart_tf_ || !slot_tf_) return;
        const auto input_ms = fixed_ms(*input_grid);
        const auto slot_ms = fixed_ms(*slot_tf_);
        if (input_ms <= 0 || (slot_ms != 0 && (slot_ms <= input_ms || slot_ms % input_ms != 0)))
            return;
        for (int i = 1; i < n_; ++i)
            if (input_[i].timestamp <= input_[i - 1].timestamp) return;
        calendar_ = cal::parse_session(session, timezone);
        if (!calendar_) return;
        // A failure anywhere means today's ENTIRE unchanged input. Decide
        // before the count/digest can emit even one aggregated row.
        Rows rows(this);
        Row row{};
        while (rows.next(row)) {}
        if (rows.failed) return;
        fallback_ = false;
        count_ = 0;
        Cursor cursor(this);
        Bar bar{};
        while (cursor.next(bar)) {
            if (count_ == std::numeric_limits<std::size_t>::max())
                throw coded<std::overflow_error>(RunFailureCode::run_options_rejected,
                                                 {{"option", "intrabar_path"}},
                                                 "magnifier path count exhausted");
            ++count_;
        }
    }

    std::size_t count() const override { return count_; }
    bool for_each(Visitor visitor, void* context) const override {
        if (visitor == nullptr) path_invariant("null magnifier visitor");
        if (fallback_) {
            for (int i = 0; i < n_; ++i) if (!visitor(context, input_[i])) return false;
        } else {
            Cursor cursor(this);
            Bar bar{};
            while (cursor.next(bar)) if (!visitor(context, bar)) return false;
        }
        return true;
    }

    NativeBarView segment(std::int64_t begin, std::int64_t end) override {
        if (end < begin || (selected_ && (begin < begin_ || end < end_)))
            path_invariant("magnifier window moved backwards");
        if (selected_ && begin == begin_ && end == end_) return view_;
        if (fallback_) {
            // Fallback is the input view itself: no path allocation or copy.
            const Bar* first = input_;
            const Bar* last = input_;
            if (n_ != 0) {
                const auto less = [](const Bar& bar, std::int64_t t) { return bar.timestamp < t; };
                first = std::lower_bound(input_, input_ + n_, begin, less);
                last = std::lower_bound(first, input_ + n_, end, less);
            }
            view_ = {first, n_ == 0 ? 0 : static_cast<std::size_t>(last - first)};
            next_ = n_ == 0 || last == input_ + n_ ? std::nullopt : std::optional<Bar>(*last);
        } else {
            // A candidate cursor and second reusable buffer make publication
            // atomic. Keep overlap rows from the previous window, then append
            // only new rows; look-ahead is a distinct single-bar buffer.
            auto candidate = cursor_ ? std::make_unique<Cursor>(*cursor_)
                                     : std::make_unique<Cursor>(this);
            auto next = next_;
            scratch_.clear();
            for (const auto& bar : segment_)
                if (bar.timestamp >= begin) scratch_.push_back(bar);
            Bar bar{};
            for (;;) {
                if (next) { bar = *next; next.reset(); }
                else if (!candidate->next(bar)) break;
                if (bar.timestamp >= end) { next = bar; break; }
                if (bar.timestamp >= begin) scratch_.push_back(bar);
            }
            segment_.swap(scratch_);
            cursor_.swap(candidate);
            next_ = next;
            view_ = {segment_.data(), segment_.size()};
        }
        begin_ = begin;
        end_ = end;
        selected_ = true;
        return view_;
    }
    const Bar* next_after_segment() const noexcept override {
        return next_ ? &*next_ : nullptr;
    }
};

} // namespace

std::unique_ptr<NativeLowerPathSource> make_magnifier_path_source(
        const Bar* bars, int n, const std::string& input_tf,
        const std::string& script_tf, const std::string& session,
        const std::string& timezone) {
    return std::make_unique<MagnifierPathSource>(bars, n, input_tf, script_tf, session, timezone);
}

}  // namespace pineforge::source
