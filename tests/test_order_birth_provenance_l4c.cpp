#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"

#include <pineforge/pending_order_mirror.hpp>

#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;

namespace {
int failed = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failed; } } while (0)
const double nan = std::numeric_limits<double>::quiet_NaN();

void rejects(const std::function<void()>& f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

void value_contract() {
    const auto open = BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 4);
    const auto close = BirthCursor::point(BirthCursorDomain::HistoricalPath, 3, 4);
    const auto segment = BirthCursor::segment(BirthCursorDomain::HistoricalPath, 1, 4);
    const auto first = OrderBirth::fill_evaluation(2, 120000, open, 100, 7, 7, 1);
    const auto later = OrderBirth::fill_evaluation(2, 120000, open, 100, 8, 8, 2);
    const auto terminal = OrderBirth::fill_evaluation(2, 120000, close, 100, 9, 9, 3);
    CHECK(first.from_fill());
    CHECK(!first.at_terminal_fill());
    CHECK(compat::pine::first_open_fill_evaluation(first));
    CHECK(!compat::pine::first_open_fill_evaluation(later));
    CHECK(terminal.at_terminal_fill());
    CHECK(!terminal.cursor().first_point());
    CHECK(terminal.cursor().following_segment() == -1);
    CHECK(first.cursor_price() == terminal.cursor_price());
    CHECK(first.cursor().index() != terminal.cursor().index());
    CHECK(first.cause() == OrderBirthCause::FillEvaluation);
    CHECK(first.bar() == 2);
    CHECK(first.timestamp() == 120000);
    CHECK(first.cursor().domain() == BirthCursorDomain::HistoricalPath);
    CHECK(first.cursor().position() == BirthCursorPosition::Point);
    CHECK(first.cursor().index() == 0);
    CHECK(first.cursor().count() == 4);
    CHECK(first.first_fill() == 7);
    CHECK(first.last_fill() == 7);
    CHECK(first.evaluation_ordinal() == 1);
    CHECK(first.cursor().first_point());
    CHECK(!first.cursor().terminal_point());
    CHECK(segment.domain() == BirthCursorDomain::HistoricalPath);
    CHECK(segment.position() == BirthCursorPosition::Segment);
    CHECK(segment.index() == 1);
    CHECK(segment.count() == 4);
    CHECK(segment.following_segment() == 1);
    CHECK(!segment.first_point());
    CHECK(!segment.terminal_point());
    const auto batch = OrderBirth::fill_evaluation(2, 120000, open, 100, 10, 12, 1);
    CHECK(batch.first_fill() == 10);
    CHECK(batch.last_fill() == 12);
    CHECK(batch.evaluation_ordinal() == 1);
    const auto copied = batch;
    CHECK(copied.first_fill() == batch.first_fill());
    CHECK(copied.last_fill() == batch.last_fill());
    CHECK(copied.cursor().index() == batch.cursor().index());
    CHECK(copied.timestamp() == batch.timestamp());
    CHECK(copied.cause() == OrderBirthCause::FillEvaluation);
    CHECK(copied.bar() == 2);
    CHECK(copied.cursor_price() == 100);
    const auto chart = OrderBirth::chart_evaluation(4, 240000);
    CHECK(!chart.from_fill());
    CHECK(chart.cause() == OrderBirthCause::ChartEvaluation);
    CHECK(chart.bar() == 4);
    CHECK(chart.timestamp() == 240000);
    const auto direct = OrderBirth::direct_command(-1, 7);
    CHECK(!direct.from_fill());
    CHECK(direct.cause() == OrderBirthCause::DirectCommand);
    CHECK(direct.bar() == -1);
    CHECK(direct.timestamp() == 7);
    CHECK(!direct.at_terminal_fill());
    CHECK(direct.first_fill() == 0);
    rejects([&] { OrderBirth::fill_evaluation(2, 0, open, 100, 0, 1, 1); });
    rejects([&] { OrderBirth::fill_evaluation(2, 0, open, 100, 3, 2, 1); });
    rejects([&] { OrderBirth::fill_evaluation(2, 0, open, 100, 1, 1, 0); });
    rejects([&] { OrderBirth::fill_evaluation(2, 0, open, nan, 1, 1, 1); });
    rejects([&] { OrderBirth::fill_evaluation(2, 0, BirthCursor{}, 100, 1, 1, 1); });
    rejects([&] { BirthCursor::point(BirthCursorDomain::HistoricalPath, 4, 4); });
    rejects([&] { BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 3); });
    rejects([&] { BirthCursor::segment(BirthCursorDomain::HistoricalPath, 3, 4); });
    rejects([&] { BirthCursor::point(BirthCursorDomain::None, 0, 4); });
}

class BirthRoute final : public pineforge::source::PineStrategyHost {
public:
    BirthRoute() {
        initial_capital_ = 100000;
        calc_on_order_fills_ = true;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        pyramiding_ = 10;
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("E", true, nan, nan, 1);
        if (bar_index_ == 1 && !issued_) {
            issued_ = true;
            strategy_entry("W", true, nan, nan, 1);
            strategy_exit("X", "E", 110, 95);
            rows = l4c_pending_orders();
        }
    }
    bool issued_ = false;
    std::vector<pineforge::source::L4cPendingOrder> rows;
};

void public_projection_contract() {
    BirthRoute route;
    const Bar bars[] = {{100,100,100,100,1,0}, {100,110,95,108,1,60000},
                        {108,109,107,108,1,120000}};
    route.run(bars, 3);
    CHECK(route.last_error().empty());
    CHECK(route.issued_);
    CHECK(!route.rows.empty());
    if (route.rows.empty()) return;
    const auto* entry = &route.rows.front();
    for (const auto& row : route.rows) if (row.id == "W") entry = &row;
    CHECK(entry->id == "W");
    CHECK(entry->birth.cause() != OrderBirthCause::Unattributed);
    CHECK(entry->birth.bar() >= 0);
    CHECK(entry->birth.timestamp() >= 0);
    CHECK(entry->birth.cursor().count() >= 0);
    CHECK(entry->birth.evaluation_ordinal() >= 0);
    CHECK(entry->incarnation != 0 || entry->created_seq >= 0);
    bool found_exit = false;
    for (const auto& row : route.rows) if (row.id == "X" && row.from_entry == "E") found_exit = true;
    CHECK(found_exit);
}
} // namespace

int main() {
    value_contract();
    public_projection_contract();
    std::printf("order birth provenance: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
