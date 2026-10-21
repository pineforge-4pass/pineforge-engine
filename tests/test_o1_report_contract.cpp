// O1W B3: a populated real strategy and a deliberately legacy provider.
// The provider export map keeps the old surface even after runtime mode
// support lands. It does not implement a mock setter or a report oracle.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
class ContractFixture final : public pineforge::source::PineStrategyHost {
public:
    ContractFixture() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 1.0;
        config.commission_value = 0.125;
        configure_pine_strategy(config);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        if (const char* path = std::getenv("PINEFORGE_O1_EXECUTION_MARKER")) {
            std::ofstream marker(path, std::ios::app);
            marker << bar.timestamp << '\n';
            if (!marker) throw std::runtime_error("cannot write contract execution witness");
        }
        if (bar_index_ == 0) strategy_entry("L1", true);
        if (bar_index_ == 2) strategy_entry("S", false);
        if (bar_index_ == 4) strategy_entry("L2", true);
    }
};
} // namespace

#ifdef PINEFORGE_O1_LEGACY_PROVIDER
namespace {
pineforge::BacktestEngine* engine(pf_strategy_t handle) {
    return static_cast<pineforge::BacktestEngine*>(handle);
}
} // namespace

extern "C" {
pf_strategy_t strategy_create(const char*) {
    return static_cast<pineforge::BacktestEngine*>(new ContractFixture);
}
void strategy_free(pf_strategy_t handle) { delete engine(handle); }
void strategy_set_input(pf_strategy_t, const char*, const char*) {}
void strategy_set_override(pf_strategy_t, const char*, const char*) {}
void run_backtest_full(pf_strategy_t handle, pf_bar_t* bars, int n,
                       const char* input_tf, const char* script_tf, int magnifier,
                       int samples, pf_magnifier_distribution_t distribution,
                       pf_report_t* out) {
    *out = {};
    try {
        std::vector<pineforge::Bar> input;
        for (int i = 0; i < n; ++i) {
            input.push_back({bars[i].open, bars[i].high, bars[i].low, bars[i].close,
                             bars[i].volume, bars[i].timestamp});
        }
        engine(handle)->run(input.data(), n, input_tf, script_tf, magnifier != 0,
                            samples, static_cast<pineforge::MagnifierDistribution>(distribution));
        engine(handle)->fill_report(reinterpret_cast<pineforge::ReportC*>(out));
    } catch (const std::exception& error) {
        pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(out));
        *out = {};
        pineforge::note_run_failure(*engine(handle), "run_backtest_full", error);
    }
}
void run_backtest(pf_strategy_t handle, pf_bar_t* bars, int n, pf_report_t* out) {
    run_backtest_full(handle, bars, n, "1", "1", 0, 4, PF_MAGNIFIER_ENDPOINTS, out);
}
void report_free(pf_report_t* report) {
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(report));
}
} // extern "C"

#else
#if __has_include(<pineforge/report_mode.h>)
#include <pineforge/report_mode.h>
#else
#error "unsupported report mode: SUMMARY_ONLY (pineforge-report/v1 declarations absent)"
#endif
#if __has_include(<pineforge/report_pages.hpp>)
#include <pineforge/report_pages.hpp>
#else
#error "O1 report visitor declarations absent (round-2 report_pages.hpp required)"
#endif
#include <pineforge/metrics_stream.hpp>
#include <type_traits>

static_assert(PF_ABI_VERSION == 4, "the legacy caller-allocated report must not grow");
static_assert(sizeof(pf_metrics_t) == 768, "the complete metrics block is unchanged");
static_assert(std::is_standard_layout<pf_report_envelope_v1>::value,
              "the envelope must remain usable by C consumers");
static_assert(std::is_same<decltype(&pineforge::BacktestEngine::set_report_mode_v1),
                          int (pineforge::BacktestEngine::*)(pineforge::ReportMode)>::value,
              "mode selection must be explicit and return a status");
static_assert(std::is_same<decltype(&pineforge::BacktestEngine::fill_report_envelope_v1),
                          int (pineforge::BacktestEngine::*)(pineforge::ReportEnvelopeV1*) const>::value,
              "the versioned read must use its own caller-allocated envelope");
