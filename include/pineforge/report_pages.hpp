#pragma once

// O1 report contract v1: C++ reader declarations, not a support probe.
// Modes/metadata remain in report_mode.h; see docs/design/o1-report-contract.md.
// Ledger and metrics lanes supply the providers, with no storage defined here.
#include <pineforge/report_mode.h>

#include <cstddef>
#include <cstdint>

namespace pineforge {
namespace report {

using LogicalIndexV1 = std::uint64_t;

// Text is transferred separately, so an arbitrarily long ID/comment does not
// force a row or a bounded page to own an equally large string.
enum class TradeTextV1 : std::uint32_t {
    EntryId = 0,
    EntryComment = 1,
    ExitId = 2,
    ExitComment = 3,
};

// Detached numeric value: preserve every field/float bit and the existing
// close-cause classification. exit_from_bracket is the separate raw 0/1 label.
struct TradeRowV1 {
    pf_trade_t value{};
    std::uint64_t entry_incarnation = 0;
    std::int32_t close_cause = 0; // pf_close_cause_t values
    std::uint32_t exit_from_bracket = 0;
};

struct VisitResultV1 {
    LogicalIndexV1 visited = 0;
    bool complete = false;
};

// Values, including callback arguments, never alias a refillable/retired page.
using TradeVisitorV1 = bool (*)(void*, LogicalIndexV1, TradeRowV1);
using EquityVisitorV1 = bool (*)(void*, LogicalIndexV1, pf_equity_point_t);

// A single frozen final presented point, also available in SUMMARY_ONLY.
// present is independent of sequence capture and does not certify completion.
// If present, index == logical point count - 1; otherwise index/value are zero.
// During execution the metrics owner holds an editable last point and its
// pre-tail fold snapshot, finalizing it only after presentation is settled.
struct HeldEquityPointV1 {
    LogicalIndexV1 index = 0;
    pf_equity_point_t value{};
    bool present = false;
};

// One source describes a fixed logical generation. Engine accessors return
// caller-owned adapters, not pointers/references to rows or resident pages.
// Their engine context is borrowed: use only until the next execution/config
// mutation and while the engine lives. Read-cache refills do not change the
// logical generation. Completed history is readable at a committed
// script callback boundary; report sources describe a completed frozen report.
// Never mutate/run the engine inside a visitor. Provider-owned pages may be
// retired/refilled between calls; detached values and caller buffers survive.
//
// size() counts ALL logical rows, including retired history. at(index) returns
// a value for index < size(). read_page(from,out,capacity) copies ascending
// indices, at most min(capacity,size()-from) rows. from==size() and capacity==0
// are valid. nullptr is valid only with capacity==0, even at the sequence end.
//
// Both visits select [from,from+count). visit() walks ascending indices;
// visit_backward() walks from+count-1 down to from, newest logical row first,
// never sorted by exit_time (including ties and nonmonotonic times). Each is
// ONE pass, without restarting execution per row or accumulating a row list.
// A false callback stops after that row: visited includes it and complete is
// false, even on the final row. A consumed range, including empty, is complete.
//
// Validate ranges without overflow: from<=size(), count<=size()-from. Invalid
// indices/ranges throw std::out_of_range; invalid buffers, text selectors or
// null visitors throw std::invalid_argument, including on empty visits.
// Validate before copying/callbacks. Context may be null. Backing/visitor
// failures propagate; they never become EOF, an empty source or a success.
// Capacity is a transfer quantum, never an admission/capacity refusal.
class TradeSourceV1 {
public:
    virtual ~TradeSourceV1() = default;
    virtual LogicalIndexV1 size() const = 0;
    virtual TradeRowV1 at(LogicalIndexV1 index) const = 0;
    virtual std::size_t read_page(LogicalIndexV1 from, TradeRowV1* out,
                                  std::size_t capacity) const = 0;
    virtual VisitResultV1 visit(LogicalIndexV1 from, LogicalIndexV1 count,
                                void* context, TradeVisitorV1 visitor) const = 0;
    virtual VisitResultV1 visit_backward(LogicalIndexV1 from, LogicalIndexV1 count,
                                         void* context, TradeVisitorV1 visitor) const = 0;
    // Exact stored text bytes, no normalization or appended NUL. Offsets and
    // lengths are bytes; a chunk may split UTF-8. Chunks belong to the caller.
    // Same end/buffer rules as read_page; byte_offset>text_size(index,field) throws.
    virtual std::uint64_t text_size(LogicalIndexV1 index, TradeTextV1 field) const = 0;
    virtual std::size_t read_text(LogicalIndexV1 index, TradeTextV1 field,
                                  std::uint64_t byte_offset, char* out,
                                  std::size_t capacity) const = 0;
};

// Same by-value/index/page/forward-visit/error rules as TradeSourceV1. Points
// are final presented values in report order, including a selected anchor.
// A source supports repeated ordered passes through bounded buffers, never an
// execution restart per at(). SUMMARY_ONLY has no captured point source:
// the metrics lane instead feeds final values to EquityVisitorV1 during its
// fixed ordered passes and keeps only the held point/folds/replay witnesses.
class EquityPointSourceV1 {
public:
    virtual ~EquityPointSourceV1() = default;
    virtual LogicalIndexV1 size() const = 0;
    virtual pf_equity_point_t at(LogicalIndexV1 index) const = 0;
    virtual std::size_t read_page(LogicalIndexV1 from, pf_equity_point_t* out,
                                  std::size_t capacity) const = 0;
    virtual VisitResultV1 visit(LogicalIndexV1 from, LogicalIndexV1 count,
                                void* context, EquityVisitorV1 visitor) const = 0;
};

} // namespace report
} // namespace pineforge
