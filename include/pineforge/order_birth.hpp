#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace pineforge {

// Geometry supplied by the dispatcher, never inferred from equal OHLC prices.
enum class BirthCursorDomain : int32_t { None, HistoricalPath, MagnifierTicks };
enum class BirthCursorPosition : int32_t { None, Point, Segment };
enum class OrderBirthCause : int32_t {
    Unattributed,       // manually constructed records; no event is asserted
    DirectCommand,     // native API call outside a chart evaluation
    ChartEvaluation,
    FillEvaluation,
};

namespace detail {
// Shared by the throwing factories and the non-allocating journal scan.
inline bool valid_birth_cursor(BirthCursorDomain domain, bool segment, int index, int count) noexcept {
    return (domain == BirthCursorDomain::HistoricalPath || domain == BirthCursorDomain::MagnifierTicks)
        && count > 0 && (domain != BirthCursorDomain::HistoricalPath || count == 4)
        && index >= 0 && index < count && (!segment || index + 1 < count);
}
inline bool valid_fill_evaluation(int bar, BirthCursorDomain domain, double price, uint64_t first_fill,
                                  uint64_t last_fill, uint64_t evaluation_ordinal) noexcept {
    return bar >= 0 && domain != BirthCursorDomain::None && std::isfinite(price)
        && first_fill != 0 && last_fill >= first_fill && evaluation_ordinal != 0;
}
} // namespace detail

class BirthCursor {
public:
    BirthCursor() = default;
    static BirthCursor point(BirthCursorDomain domain, int index, int count) {
        return make(domain, BirthCursorPosition::Point, index, count);
    }
    static BirthCursor segment(BirthCursorDomain domain, int index, int count) {
        return make(domain, BirthCursorPosition::Segment, index, count);
    }
    BirthCursorDomain domain() const { return domain_; }
    BirthCursorPosition position() const { return position_; }
    int index() const { return index_; }
    int count() const { return count_; }
    bool first_point() const { return position_ == BirthCursorPosition::Point && index_ == 0; }
    bool terminal_point() const {
        return position_ == BirthCursorPosition::Point && index_ + 1 == count_;
    }
    int following_segment() const { return terminal_point() ? -1 : index_; }
private:
    static BirthCursor make(BirthCursorDomain domain, BirthCursorPosition position,
                            int index, int count) {
        if (!detail::valid_birth_cursor(domain, position == BirthCursorPosition::Segment, index, count))
            throw std::invalid_argument("invalid order-birth dispatch cursor");
        BirthCursor out;
        out.domain_ = domain; out.position_ = position;
        out.index_ = index; out.count_ = count;
        return out;
    }
    BirthCursorDomain domain_ = BirthCursorDomain::None;
    BirthCursorPosition position_ = BirthCursorPosition::None;
    int index_ = -1;
    int count_ = 0;
};

// Immutable value: fields have no setters. Replacement constructs a new value;
// copy/move of an order preserves the complete receipt. A grouped callback may
// cover an actual contiguous fill-event interval; it does not invent one fill.
class OrderBirth {
public:
    OrderBirth() = default;
    static OrderBirth direct_command(int bar, int64_t timestamp) {
        return ordinary(OrderBirthCause::DirectCommand, bar, timestamp);
    }
    static OrderBirth chart_evaluation(int bar, int64_t timestamp) {
        return ordinary(OrderBirthCause::ChartEvaluation, bar, timestamp);
    }
    static OrderBirth fill_evaluation(int bar, int64_t timestamp, BirthCursor cursor,
                                      double price, uint64_t first_fill,
                                      uint64_t last_fill, uint64_t evaluation_ordinal) {
        if (!detail::valid_fill_evaluation(bar, cursor.domain(), price, first_fill, last_fill, evaluation_ordinal))
            throw std::invalid_argument("invalid order-birth fill evaluation");
        OrderBirth out = ordinary(OrderBirthCause::FillEvaluation, bar, timestamp);
        out.cursor_ = cursor; out.cursor_price_ = price;
        out.first_fill_ = first_fill; out.last_fill_ = last_fill;
        out.evaluation_ordinal_ = evaluation_ordinal;
        return out;
    }
    OrderBirthCause cause() const { return cause_; }
    bool from_fill() const { return cause_ == OrderBirthCause::FillEvaluation; }
    bool at_terminal_fill() const { return from_fill() && cursor_.terminal_point(); }
    int bar() const { return bar_; }
    int64_t timestamp() const { return timestamp_; }
    const BirthCursor& cursor() const { return cursor_; }
    double cursor_price() const { return cursor_price_; }
    uint64_t first_fill() const { return first_fill_; }
    uint64_t last_fill() const { return last_fill_; }
    uint64_t evaluation_ordinal() const { return evaluation_ordinal_; }
private:
    static OrderBirth ordinary(OrderBirthCause cause, int bar, int64_t timestamp) {
        OrderBirth out;
        out.cause_ = cause; out.bar_ = bar; out.timestamp_ = timestamp;
        return out;
    }
    OrderBirthCause cause_ = OrderBirthCause::Unattributed;
    int bar_ = -1;
    int64_t timestamp_ = 0;
    BirthCursor cursor_;
    double cursor_price_ = std::numeric_limits<double>::quiet_NaN();
    uint64_t first_fill_ = 0;
    uint64_t last_fill_ = 0;
    uint64_t evaluation_ordinal_ = 0;
};

} // namespace pineforge