static_assert(std::is_same<pineforge::report::TradeVisitorV1,
                          bool (*)(void*, std::uint64_t, pineforge::report::TradeRowV1)>::value,
              "trade callbacks must receive detached row values");
static_assert(std::is_same<pineforge::report::EquityVisitorV1,
                          bool (*)(void*, std::uint64_t, pf_equity_point_t)>::value,
              "point callbacks must receive detached point values");
static_assert(std::is_same<decltype(std::declval<const pineforge::report::TradeSourceV1&>().at(0)),
                          pineforge::report::TradeRowV1>::value,
              "indexed trade reads must return values");
static_assert(std::is_same<decltype(std::declval<const pineforge::report::EquityPointSourceV1&>().at(0)),
                          pf_equity_point_t>::value,
              "indexed point reads must return values");
static_assert(std::is_same<decltype(&pineforge::BacktestEngine::completed_trade_source_v1),
                          std::unique_ptr<const pineforge::report::TradeSourceV1>
                              (pineforge::BacktestEngine::*)() const>::value,
              "completed history must use the single value/page/visitor family");
static_assert(std::is_same<decltype(&pineforge::BacktestEngine::report_trade_source_v1),
                          std::unique_ptr<const pineforge::report::TradeSourceV1>
                              (pineforge::BacktestEngine::*)() const>::value,
              "report rows must use the same reader family");
static_assert(std::is_same<decltype(&pineforge::BacktestEngine::report_equity_source_v1),
                          std::unique_ptr<const pineforge::report::EquityPointSourceV1>
                              (pineforge::BacktestEngine::*)() const>::value,
              "report points must use the single point reader family");
static_assert(std::is_same<decltype(&pineforge::BacktestEngine::held_report_equity_point_v1),
                          pineforge::report::HeldEquityPointV1
                              (pineforge::BacktestEngine::*)() const>::value,
              "summary tail access must return one detached point");

using PointStatsV1 = pf_equity_stats_t (*)(
    const pineforge::report::EquityPointSourceV1&, std::int64_t, double,
    const std::string&, double, double, std::int64_t, double);
using SelectedPointStatsV1 = pf_equity_stats_t (*)(
    const pineforge::report::EquityPointSourceV1&, std::int64_t, double,
    const std::string&, double, double, std::int64_t, double, std::int64_t);
static_assert(std::is_same<decltype(static_cast<PointStatsV1>(
                              &pineforge::metrics::compute_equity_stats)), PointStatsV1>::value,
              "ordinary metrics must accept the report point provider");
static_assert(std::is_same<decltype(static_cast<SelectedPointStatsV1>(
                              &pineforge::metrics::compute_equity_stats)), SelectedPointStatsV1>::value,
              "selected metrics must keep observations separate from points");

