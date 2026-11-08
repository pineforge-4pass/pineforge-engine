#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/report_pages.hpp>
#include <pineforge/trade_ledger.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace pineforge {
namespace report {
namespace trade_source_detail {

// Adapters borrow a frozen generation. Existing owning accessors supply their
// temporary values; no storage pointer or borrowed string escapes a call.
class TradeValueSourceV1 : public TradeSourceV1 {
public:
    TradeRowV1 at(LogicalIndexV1 index) const override {
        check_index(index);
        const auto row = numeric_value(index);
        TradeRowV1 result{};
        auto& value = result.value;
        value.entry_time = row.entry_time;
        value.exit_time = row.exit_time;
        value.entry_price = row.entry_price;
        value.exit_price = row.exit_price;
        value.pnl = row.pnl;
        value.pnl_pct = row.pnl_pct;
        value.is_long = row.is_long ? 1 : 0;
        value.max_runup = row.max_runup;
        value.max_drawdown = row.max_drawdown;
        value.qty = row.qty;
        value.commission = row.commission;
        value.entry_bar_index = row.entry_bar_index;
        value.exit_bar_index = row.exit_bar_index;
        value.open_at_end = row.open_at_end ? 1 : 0;
        result.entry_incarnation = row.entry_incarnation;
        result.close_cause = BacktestEngine::classify_report_close_cause(row);
        result.exit_from_bracket = row.exit_from_bracket ? 1 : 0;
        return result;
    }

    std::size_t read_page(LogicalIndexV1 from, TradeRowV1* out,
                          std::size_t capacity) const override {
        check_range(from, 0);
        if (capacity && !out) throw std::invalid_argument("trade page buffer");
        const auto count = static_cast<std::size_t>(
            std::min<LogicalIndexV1>(capacity, size() - from));
        for (std::size_t i = 0; i < count; ++i) out[i] = at(from + i);
        return count;
    }

    VisitResultV1 visit(LogicalIndexV1 from, LogicalIndexV1 count,
                        void* context, TradeVisitorV1 visitor) const override {
        return visit_values(from, count, context, visitor, false);
    }

    VisitResultV1 visit_backward(LogicalIndexV1 from, LogicalIndexV1 count,
                                 void* context, TradeVisitorV1 visitor) const override {
        return visit_values(from, count, context, visitor, true);
    }

    std::uint64_t text_size(LogicalIndexV1 index, TradeTextV1 field) const override {
        check_text(index, field);
        return text_value(index, field).size();
    }

    std::size_t read_text(LogicalIndexV1 index, TradeTextV1 field,
                          std::uint64_t byte_offset, char* out,
                          std::size_t capacity) const override {
        check_text(index, field);
        const auto text = text_value(index, field);
        if (byte_offset > text.size()) throw std::out_of_range("trade text offset");
        if (capacity && !out) throw std::invalid_argument("trade text buffer");
        const auto offset = static_cast<std::size_t>(byte_offset);
        const auto count = std::min(capacity, text.size() - offset);
        if (count) std::memcpy(out, text.data() + offset, count);
        return count;
    }

protected:
    virtual Trade numeric_value(LogicalIndexV1 index) const = 0;
    virtual std::string text_value(LogicalIndexV1 index, TradeTextV1 field) const = 0;

private:
    void check_index(LogicalIndexV1 index) const {
        if (index >= size()) throw std::out_of_range("trade source index");
    }
    void check_range(LogicalIndexV1 from, LogicalIndexV1 count) const {
        const auto length = size();
        if (from > length || count > length - from)
            throw std::out_of_range("trade source range");
    }
    void check_text(LogicalIndexV1 index, TradeTextV1 field) const {
        check_index(index);
        if (static_cast<std::uint32_t>(field) > 3)
            throw std::invalid_argument("trade text selector");
    }
    VisitResultV1 visit_values(LogicalIndexV1 from, LogicalIndexV1 count,
                               void* context, TradeVisitorV1 visitor, bool reverse) const {
        check_range(from, count);
        if (!visitor) throw std::invalid_argument("trade source visitor");
        VisitResultV1 result{};
        while (result.visited < count) {
            const auto index = reverse ? from + (count - result.visited - 1)
                                       : from + result.visited;
            const auto row = at(index);
            ++result.visited;
            if (!visitor(context, index, row)) return result;
        }
        result.complete = true;
        return result;
    }
};

} // namespace trade_source_detail

// No inheritance or new data member is added to the installed ledger object.
// Reads below a physically retired boundary retain the ledger's own refusal.
class LedgerTradeSourceV1 final : public trade_source_detail::TradeValueSourceV1 {
public:
    explicit LedgerTradeSourceV1(const TradeLedger& ledger) : ledger_(ledger) {}
    LogicalIndexV1 size() const override { return ledger_.size(); }

private:
    Trade numeric_value(LogicalIndexV1 index) const override {
        return ledger_.numeric_at(static_cast<std::size_t>(index));
    }
    std::string text_value(LogicalIndexV1 index, TradeTextV1 field) const override {
        const auto row = static_cast<std::size_t>(index);
        switch (field) {
        case TradeTextV1::EntryId: return ledger_.entry_id_at(row);
        case TradeTextV1::EntryComment: return ledger_.entry_comment_at(row);
        case TradeTextV1::ExitId: return ledger_.exit_id_at(row);
        case TradeTextV1::ExitComment: return ledger_.exit_comment_at(row);
        }
        throw std::invalid_argument("trade text selector");
    }
    const TradeLedger& ledger_;
};

// Finished report order includes the report-only range-end suffix. The engine
// must outlive this adapter and remain unmodified for its entire use.
class EngineReportTradeSourceV1 final : public trade_source_detail::TradeValueSourceV1 {
public:
    explicit EngineReportTradeSourceV1(const BacktestEngine& engine) : engine_(engine) {}
    LogicalIndexV1 size() const override { return engine_.report_trade_count(); }

private:
    Trade numeric_value(LogicalIndexV1 index) const override {
        return engine_.get_report_trade(static_cast<int>(index));
    }
    std::string text_value(LogicalIndexV1 index, TradeTextV1 field) const override {
        auto row = engine_.get_report_trade(static_cast<int>(index));
        switch (field) {
        case TradeTextV1::EntryId: return std::move(row.entry_id);
        case TradeTextV1::EntryComment: return std::move(row.entry_comment);
        case TradeTextV1::ExitId: return std::move(row.exit_id);
        case TradeTextV1::ExitComment: return std::move(row.exit_comment);
        }
        throw std::invalid_argument("trade text selector");
    }
    const BacktestEngine& engine_;
};

} // namespace report
} // namespace pineforge