namespace {
using namespace pineforge::report;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Error, class Operation>
void require_throws(Operation operation, const char* message) {
    try {
        operation();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error(message);
}

void check_range(LogicalIndexV1 size, LogicalIndexV1 from, LogicalIndexV1 count) {
    if (from > size || count > size - from) throw std::out_of_range("mock source range");
}

// Vector-backed TEST ONLY providers exercise the public interface. They make
// no claim about the ledger/metrics implementations or their resident bound.
template <class Row>
std::size_t copy_page(const std::vector<Row>& rows, LogicalIndexV1 from,
                      Row* out, std::size_t capacity) {
    check_range(rows.size(), from, 0);
    if (capacity && !out) throw std::invalid_argument("mock page buffer");
    const auto offset = static_cast<std::size_t>(from);
    const auto n = std::min(capacity, rows.size() - offset);
    if (n) std::copy_n(rows.begin() + offset, n, out);
    return n;
}

template <class Row, class Visitor>
VisitResultV1 visit_values(const std::vector<Row>& rows, LogicalIndexV1 from,
                           LogicalIndexV1 count, void* context, Visitor visitor,
                           bool backward = false) {
    check_range(rows.size(), from, count);
    if (!visitor) throw std::invalid_argument("mock null visitor");
    VisitResultV1 result{};
    for (LogicalIndexV1 offset = 0; offset < count; ++offset) {
        const auto index = backward ? from + (count - 1 - offset) : from + offset;
        ++result.visited;
        if (!visitor(context, index, rows[static_cast<std::size_t>(index)])) return result;
    }
    result.complete = true;
    return result;
}

class MockTradeSource final : public TradeSourceV1 {
public:
    explicit MockTradeSource(std::vector<TradeRowV1> rows) : rows_(std::move(rows)) {}
    LogicalIndexV1 size() const override { return rows_.size(); }
    TradeRowV1 at(LogicalIndexV1 index) const override {
        if (index >= size()) throw std::out_of_range("mock trade index");
        return rows_[static_cast<std::size_t>(index)];
    }
    std::size_t read_page(LogicalIndexV1 from, TradeRowV1* out,
                          std::size_t capacity) const override {
        return copy_page(rows_, from, out, capacity);
    }
    VisitResultV1 visit(LogicalIndexV1 from, LogicalIndexV1 count,
                        void* context, TradeVisitorV1 visitor) const override {
        return visit_values(rows_, from, count, context, visitor);
    }
    VisitResultV1 visit_backward(LogicalIndexV1 from, LogicalIndexV1 count,
                                 void* context, TradeVisitorV1 visitor) const override {
        return visit_values(rows_, from, count, context, visitor, true);
    }
    std::uint64_t text_size(LogicalIndexV1 index, TradeTextV1 field) const override {
        validate_text(index, field);
        return texts_[static_cast<std::size_t>(field)].size();
    }
    std::size_t read_text(LogicalIndexV1 index, TradeTextV1 field,
                          std::uint64_t byte_offset, char* out,
                          std::size_t capacity) const override {
        const auto length = text_size(index, field);
        if (byte_offset > length) throw std::out_of_range("mock text offset");
        if (capacity && !out) throw std::invalid_argument("mock text buffer");
        const auto& text = texts_[static_cast<std::size_t>(field)];
        const auto offset = static_cast<std::size_t>(byte_offset);
        const auto n = std::min(capacity, text.size() - offset);
        if (n) std::memcpy(out, text.data() + offset, n);
        return n;
    }
private:
    void validate_text(LogicalIndexV1 index, TradeTextV1 field) const {
        if (index >= size()) throw std::out_of_range("mock text index");
        if (static_cast<std::uint32_t>(field) >= texts_.size())
            throw std::invalid_argument("mock text selector");
    }
    std::vector<TradeRowV1> rows_;
    const std::array<std::string, 4> texts_{{std::string("e\0id", 4), "entry", "exit", "done"}};
};

class MockPointSource final : public EquityPointSourceV1 {
public:
    explicit MockPointSource(std::vector<pf_equity_point_t> rows) : rows_(std::move(rows)) {}
    LogicalIndexV1 size() const override { return rows_.size(); }
    pf_equity_point_t at(LogicalIndexV1 index) const override {
        if (index >= size()) throw std::out_of_range("mock point index");
        return rows_[static_cast<std::size_t>(index)];
    }
    std::size_t read_page(LogicalIndexV1 from, pf_equity_point_t* out,
                          std::size_t capacity) const override {
        return copy_page(rows_, from, out, capacity);
    }
    VisitResultV1 visit(LogicalIndexV1 from, LogicalIndexV1 count,
                        void* context, EquityVisitorV1 visitor) const override {
        return visit_values(rows_, from, count, context, visitor);
    }
private:
    std::vector<pf_equity_point_t> rows_;
};

double from_bits(std::uint64_t bits) {
    double value;
    static_assert(sizeof(value) == sizeof(bits), "fixture requires binary64");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void same_double(double actual, double expected) {
    require(std::memcmp(&actual, &expected, sizeof(double)) == 0,
            "reader must preserve every double bit, including NaNs and signed zero");
}

void same_trade(TradeRowV1 actual, TradeRowV1 expected) {
    require(actual.entry_incarnation == expected.entry_incarnation
                && actual.close_cause == expected.close_cause
                && actual.exit_from_bracket == expected.exit_from_bracket,
            "trade identity/close metadata must be exact");
    const auto& a = actual.value;
    const auto& e = expected.value;
    require(a.entry_time == e.entry_time && a.exit_time == e.exit_time
                && a.is_long == e.is_long && a.entry_bar_index == e.entry_bar_index
                && a.exit_bar_index == e.exit_bar_index && a.open_at_end == e.open_at_end,
            "trade coordinates/flags must be exact");
    same_double(a.entry_price, e.entry_price);
    same_double(a.exit_price, e.exit_price);
    same_double(a.pnl, e.pnl);
    same_double(a.pnl_pct, e.pnl_pct);
    same_double(a.max_runup, e.max_runup);
    same_double(a.max_drawdown, e.max_drawdown);
    same_double(a.qty, e.qty);
    same_double(a.commission, e.commission);
}

void same_point(pf_equity_point_t actual, pf_equity_point_t expected) {
    require(actual.time_ms == expected.time_ms, "point coordinate must be exact");
    same_double(actual.equity, expected.equity);
    same_double(actual.open_profit, expected.open_profit);
}

struct TradeVisit {
    std::vector<LogicalIndexV1> indices;
    std::vector<TradeRowV1> rows;
    std::size_t stop_after = std::numeric_limits<std::size_t>::max();
    double pnl = 0.0;
};

bool collect_trade(void* context, LogicalIndexV1 index, TradeRowV1 row) {
    auto& seen = *static_cast<TradeVisit*>(context);
    seen.indices.push_back(index);
    seen.rows.push_back(row);
    seen.pnl += row.value.pnl;
    row.value.pnl = 42.0; // Changing the callback's value cannot edit the source.
    return seen.indices.size() < seen.stop_after;
}

void check_trade_visit(const TradeVisit& seen, const std::vector<LogicalIndexV1>& indices,
                        const std::vector<TradeRowV1>& expected) {
    require(seen.indices == indices, "trade visit must preserve logical index order");
    require(seen.rows.size() == indices.size(), "every visited trade must be delivered");
    for (std::size_t i = 0; i < indices.size(); ++i) same_trade(seen.rows[i], expected[indices[i]]);
}

void check_trade_readers() {
    // Exit times tie AND go backwards; order is the logical row order.
    // Opposite large PnLs make ascending and descending folds distinguishable.
    const std::vector<TradeRowV1> expected{
        {{11, 30, 100.25, 101.5, 1e16, -0.0, 1, 4.25, -2.5, 1.25, .125, 1, 4, 0},
         UINT64_C(0x100000001), PF_CLOSE_CAUSE_SCRIPT, 0},
        {{12, 50, 102.5, 99.25, -1e16, from_bits(UINT64_C(0x7ff8000000000123)),
          0, 8.5, -3.75, 2.5, .25, 2, 8, 0},
         UINT64_C(0x200000002), PF_CLOSE_CAUSE_LIQUIDATION, 1},
        {{13, 50, 98.75, 100.5, 1.0, .5, 1, 1.25, -0.0, 3.75, .5, 3, 8, 0},
         UINT64_C(0x300000003), PF_CLOSE_CAUSE_BRACKET, 1},
        {{14, 40, 101.5, 101.5, 0.0, -0.0, 0, 2.5, -1.25, 5.0, 1.0, 4, 7, 0},
         UINT64_C(0x400000004), PF_CLOSE_CAUSE_RISK_LIMIT, 0}
    };
    MockTradeSource provider(expected);
    const TradeSourceV1& source = provider;
    require(source.size() == 4, "mock must contain four completed rows before reader checks");
    for (LogicalIndexV1 i = 0; i < source.size(); ++i) same_trade(source.at(i), expected[i]);
    auto detached = source.at(0);
    detached.value.pnl = 7.0;
    same_double(detached.value.pnl, 7.0);
    same_trade(source.at(0), expected[0]);

    TradeVisit forward, backward;
    auto result = source.visit(0, 4, &forward, collect_trade);
    require(result.complete && result.visited == 4, "forward pass must consume four rows");
    check_trade_visit(forward, {0, 1, 2, 3}, expected);
    result = source.visit_backward(0, 4, &backward, collect_trade);
    require(result.complete && result.visited == 4, "backward pass must consume four rows");
    check_trade_visit(backward, {3, 2, 1, 0}, expected);
    same_double(forward.pnl, 1.0);
    same_double(backward.pnl, 0.0);
    same_trade(source.at(0), expected[0]);

    TradeVisit middle;
    result = source.visit_backward(1, 2, &middle, collect_trade);
    require(result.complete && result.visited == 2, "backward subrange must consume two rows");
    check_trade_visit(middle, {2, 1}, expected);
    TradeVisit stopped;
    stopped.stop_after = 2;
    result = source.visit(1, 2, &stopped, collect_trade);
    require(!result.complete && result.visited == 2, "false on the final row still means stopped");
    check_trade_visit(stopped, {1, 2}, expected);
    TradeVisit barrier;
    result = source.visit_backward(0, 4, &barrier,
        [](void* context, LogicalIndexV1 index, TradeRowV1 row) {
            collect_trade(context, index, row);
            return row.value.exit_time >= 45;
        });
    require(!result.complete && result.visited == 1, "tail barrier must stop on the newest row");
    check_trade_visit(barrier, {3}, expected); // Do not skip to older rows with later times.

    std::array<TradeRowV1, 3> page{};
    require(source.read_page(1, page.data(), 2) == 2, "bounded trade page must copy two rows");
    same_trade(page[0], expected[1]);
    same_trade(page[1], expected[2]);
    same_trade(page[2], TradeRowV1{});
    require(source.read_page(3, page.data(), page.size()) == 1, "last trade page must be short");
    same_trade(page[0], expected[3]);
    require(source.read_page(4, page.data(), 1) == 0, "page at logical end must be empty");
    require(source.read_page(4, nullptr, 0) == 0, "zero-capacity page permits a null buffer");
    TradeVisit empty;
    for (bool reverse : {false, true}) {
        result = reverse ? source.visit_backward(4, 0, &empty, collect_trade)
                         : source.visit(4, 0, &empty, collect_trade);
        require(result.complete && result.visited == 0, "empty range must be fully consumed");
    }
    require(empty.indices.empty(), "empty visits must call no visitor");
    const auto huge = std::numeric_limits<LogicalIndexV1>::max();
    require_throws<std::out_of_range>([&] { source.at(4); }, "at(size) must throw");
    require_throws<std::out_of_range>([&] { source.at(huge); }, "huge index must throw");
    require_throws<std::out_of_range>(
        [&] { source.read_page(5, page.data(), 1); }, "page beyond end must throw");
    require_throws<std::invalid_argument>(
        [&] { source.read_page(4, nullptr, 1); }, "null positive-capacity buffer must throw");
    for (bool reverse : {false, true}) {
        auto visit = [&](LogicalIndexV1 from, LogicalIndexV1 count, TradeVisitorV1 visitor) {
            return reverse ? source.visit_backward(from, count, &empty, visitor)
                           : source.visit(from, count, &empty, visitor);
        };
        require_throws<std::out_of_range>(
            [&] { visit(5, 0, collect_trade); }, "range start beyond end must throw");
        require_throws<std::out_of_range>(
            [&] { visit(3, 2, collect_trade); }, "range beyond end must throw");
        require_throws<std::out_of_range>(
            [&] { visit(1, huge, collect_trade); }, "overflowing range must throw");
        require_throws<std::invalid_argument>(
            [&] { visit(4, 0, nullptr); }, "null visitor must throw even on an empty range");
    }
    require(empty.indices.empty(), "invalid ranges must fail before invoking callbacks");
    MockTradeSource no_rows(std::vector<TradeRowV1>{});
    require(no_rows.size() == 0, "empty captured provider must explicitly have zero rows");
    result = no_rows.visit_backward(0, 0, &empty, collect_trade);
    require(result.complete && result.visited == 0, "empty provider has a valid backward pass");
    require(no_rows.read_page(0, nullptr, 0) == 0, "empty provider has a valid empty page");
    require_throws<std::out_of_range>([&] { no_rows.at(0); }, "empty provider has no row zero");
    require_throws<std::runtime_error>([&] {
        source.visit_backward(0, 4, nullptr,
            [](void*, LogicalIndexV1, TradeRowV1) -> bool {
                throw std::runtime_error("visitor failed");
            });
    }, "callback failure must propagate");

    std::array<char, 5> text{{'?', '?', '?', '?', '?'}};
    require(source.text_size(0, TradeTextV1::EntryId) == 4, "text byte count includes embedded NUL");
    require(source.read_text(0, TradeTextV1::EntryId, 0, text.data(), 2) == 2,
            "first bounded text chunk must contain two bytes");
    require(source.read_text(0, TradeTextV1::EntryId, 2, text.data() + 2, 2) == 2,
            "second bounded text chunk must contain two bytes");
    require(std::memcmp(text.data(), "e\0id", 4) == 0 && text[4] == '?',
            "text reads must preserve exact bytes and append no terminator");
    require(source.read_text(0, TradeTextV1::EntryId, 4, nullptr, 0) == 0,
            "text read at logical end must be empty");
    require_throws<std::out_of_range>(
        [&] { source.read_text(0, TradeTextV1::EntryId, 5, text.data(), 1); },
        "text offset beyond end must throw");
    require_throws<std::out_of_range>(
        [&] { source.text_size(4, TradeTextV1::EntryId); }, "text row beyond end must throw");
    require_throws<std::invalid_argument>(
        [&] { source.text_size(0, static_cast<TradeTextV1>(99)); }, "unknown text field must throw");
    require_throws<std::invalid_argument>(
        [&] { source.read_text(0, TradeTextV1::EntryId, 0, nullptr, 1); },
        "null positive-capacity text buffer must throw");
    same_trade(backward.rows[0], expected[3]); // Earlier values survive subsequent page/text reads.
}

struct PointVisit {
    std::vector<LogicalIndexV1> indices;
    std::vector<pf_equity_point_t> rows;
    bool keep_going = true;
};

bool collect_point(void* context, LogicalIndexV1 index, pf_equity_point_t point) {
    auto& seen = *static_cast<PointVisit*>(context);
    seen.indices.push_back(index);
    seen.rows.push_back(point);
    return seen.keep_going;
}

void check_point_readers() {
    const std::vector<pf_equity_point_t> expected{
        {100, 1000.0, -0.0}, {200, 950.25, from_bits(UINT64_C(0x7ff8000000000456))},
        {300, 975.5, -2.25}, {400, 1010.75, 3.5}
    };
    PointVisit seen;
    HeldEquityPointV1 held{};
    {
        MockPointSource provider(expected);
        const EquityPointSourceV1& source = provider;
        require(source.size() == 4, "mock must contain four points before reader checks");
        for (LogicalIndexV1 i = 0; i < source.size(); ++i) same_point(source.at(i), expected[i]);
        auto detached = source.at(0);
        detached.equity = -1.0;
        same_double(detached.equity, -1.0);
        same_point(source.at(0), expected[0]);
        auto result = source.visit(0, 4, &seen, collect_point);
        require(result.complete && result.visited == 4, "point pass must consume four points");
        require(seen.indices == std::vector<LogicalIndexV1>({0, 1, 2, 3}),
                "point pass must preserve report order");
        std::array<pf_equity_point_t, 2> page{};
        require(source.read_page(2, page.data(), 2) == 2, "point page must respect capacity");
        same_point(page[0], expected[2]);
        same_point(page[1], expected[3]);
        require(source.read_page(3, page.data(), 2) == 1, "last point page must be short");
        require(source.read_page(4, nullptr, 0) == 0, "empty point page permits a null buffer");
        PointVisit stopped;
        stopped.keep_going = false;
        result = source.visit(1, 3, &stopped, collect_point);
        require(!result.complete && result.visited == 1
                    && stopped.indices == std::vector<LogicalIndexV1>({1}),
                "false point callback must stop after the delivered point");
        PointVisit empty;
        result = source.visit(4, 0, &empty, collect_point);
        require(result.complete && result.visited == 0 && empty.rows.empty(),
                "empty point range must be complete without callbacks");
        require_throws<std::out_of_range>([&] { source.at(4); }, "point at(size) must throw");
        require_throws<std::out_of_range>(
            [&] { source.read_page(5, page.data(), 1); }, "point page beyond end must throw");
        require_throws<std::out_of_range>([&] {
            source.visit(1, std::numeric_limits<LogicalIndexV1>::max(), &empty, collect_point);
        }, "overflowing point range must throw before callbacks");
        require_throws<std::invalid_argument>(
            [&] { source.read_page(4, nullptr, 1); }, "null point buffer must throw");
        require_throws<std::invalid_argument>(
            [&] { source.visit(4, 0, nullptr, nullptr); }, "null point visitor must throw");
        require(empty.rows.empty(), "invalid point range must invoke no callbacks");
        held = {source.size() - 1, source.at(source.size() - 1), true};
    }
    require(seen.rows.size() == 4, "callback copies must survive provider destruction");
    for (std::size_t i = 0; i < expected.size(); ++i) same_point(seen.rows[i], expected[i]);
    require(held.present && held.index == 3, "held point is one detached final logical value");
    same_point(held.value, expected[3]);
    const HeldEquityPointV1 absent{};
    require(!absent.present && absent.index == 0, "an absent held point must be explicit");
    same_point(absent.value, pf_equity_point_t{});
    MockPointSource no_points(std::vector<pf_equity_point_t>{});
    PointVisit empty;
    require(no_points.size() == 0, "empty captured provider must explicitly have zero points");
    const auto result = no_points.visit(0, 0, &empty, collect_point);
    require(result.complete && result.visited == 0 && empty.rows.empty(),
            "empty point provider has a valid complete pass");
    require_throws<std::out_of_range>(
        [&] { no_points.at(0); }, "empty point provider has no point zero");
}
} // namespace

int main() {
    // Same eight bars as fixtures/o1_report_contract/bars.csv. Three entries
    // produce two reversal closes and one report-only range-end row.
    const std::array<pineforge::Bar, 8> bars{{
        {100, 102, 99, 101, 10, INT64_C(1704067200000)},
        {101, 104, 100, 103, 11, INT64_C(1704067260000)},
        {103, 104, 100, 101, 12, INT64_C(1704067320000)},
        {101, 102, 97, 98, 13, INT64_C(1704067380000)},
        {98, 101, 97, 100, 14, INT64_C(1704067440000)},
        {100, 104, 99, 103, 15, INT64_C(1704067500000)},
        {103, 106, 102, 105, 16, INT64_C(1704067560000)},
        {105, 106, 101, 102, 17, INT64_C(1704067620000)}
    }};
    pineforge::ReportC report{};
    try {
        ContractFixture fixture;
        fixture.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(fixture.last_error().empty(), fixture.last_error().c_str());
        fixture.fill_report(&report);
        require(report.input_bars_processed == 8, "fixture must consume eight input bars");
        require(report.script_bars_processed == 8, "fixture must evaluate eight script bars");
        require(report.total_trades == 3, "fixture must produce three logical trades");
        require(report.trades_len == 3 && report.trades, "legacy fixture must own all trade rows");
        require(report.equity_curve_len == 8 && report.equity_curve,
                "legacy fixture must own eight equity points");
        require(report.metrics.all.num_trades == 3, "metrics must cover all three trades");
        require(report.metrics.longs.num_trades == 2, "fixture must include two long trades");
        require(report.metrics.shorts.num_trades == 1, "fixture must include one short trade");
        require(report.trades[2].open_at_end == 1, "logical total must include the terminal row");
        require(report.equity_curve[0].time_ms == bars.front().timestamp,
                "curve must start at the first script coordinate");
        require(report.equity_curve[7].time_ms == bars.back().timestamp,
                "curve must finish at the last script coordinate");
        check_trade_readers();
        check_point_readers();
        pineforge::BacktestEngine::free_report(&report);
        std::puts("PASS O1 report contract: bars=8 trades=3 equity_points=8; "
                  "legacy arrays preserved; mock_rows=4 mock_points=4; "
                  "value/index/page/forward/backward visitors checked");
        return 0;
    } catch (const std::exception& error) {
        pineforge::BacktestEngine::free_report(&report);
        std::fprintf(stderr, "FAIL O1 report contract: %s\n", error.what());
        return 1;
    }
}
#endif
